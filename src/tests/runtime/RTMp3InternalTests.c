//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTMp3InternalTests.c
// Purpose: Exercise private MP3 parser, reservoir, and synthesis hardening.
// Key invariants:
//   - Truncated fields, malformed side info, and inconsistent streams fail.
//   - CRC, ID3, short-block, and mixed-block offsets follow their formats.
//   - Failure outputs are inert and float-to-PCM conversion is defined.
// Ownership/Lifetime:
//   - Test buffers are stack-owned; decoders are released by each test.
// Links: src/runtime/audio/rt_mp3.c, src/runtime/audio/rt_mp3_tables.h
//
//===----------------------------------------------------------------------===//

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../../runtime/audio/rt_mp3.c"

static void put_msb_bits(uint8_t *data, size_t bit_pos, uint32_t value, int count) {
    for (int i = 0; i < count; i++) {
        size_t pos = bit_pos + (size_t)i;
        uint8_t mask = (uint8_t)(1u << (7u - (unsigned)(pos % 8u)));
        if (value & (1u << (count - 1 - i)))
            data[pos / 8u] |= mask;
        else
            data[pos / 8u] &= (uint8_t)~mask;
    }
}

static size_t make_frame(uint8_t *frame, size_t cap, const uint8_t header[4]) {
    mp3_frame_header_t parsed;
    assert(mp3_parse_header(header, &parsed) == 0);
    assert(parsed.frame_size > 0);
    assert((size_t)parsed.frame_size <= cap);
    memset(frame, 0, (size_t)parsed.frame_size);
    memcpy(frame, header, 4);
    return (size_t)parsed.frame_size;
}

static void test_bit_reader_rejects_truncated_and_invalid_fields(void) {
    uint8_t data[1] = {0xA5};
    mp3_bits_t bits;

    mp3_bits_init(&bits, data, sizeof(data));
    assert(mp3_bits_read(&bits, 8) == 0xA5u);
    assert(bits.error == 0);
    assert(mp3_bits_read(&bits, 1) == 0);
    assert(bits.error != 0);

    mp3_bits_init(&bits, data, sizeof(data));
    assert(mp3_bits_read(&bits, 33) == 0);
    assert(bits.error != 0);
}

static void test_header_parser_clears_output_and_accounts_for_crc(void) {
    const uint8_t protected_header[4] = {0xFF, 0xFA, 0x90, 0x00};
    const uint8_t invalid_header[4] = {0, 0, 0, 0};
    mp3_frame_header_t header;

    memset(&header, 0xA5, sizeof(header));
    assert(mp3_parse_header(invalid_header, &header) == -1);
    for (size_t i = 0; i < sizeof(header); i++)
        assert(((const uint8_t *)&header)[i] == 0);

    assert(mp3_parse_header(protected_header, &header) == 0);
    assert(header.crc_size == 2);
    assert(header.main_data_size == header.frame_size - 4 - 2 - header.side_info_size);
    assert(mp3_parse_header(NULL, &header) == -1);
    assert(mp3_parse_header(protected_header, NULL) == -1);
}

static void test_header_rejects_reserved_emphasis(void) {
    const uint8_t header[4] = {0xFF, 0xFB, 0x90, 0x02};
    mp3_frame_header_t parsed;
    assert(mp3_parse_header(header, &parsed) == -1);
}

static void test_side_info_validates_shape_and_reserved_values(void) {
    uint8_t side[32];
    mp3_side_info_t info;

    memset(side, 0, sizeof(side));
    memset(&info, 0xA5, sizeof(info));
    assert(mp3_parse_side_info(NULL, 32, 2, 1, &info) == -1);
    for (size_t i = 0; i < sizeof(info); i++)
        assert(((const uint8_t *)&info)[i] == 0);
    assert(mp3_parse_side_info(side, 31, 2, 1, &info) == -1);
    assert(mp3_parse_side_info(side, 32, 0, 1, &info) == -1);
    assert(mp3_parse_side_info(side, 32, 2, 1, NULL) == -1);

    memset(side, 0, sizeof(side));
    put_msb_bits(side, 32, 289, 9); // first granule/channel big_values
    assert(mp3_parse_side_info(side, 32, 2, 1, &info) == -1);

    memset(side, 0, sizeof(side));
    put_msb_bits(side, 53, 1, 1); // window switching with reserved block_type 0
    assert(mp3_parse_side_info(side, 32, 2, 1, &info) == -1);
}

