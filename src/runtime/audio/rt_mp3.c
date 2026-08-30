//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/audio/rt_mp3.c
// Purpose: MPEG-1/2/2.5 Layer III (MP3) audio decoder.
// Key invariants:
//   - Full ISO 11172-3 / ISO 13818-3 Layer III decode: MPEG-1, MPEG-2 (LSF)
//     and MPEG-2.5; mono, stereo, dual and joint (M/S + intensity) stereo
//   - Pipeline: frame sync → side info → scalefactors (scfsi / LSF
//     partitions) → Huffman (all 32 pair tables + count1 A/B) → requantize
//     (short-block reorder) → joint stereo → alias reduction → IMDCT →
//     overlap-add + frequency inversion → polyphase synthesis
//   - All tables from ISO 11172-3 / ISO 13818-3; the MPEG-2.5 11025 / 12000
//     Hz band tables follow the LAME convention (there is no ISO text)
//   - Output: interleaved 16-bit signed PCM; verified against independent
//     decoders at ~80 dB SNR (see docs/internals/mp3-decoder-conformance.md)
// Ownership/Lifetime:
//   - Decoder state owned by mp3_decoder_t; caller frees via mp3_decoder_free
//   - Output PCM buffer malloc'd by decoder; caller frees
// Links: rt_mp3.h (public API), rt_mp3_tables.h (spec constants)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded batch and frame-streaming MP3 decoding to PCM.
/// @details The decoder parses MPEG Layer III headers and side information,
///          maintains the main-data reservoir, reconstructs spectral samples,
///          and performs IMDCT plus polyphase synthesis into interleaved signed
///          16-bit PCM. Decoder instances own overlap/reservoir state; batch
///          output buffers transfer to the caller, while streaming output is a
///          borrowed frame buffer owned by the stream.

#include "rt_mp3.h"
#include "rt_file_stdio.h"
#include "rt_mp3_tables.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MP3_MAX_GRANULES 2
#define MP3_MAX_CHANNELS 2
#define MP3_SUBBANDS 32
#define MP3_SBLIMIT 576 // 32 subbands × 18 samples
#define MP3_MAX_DECODED_PCM_BYTES ((size_t)100 * 1024 * 1024)

//===----------------------------------------------------------------------===//
// MSB-first bitstream reader
//===----------------------------------------------------------------------===//

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t pos; // bit position
    int error;  // sticky invalid-request/truncation indicator
} mp3_bits_t;

// ---------------------------------------------------------------------------
// MP3 uses an MSB-first bitstream, opposite of Vorbis. The struct
// tracks position by *bit* offset for cheap arbitrary-bit reads
// (header field widths from 1..32 bits).
// ---------------------------------------------------------------------------

/// @brief Reset an MSB-first bit reader onto a byte buffer.
/// @param b Reader state to initialize.
/// @param data Borrowed encoded bytes.
/// @param byte_len Number of readable bytes; converted to the stored bit length.
static void mp3_bits_init(mp3_bits_t *b, const uint8_t *data, size_t byte_len) {
    b->data = data;
    b->len = 0;
    b->pos = 0;
    b->error = 0;
    if ((!data && byte_len != 0) || byte_len > SIZE_MAX / 8u) {
        b->error = 1;
        return;
    }
    b->len = byte_len * 8u;
}

/// @brief Pull up to 32 MSB-first bits from the current reader position.
/// @details If the buffer ends mid-field, returns zero and marks the reader
///          failed. The failure is sticky for the remainder of the packet.
/// @param b Initialized bit reader.
/// @param count Number of bits to consume; non-positive values consume none.
/// @return Decoded unsigned field, or zero when no bits are requested/available.
static uint32_t mp3_bits_read(mp3_bits_t *b, int count) {
    if (!b || b->error)
        return 0;
    if (count == 0)
        return 0;
    if (count < 0 || count > 32 || (size_t)count > b->len - b->pos) {
        b->pos = b->len;
        b->error = 1;
        return 0;
    }
    uint32_t val = 0;
    for (int i = 0; i < count; i++) {
        size_t byte_idx = b->pos / 8;
        int bit_idx = 7 - (int)(b->pos % 8); // MSB first
        if (b->data[byte_idx] & (1 << bit_idx))
            val |= (1u << (count - 1 - i));
        b->pos++;
    }
    return val;
}

//===----------------------------------------------------------------------===//
// Frame header
//===----------------------------------------------------------------------===//

typedef struct {
    int mpeg_version; // 0=2.5, 1=reserved, 2=MPEG2, 3=MPEG1
    int layer;        // 1=III, 2=II, 3=I (encoded as 4-layer)
    int bitrate;
    int sample_rate;
    int padding;
    int channel_mode; // 0=stereo, 1=joint, 2=dual, 3=mono
    int mode_ext;
    int channels;
    int frame_size; // total frame bytes including header
    int crc_size;
    int side_info_size;
    int main_data_size;
} mp3_frame_header_t;

/// @brief Parse the 4-byte MP3 frame header into `out`.
///
/// Validates the 11-bit sync word, decodes the version / layer /
/// bitrate / sample-rate / channel-mode fields per ISO 11172-3,
/// and computes the frame size. Returns -1 on invalid header.
/// @param hdr Pointer to at least four frame-header bytes.
/// @param out Receives parsed header fields and derived sizes on success.
/// @return `0` for a supported Layer III header, otherwise `-1`.
static int mp3_parse_header(const uint8_t *hdr, mp3_frame_header_t *out) {
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!hdr)
        return -1;

    // Sync word check: 11 bits of 1
    if (hdr[0] != 0xFF || (hdr[1] & 0xE0) != 0xE0)
        return -1;

    out->mpeg_version = (hdr[1] >> 3) & 0x03;
    int layer_bits = (hdr[1] >> 1) & 0x03;
    // int crc = !((hdr[1]) & 0x01);

    // Layer: 01=III, 10=II, 11=I
    if (layer_bits == 0)
        return -1;               // reserved
    out->layer = 4 - layer_bits; // convert to 1=I, 2=II, 3=III

    if (out->layer != 3)
        return -1; // only Layer III

    int ver_idx;
    if (out->mpeg_version == 3)
        ver_idx = 1; // MPEG1
    else if (out->mpeg_version == 2)
        ver_idx = 0; // MPEG2
    else if (out->mpeg_version == 0)
        ver_idx = 0; // MPEG2.5
    else
        return -1; // reserved

    int bitrate_idx = (hdr[2] >> 4) & 0x0F;
    if (bitrate_idx == 0 || bitrate_idx == 15)
        return -1;
    out->bitrate = mp3_bitrate_table[ver_idx][2][bitrate_idx] * 1000;

    int sr_idx = (hdr[2] >> 2) & 0x03;
    if (sr_idx == 3)
        return -1;
    int sr_ver = (out->mpeg_version == 3) ? 2 : (out->mpeg_version == 2) ? 1 : 0;
    out->sample_rate = mp3_samplerate_table[sr_ver][sr_idx];

    out->padding = (hdr[2] >> 1) & 0x01;
    out->channel_mode = (hdr[3] >> 6) & 0x03;
    out->mode_ext = (hdr[3] >> 4) & 0x03;
    out->channels = (out->channel_mode == 3) ? 1 : 2;
    out->crc_size = (hdr[1] & 0x01) ? 0 : 2;
    if ((hdr[3] & 0x03) == 2)
        return -1; // reserved emphasis

    // Frame size for Layer III
    if (out->mpeg_version == 3) {
        // MPEG1
        out->frame_size = 144 * out->bitrate / out->sample_rate + out->padding;
        out->side_info_size = (out->channels == 1) ? 17 : 32;
    } else {
        // MPEG2/2.5
        out->frame_size = 72 * out->bitrate / out->sample_rate + out->padding;
        out->side_info_size = (out->channels == 1) ? 9 : 17;
    }

    out->main_data_size = out->frame_size - 4 - out->crc_size - out->side_info_size;
    if (out->main_data_size < 0)
        return -1;

    return 0;
}

//===----------------------------------------------------------------------===//
// Side information
//===----------------------------------------------------------------------===//

typedef struct {
    int part2_3_length;
    int big_values;
    int global_gain;
    int scalefac_compress;
    int window_switching;
    int block_type;
    int mixed_block;
    int table_select[3];
    int subblock_gain[3];
    int region0_count;
    int region1_count;
    int preflag;
    int scalefac_scale;
    int count1table_select;
} mp3_granule_info_t;

typedef struct {
    int main_data_begin;
    int scfsi[MP3_MAX_CHANNELS][4];
    mp3_granule_info_t granules[MP3_MAX_GRANULES][MP3_MAX_CHANNELS];
} mp3_side_info_t;

