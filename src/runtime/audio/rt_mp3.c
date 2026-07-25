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
//   - Decodes baseline MP3: MPEG-1 Layer III, mono and joint/stereo
//   - Pipeline: frame sync → side info → Huffman → requantize → stereo →
//     anti-alias → IMDCT → window → overlap-add → polyphase synthesis → PCM
//   - All tables from ISO 11172-3 / ISO 13818-3
//   - Output: interleaved 16-bit signed PCM
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
    b->len = byte_len * 8;
    b->pos = 0;
}

/// @brief Pull up to 32 MSB-first bits from the current reader position.
/// @details If the buffer ends mid-field, returns the accumulated prefix and
///          leaves unread low-order result bits as zero.
/// @param b Initialized bit reader.
/// @param count Number of bits to consume; non-positive values consume none.
/// @return Decoded unsigned field, or zero when no bits are requested/available.
static uint32_t mp3_bits_read(mp3_bits_t *b, int count) {
    if (count <= 0)
        return 0;
    uint32_t val = 0;
    for (int i = 0; i < count; i++) {
        if (b->pos >= b->len)
            return val;
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

    out->main_data_size = out->frame_size - 4 - out->side_info_size;
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
            gi->global_gain = (int)mp3_bits_read(&bits, 8);
            gi->scalefac_compress = (int)mp3_bits_read(&bits, mpeg1 ? 4 : 9);
            gi->window_switching = (int)mp3_bits_read(&bits, 1);
            if (gi->window_switching) {
                gi->block_type = (int)mp3_bits_read(&bits, 2);
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

    return 0;
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
mp3_decoder_t *mp3_decoder_new(void) {
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
        node = -val + bit;
        depth++;
    }
    return -1;
}

/// @brief Get the Huffman tree for a given table index (small tables only).
/// @param table_idx Layer III Huffman table index.
/// @param out_size Receives the flat tree's entry count, or zero when absent.
/// @return Static tree pointer, or NULL when no explicit tree is stored.
static const mp3_huff_node_t *mp3_get_huff_tree(int table_idx, int *out_size) {
    switch (table_idx) {
        case 1:
            *out_size = 7;
            return mp3_htree_1;
        case 2:
        case 3:
            *out_size = 16;
            return mp3_htree_2;
        case 5:
        case 6:
            *out_size = 28;
            return mp3_htree_5;
        default:
            *out_size = 0;
            return NULL;
    }
}

/// @brief Probe whether table @p table_idx has a baked tree or a known fallback.
/// @details This decoder only ships explicit Huffman trees for a small
///          subset of the 32 ISO tables (1, 2/3, 5/6); table 0 is the
///          empty/"zero" table. Returns 1 for those indices and 0 for
///          unsupported ones so the caller can fall back to the
///          bit-width approximation in @ref mp3_huff_decode_pair without
///          attempting an out-of-range tree walk.
/// @param table_idx Huffman table index from the side-info `table_select`.
/// @return 1 if supported, 0 otherwise.
static int mp3_huff_table_supported(int table_idx) {
    switch (table_idx) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 5:
        case 6:
            return 1;
        default:
            return 0;
    }
}

/// @brief Decode one Huffman pair using tree walk (small tables) or bit-width
/// approximation (large tables where full ISO trees aren't stored).
/// @param bits Main-data bit reader.
/// @param table_idx Layer III Huffman table index.
/// @param x Receives the first non-negative spectral value.
/// @param y Receives the second non-negative spectral value.
static void mp3_huff_decode_pair(mp3_bits_t *bits, int table_idx, int *x, int *y) {
    if (table_idx <= 0 || table_idx >= 32 || mp3_huff_info[table_idx].max_val == 0) {
        *x = *y = 0;
        return;
    }

    int tree_size;
    const mp3_huff_node_t *tree = mp3_get_huff_tree(table_idx, &tree_size);
    if (tree) {
        if (mp3_huff_tree_decode(bits, tree, tree_size, x, y) != 0)
            *x = *y = 0;
        return;
    }

    // Fallback for larger tables: read variable-length values.
    // Use a simple Elias-gamma-like approach: read bits until a valid
    // pair is formed within [0, max_val].
    int max_val = mp3_huff_info[table_idx].max_val;
    int nbits = 0;
    {
        int tmp = max_val;
        while (tmp > 0) {
            nbits++;
            tmp >>= 1;
        }
    }

    *x = (int)mp3_bits_read(bits, nbits);
    *y = (int)mp3_bits_read(bits, nbits);
    if (*x > max_val)
        *x = max_val;
    if (*y > max_val)
        *y = max_val;
}

//===----------------------------------------------------------------------===//
// IMDCT
//===----------------------------------------------------------------------===//

/// @brief 36-point IMDCT used for "long" blocks (most steady-state samples).
///
/// Converts 18 frequency-domain samples back to 36 time-domain
/// samples, with the standard MDCT windowing folded in. Adjacent
/// long blocks overlap-add by 18 samples to give continuous output.
/// @param in Eighteen frequency-domain coefficients.
/// @param out Receives 36 time-domain samples.
static void mp3_imdct36(const float *in, float *out) {
    // 36-point IMDCT: X[i] = sum_{k=0}^{17} in[k] * cos(pi/36 * (2*i+1+18) * (2*k+1) / 2)
    for (int i = 0; i < 36; i++) {
        double sum = 0.0;
        for (int k = 0; k < 18; k++)
            sum += (double)in[k] *
                   cos(M_PI / 36.0 * (double)(2 * i + 1 + 18) * (double)(2 * k + 1) / 2.0);
        out[i] = (float)sum;
    }
}

/// @brief 12-point IMDCT used for "short" blocks (transients — three sub-blocks per granule).
///
/// Short blocks improve the time-resolution of the codec for
/// percussive content; the encoder switches to them via the
/// `block_type` flag in side-info.
/// @param in Six frequency-domain coefficients.
/// @param out Receives 12 time-domain samples.
static void mp3_imdct12(const float *in, float *out) {
    // 12-point IMDCT for short blocks
    for (int i = 0; i < 12; i++) {
        double sum = 0.0;
        for (int k = 0; k < 6; k++)
            sum += (double)in[k] *
                   cos(M_PI / 12.0 * (double)(2 * i + 1 + 6) * (double)(2 * k + 1) / 2.0);
        out[i] = (float)sum;
    }
}

//===----------------------------------------------------------------------===//
// Polyphase synthesis filterbank
//===----------------------------------------------------------------------===//

/// @brief Polyphase synthesis filter — converts subband samples back to PCM.
///
/// MP3's final stage: each frame yields 32 subband samples per
/// channel per "subband sample slot" (32 slots per frame).  This
/// filter combines the 32 subband values via the standard
/// pre-baked window matrix and outputs 32 PCM samples per call.
/// Per-channel state (`v[]` ring buffer) carries across calls for
/// the windowed overlap-add.
/// @param dec Decoder carrying per-channel synthesis history.
/// @param ch Zero-based channel index.
/// @param subbands Thirty-two reconstructed subband samples.
/// @param pcm_out Receives 32 clamped signed 16-bit PCM samples.
static void mp3_synth_filter(mp3_decoder_t *dec,
                             int ch,
                             const float subbands[32],
                             int16_t *pcm_out) {
    float *buf = dec->synth_buf[ch];
    int offset = dec->synth_offset[ch];

    // Shift buffer and matrixing (32-point DCT-IV via direct computation)
    offset = (offset - 64 + 1024) % 1024;
    dec->synth_offset[ch] = offset;

    // Matrixing: Ni[k] = sum_{j=0}^{31} S[j] * cos(pi/64 * (2*k+1) * (2*j+1+32) / 2)
    for (int k = 0; k < 64; k++) {
        double sum = 0.0;
        for (int j = 0; j < 32; j++) {
            sum += (double)subbands[j] *
                   cos(M_PI / 64.0 * (double)(2 * k + 1) * (double)(2 * j + 1 + 32) / 2.0);
        }
        buf[(offset + k) % 1024] = (float)sum;
    }

    // Dewindowing and output
    for (int j = 0; j < 32; j++) {
        double sum = 0.0;
        for (int i = 0; i < 16; i++) {
            int idx = (offset + j + 64 * i) % 1024;
            int d_idx = j + 32 * i;
            if (d_idx < 512)
                sum += (double)buf[idx] * (double)mp3_synth_d[d_idx];
        }
        // Clamp to 16-bit
        int s = (int)(sum * 32768.0);
        if (s > 32767)
            s = 32767;
        if (s < -32768)
            s = -32768;
        pcm_out[j] = (int16_t)s;
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
    if (len >= 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
        // Syncsafe integer size (4 bytes, 7 bits each)
        size_t tag_size = ((size_t)(data[6] & 0x7F) << 21) | ((size_t)(data[7] & 0x7F) << 14) |
                          ((size_t)(data[8] & 0x7F) << 7) | ((size_t)(data[9] & 0x7F));
        size_t total = 10 + tag_size;
        return (total <= len) ? total : len;
    }
    return 0;
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
        if (scan_pos + (size_t)hdr.frame_size > effective_len)
            break;
        total_samples += (hdr.mpeg_version == 3) ? 1152 : 576;
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
    if (!dec || !data || !io_pos || !pcm_out || !out_frames || !out_channels || !out_sample_rate)
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
    if (pos + (size_t)hdr.frame_size > effective_len) {
        *io_pos = effective_len;
        return 0;
    }

    const uint8_t *frame_data = data + pos + 4;
    mp3_side_info_t si;
    memset(&si, 0, sizeof(si));
    int is_mpeg1 = (hdr.mpeg_version == 3) ? 1 : 0;
    mp3_parse_side_info(frame_data, hdr.side_info_size, hdr.channels, is_mpeg1, &si);

    const uint8_t *main_data = frame_data + hdr.side_info_size;
    int main_data_len = hdr.main_data_size;
    uint8_t combined[4096];
    if (si.main_data_begin > 0 && dec->reservoir_size >= si.main_data_begin) {
        int res_start = dec->reservoir_size - si.main_data_begin;
        int total = si.main_data_begin + main_data_len;
        if (total > (int)sizeof(combined))
            total = (int)sizeof(combined);
        int from_res = si.main_data_begin;
        if (from_res > (int)sizeof(combined))
            from_res = (int)sizeof(combined);
        memcpy(combined, dec->reservoir + res_start, (size_t)from_res);
        int from_frame = total - from_res;
        if (from_frame > main_data_len)
            from_frame = main_data_len;
        if (from_frame > 0)
            memcpy(combined + from_res, main_data, (size_t)from_frame);
        main_data = combined;
        main_data_len = total;
    }

    if (hdr.main_data_size > 0) {
        int to_save = hdr.main_data_size;
        if (to_save > (int)sizeof(dec->reservoir))
            to_save = (int)sizeof(dec->reservoir);
        if (dec->reservoir_size + to_save > (int)sizeof(dec->reservoir)) {
            int shift = dec->reservoir_size + to_save - (int)sizeof(dec->reservoir);
            memmove(dec->reservoir, dec->reservoir + shift, (size_t)(dec->reservoir_size - shift));
            dec->reservoir_size -= shift;
        }
        memcpy(
            dec->reservoir + dec->reservoir_size, frame_data + hdr.side_info_size, (size_t)to_save);
        dec->reservoir_size += to_save;
    }

    int samples_per_frame = (hdr.mpeg_version == 3) ? 1152 : 576;
    memset(pcm_out, 0, (size_t)samples_per_frame * (size_t)hdr.channels * sizeof(int16_t));

    int ngranules = is_mpeg1 ? 2 : 1;
    mp3_bits_t bits;
    mp3_bits_init(&bits, main_data, (size_t)main_data_len);

    for (int gr = 0; gr < ngranules; gr++) {
        float samples[MP3_MAX_CHANNELS][MP3_SBLIMIT];
        memset(samples, 0, sizeof(samples));

        for (int ch = 0; ch < hdr.channels; ch++) {
            mp3_granule_info_t *gi = &si.granules[gr][ch];
            for (int region = 0; region < 3; region++) {
                if (!mp3_huff_table_supported(gi->table_select[region])) {
                    *io_pos = effective_len;
                    return -2;
                }
            }

            size_t part_start = bits.pos;
            size_t part_end = part_start + (size_t)gi->part2_3_length;
            if (part_end > bits.len)
                part_end = bits.len;

            int scalefac_l[22];
            int scalefac_s[13][3];
            memset(scalefac_l, 0, sizeof(scalefac_l));
            memset(scalefac_s, 0, sizeof(scalefac_s));

            int sfc = gi->scalefac_compress;
            int slen1 = mp3_slen_table[0][sfc & 0x0F];
            int slen2 = mp3_slen_table[1][sfc & 0x0F];

            if (gi->window_switching && gi->block_type == 2) {
                if (gi->mixed_block) {
                    for (int sfb = 0; sfb < 8; sfb++)
                        scalefac_l[sfb] = slen1 > 0 ? (int)mp3_bits_read(&bits, slen1) : 0;
                    for (int sfb = 3; sfb < 6; sfb++)
                        for (int win = 0; win < 3; win++)
                            scalefac_s[sfb][win] = slen1 > 0 ? (int)mp3_bits_read(&bits, slen1) : 0;
                    for (int sfb = 6; sfb < 12; sfb++)
                        for (int win = 0; win < 3; win++)
                            scalefac_s[sfb][win] = slen2 > 0 ? (int)mp3_bits_read(&bits, slen2) : 0;
                } else {
                    for (int sfb = 0; sfb < 6; sfb++)
                        for (int win = 0; win < 3; win++)
                            scalefac_s[sfb][win] = slen1 > 0 ? (int)mp3_bits_read(&bits, slen1) : 0;
                    for (int sfb = 6; sfb < 12; sfb++)
                        for (int win = 0; win < 3; win++)
                            scalefac_s[sfb][win] = slen2 > 0 ? (int)mp3_bits_read(&bits, slen2) : 0;
                }
            } else {
                for (int sfb = 0; sfb < 11; sfb++)
                    scalefac_l[sfb] = slen1 > 0 ? (int)mp3_bits_read(&bits, slen1) : 0;
                for (int sfb = 11; sfb < 21; sfb++)
                    scalefac_l[sfb] = slen2 > 0 ? (int)mp3_bits_read(&bits, slen2) : 0;
            }

            int is_values[MP3_SBLIMIT];
            memset(is_values, 0, sizeof(is_values));
            int line = 0;

            int sr_idx = (hdr.sample_rate == 44100) ? 0 : (hdr.sample_rate == 48000) ? 1 : 2;
            int region1_start, region2_start;
            if (gi->window_switching && gi->block_type == 2) {
                region1_start = 36;
                region2_start = MP3_SBLIMIT;
            } else {
                int r0 = gi->region0_count + 1;
                int r1 = gi->region1_count + 1;
                region1_start = (r0 < 22) ? mp3_sfb_long_cumul[sr_idx][r0] : MP3_SBLIMIT;
                region2_start = (r0 + r1 < 22) ? mp3_sfb_long_cumul[sr_idx][r0 + r1] : MP3_SBLIMIT;
            }
            if (region1_start > gi->big_values * 2)
                region1_start = gi->big_values * 2;
            if (region2_start > gi->big_values * 2)
                region2_start = gi->big_values * 2;

            int big_end = gi->big_values * 2;
            if (big_end > MP3_SBLIMIT)
                big_end = MP3_SBLIMIT;

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
                if (table_idx < 0 || table_idx >= 32)
                    table_idx = 0;

                int linbits = mp3_huff_info[table_idx].linbits;
                int max_val = mp3_huff_info[table_idx].max_val;

                while (line < region_end && bits.pos < part_end) {
                    if (max_val == 0) {
                        is_values[line++] = 0;
                        if (line < region_end)
                            is_values[line++] = 0;
                        continue;
                    }

                    int x, y;
                    mp3_huff_decode_pair(&bits, table_idx, &x, &y);

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

            while (line + 3 < MP3_SBLIMIT && bits.pos < part_end) {
                if (gi->count1table_select == 0) {
                    uint32_t code = mp3_bits_read(&bits, 4);
                    is_values[line++] = (code & 8) ? ((mp3_bits_read(&bits, 1)) ? -1 : 1) : 0;
                    is_values[line++] = (code & 4) ? ((mp3_bits_read(&bits, 1)) ? -1 : 1) : 0;
                    is_values[line++] = (code & 2) ? ((mp3_bits_read(&bits, 1)) ? -1 : 1) : 0;
                    is_values[line++] = (code & 1) ? ((mp3_bits_read(&bits, 1)) ? -1 : 1) : 0;
                } else {
                    for (int q = 0; q < 4 && line < MP3_SBLIMIT; q++) {
                        int v = (int)mp3_bits_read(&bits, 1);
                        if (v && bits.pos < part_end)
                            v = mp3_bits_read(&bits, 1) ? -1 : 1;
                        is_values[line++] = v;
                    }
                }
            }

            bits.pos = part_end;

            float xr[MP3_SBLIMIT];
            memset(xr, 0, sizeof(xr));
            double global_gain_pow = pow(2.0, (double)(gi->global_gain - 210) / 4.0);
            int sfac_scale = gi->scalefac_scale ? 2 : 1;

            if (gi->window_switching && gi->block_type == 2 && !gi->mixed_block) {
                for (int sfb = 0; sfb < 12; sfb++) {
                    int width = mp3_sfb_short_44100[sfb];
                    for (int win = 0; win < 3; win++) {
                        double sfac_pow =
                            pow(2.0, -0.5 * (double)(scalefac_s[sfb][win] * sfac_scale));
                        double subblock_pow = pow(2.0, -0.5 * (double)(gi->subblock_gain[win] * 8));
                        for (int i = 0; i < width; i++) {
                            int idx = sfb * 3 * width + win * width + i;
                            if (idx >= MP3_SBLIMIT)
                                break;
                            double val = (double)abs(is_values[idx]);
                            val = (is_values[idx] < 0 ? -1.0 : 1.0) * pow(val, 4.0 / 3.0);
                            xr[idx] = (float)(val * global_gain_pow * sfac_pow * subblock_pow);
                        }
                    }
                }
            } else {
                for (int sfb = 0; sfb < 21; sfb++) {
                    int start = (sr_idx < 3) ? mp3_sfb_long_cumul[sr_idx][sfb] : sfb * 18;
                    int end = (sr_idx < 3) ? mp3_sfb_long_cumul[sr_idx][sfb + 1] : (sfb + 1) * 18;
                    if (end > MP3_SBLIMIT)
                        end = MP3_SBLIMIT;

                    int sf = scalefac_l[sfb];
                    if (gi->preflag)
                        sf += mp3_pretab[sfb];
                    double sfac_pow = pow(2.0, -0.5 * (double)(sf * sfac_scale));

                    for (int i = start; i < end; i++) {
                        double val = (double)abs(is_values[i]);
                        val = (is_values[i] < 0 ? -1.0 : 1.0) * pow(val, 4.0 / 3.0);
                        xr[i] = (float)(val * global_gain_pow * sfac_pow);
                    }
                }
            }

            if (gi->block_type != 2) {
                for (int sb = 1; sb < 32; sb++) {
                    for (int i = 0; i < 8; i++) {
                        int a_idx = sb * 18 - 1 - i;
                        int b_idx = sb * 18 + i;
                        if (a_idx >= 0 && b_idx < MP3_SBLIMIT) {
                            float a = xr[a_idx];
                            float b = xr[b_idx];
                            xr[a_idx] = a * mp3_cs[i] - b * mp3_ca[i];
                            xr[b_idx] = b * mp3_cs[i] + a * mp3_ca[i];
                        }
                    }
                }
            }

            for (int sb = 0; sb < 32; sb++) {
                float imdct_out[36];

                if (gi->block_type == 2) {
                    memset(imdct_out, 0, sizeof(imdct_out));
                    for (int win = 0; win < 3; win++) {
                        float short_in[6];
                        for (int i = 0; i < 6; i++)
                            short_in[i] = xr[sb * 18 + win * 6 + i];
                        float short_out[12];
                        mp3_imdct12(short_in, short_out);
                        for (int i = 0; i < 12; i++)
                            imdct_out[6 + win * 6 + i % 6] += short_out[i] * mp3_win_short[i];
                    }
                } else {
                    float long_in[18];
                    for (int i = 0; i < 18; i++)
                        long_in[i] = xr[sb * 18 + i];
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
                    samples[ch][sb * 18 + i] = imdct_out[i] + dec->overlap[ch][sb][i];
                    dec->overlap[ch][sb][i] = imdct_out[18 + i];
                }
            }
        }

        if (hdr.channel_mode == 1 && (hdr.mode_ext & 0x02)) {
            for (int i = 0; i < MP3_SBLIMIT; i++) {
                float m = samples[0][i];
                float s = samples[1][i];
                samples[0][i] = (m + s) * 0.707106781f;
                samples[1][i] = (m - s) * 0.707106781f;
            }
        }

        for (int ss = 0; ss < 18; ss++) {
            for (int ch = 0; ch < hdr.channels; ch++) {
                float subbands[32];
                for (int sb = 0; sb < 32; sb++)
                    subbands[sb] = samples[ch][sb * 18 + ss];

                int16_t pcm_samples[32];
                mp3_synth_filter(dec, ch, subbands, pcm_samples);

                for (int j = 0; j < 32; j++) {
                    size_t out_idx = ((size_t)gr * (size_t)(samples_per_frame / ngranules) +
                                      (size_t)ss * 32 + (size_t)j) *
                                         (size_t)hdr.channels +
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
    if (!dec || !data || len < 4 || !out_pcm || !out_samples || !out_channels || !out_sample_rate) {
        return -1;
    }

    *out_pcm = NULL;
    *out_samples = 0;
    *out_channels = 0;
    *out_sample_rate = 0;

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

    FILE *f = fopen(filepath, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen <= 0 || flen > 256 * 1024 * 1024) {
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
    if (!stream || !out_pcm)
        return -1;

    *out_pcm = NULL;
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