static void test_failed_late_granule_restores_synthesis_state(void) {
    const uint8_t header[4] = {0xFF, 0xFB, 0x90, 0xC0};
    uint8_t frame[500];
    int16_t pcm[1152];
    size_t frame_size = make_frame(frame, sizeof(frame), header);
    size_t pos = 0;
    int frames = 0;
    int channels = 0;
    int rate = 0;
    mp3_decoder_t *dec = mp3_decoder_new();

    assert(dec != NULL);
    put_msb_bits(frame + 4, 111, 7, 5); // granule 1 uses an unsupported Huffman table
    assert(mp3_decode_frame_internal(
               dec, frame, frame_size, &pos, pcm, &frames, &channels, &rate) == -2);
    assert(dec->synth_offset[0] == 0);
    for (int sb = 0; sb < MP3_SUBBANDS; sb++)
        for (int i = 0; i < 18; i++)
            assert(dec->overlap[0][sb][i] == 0.0f);
    mp3_decoder_free(dec);
}

static void test_id3_parser_rejects_non_syncsafe_and_counts_footer(void) {
    uint8_t tag[20];

    memset(tag, 0, sizeof(tag));
    memcpy(tag, "ID3", 3);
    tag[3] = 4;
    tag[6] = 0x80;
    assert(mp3_skip_id3v2(tag, sizeof(tag)) == sizeof(tag));

    memset(tag, 0, sizeof(tag));
    memcpy(tag, "ID3", 3);
    tag[3] = 4;
    tag[5] = 0x10;
    assert(mp3_skip_id3v2(tag, sizeof(tag)) == sizeof(tag));
}

static void test_metadata_scan_rejects_format_changes_and_clears_outputs(void) {
    const uint8_t header_44100[4] = {0xFF, 0xFB, 0x90, 0x00};
    const uint8_t header_48000[4] = {0xFF, 0xFB, 0x94, 0x00};
    uint8_t data[900];
    size_t first_size = make_frame(data, sizeof(data), header_44100);
    size_t second_size = make_frame(data + first_size, sizeof(data) - first_size, header_48000);
    size_t first = 99;
    size_t effective = 99;
    int channels = 99;
    int rate = 99;
    int samples = 99;

    assert(mp3_scan_stream_metadata(NULL, 0, &first, &effective, &channels, &rate, &samples) == -1);
    assert(first == 0 && effective == 0 && channels == 0 && rate == 0 && samples == 0);

    assert(mp3_scan_stream_metadata(
               data, first_size + second_size, &first, &effective, &channels, &rate, &samples) ==
           -1);
}

static void test_sample_accumulator_rejects_integer_overflow(void) {
    int total = INT_MAX - 100;
    assert(mp3_add_sample_count(&total, 1152) == -1);
    assert(total == INT_MAX - 100);
}

static void test_missing_reservoir_rejects_frame_with_inert_outputs(void) {
    const uint8_t header[4] = {0xFF, 0xFB, 0x90, 0x00};
    uint8_t frame[500];
    int16_t pcm[1152 * 2];
    size_t frame_size = make_frame(frame, sizeof(frame), header);
    size_t pos = 0;
    int frames = 99;
    int channels = 99;
    int rate = 99;
    mp3_decoder_t *dec = mp3_decoder_new();

    assert(dec != NULL);
    put_msb_bits(frame + 4, 0, 1, 9);
    assert(mp3_decode_frame_internal(
               dec, frame, frame_size, &pos, pcm, &frames, &channels, &rate) == -1);
    assert(pos == frame_size);
    assert(frames == 0 && channels == 0 && rate == 0);
    assert(dec->reservoir_size > 0);
    mp3_decoder_free(dec);
}