/// @brief Parse the 17- or 32-byte Layer III side-info block (per granule, per channel).
///
/// Extracts main-data-begin pointer, scfsi flags, and per-granule
/// fields (part2_3_length, big-values, global gain, scalefac
/// compress, window switching flag, table selectors, region counts,
/// preflag, scalefac_scale, count1table_select). These drive the
/// Huffman + scalefactor decode for each granule's main data.
/// @param data Borrowed side-information bytes.
/// @param size Side-information length in bytes.
/// @param channels Parsed channel count.
/// @param mpeg1 Non-zero for the two-granule MPEG-1 layout.
/// @param si Receives parsed stream and granule fields.
/// @return `0` after parsing.
static int mp3_parse_side_info(
    const uint8_t *data, int size, int channels, int mpeg1, mp3_side_info_t *si) {
    if (si)
        memset(si, 0, sizeof(*si));
    int expected_size = mpeg1 ? (channels == 1 ? 17 : 32) : (channels == 1 ? 9 : 17);
    if (!data || !si || (channels != 1 && channels != 2) || (mpeg1 != 0 && mpeg1 != 1) ||
        size != expected_size)
        return -1;
    mp3_bits_t bits;
    mp3_bits_init(&bits, data, (size_t)size);

    si->main_data_begin = (int)mp3_bits_read(&bits, mpeg1 ? 9 : 8);
    // Private bits
    mp3_bits_read(&bits, mpeg1 ? (channels == 1 ? 5 : 3) : (channels == 1 ? 1 : 2));

    // SCFSI (only MPEG1)
    if (mpeg1) {
        for (int ch = 0; ch < channels; ch++)
            for (int i = 0; i < 4; i++)
                si->scfsi[ch][i] = (int)mp3_bits_read(&bits, 1);
    }

    int ngranules = mpeg1 ? 2 : 1;
    for (int gr = 0; gr < ngranules; gr++) {
        for (int ch = 0; ch < channels; ch++) {
            mp3_granule_info_t *gi = &si->granules[gr][ch];
            gi->part2_3_length = (int)mp3_bits_read(&bits, 12);
            gi->big_values = (int)mp3_bits_read(&bits, 9);
            if (gi->big_values > 288)
                return -1;
            gi->global_gain = (int)mp3_bits_read(&bits, 8);
            gi->scalefac_compress = (int)mp3_bits_read(&bits, mpeg1 ? 4 : 9);
            gi->window_switching = (int)mp3_bits_read(&bits, 1);
            if (gi->window_switching) {
                gi->block_type = (int)mp3_bits_read(&bits, 2);
                if (gi->block_type == 0)
                    return -1;
                gi->mixed_block = (int)mp3_bits_read(&bits, 1);
                gi->table_select[0] = (int)mp3_bits_read(&bits, 5);
                gi->table_select[1] = (int)mp3_bits_read(&bits, 5);
                gi->table_select[2] = 0;
                gi->subblock_gain[0] = (int)mp3_bits_read(&bits, 3);
                gi->subblock_gain[1] = (int)mp3_bits_read(&bits, 3);
                gi->subblock_gain[2] = (int)mp3_bits_read(&bits, 3);
                if (gi->block_type == 2 && !gi->mixed_block)
                    gi->region0_count = 8;
                else
                    gi->region0_count = 7;
                gi->region1_count = 20 - gi->region0_count;
            } else {
                gi->block_type = 0;
                gi->mixed_block = 0;
                gi->table_select[0] = (int)mp3_bits_read(&bits, 5);
                gi->table_select[1] = (int)mp3_bits_read(&bits, 5);
                gi->table_select[2] = (int)mp3_bits_read(&bits, 5);
                gi->region0_count = (int)mp3_bits_read(&bits, 4);
                gi->region1_count = (int)mp3_bits_read(&bits, 3);
                gi->subblock_gain[0] = gi->subblock_gain[1] = gi->subblock_gain[2] = 0;
            }
            if (mpeg1) {
                gi->preflag = (int)mp3_bits_read(&bits, 1);
                gi->scalefac_scale = (int)mp3_bits_read(&bits, 1);
                gi->count1table_select = (int)mp3_bits_read(&bits, 1);
            } else {
                gi->preflag = 0;
                gi->scalefac_scale = (int)mp3_bits_read(&bits, 1);
                gi->count1table_select = (int)mp3_bits_read(&bits, 1);
            }
        }
    }

    return bits.error ? -1 : 0;
}

//===----------------------------------------------------------------------===//
// Decoder state
//===----------------------------------------------------------------------===//

struct mp3_decoder {
    // Overlap-add buffer per channel per subband
    float overlap[MP3_MAX_CHANNELS][MP3_SUBBANDS][18];

    // Polyphase synthesis FIFO (V buffer) per channel
    float synth_buf[MP3_MAX_CHANNELS][1024];
    int synth_offset[MP3_MAX_CHANNELS];

    // Main data buffer (for bit reservoir)
    uint8_t reservoir[2048];
    int reservoir_size;
};

/// @brief Allocate zero-initialized MP3 decoder state.
/// @return Caller-owned decoder, or NULL on allocation failure.
static void mp3_ensure_tables(void);

mp3_decoder_t *mp3_decoder_new(void) {
    mp3_ensure_tables();
    mp3_decoder_t *dec = (mp3_decoder_t *)calloc(1, sizeof(mp3_decoder_t));
    return dec;
}

/// @brief Free an MP3 decoder state.
/// @param dec Decoder returned by @ref mp3_decoder_new; NULL is accepted.
void mp3_decoder_free(mp3_decoder_t *dec) {
    free(dec);
}

/// @brief Wipe per-frame decoder state without freeing the allocation.
/// @details Zeroes the overlap-add buffer, polyphase synthesis ring,
///          ring-offsets, and the main-data reservoir. Called before
///          each batch decode and on stream rewind so previous frames
///          don't bleed audio into the new playback position.
/// @param dec Decoder instance; NULL is a no-op.
static void mp3_decoder_reset(mp3_decoder_t *dec) {
    if (!dec)
        return;
    memset(dec->overlap, 0, sizeof(dec->overlap));
    memset(dec->synth_buf, 0, sizeof(dec->synth_buf));
    memset(dec->synth_offset, 0, sizeof(dec->synth_offset));
    dec->reservoir_size = 0;
}

//===----------------------------------------------------------------------===//
// Huffman pair decode
//===----------------------------------------------------------------------===//

/// @brief Decode one (x, y) pair from a Huffman tree stored as flat node array.
/// @param bits Main-data bit reader.
/// @param tree Flat branch/leaf table.
/// @param tree_size Number of entries in @p tree.
/// @param x Receives the high-nibble symbol.
/// @param y Receives the low-nibble symbol.
/// @return `0` on reaching a leaf, or `-1` for an invalid tree/walk.
static int mp3_huff_tree_decode(
    mp3_bits_t *bits, const mp3_huff_node_t *tree, int tree_size, int *x, int *y) {
    if (!tree || tree_size <= 0)
        return -1;
    int node = 0;
    int depth = 0;
    while (depth < 32) { // guard against infinite loops
        if (node < 0 || node >= tree_size)
            return -1;
        int16_t val = tree[node].value;
        if (val >= 0) {
            // Leaf node
            *x = (val >> 4) & 0x0F;
            *y = val & 0x0F;
            return 0;
        }
        // Branch: read one bit to choose left/right child
        int bit = (int)mp3_bits_read(bits, 1);
        if (bits->error)
            return -1;
        node = -val + bit;
        depth++;
    }
    return -1;
}

/// @brief Get the flat Huffman tree that decodes @p table_idx.
/// @details Every ISO pair table is backed by a generated tree: 1-3, 5-13 and
///          15 own theirs, 16-23 share table 16's codes and 24-31 share table
///          24's (only their linbits differ). Table 0 has no tree (every pair
///          is (0, 0)); the reserved tables 4 and 14 have none either.
/// @param table_idx Layer III Huffman table index.
/// @param out_size Receives the flat tree's entry count, or zero when absent.
/// @return Static tree pointer, or NULL when no tree exists for the index.
static const mp3_huff_node_t *mp3_get_huff_tree(int table_idx, int *out_size) {
#define MP3_HUFF_RETURN_TREE(tree)                                                                 \
    do {                                                                                           \
        *out_size = (int)(sizeof(tree) / sizeof((tree)[0]));                                       \
        return (tree);                                                                             \
    } while (0)
    switch (table_idx) {
        case 1:
            MP3_HUFF_RETURN_TREE(mp3_htree_1);
        case 2:
            MP3_HUFF_RETURN_TREE(mp3_htree_2);
        case 3:
            MP3_HUFF_RETURN_TREE(mp3_htree_3);
        case 5:
            MP3_HUFF_RETURN_TREE(mp3_htree_5);
        case 6:
            MP3_HUFF_RETURN_TREE(mp3_htree_6);
        case 7:
            MP3_HUFF_RETURN_TREE(mp3_htree_7);
        case 8:
            MP3_HUFF_RETURN_TREE(mp3_htree_8);
        case 9:
            MP3_HUFF_RETURN_TREE(mp3_htree_9);
        case 10:
            MP3_HUFF_RETURN_TREE(mp3_htree_10);
        case 11:
            MP3_HUFF_RETURN_TREE(mp3_htree_11);
        case 12:
            MP3_HUFF_RETURN_TREE(mp3_htree_12);
        case 13:
            MP3_HUFF_RETURN_TREE(mp3_htree_13);
        case 15:
            MP3_HUFF_RETURN_TREE(mp3_htree_15);
        case 16:
        case 17:
        case 18:
        case 19:
        case 20:
        case 21:
        case 22:
        case 23:
            MP3_HUFF_RETURN_TREE(mp3_htree_16);
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29:
        case 30:
        case 31:
            MP3_HUFF_RETURN_TREE(mp3_htree_24);
        default:
            *out_size = 0;
            return NULL;
    }
#undef MP3_HUFF_RETURN_TREE
}

/// @brief Get the count1 quadruple tree selected by `count1table_select`.
/// @param select Side-info `count1table_select` bit (0 = table A, 1 = table B).
/// @param out_size Receives the flat tree's entry count.
/// @return Static tree pointer (never NULL).
static const mp3_huff_node_t *mp3_get_quad_tree(int select, int *out_size) {
    if (select) {
        *out_size = (int)(sizeof(mp3_htree_quad_b) / sizeof(mp3_htree_quad_b[0]));
        return mp3_htree_quad_b;
    }
    *out_size = (int)(sizeof(mp3_htree_quad_a) / sizeof(mp3_htree_quad_a[0]));
    return mp3_htree_quad_a;
}

