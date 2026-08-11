//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTVorbisInternalTests.c
// Purpose: Exercise private Vorbis parser and decoder hardening invariants.
// Key invariants:
//   - Truncated bitstreams cannot synthesize zero-valued fields.
//   - Header packets are complete, ordered, and framed before state advances.
//   - Failed public decode calls clear caller-visible output parameters.
// Ownership/Lifetime:
//   - Each test owns its packet bytes and releases every decoder it creates.
// Links: src/runtime/audio/rt_vorbis.c, src/runtime/audio/rt_vorbis.h
//
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/audio/rt_vorbis.c"

typedef struct {
    uint8_t data[128];
    size_t len;
    uint8_t cur;
    int bits_used;
} bit_writer_t;

static void make_identification(uint8_t packet[30], uint8_t block_sizes, uint8_t framing) {
    memset(packet, 0, 30);
    packet[0] = 1;
    memcpy(packet + 1, "vorbis", 6);
    packet[11] = 2;
    packet[12] = 0x44;
    packet[13] = 0xAC;
    packet[28] = block_sizes;
    packet[29] = framing;
}

static size_t make_minimal_comment(uint8_t packet[16]) {
    memset(packet, 0, 16);
    packet[0] = 3;
    memcpy(packet + 1, "vorbis", 6);
    packet[15] = 1;
    return 16;
}

static void bw_put_bits(bit_writer_t *bw, uint32_t value, int count) {
    for (int i = 0; i < count; i++) {
        bw->cur |= (uint8_t)(((value >> i) & 1u) << bw->bits_used);
        bw->bits_used++;
        if (bw->bits_used == 8) {
            assert(bw->len < sizeof(bw->data));
            bw->data[bw->len++] = bw->cur;
            bw->cur = 0;
            bw->bits_used = 0;
        }
    }
}

static size_t bw_finish_setup(bit_writer_t *bw, uint8_t *out, size_t out_cap) {
    static const uint8_t prefix[7] = {5, 'v', 'o', 'r', 'b', 'i', 's'};
    assert(out_cap >= sizeof(prefix) + bw->len + 1);
    memcpy(out, prefix, sizeof(prefix));
    if (bw->bits_used != 0) {
        assert(bw->len < sizeof(bw->data));
        bw->data[bw->len++] = bw->cur;
        bw->cur = 0;
        bw->bits_used = 0;
    }
    memcpy(out + sizeof(prefix), bw->data, bw->len);
    return sizeof(prefix) + bw->len;
}

static void bw_put_minimal_codebook(bit_writer_t *bw) {
    bw_put_bits(bw, 0x564342u, 24); // sync
    bw_put_bits(bw, 1, 16);         // dimensions
    bw_put_bits(bw, 1, 24);         // entries
    bw_put_bits(bw, 0, 1);          // unordered
    bw_put_bits(bw, 0, 1);          // not sparse
    bw_put_bits(bw, 0, 5);          // code length 1
}

static void test_25_bit_codeword_decodes_without_truncation(void) {
    vorbis_codebook_t cb;
    uint8_t lengths[2] = {25, 25};
    uint8_t bits_data[4] = {0, 0, 0, 1};
    vorbis_bits_t bits;

    memset(&cb, 0, sizeof(cb));
    cb.entries = 2;
    cb.dimensions = 1;
    cb.lengths = lengths;

    assert(codebook_build_tree(&cb) == 0);
    assert(cb.sorted_count == 2);
    assert(cb.sorted_code_lengths[0] == 25);
    assert(cb.sorted_code_lengths[1] == 25);
    assert(cb.sorted_codes[1] == (1u << 24));

    bits_init(&bits, bits_data, sizeof(bits_data));
    assert(codebook_decode_scalar(&cb, &bits) == 1);

    free(cb.sorted_codes);
    free(cb.sorted_code_lengths);
    free(cb.sorted_indices);
}

static void test_codeword_can_end_on_final_packet_bit(void) {
    vorbis_codebook_t cb;
    uint32_t code = 0;
    uint8_t code_len = 8;
    int index = 17;
    uint8_t data[1] = {0};
    vorbis_bits_t bits;

    memset(&cb, 0, sizeof(cb));
    cb.sorted_codes = &code;
    cb.sorted_code_lengths = &code_len;
    cb.sorted_indices = &index;
    cb.sorted_count = 1;

    bits_init(&bits, data, sizeof(data));
    assert(codebook_decode_scalar(&cb, &bits) == index);
}