static void test_reservoir_retains_most_recent_bytes(void) {
    uint8_t data[2050];
    mp3_decoder_t *dec = mp3_decoder_new();

    assert(dec != NULL);
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)i;
    mp3_reservoir_append(dec, data, (int)sizeof(data));
    assert(dec->reservoir_size == (int)sizeof(dec->reservoir));
    assert(dec->reservoir[0] == data[2]);
    assert(dec->reservoir[sizeof(dec->reservoir) - 1] == data[sizeof(data) - 1]);
    mp3_decoder_free(dec);
}

static void test_part_length_bounds_every_entropy_read(void) {
    const uint8_t header[4] = {0xFF, 0xFB, 0x90, 0x00};
    uint8_t frame[500];
    int16_t pcm[1152 * 2];
    size_t frame_size = make_frame(frame, sizeof(frame), header);
    size_t pos = 0;
    int frames = 0;
    int channels = 0;
    int rate = 0;
    mp3_decoder_t *dec = mp3_decoder_new();

    assert(dec != NULL);
    put_msb_bits(frame + 4, 20, 1, 12);
    assert(mp3_decode_frame_internal(
               dec, frame, frame_size, &pos, pcm, &frames, &channels, &rate) == -1);
    assert(frames == 0 && channels == 0 && rate == 0);
    mp3_decoder_free(dec);
}

static void test_short_and_mixed_block_index_helpers(void) {
    mp3_granule_info_t info;
    memset(&info, 0, sizeof(info));

    assert(mp3_short_band_index(0, 4, 2, 5) == 65);
    assert(mp3_short_imdct_offset(2, 11) == 29);

    info.block_type = 2;
    info.mixed_block = 1;
    assert(mp3_uses_short_imdct(&info, 0) == 0);
    assert(mp3_uses_short_imdct(&info, 1) == 0);
    assert(mp3_uses_short_imdct(&info, 2) != 0);
    info.mixed_block = 0;
    assert(mp3_uses_short_imdct(&info, 0) != 0);
    info.block_type = 0;
    assert(mp3_uses_short_imdct(&info, 3) == 0);
}

static void test_pcm_conversion_is_defined_for_extreme_values(void) {
    assert(mp3_pcm_s16_from_double(NAN) == 0);
    assert(mp3_pcm_s16_from_double(INFINITY) == 32767);
    assert(mp3_pcm_s16_from_double(-INFINITY) == -32768);
    assert(mp3_pcm_s16_from_double(2.0) == 32767);
    assert(mp3_pcm_s16_from_double(-2.0) == -32768);
}

static void test_public_failure_outputs_are_inert(void) {
    int16_t *pcm = (int16_t *)(uintptr_t)1;
    int samples = 99;
    int channels = 99;
    int rate = 99;

    assert(mp3_decode_file(NULL, NULL, 0, &pcm, &samples, &channels, &rate) == -1);
    assert(pcm == NULL && samples == 0 && channels == 0 && rate == 0);

    pcm = (int16_t *)(uintptr_t)1;
    assert(mp3_stream_decode_frame(NULL, &pcm) == -1);
    assert(pcm == NULL);
}

int main(void) {
    test_bit_reader_rejects_truncated_and_invalid_fields();
    test_header_parser_clears_output_and_accounts_for_crc();
    test_header_rejects_reserved_emphasis();
    test_side_info_validates_shape_and_reserved_values();
    test_failed_late_granule_restores_synthesis_state();
    test_id3_parser_rejects_non_syncsafe_and_counts_footer();
    test_metadata_scan_rejects_format_changes_and_clears_outputs();
    test_sample_accumulator_rejects_integer_overflow();
    test_missing_reservoir_rejects_frame_with_inert_outputs();
    test_reservoir_retains_most_recent_bytes();
    test_part_length_bounds_every_entropy_read();
    test_short_and_mixed_block_index_helpers();
    test_pcm_conversion_is_defined_for_extreme_values();
    test_public_failure_outputs_are_inert();
    return 0;
}