/// @brief Whether @p table_idx names a decodable big_values table.
/// @details Table 0 and every ISO pair table 1-3, 5-13, 15-31 decode; the
///          reserved indices 4 and 14 (and anything out of range) do not, and
///          a frame that selects one is rejected as unsupported data.
/// @param table_idx Huffman table index from the side-info `table_select`.
/// @return 1 if supported, 0 otherwise.
static int mp3_huff_table_supported(int table_idx) {
    if (table_idx < 0 || table_idx >= 32)
        return 0;
    return table_idx != 4 && table_idx != 14;
}

/// @brief Decode one big_values (x, y) pair from the selected ISO table.
/// @param bits Main-data bit reader.
/// @param table_idx Layer III Huffman table index (0 consumes no bits).
/// @param x Receives the first non-negative spectral value (before linbits).
/// @param y Receives the second non-negative spectral value (before linbits).
/// @return `0` on success, or `-1` for a reserved table or an invalid walk.
static int mp3_huff_decode_pair(mp3_bits_t *bits, int table_idx, int *x, int *y) {
    if (!bits || !x || !y)
        return -1;
    *x = *y = 0;
    if (table_idx == 0)
        return 0;
    int tree_size = 0;
    const mp3_huff_node_t *tree = mp3_get_huff_tree(table_idx, &tree_size);
    if (!tree)
        return -1;
    if (mp3_huff_tree_decode(bits, tree, tree_size, x, y) != 0) {
        *x = *y = 0;
        return -1;
    }
    return 0;
}

/// @brief Verify every stored Huffman tree against the ISO invariants.
/// @details Walks each pair table (1-3, 5-13, 15-31 through their shared
///          trees) and both count1 quad tables, checking that every branch
///          index stays inside the tree, no walk exceeds 24 bits, the code is
///          complete (Kraft sum exactly 1), and the leaf set is exactly the
///          (max_val + 1)^2 value square (16 quad symbols for A / B) with no
///          duplicate leaves. Diagnostic entry point for the unit tests.
/// @return `0` when every tree passes, otherwise `-(table index)` of the first
///         failing pair table (`-32` / `-33` for quad table A / B).
static int mp3_huffman_check_tree(const mp3_huff_node_t *tree,
                                  int tree_size,
                                  int node,
                                  int depth,
                                  uint64_t *kraft,
                                  uint8_t *seen) {
    if (node < 0 || node >= tree_size || depth > 24)
        return -1;
    int16_t val = tree[node].value;
    if (val >= 0) {
        if (val > 0xFF || seen[val])
            return -1;
        seen[val] = 1;
        *kraft += (uint64_t)1 << (32 - depth);
        return 0;
    }
    if (mp3_huffman_check_tree(tree, tree_size, -val, depth + 1, kraft, seen) != 0)
        return -1;
    return mp3_huffman_check_tree(tree, tree_size, -val + 1, depth + 1, kraft, seen);
}

int mp3_huffman_self_check(void) {
    for (int table = 1; table < 34; table++) {
        if (table == 4 || table == 14)
            continue;
        int tree_size = 0;
        const mp3_huff_node_t *tree = table < 32 ? mp3_get_huff_tree(table, &tree_size)
                                                 : mp3_get_quad_tree(table - 32, &tree_size);
        if (!tree || tree_size <= 0)
            return -table;
        uint64_t kraft = 0;
        uint8_t seen[256];
        memset(seen, 0, sizeof(seen));
        if (mp3_huffman_check_tree(tree, tree_size, 0, 0, &kraft, seen) != 0)
            return -table;
        if (kraft != ((uint64_t)1 << 32))
            return -table;
        int max_val = table < 32 ? mp3_huff_info[table].max_val : 0;
        for (int v = 0; v < 256; v++) {
            int x = v >> 4;
            int y = v & 0xF;
            int expected = table < 32 ? (x <= max_val && y <= max_val) : (x == 0);
            if ((seen[v] != 0) != (expected != 0))
                return -table;
        }
        if (table < 32 && mp3_huff_info[table].tree_size != tree_size)
            return -table;
    }
    return 0;
}

//===----------------------------------------------------------------------===//
// Transform tables
//===----------------------------------------------------------------------===//

static double mp3_cos36_table[36][18];
static double mp3_cos12_table[12][6];
static double mp3_synth_n_table[64][32];
static int mp3_tables_ready = 0;

/// @brief Build the IMDCT and synthesis cosine tables once per process.
/// @details Every entry is a pure function of its indices, so a first call
///          racing on two threads computes identical values.
static void mp3_ensure_tables(void) {
    if (mp3_tables_ready)
        return;
    for (int i = 0; i < 36; i++)
        for (int k = 0; k < 18; k++)
            mp3_cos36_table[i][k] =
                cos(M_PI / 72.0 * (double)(2 * i + 1 + 18) * (double)(2 * k + 1));
    for (int i = 0; i < 12; i++)
        for (int k = 0; k < 6; k++)
            mp3_cos12_table[i][k] =
                cos(M_PI / 24.0 * (double)(2 * i + 1 + 6) * (double)(2 * k + 1));
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < 32; k++)
            mp3_synth_n_table[i][k] = cos((double)(16 + i) * (double)(2 * k + 1) * M_PI / 64.0);
    mp3_tables_ready = 1;
}

//===----------------------------------------------------------------------===//
// IMDCT
//===----------------------------------------------------------------------===//

/// @brief 36-point IMDCT used for "long" blocks (most steady-state samples).
/// @details x[i] = sum_{k<18} X[k] cos(pi/72 (2i + 19)(2k + 1)), ISO 11172-3 2.4.3.4.10.3.
/// @param in Eighteen frequency-domain coefficients.
/// @param out Receives 36 time-domain samples.
static void mp3_imdct36(const float *in, float *out) {
    for (int i = 0; i < 36; i++) {
        double sum = 0.0;
        const double *row = mp3_cos36_table[i];
        for (int k = 0; k < 18; k++)
            sum += (double)in[k] * row[k];
        out[i] = (float)sum;
    }
}

/// @brief 12-point IMDCT used for "short" blocks (transients — three sub-blocks per granule).
/// @details x[i] = sum_{k<6} X[k] cos(pi/24 (2i + 7)(2k + 1)).
/// @param in Six frequency-domain coefficients.
/// @param out Receives 12 time-domain samples.
static void mp3_imdct12(const float *in, float *out) {
    for (int i = 0; i < 12; i++) {
        double sum = 0.0;
        const double *row = mp3_cos12_table[i];
        for (int k = 0; k < 6; k++)
            sum += (double)in[k] * row[k];
        out[i] = (float)sum;
    }
}

//===----------------------------------------------------------------------===//
// Polyphase synthesis filterbank
//===----------------------------------------------------------------------===//

/// @brief Convert a normalized synthesis sample to saturated signed 16-bit PCM.
/// @param sample Floating-point sample.
/// @return Zero for NaN, otherwise a value saturated to the int16 range.
static int16_t mp3_pcm_s16_from_double(double sample) {
    if (isnan(sample))
        return 0;
    if (sample >= 1.0)
        return INT16_MAX;
    if (sample <= -1.0)
        return INT16_MIN;
    return (int16_t)(sample * 32768.0);
}

/// @brief Polyphase synthesis filter — converts subband samples back to PCM.
///
/// The ISO 11172-3 Annex B synthesis procedure: the 32 subband samples are
/// matrixed into 64 new V values (N[i][k] = cos((16 + i)(2k + 1) pi / 64)),
/// pushed onto the 1024-entry V FIFO, the U vector is gathered from the first
/// and last 32 entries of each 128-block, windowed by the D[] table and summed
/// sixteen-fold into 32 output samples. Per-channel state (the V ring and its
/// write offset) carries across calls.
/// @param dec Decoder carrying per-channel synthesis history.
/// @param ch Zero-based channel index.
/// @param subbands Thirty-two reconstructed subband samples.
/// @param pcm_out Receives 32 clamped signed 16-bit PCM samples.
static void mp3_synth_filter(mp3_decoder_t *dec,
                             int ch,
                             const float subbands[32],
                             int16_t *pcm_out) {
    float *v = dec->synth_buf[ch];
    int offset = (dec->synth_offset[ch] - 64 + 1024) & 1023;
    dec->synth_offset[ch] = offset;

    for (int i = 0; i < 64; i++) {
        double sum = 0.0;
        const double *row = mp3_synth_n_table[i];
        for (int k = 0; k < 32; k++)
            sum += row[k] * (double)subbands[k];
        v[(offset + i) & 1023] = (float)sum;
    }

    for (int j = 0; j < 32; j++) {
        double sum = 0.0;
        for (int p = 0; p < 8; p++) {
            sum += (double)mp3_synth_d[j + 64 * p] * (double)v[(offset + 128 * p + j) & 1023];
            sum += (double)mp3_synth_d[j + 32 + 64 * p] *
                   (double)v[(offset + 128 * p + 96 + j) & 1023];
        }
        pcm_out[j] = mp3_pcm_s16_from_double(sum);
    }
}

//===----------------------------------------------------------------------===//
// Main decode function
//===----------------------------------------------------------------------===//