static void test_bit_reader_marks_truncated_fields(void) {
    uint8_t data[1] = {0xA5};
    vorbis_bits_t bits;

    bits_init(&bits, data, sizeof(data));
    assert(bits_read(&bits, 8) == 0xA5u);
    assert(bits.error == 0);
    assert(bits_read1(&bits) == 0);
    assert(bits.error != 0);
}

static void test_identification_requires_exact_framed_packet(void) {
    uint8_t ident[31];
    vorbis_decoder_t *dec;

    make_identification(ident, 0xB8, 1);
    ident[30] = 0;
    dec = vorbis_decoder_new();
    assert(dec != NULL);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == -1);
    vorbis_decoder_free(dec);

    make_identification(ident, 0xB8, 0);
    dec = vorbis_decoder_new();
    assert(dec != NULL);
    assert(vorbis_decode_header(dec, ident, 30, 0) == -1);
    vorbis_decoder_free(dec);
}

static void test_identification_rejects_oversized_blocks(void) {
    uint8_t ident[30];
    vorbis_decoder_t *dec = vorbis_decoder_new();

    assert(dec != NULL);
    make_identification(ident, 0xE8, 1);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == -1);
    vorbis_decoder_free(dec);
}

static void test_headers_must_be_unique_and_ordered(void) {
    uint8_t ident[30];
    uint8_t comment[16];
    vorbis_decoder_t *dec = vorbis_decoder_new();

    assert(dec != NULL);
    make_identification(ident, 0xB8, 1);
    make_minimal_comment(comment);
    assert(vorbis_decode_header(dec, comment, sizeof(comment), 1) == -1);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == 0);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == -1);
    assert(vorbis_decode_header(dec, comment, sizeof(comment), 1) == 0);
    assert(vorbis_decode_header(dec, comment, sizeof(comment), 1) == -1);
    vorbis_decoder_free(dec);
}

static void test_comment_packet_is_fully_bounded(void) {
    uint8_t ident[30];
    uint8_t comment[32];
    vorbis_decoder_t *dec;

    make_identification(ident, 0xB8, 1);

    dec = vorbis_decoder_new();
    assert(dec != NULL);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == 0);
    memset(comment, 0, sizeof(comment));
    comment[0] = 3;
    memcpy(comment + 1, "vorbis", 6);
    assert(vorbis_decode_header(dec, comment, 7, 1) == -1);
    vorbis_decoder_free(dec);

    dec = vorbis_decoder_new();
    assert(dec != NULL);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == 0);
    make_minimal_comment(comment);
    comment[7] = 20;
    assert(vorbis_decode_header(dec, comment, 16, 1) == -1);
    vorbis_decoder_free(dec);

    dec = vorbis_decoder_new();
    assert(dec != NULL);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == 0);
    make_minimal_comment(comment);
    comment[11] = 1;
    assert(vorbis_decode_header(dec, comment, 16, 1) == -1);
    vorbis_decoder_free(dec);

    dec = vorbis_decoder_new();
    assert(dec != NULL);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == 0);
    make_minimal_comment(comment);
    comment[15] = 0;
    assert(vorbis_decode_header(dec, comment, 16, 1) == -1);
    vorbis_decoder_free(dec);

    dec = vorbis_decoder_new();
    assert(dec != NULL);
    assert(vorbis_decode_header(dec, ident, sizeof(ident), 0) == 0);
    make_minimal_comment(comment);
    comment[16] = 0;
    assert(vorbis_decode_header(dec, comment, 17, 1) == -1);
    vorbis_decoder_free(dec);
}

static void test_failed_packet_decode_clears_outputs(void) {
    uint8_t packet[1] = {0};
    int16_t *pcm = (int16_t *)(uintptr_t)1;
    int samples = 99;
    vorbis_decoder_t *dec = vorbis_decoder_new();

    assert(dec != NULL);
    assert(vorbis_decode_packet(dec, packet, sizeof(packet), &pcm, &samples) == -1);
    assert(pcm == NULL);
    assert(samples == 0);
    assert(vorbis_decode_packet(dec, packet, sizeof(packet), NULL, &samples) == -1);
    assert(vorbis_decode_packet(dec, packet, sizeof(packet), &pcm, NULL) == -1);
    vorbis_decoder_free(dec);
}