/// @brief Determine the byte offset immediately after an optional ID3v2 tag.
/// @param data Complete MP3 byte buffer.
/// @param len Buffer length in bytes.
/// @return Tag-end offset, zero when no ID3v2 prefix exists, or @p len when a
///         declared tag extends beyond the available buffer.
static size_t mp3_skip_id3v2(const uint8_t *data, size_t len) {
    if (!data || len < 3 || data[0] != 'I' || data[1] != 'D' || data[2] != '3')
        return 0;
    if (len < 10)
        return len;

    uint8_t version = data[3];
    uint8_t flags = data[5];
    uint8_t allowed_flags = version == 2 ? 0xC0u : version == 3 ? 0xE0u : 0xF0u;
    if (version < 2 || version > 4 || data[4] == 0xFF || (flags & (uint8_t)~allowed_flags) != 0)
        return len;
    for (int i = 6; i < 10; i++) {
        if ((data[i] & 0x80u) != 0)
            return len;
    }

    {
        // Syncsafe integer size (4 bytes, 7 bits each)
        size_t tag_size = ((size_t)(data[6] & 0x7F) << 21) | ((size_t)(data[7] & 0x7F) << 14) |
                          ((size_t)(data[8] & 0x7F) << 7) | ((size_t)(data[9] & 0x7F));
        size_t footer_size = (version == 4 && (flags & 0x10u) != 0) ? 10u : 0u;
        size_t total = 10u + tag_size + footer_size;
        return (total <= len) ? total : len;
    }
}

/// @brief Find the next parseable Layer III frame header.
/// @param data Complete MP3 byte buffer.
/// @param len Buffer length in bytes.
/// @param start First byte offset to consider.
/// @return Offset of the next valid header, or @p len when none is found.
static size_t mp3_find_sync(const uint8_t *data, size_t len, size_t start) {
    for (size_t i = start; i + 3 < len; i++) {
        if (data[i] == 0xFF && (data[i + 1] & 0xE0) == 0xE0) {
            mp3_frame_header_t hdr;
            if (mp3_parse_header(data + i, &hdr) == 0 && hdr.frame_size > 0)
                return i;
        }
    }
    return len; // not found
}

/// @brief Add a frame's sample count without overflowing the public integer total.
/// @param total In/out accumulated per-channel sample count.
/// @param frame_samples Positive sample count for one frame.
/// @return Zero on success, or -1 when arguments are invalid or the sum overflows.
static int mp3_add_sample_count(int *total, int frame_samples) {
    if (!total || *total < 0 || frame_samples <= 0 || *total > INT32_MAX - frame_samples)
        return -1;
    *total += frame_samples;
    return 0;
}

/// @brief Pre-walk an MP3 stream to extract channel/rate and total-sample counts.
/// @details Skips an optional ID3v2 prefix and an optional ID3v1 suffix
///          (trailing `"TAG"` 128-byte block), locates the first valid
///          frame sync, and then walks every subsequent header to sum
///          decoded sample counts (1152 per frame for MPEG-1, 576 for
///          MPEG-2/2.5). Lets the streaming API report duration without
///          performing the full IMDCT/synthesis decode.
/// @param data              Byte buffer holding the entire MP3 file.
/// @param len               Length of @p data.
/// @param out_first_pos     Receives the offset of the first valid frame.
/// @param out_effective_len Receives the byte length excluding trailing ID3v1.
/// @param out_channels      Receives channel count from the first frame.
/// @param out_sample_rate   Receives sample rate from the first frame.
/// @param out_total_samples Receives total decoded samples per channel.
/// @return 0 on success, -1 if no valid frames or any required output arg is NULL.
static int mp3_scan_stream_metadata(const uint8_t *data,
                                    size_t len,
                                    size_t *out_first_pos,
                                    size_t *out_effective_len,
                                    int *out_channels,
                                    int *out_sample_rate,
                                    int *out_total_samples) {
    if (out_first_pos)
        *out_first_pos = 0;
    if (out_effective_len)
        *out_effective_len = 0;
    if (out_channels)
        *out_channels = 0;
    if (out_sample_rate)
        *out_sample_rate = 0;
    if (out_total_samples)
        *out_total_samples = 0;
    if (!data || len < 4 || !out_first_pos || !out_effective_len || !out_channels ||
        !out_sample_rate || !out_total_samples) {
        return -1;
    }

    size_t pos = mp3_skip_id3v2(data, len);
    size_t effective_len = len;
    if (len >= 128 && data[len - 128] == 'T' && data[len - 127] == 'A' && data[len - 126] == 'G')
        effective_len = len - 128;

    pos = mp3_find_sync(data, effective_len, pos);
    if (pos >= effective_len)
        return -1;

    mp3_frame_header_t first_hdr;
    if (mp3_parse_header(data + pos, &first_hdr) != 0)
        return -1;

    int total_samples = 0;
    size_t scan_pos = pos;
    while (scan_pos + 4 <= effective_len) {
        mp3_frame_header_t hdr;
        if (mp3_parse_header(data + scan_pos, &hdr) != 0) {
            scan_pos = mp3_find_sync(data, effective_len, scan_pos + 1);
            continue;
        }
        if ((size_t)hdr.frame_size > effective_len - scan_pos)
            break;
        if (hdr.channels != first_hdr.channels || hdr.sample_rate != first_hdr.sample_rate)
            return -1;
        if (mp3_add_sample_count(&total_samples, (hdr.mpeg_version == 3) ? 1152 : 576) != 0)
            return -1;
        scan_pos += (size_t)hdr.frame_size;
    }

    if (total_samples <= 0)
        return -1;

    *out_first_pos = pos;
    *out_effective_len = effective_len;
    *out_channels = first_hdr.channels;
    *out_sample_rate = first_hdr.sample_rate;
    *out_total_samples = total_samples;
    return 0;
}

/// @brief Append the current frame's main-data bytes to the bounded reservoir.
/// @param dec Decoder owning the reservoir.
/// @param data Main-data bytes from the current frame.
/// @param size Number of bytes to append.
static void mp3_reservoir_append(mp3_decoder_t *dec, const uint8_t *data, int size) {
    if (!dec || !data || size <= 0)
        return;
    int to_save = size;
    if (to_save > (int)sizeof(dec->reservoir)) {
        data += to_save - (int)sizeof(dec->reservoir);
        to_save = (int)sizeof(dec->reservoir);
    }
    if (dec->reservoir_size + to_save > (int)sizeof(dec->reservoir)) {
        int shift = dec->reservoir_size + to_save - (int)sizeof(dec->reservoir);
        memmove(dec->reservoir, dec->reservoir + shift, (size_t)(dec->reservoir_size - shift));
        dec->reservoir_size -= shift;
    }
    memcpy(dec->reservoir + dec->reservoir_size, data, (size_t)to_save);
    dec->reservoir_size += to_save;
}

/// @brief Select the scalefactor-band table row for a frame's version and rate.
/// @details MPEG-1 and MPEG-2 rows follow the ISO tables. MPEG-2.5 is a
///          de-facto extension: 8000 Hz has its own bands, while 11025 and
///          12000 Hz use the 22050 Hz long bands with the 16000 Hz short bands
///          (the convention LAME's encoder and decoder share).
/// @param mpeg_version Header version code (3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5).
/// @param sample_rate Frame sample rate in hertz.
/// @return Row index into mp3_sfb_long_cumul / mp3_sfb_short_cumul, or -1.
static int mp3_sfb_row_for(int mpeg_version, int sample_rate) {
    switch (sample_rate) {
        case 44100:
            return 0;
        case 48000:
            return 1;
        case 32000:
            return 2;
        case 22050:
            return 3;
        case 24000:
            return 4;
        case 16000:
            return 5;
        case 8000:
            return 6;
        case 11025:
        case 12000:
            return mpeg_version == 0 ? 7 : -1;
        default:
            return -1;
    }
}

/// @brief Read one granule's MPEG-1 scalefactors (ISO 11172-3 2.4.2.7).
/// @details Long blocks honour scfsi: a band group flagged for the second
///          granule reuses the first granule's scalefactors instead of reading
///          new ones; granule 0's long scalefactors are kept in @p prev_l.
static void mp3_read_scalefactors_mpeg1(mp3_bits_t *bits,
                                        const mp3_granule_info_t *gi,
                                        int gr,
                                        const int scfsi[4],
                                        int prev_l[22],
                                        int scalefac_l[22],
                                        int scalefac_s[13][3]) {
    memset(scalefac_l, 0, 22 * sizeof(int));
    memset(scalefac_s, 0, 13 * 3 * sizeof(int));
    int sfc = gi->scalefac_compress & 0x0F;
    int slen1 = mp3_slen_table[0][sfc];
    int slen2 = mp3_slen_table[1][sfc];
    if (gi->window_switching && gi->block_type == 2) {
        int first_short = 0;
        if (gi->mixed_block) {
            for (int sfb = 0; sfb < 8; sfb++)
                scalefac_l[sfb] = slen1 > 0 ? (int)mp3_bits_read(bits, slen1) : 0;
            first_short = 3;
        }
        for (int sfb = first_short; sfb < 12; sfb++) {
            int slen = sfb < 6 ? slen1 : slen2;
            for (int win = 0; win < 3; win++)
                scalefac_s[sfb][win] = slen > 0 ? (int)mp3_bits_read(bits, slen) : 0;
        }
        return;
    }
    for (int sfb = 0; sfb < 21; sfb++) {
        int group = sfb < 6 ? 0 : (sfb < 11 ? 1 : (sfb < 16 ? 2 : 3));
        int slen = sfb < 11 ? slen1 : slen2;
        if (gr == 1 && scfsi[group])
            scalefac_l[sfb] = prev_l[sfb];
        else
            scalefac_l[sfb] = slen > 0 ? (int)mp3_bits_read(bits, slen) : 0;
    }
    if (gr == 0)
        memcpy(prev_l, scalefac_l, 22 * sizeof(int));
}

/// @brief Read one LSF (MPEG-2 / 2.5) granule's scalefactors (ISO 13818-3 2.4.3.2).
/// @details scalefac_compress selects one of six partition layouts (three for
///          an intensity-stereo right channel); each partition's count from
///          mp3_lsf_nr_of_sfb is read sequentially in long-then-short order
///          (a mixed block's leading long bands first). Also derives preflag
///          and the per-band illegal intensity position (is_max) the stereo
///          stage needs.
static void mp3_read_scalefactors_lsf(mp3_bits_t *bits,
                                      mp3_granule_info_t *gi,
                                      int is_right,
                                      int mixed_long_bands,
                                      int scalefac_l[22],
                                      int scalefac_s[13][3],
                                      int is_max_l[22],
                                      int is_max_s[13][3]) {
    memset(scalefac_l, 0, 22 * sizeof(int));
    memset(scalefac_s, 0, 13 * 3 * sizeof(int));
    memset(is_max_l, 0, 22 * sizeof(int));
    memset(is_max_s, 0, 13 * 3 * sizeof(int));
    int sfc = gi->scalefac_compress;
    int slen[4] = {0, 0, 0, 0};
    int blocknumber = 0;
    gi->preflag = 0;
    if (is_right) {
        sfc >>= 1;
        if (sfc < 180) {
            slen[0] = sfc / 36;
            slen[1] = (sfc % 36) / 6;
            slen[2] = (sfc % 36) % 6;
            blocknumber = 3;
        } else if (sfc < 244) {
            sfc -= 180;
            slen[0] = (sfc % 64) >> 4;
            slen[1] = (sfc % 16) >> 2;
            slen[2] = sfc % 4;
            blocknumber = 4;
        } else {
            sfc -= 244;
            slen[0] = sfc / 3;
            slen[1] = sfc % 3;
            blocknumber = 5;
        }
    } else {
        if (sfc < 400) {
            slen[0] = (sfc >> 4) / 5;
            slen[1] = (sfc >> 4) % 5;
            slen[2] = (sfc % 16) >> 2;
            slen[3] = sfc % 4;
            blocknumber = 0;
        } else if (sfc < 500) {
            sfc -= 400;
            slen[0] = (sfc >> 2) / 5;
            slen[1] = (sfc >> 2) % 5;
            slen[2] = sfc % 4;
            blocknumber = 1;
        } else {
            sfc -= 500;
            slen[0] = sfc / 3;
            slen[1] = sfc % 3;
            blocknumber = 2;
            gi->preflag = 1;
        }
    }
    int bt = 0;
    if (gi->window_switching && gi->block_type == 2)
        bt = gi->mixed_block ? 2 : 1;
    int sfb_l = 0;
    int sfb_s = (bt == 2) ? 3 : 0;
    int win = 0;
    for (int part = 0; part < 4; part++) {
        int count = mp3_lsf_nr_of_sfb[blocknumber][bt][part];
        int max = (1 << slen[part]) - 1;
        for (int i = 0; i < count; i++) {
            int value = slen[part] > 0 ? (int)mp3_bits_read(bits, slen[part]) : 0;
            int use_long = (bt == 0) || (bt == 2 && sfb_l < mixed_long_bands);
            if (use_long) {
                if (sfb_l < 22) {
                    scalefac_l[sfb_l] = value;
                    is_max_l[sfb_l] = max;
                }
                sfb_l++;
            } else {
                if (sfb_s < 13) {
                    scalefac_s[sfb_s][win] = value;
                    is_max_s[sfb_s][win] = max;
                }
                if (++win == 3) {
                    win = 0;
                    sfb_s++;
                }
            }
        }
    }
}

/// @brief Reordered storage index of short-window frequency line @p f.
/// @details Short-block lines are stored subband-major and window-major —
///          (f / 6) * 18 + window * 6 + f % 6 — the layout the short IMDCT
///          reads six lines per window from.
/// @param f Frequency line within the window's 192-line spectrum.
/// @param window Window index 0..2.
/// @return Index into the 576-line granule, or -1 when out of range.
static int mp3_short_line_index(int f, int window) {
    if (f < 0 || f >= 192 || window < 0 || window >= 3)
        return -1;
    return (f / 6) * 18 + window * 6 + (f % 6);
}

/// @brief Sign-preserving |v|^(4/3) requantization of one Huffman value.
static double mp3_requant_value(int v) {
    double mag = pow((double)abs(v), 4.0 / 3.0);
    return v < 0 ? -mag : mag;
}

/// @brief Requantize one granule's Huffman values into spectral lines.
/// @details ISO 11172-3 2.4.3.4.7: xr = sign |is|^(4/3) 2^((global_gain - 210
///          - 8 subblock_gain) / 4) 2^(-(scalefac_multiplier (scalefac +
///          preflag pretab))). Long-block values stay in bitstream order;
///          short-window values are read in bitstream (band, window, line)
///          order and stored subband-major / window-major — the reorder the
///          short IMDCT consumes — so no separate reorder pass exists. The
///          last long band (21) and short band (12) carry no scalefactor.
/// @param xr Receives 576 spectral lines (zero-filled first).
static void mp3_requantize(const mp3_granule_info_t *gi,
                           const int *is_values,
                           const int scalefac_l[22],
                           const int scalefac_s[13][3],
                           const int *sfb_long,
                           const int *sfb_short,
                           int mixed_long_bands,
                           float *xr) {
    memset(xr, 0, MP3_SBLIMIT * sizeof(float));
    double global_gain_pow = pow(2.0, (double)(gi->global_gain - 210) / 4.0);
    int sfac_scale = gi->scalefac_scale ? 2 : 1;
    int short_blocks = gi->window_switching && gi->block_type == 2;

    if (!short_blocks || gi->mixed_block) {
        int limit = short_blocks ? 36 : MP3_SBLIMIT;
        int bands = short_blocks ? mixed_long_bands : 22;
        for (int sfb = 0; sfb < bands && sfb < 22; sfb++) {
            int start = sfb_long[sfb];
            int end = sfb_long[sfb + 1];
            if (end > limit)
                end = limit;
            int sf = sfb < 21 ? scalefac_l[sfb] : 0;
            if (gi->preflag)
                sf += mp3_pretab[sfb];
            double sfac_pow = pow(2.0, -0.5 * (double)(sf * sfac_scale));
            for (int i = start; i < end; i++)
                xr[i] = (float)(mp3_requant_value(is_values[i]) * global_gain_pow * sfac_pow);
        }
    }
    if (short_blocks) {
        int first = gi->mixed_block ? 3 : 0;
        for (int sfb = first; sfb < 13; sfb++) {
            int start = sfb_short[sfb];
            int width = sfb_short[sfb + 1] - start;
            for (int win = 0; win < 3; win++) {
                int sf = sfb < 12 ? scalefac_s[sfb][win] : 0;
                double sfac_pow = pow(2.0, -0.5 * (double)(sf * sfac_scale)) *
                                  pow(2.0, -2.0 * (double)gi->subblock_gain[win]);
                for (int i = 0; i < width; i++) {
                    int src = 3 * start + win * width + i;
                    int dst = mp3_short_line_index(start + i, win);
                    if (src >= MP3_SBLIMIT || dst < 0)
                        break;
                    xr[dst] =
                        (float)(mp3_requant_value(is_values[src]) * global_gain_pow * sfac_pow);
                }
            }
        }
    }
}