static void test_pcm_conversion_handles_nonfinite_and_extreme_values(void) {
    assert(pcm_s16_from_float(NAN) == 0);
    assert(pcm_s16_from_float(INFINITY) == 32767);
    assert(pcm_s16_from_float(-INFINITY) == -32768);
    assert(pcm_s16_from_float(2.0f) == 32767);
    assert(pcm_s16_from_float(-2.0f) == -32768);
    assert(pcm_s16_from_float(0.0f) == 0);
}

static void test_setup_rejects_oversized_codebook_dimension(void) {
    vorbis_decoder_t *dec = vorbis_decoder_new();
    bit_writer_t bw;
    uint8_t setup[160];
    size_t setup_len;

    memset(&bw, 0, sizeof(bw));
    bw_put_bits(&bw, 0, 8); // one codebook
    bw_put_bits(&bw, 0x564342u, 24);
    bw_put_bits(&bw, 257, 16); // dimensions exceed internal vector buffers
    bw_put_bits(&bw, 1, 24);
    setup_len = bw_finish_setup(&bw, setup, sizeof(setup));

    assert(decode_setup(dec, setup, setup_len) == -1);
    vorbis_decoder_free(dec);
}

static void test_setup_rejects_zero_progress_ordered_codebook(void) {
    vorbis_decoder_t *dec = vorbis_decoder_new();
    bit_writer_t bw;
    uint8_t setup[160];
    size_t setup_len;

    assert(dec != NULL);
    memset(&bw, 0, sizeof(bw));
    bw_put_bits(&bw, 0, 8); // one codebook
    bw_put_bits(&bw, 0x564342u, 24);
    bw_put_bits(&bw, 1, 16); // dimensions
    bw_put_bits(&bw, 2, 24); // entries
    bw_put_bits(&bw, 1, 1);  // ordered
    bw_put_bits(&bw, 0, 5);  // initial code length 1
    bw_put_bits(&bw, 0, 2);  // invalid zero-entry run
    setup_len = bw_finish_setup(&bw, setup, sizeof(setup));

    assert(decode_setup(dec, setup, setup_len) == -1);
    vorbis_decoder_free(dec);
}

static void test_setup_rejects_invalid_codebook_lookup_type(void) {
    vorbis_decoder_t *dec = vorbis_decoder_new();
    bit_writer_t bw;
    uint8_t setup[160];
    size_t setup_len;

    memset(&bw, 0, sizeof(bw));
    bw_put_bits(&bw, 0, 8); // one codebook
    bw_put_minimal_codebook(&bw);
    bw_put_bits(&bw, 3, 4); // only lookup types 0, 1, and 2 are valid
    setup_len = bw_finish_setup(&bw, setup, sizeof(setup));

    assert(decode_setup(dec, setup, setup_len) == -1);
    vorbis_decoder_free(dec);
}

static void test_setup_rejects_nonzero_time_transform(void) {
    vorbis_decoder_t *dec = vorbis_decoder_new();
    bit_writer_t bw;
    uint8_t setup[160];
    size_t setup_len;

    memset(&bw, 0, sizeof(bw));
    bw_put_bits(&bw, 0, 8); // one codebook
    bw_put_minimal_codebook(&bw);
    bw_put_bits(&bw, 0, 4);  // lookup type 0
    bw_put_bits(&bw, 0, 6);  // one time-domain transform
    bw_put_bits(&bw, 1, 16); // time-domain transforms must be zero
    setup_len = bw_finish_setup(&bw, setup, sizeof(setup));

    assert(decode_setup(dec, setup, setup_len) == -1);
    vorbis_decoder_free(dec);
}

int main(void) {
    test_25_bit_codeword_decodes_without_truncation();
    test_codeword_can_end_on_final_packet_bit();
    test_bit_reader_marks_truncated_fields();
    test_identification_requires_exact_framed_packet();
    test_identification_rejects_oversized_blocks();
    test_headers_must_be_unique_and_ordered();
    test_comment_packet_is_fully_bounded();
    test_failed_packet_decode_clears_outputs();
    test_pcm_conversion_handles_nonfinite_and_extreme_values();
    test_setup_rejects_oversized_codebook_dimension();
    test_setup_rejects_zero_progress_ordered_codebook();
    test_setup_rejects_invalid_codebook_lookup_type();
    test_setup_rejects_nonzero_time_transform();
    return 0;
}