/// @brief Apply joint-stereo processing to one granule in the spectral domain.
/// @details Intensity stereo (ISO 11172-3 2.4.3.4.9.3 / ISO 13818-3 2.4.3.2)
///          reconstructs both channels from the left channel for every band
///          lying entirely above the right channel's last nonzero line, using
///          the right channel's scalefactor as is_pos (a band at the illegal
///          position keeps its coded values). Every remaining line is M/S
///          decoded when ms_stereo is set: L = (M + S) / sqrt 2, R = (M - S) / sqrt 2.
static void mp3_stereo_process(float xr[MP3_MAX_CHANNELS][MP3_SBLIMIT],
                               const mp3_granule_info_t *gi_right,
                               const int scalefac_l_r[22],
                               const int scalefac_s_r[13][3],
                               const int is_max_l[22],
                               const int is_max_s[13][3],
                               int ms_stereo,
                               int i_stereo,
                               int is_mpeg1,
                               int lsf_intensity_scale,
                               const int *sfb_long,
                               const int *sfb_short,
                               int mixed_long_bands) {
    int is_pos[MP3_SBLIMIT];
    for (int i = 0; i < MP3_SBLIMIT; i++)
        is_pos[i] = -1;

    if (i_stereo) {
        int short_blocks = gi_right->window_switching && gi_right->block_type == 2;
        if (!short_blocks || gi_right->mixed_block) {
            int limit = short_blocks ? 36 : MP3_SBLIMIT;
            int rzero = 0;
            for (int i = 0; i < limit; i++)
                if (xr[1][i] != 0.0f)
                    rzero = i + 1;
            int bands = short_blocks ? mixed_long_bands : 22;
            for (int sfb = 0; sfb < bands && sfb < 22; sfb++) {
                int start = sfb_long[sfb];
                int end = sfb_long[sfb + 1];
                if (end > limit)
                    end = limit;
                if (start < rzero)
                    continue;
                int src = sfb < 21 ? sfb : 20;
                int pos = scalefac_l_r[src];
                int illegal = is_mpeg1 ? 7 : is_max_l[src];
                if (pos == illegal)
                    continue;
                for (int i = start; i < end; i++)
                    is_pos[i] = pos;
            }
        }
        if (short_blocks) {
            int first = gi_right->mixed_block ? 3 : 0;
            for (int win = 0; win < 3; win++) {
                int rzero = 0;
                for (int f = 0; f < 192; f++) {
                    if (xr[1][mp3_short_line_index(f, win)] != 0.0f)
                        rzero = f + 1;
                }
                for (int sfb = first; sfb < 13; sfb++) {
                    int start = sfb_short[sfb];
                    int end = sfb_short[sfb + 1];
                    if (start < rzero)
                        continue;
                    int src = sfb < 12 ? sfb : 11;
                    int pos = scalefac_s_r[src][win];
                    int illegal = is_mpeg1 ? 7 : is_max_s[src][win];
                    if (pos == illegal)
                        continue;
                    for (int f = start; f < end; f++)
                        is_pos[mp3_short_line_index(f, win)] = pos;
                }
            }
        }
    }

    for (int i = 0; i < MP3_SBLIMIT; i++) {
        int pos = is_pos[i];
        if (pos >= 0) {
            double kl;
            double kr;
            if (is_mpeg1) {
                if (pos > 6)
                    pos = 6;
                kl = mp3_is_gain_mpeg1[pos][0];
                kr = mp3_is_gain_mpeg1[pos][1];
            } else {
                double io = lsf_intensity_scale ? 0.840896415 : 0.707106781;
                if (pos == 0) {
                    kl = 1.0;
                    kr = 1.0;
                } else if (pos & 1) {
                    kl = 1.0;
                    kr = pow(io, (double)((pos + 1) / 2));
                } else {
                    kl = pow(io, (double)(pos / 2));
                    kr = 1.0;
                }
            }
            float l = xr[0][i];
            xr[0][i] = (float)(l * kl);
            xr[1][i] = (float)(l * kr);
        } else if (ms_stereo) {
            float m = xr[0][i];
            float sd = xr[1][i];
            xr[0][i] = (m + sd) * 0.707106781f;
            xr[1][i] = (m - sd) * 0.707106781f;
        }
    }
}

/// @brief Return the time-domain destination for a short-window IMDCT sample.
static int mp3_short_imdct_offset(int window, int sample) {
    if (window < 0 || window >= 3 || sample < 0 || sample >= 12)
        return -1;
    return 6 + window * 6 + sample;
}

/// @brief Select short IMDCT for a subband, respecting mixed-block long bands.
static int mp3_uses_short_imdct(const mp3_granule_info_t *gi, int subband) {
    if (!gi || subband < 0 || subband >= MP3_SUBBANDS || gi->block_type != 2)
        return 0;
    return !gi->mixed_block || subband >= 2;
}

/// @brief Decode one MP3 frame at the cursor into @p pcm_out.
/// @details Locates the next sync starting at `*io_pos`, parses the
///          header + side info, splices the bit-reservoir front-data
///          (@c main_data_begin bytes from the previous frame) into the
///          frame's main data, decodes Huffman pairs / scalefactors per
///          granule and channel, runs the IMDCT (long, short, mixed) and
///          polyphase synthesis filter to emit interleaved 16-bit PCM,
///          and advances `*io_pos`. Returns -2 when the frame uses an
///          unsupported Huffman table so the caller can abort the
///          stream cleanly.
/// @param dec             Decoder state (carries overlap-add + reservoir).
/// @param data            Full MP3 byte buffer.
/// @param effective_len   Byte length to scan (excludes trailing tags).
/// @param io_pos          In/out cursor; updated past the consumed frame.
/// @param pcm_out         Buffer sized for `samples_per_frame * channels`.
/// @param out_frames      Receives decoded sample count per channel.
/// @param out_channels    Receives the frame's channel count.
/// @param out_sample_rate Receives the frame's sample rate.
/// @return 0 on success / EOF (with `*out_frames == 0`), -1 on parse
///         error (cursor advanced past the bad header), -2 on
///         unsupported Huffman table.
static int mp3_decode_frame_internal(mp3_decoder_t *dec,
                                     const uint8_t *data,
                                     size_t effective_len,
                                     size_t *io_pos,
                                     int16_t *pcm_out,
                                     int *out_frames,
                                     int *out_channels,
                                     int *out_sample_rate) {
    if (out_frames)
        *out_frames = 0;
    if (out_channels)
        *out_channels = 0;
    if (out_sample_rate)
        *out_sample_rate = 0;
    if (!dec || !data || !io_pos || !pcm_out || !out_frames || !out_channels || !out_sample_rate ||
        *io_pos > effective_len)
        return -1;

    size_t pos = mp3_find_sync(data, effective_len, *io_pos);
    if (pos >= effective_len) {
        *io_pos = effective_len;
        return 0;
    }

    mp3_frame_header_t hdr;
    if (mp3_parse_header(data + pos, &hdr) != 0) {
        *io_pos = pos + 1;
        return -1;
    }
    if ((size_t)hdr.frame_size > effective_len - pos) {
        *io_pos = effective_len;
        return 0;
    }

    const uint8_t *frame_data = data + pos + 4 + (size_t)hdr.crc_size;
    mp3_side_info_t si;
    memset(&si, 0, sizeof(si));
    int is_mpeg1 = (hdr.mpeg_version == 3) ? 1 : 0;
    if (mp3_parse_side_info(frame_data, hdr.side_info_size, hdr.channels, is_mpeg1, &si) != 0) {
        *io_pos = pos + (size_t)hdr.frame_size;
        return -1;
    }

    const uint8_t *main_data = frame_data + hdr.side_info_size;
    int main_data_len = hdr.main_data_size;
    uint8_t combined[4096];
    if (si.main_data_begin > dec->reservoir_size) {
        mp3_reservoir_append(dec, main_data, main_data_len);
        *io_pos = pos + (size_t)hdr.frame_size;
        return -1;
    }
    if (si.main_data_begin > 0) {
        int res_start = dec->reservoir_size - si.main_data_begin;
        int total = si.main_data_begin + main_data_len;
        if (total > (int)sizeof(combined)) {
            *io_pos = pos + (size_t)hdr.frame_size;
            return -1;
        }
        int from_res = si.main_data_begin;
        memcpy(combined, dec->reservoir + res_start, (size_t)from_res);
        int from_frame = total - from_res;
        if (from_frame > main_data_len)
            from_frame = main_data_len;
        if (from_frame > 0)
            memcpy(combined + from_res, main_data, (size_t)from_frame);
        main_data = combined;
        main_data_len = total;
    }

    mp3_reservoir_append(dec, frame_data + hdr.side_info_size, hdr.main_data_size);

    float saved_overlap[MP3_MAX_CHANNELS][MP3_SUBBANDS][18];
    float saved_synth_buf[MP3_MAX_CHANNELS][1024];
    int saved_synth_offset[MP3_MAX_CHANNELS];
    memcpy(saved_overlap, dec->overlap, sizeof(saved_overlap));
    memcpy(saved_synth_buf, dec->synth_buf, sizeof(saved_synth_buf));
    memcpy(saved_synth_offset, dec->synth_offset, sizeof(saved_synth_offset));
    int frame_error_code = -1;

    int samples_per_frame = (hdr.mpeg_version == 3) ? 1152 : 576;
    memset(pcm_out, 0, (size_t)samples_per_frame * (size_t)hdr.channels * sizeof(int16_t));

    int ngranules = is_mpeg1 ? 2 : 1;
    mp3_bits_t bits;
    mp3_bits_init(&bits, main_data, (size_t)main_data_len);

    int sfb_row = mp3_sfb_row_for(hdr.mpeg_version, hdr.sample_rate);
    if (sfb_row < 0)
        goto frame_data_error;
    const int *sfb_long = mp3_sfb_long_cumul[sfb_row];
    const int *sfb_short = mp3_sfb_short_cumul[sfb_row];
    int mixed_long_bands = is_mpeg1 ? 8 : (hdr.sample_rate == 8000 ? 3 : 6);
    int joint = hdr.channels == 2 && hdr.channel_mode == 1;
    int ms_stereo = joint && (hdr.mode_ext & 0x02) != 0;
    int i_stereo = joint && (hdr.mode_ext & 0x01) != 0;
    int scalefac_prev[MP3_MAX_CHANNELS][22];
    memset(scalefac_prev, 0, sizeof(scalefac_prev));
    mp3_ensure_tables();

    for (int gr = 0; gr < ngranules; gr++) {
        float xr[MP3_MAX_CHANNELS][MP3_SBLIMIT];
        float samples[MP3_MAX_CHANNELS][MP3_SBLIMIT];
        int scalefac_l[MP3_MAX_CHANNELS][22];
        int scalefac_s[MP3_MAX_CHANNELS][13][3];
        int is_max_l[22];
        int is_max_s[13][3];
        memset(xr, 0, sizeof(xr));
        memset(samples, 0, sizeof(samples));
        memset(scalefac_l, 0, sizeof(scalefac_l));
        memset(scalefac_s, 0, sizeof(scalefac_s));
        memset(is_max_l, 0, sizeof(is_max_l));
        memset(is_max_s, 0, sizeof(is_max_s));

        // Pass 1: side info → scalefactors → Huffman values → spectral lines.
        for (int ch = 0; ch < hdr.channels; ch++) {
            mp3_granule_info_t *gi = &si.granules[gr][ch];
            for (int region = 0; region < 3; region++) {
                if (!mp3_huff_table_supported(gi->table_select[region])) {
                    *io_pos = effective_len;
                    frame_error_code = -2;
                    goto frame_data_error;
                }
            }

            size_t part_start = bits.pos;
            if ((size_t)gi->part2_3_length > bits.len - part_start)
                goto frame_data_error;
            size_t part_end = part_start + (size_t)gi->part2_3_length;
            size_t main_data_bit_len = bits.len;
            bits.len = part_end;

            if (is_mpeg1) {
                mp3_read_scalefactors_mpeg1(
                    &bits, gi, gr, si.scfsi[ch], scalefac_prev[ch], scalefac_l[ch], scalefac_s[ch]);
            } else {
                int lsf_max_l[22];
                int lsf_max_s[13][3];
                mp3_read_scalefactors_lsf(&bits,
                                          gi,
                                          i_stereo && ch == 1,
                                          mixed_long_bands,
                                          scalefac_l[ch],
                                          scalefac_s[ch],
                                          lsf_max_l,
                                          lsf_max_s);
                if (ch == 1) {
                    memcpy(is_max_l, lsf_max_l, sizeof(is_max_l));
                    memcpy(is_max_s, lsf_max_s, sizeof(is_max_s));
                }
            }
            if (bits.error)
                goto frame_data_error;

            int is_values[MP3_SBLIMIT];
            memset(is_values, 0, sizeof(is_values));
            int line = 0;

            int region1_start;
            int region2_start;
            if (gi->window_switching && gi->block_type == 2) {
                region1_start = gi->mixed_block ? 36 : 3 * sfb_short[3];
                region2_start = MP3_SBLIMIT;
            } else {
                int r0 = gi->region0_count + 1;
                int r1 = gi->region1_count + 1;
                region1_start = (r0 < 22) ? sfb_long[r0] : MP3_SBLIMIT;
                region2_start = (r0 + r1 < 22) ? sfb_long[r0 + r1] : MP3_SBLIMIT;
            }
            int big_end = gi->big_values * 2;
            if (big_end > MP3_SBLIMIT)
                big_end = MP3_SBLIMIT;
            if (region1_start > big_end)
                region1_start = big_end;
            if (region2_start > big_end)
                region2_start = big_end;

            for (int region = 0; region < 3 && line < big_end; region++) {
                int region_end;
                int table_idx;
                if (region == 0) {
                    region_end = region1_start;
                    table_idx = gi->table_select[0];
                } else if (region == 1) {
                    region_end = region2_start;
                    table_idx = gi->table_select[1];
                } else {
                    region_end = big_end;
                    table_idx = gi->table_select[2];
                }
                if (region_end > big_end)
                    region_end = big_end;

                int linbits = mp3_huff_info[table_idx].linbits;
                int max_val = mp3_huff_info[table_idx].max_val;

                while (line < region_end && bits.pos < part_end) {
                    if (max_val == 0) {
                        is_values[line++] = 0;
                        if (line < region_end)
                            is_values[line++] = 0;
                        continue;
                    }

                    int x;
                    int y;
                    if (mp3_huff_decode_pair(&bits, table_idx, &x, &y) != 0)
                        goto frame_data_error;

                    if (linbits > 0 && x >= 15)
                        x += (int)mp3_bits_read(&bits, linbits);
                    if (x != 0 && mp3_bits_read(&bits, 1))
                        x = -x;

                    if (linbits > 0 && y >= 15)
                        y += (int)mp3_bits_read(&bits, linbits);
                    if (y != 0 && mp3_bits_read(&bits, 1))
                        y = -y;

                    is_values[line++] = x;
                    if (line < MP3_SBLIMIT)
                        is_values[line++] = y;
                }
            }

            // part2_3_length bounds every entropy read: a big_values region
            // the granule's own length cannot hold is truncated data.
            if (bits.error || line < big_end)
                goto frame_data_error;

            // count1 region (ISO 11172-3 2.4.3.4.6): quadruples through quad
            // table A or B, each nonzero value followed by its sign bit. The
            // encoder may leave a quadruple that overruns part2_3_length; the
            // standard discards it, so the walk is allowed to probe past
            // part_end (within the main data) and rolls back on overrun.
            {
                int quad_size = 0;
                const mp3_huff_node_t *quad_tree =
                    mp3_get_quad_tree(gi->count1table_select, &quad_size);
                while (line + 3 < MP3_SBLIMIT && bits.pos < part_end) {
                    int qx = 0;
                    int qy = 0;
                    bits.len = main_data_bit_len;
                    int rc_quad = mp3_huff_tree_decode(&bits, quad_tree, quad_size, &qx, &qy);
                    int quad[4] = {(qy >> 3) & 1, (qy >> 2) & 1, (qy >> 1) & 1, qy & 1};
                    if (rc_quad == 0) {
                        for (int q = 0; q < 4; q++) {
                            if (quad[q] && mp3_bits_read(&bits, 1))
                                quad[q] = -quad[q];
                        }
                    }
                    bits.len = part_end;
                    if (rc_quad != 0 || bits.error)
                        goto frame_data_error;
                    if (bits.pos > part_end) {
                        bits.pos = part_end;
                        break;
                    }
                    for (int q = 0; q < 4; q++)
                        is_values[line++] = quad[q];
                }
            }

            bits.pos = part_end;
            bits.len = main_data_bit_len;

            mp3_requantize(gi,
                           is_values,
                           scalefac_l[ch],
                           scalefac_s[ch],
                           sfb_long,
                           sfb_short,
                           mixed_long_bands,
                           xr[ch]);
        }

        // Pass 2: joint stereo in the spectral domain.
        if (hdr.channels == 2 && (ms_stereo || i_stereo)) {
            mp3_stereo_process(xr,
                               &si.granules[gr][1],
                               scalefac_l[1],
                               scalefac_s[1],
                               is_max_l,
                               is_max_s,
                               ms_stereo,
                               i_stereo,
                               is_mpeg1,
                               si.granules[gr][1].scalefac_compress & 1,
                               sfb_long,
                               sfb_short,
                               mixed_long_bands);
        }

        // Pass 3: alias reduction → IMDCT → overlap-add → frequency inversion.
        for (int ch = 0; ch < hdr.channels; ch++) {
            const mp3_granule_info_t *gi = &si.granules[gr][ch];
            float *lines = xr[ch];

            if (gi->block_type != 2 || gi->mixed_block) {
                int alias_subbands = gi->mixed_block ? 2 : 32;
                for (int sb = 1; sb < alias_subbands; sb++) {
                    for (int i = 0; i < 8; i++) {
                        int a_idx = sb * 18 - 1 - i;
                        int b_idx = sb * 18 + i;
                        float a = lines[a_idx];
                        float b = lines[b_idx];
                        lines[a_idx] = a * mp3_cs[i] - b * mp3_ca[i];
                        lines[b_idx] = b * mp3_cs[i] + a * mp3_ca[i];
                    }
                }
            }

            for (int sb = 0; sb < 32; sb++) {
                float imdct_out[36];

                if (mp3_uses_short_imdct(gi, sb)) {
                    memset(imdct_out, 0, sizeof(imdct_out));
                    for (int win = 0; win < 3; win++) {
                        float short_in[6];
                        for (int i = 0; i < 6; i++)
                            short_in[i] = lines[sb * 18 + win * 6 + i];
                        float short_out[12];
                        mp3_imdct12(short_in, short_out);
                        for (int i = 0; i < 12; i++) {
                            int out_index = mp3_short_imdct_offset(win, i);
                            imdct_out[out_index] += short_out[i] * mp3_win_short[i];
                        }
                    }
                } else {
                    float long_in[18];
                    for (int i = 0; i < 18; i++)
                        long_in[i] = lines[sb * 18 + i];
                    mp3_imdct36(long_in, imdct_out);

                    const float *win;
                    if (gi->block_type == 1)
                        win = mp3_win_start;
                    else if (gi->block_type == 3)
                        win = mp3_win_stop;
                    else
                        win = mp3_win_normal;
                    for (int i = 0; i < 36; i++)
                        imdct_out[i] *= win[i];
                }

                for (int i = 0; i < 18; i++) {
                    float value = imdct_out[i] + dec->overlap[ch][sb][i];
                    // Frequency inversion: every odd time sample of every odd
                    // subband is negated to undo the polyphase inversion.
                    if ((sb & 1) && (i & 1))
                        value = -value;
                    samples[ch][sb * 18 + i] = value;
                    dec->overlap[ch][sb][i] = imdct_out[18 + i];
                }
            }
        }

        // Pass 4: polyphase synthesis into interleaved PCM.
        for (int ss = 0; ss < 18; ss++) {
            for (int ch = 0; ch < hdr.channels; ch++) {
                float subbands[32];
                for (int sb = 0; sb < 32; sb++)
                    subbands[sb] = samples[ch][sb * 18 + ss];

                int16_t pcm_samples[32];
                mp3_synth_filter(dec, ch, subbands, pcm_samples);

                for (int j = 0; j < 32; j++) {
                    size_t out_idx =
                        ((size_t)gr * 576u + (size_t)ss * 32 + (size_t)j) * (size_t)hdr.channels +
                        (size_t)ch;
                    pcm_out[out_idx] = pcm_samples[j];
                }
            }
        }
    }

    *io_pos = pos + (size_t)hdr.frame_size;
    *out_frames = samples_per_frame;
    *out_channels = hdr.channels;
    *out_sample_rate = hdr.sample_rate;
    return 1;

frame_data_error:
    memcpy(dec->overlap, saved_overlap, sizeof(saved_overlap));
    memcpy(dec->synth_buf, saved_synth_buf, sizeof(saved_synth_buf));
    memcpy(dec->synth_offset, saved_synth_offset, sizeof(saved_synth_offset));
    if (frame_error_code != -2)
        *io_pos = pos + (size_t)hdr.frame_size;
    return frame_error_code;
}

/// @brief Decode an entire MP3 file in one shot — header sniff → per-frame decode → PCM out.
///
/// Walks the container scanning for valid sync words, skipping any
/// ID3 prefix or junk bytes. Each frame is decoded into a
/// per-frame PCM block and appended to the output buffer.
/// @param dec Reusable decoder instance; its state is reset before decoding.
/// @param data Complete borrowed MP3 byte buffer.
/// @param len Buffer length in bytes.
/// @param out_pcm Receives a `malloc`-allocated interleaved PCM buffer.
/// @param out_samples Receives decoded frame count per channel.
/// @param out_channels Receives the constant stream channel count.
/// @param out_sample_rate Receives the constant stream sample rate in hertz.
/// @return `0` on success, or `-1` on validation, unsupported data, inconsistent
///         stream format, size-limit, allocation, or decode failure.
/// @note The caller must free successful @p out_pcm storage.
int mp3_decode_file(mp3_decoder_t *dec,
                    const uint8_t *data,
                    size_t len,
                    int16_t **out_pcm,
                    int *out_samples,
                    int *out_channels,
                    int *out_sample_rate) {
    if (out_pcm)
        *out_pcm = NULL;
    if (out_samples)
        *out_samples = 0;
    if (out_channels)
        *out_channels = 0;
    if (out_sample_rate)
        *out_sample_rate = 0;
    if (!dec || !data || len < 4 || !out_pcm || !out_samples || !out_channels || !out_sample_rate) {
        return -1;
    }

    size_t first_frame_pos = 0;
    size_t effective_len = 0;
    int channels = 0;
    int sample_rate = 0;
    int total_samples = 0;
    if (mp3_scan_stream_metadata(
            data, len, &first_frame_pos, &effective_len, &channels, &sample_rate, &total_samples) !=
        0) {
        return -1;
    }

    if (total_samples <= 0 || channels <= 0 || (size_t)total_samples > SIZE_MAX / (size_t)channels)
        return -1;
    size_t pcm_values = (size_t)total_samples * (size_t)channels;
    if (pcm_values > SIZE_MAX / sizeof(int16_t) ||
        pcm_values * sizeof(int16_t) > MP3_MAX_DECODED_PCM_BYTES)
        return -1;
    int16_t *pcm = (int16_t *)malloc(pcm_values * sizeof(int16_t));
    if (!pcm)
        return -1;

    mp3_decoder_reset(dec);

    size_t pos = first_frame_pos;
    int pcm_len = 0;
    while (pos < effective_len && pcm_len < total_samples) {
        int frame_samples = 0;
        int frame_channels = 0;
        int frame_sample_rate = 0;
        int rc = mp3_decode_frame_internal(dec,
                                           data,
                                           effective_len,
                                           &pos,
                                           pcm + (size_t)pcm_len * (size_t)channels,
                                           &frame_samples,
                                           &frame_channels,
                                           &frame_sample_rate);
        if (rc == -2) {
            free(pcm);
            return -1;
        }
        if (rc < 0)
            continue;
        if (rc == 0)
            break;
        if (frame_channels != channels || frame_sample_rate != sample_rate ||
            pcm_len + frame_samples > total_samples) {
            free(pcm);
            return -1;
        }
        pcm_len += frame_samples;
    }

    if (pcm_len <= 0) {
        free(pcm);
        return -1;
    }

    if (pcm_len != total_samples) {
        size_t trimmed_values = (size_t)pcm_len * (size_t)channels;
        int16_t *trimmed = (int16_t *)realloc(pcm, trimmed_values * sizeof(int16_t));
        if (trimmed)
            pcm = trimmed;
        total_samples = pcm_len;
    }

    *out_pcm = pcm;
    *out_samples = total_samples;
    *out_channels = channels;
    *out_sample_rate = sample_rate;
    return 0;
}

//===----------------------------------------------------------------------===//
// Streaming API (per-frame decode)
//===----------------------------------------------------------------------===//

struct mp3_stream {
    uint8_t *data;
    size_t data_len;
    size_t effective_len;
    size_t first_frame_pos;
    size_t pos;
    mp3_decoder_t *dec;
    int16_t frame_pcm[1152 * MP3_MAX_CHANNELS];
    int total_samples;
    int sample_rate;
    int channels;
};

/// @brief Open an MP3 file for frame-sliced playback.
/// @details Reads the bounded file into memory and pre-scans frame headers for
///          the first-frame offset, stable channel/rate metadata, and total
///          per-channel sample count. Audio frames remain encoded until
///          @ref mp3_stream_decode_frame advances through them.
/// @param filepath NUL-terminated MP3 file path; files above 256 MiB are rejected.
/// @return Caller-owned stream, or NULL on file, metadata, or allocation failure.
mp3_stream_t *mp3_stream_open(const char *filepath) {
    if (!filepath)
        return NULL;

    FILE *f = rt_file_stdio_open_utf8(filepath, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long flen = ftell(f);
    if (flen <= 0 || flen > 256 * 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    uint8_t *data = (uint8_t *)malloc((size_t)flen);
    if (!data) {
        fclose(f);
        return NULL;
    }
    if (fread(data, 1, (size_t)flen, f) != (size_t)flen) {
        free(data);
        fclose(f);
        return NULL;
    }
    fclose(f);

    mp3_stream_t *s = (mp3_stream_t *)calloc(1, sizeof(mp3_stream_t));
    if (!s) {
        free(data);
        return NULL;
    }

    s->dec = mp3_decoder_new();
    if (!s->dec) {
        free(data);
        free(s);
        return NULL;
    }

    if (mp3_scan_stream_metadata(data,
                                 (size_t)flen,
                                 &s->first_frame_pos,
                                 &s->effective_len,
                                 &s->channels,
                                 &s->sample_rate,
                                 &s->total_samples) != 0 ||
        s->channels < 1 || s->channels > 2 || s->sample_rate <= 0 || s->total_samples <= 0) {
        mp3_decoder_free(s->dec);
        free(data);
        free(s);
        return NULL;
    }

    s->data = data;
    s->data_len = (size_t)flen;
    s->pos = s->first_frame_pos;
    mp3_decoder_reset(s->dec);

    return s;
}

/// @brief Decode the next valid MP3 frame into stream-owned PCM storage.
/// @details Malformed headers are skipped while scanning forward. The returned
///          pointer is overwritten by the next decode or rewind and becomes
///          invalid when the stream is freed.
/// @param stream Open MP3 stream.
/// @param out_pcm Receives a borrowed interleaved PCM frame on success, or NULL
///        at end-of-stream/error.
/// @return Frame count per channel, `0` at end-of-stream, or `-1` for invalid
///         arguments, unsupported data, or a mid-stream format change.
int mp3_stream_decode_frame(mp3_stream_t *stream, int16_t **out_pcm) {
    if (out_pcm)
        *out_pcm = NULL;
    if (!stream || !out_pcm)
        return -1;
    while (stream->pos < stream->effective_len) {
        int frame_samples = 0;
        int frame_channels = 0;
        int frame_sample_rate = 0;
        int rc = mp3_decode_frame_internal(stream->dec,
                                           stream->data,
                                           stream->effective_len,
                                           &stream->pos,
                                           stream->frame_pcm,
                                           &frame_samples,
                                           &frame_channels,
                                           &frame_sample_rate);
        if (rc == -2)
            return -1;
        if (rc < 0)
            continue;
        if (rc == 0)
            return 0;
        if (frame_channels != stream->channels || frame_sample_rate != stream->sample_rate)
            return -1;
        *out_pcm = stream->frame_pcm;
        return frame_samples;
    }

    return 0;
}

/// @brief Get the sample rate of the MP3 stream (determined from the first frame header).
/// @param stream Open stream, or NULL.
/// @return Sample rate in hertz, or zero for NULL.
int mp3_stream_sample_rate(const mp3_stream_t *stream) {
    return stream ? stream->sample_rate : 0;
}

/// @brief Get the channel count of the MP3 stream (1=mono, 2=stereo).
/// @param stream Open stream, or NULL.
/// @return Channel count, or zero for NULL.
int mp3_stream_channels(const mp3_stream_t *stream) {
    return stream ? stream->channels : 0;
}

/// @brief Get the total decoded frame count of the MP3 stream.
/// @param stream Open stream, or NULL.
/// @return Pre-scanned sample-frame count per channel, or zero for NULL.
int mp3_stream_total_samples(const mp3_stream_t *stream) {
    return stream ? stream->total_samples : 0;
}

/// @brief Rewind the MP3 stream to the beginning for re-playback.
/// @details Restores the first-frame cursor and clears reservoir, overlap-add,
///          and synthesis-filter history.
/// @param stream Open stream; NULL is ignored.
void mp3_stream_rewind(mp3_stream_t *stream) {
    if (!stream)
        return;
    stream->pos = stream->first_frame_pos;
    mp3_decoder_reset(stream->dec);
}

/// @brief Close an MP3 stream and release all memory it owns.
/// @details Frees the decoder state, in-memory encoded file bytes, and stream
///          object. No file handle remains open after @ref mp3_stream_open.
/// @param stream Stream returned by @ref mp3_stream_open; NULL is accepted.
void mp3_stream_free(mp3_stream_t *stream) {
    if (!stream)
        return;
    mp3_decoder_free(stream->dec);
    free(stream->data);
    free(stream);
}
