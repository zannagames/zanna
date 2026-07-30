//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTOggTests.cpp
// Purpose: Validate Ogg page integrity, packet extraction, failure outputs,
//   and hostile-input resource ceilings.
// Key invariants:
//   - CRC-invalid and unsupported-version pages never publish packet bytes.
//   - Failed reads clear every caller-visible output deterministically.
//   - A container cannot retain unbounded unfinished logical-stream state.
// Ownership/Lifetime:
//   - Test vectors own all memory-reader bytes for the reader's lifetime.
//   - Packet output remains reader-owned and is never freed by the tests.
// Links: src/runtime/audio/rt_ogg.c, src/runtime/audio/rt_ogg.h
//
//===----------------------------------------------------------------------===//

#include "rt_ogg.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

uint32_t g_crc_table[256];
bool g_crc_ready = false;

void crc_init() {
    if (g_crc_ready)
        return;
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int j = 0; j < 8; j++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
        g_crc_table[i] = c;
    }
    g_crc_ready = true;
}

uint32_t ogg_crc(const std::vector<uint8_t> &data) {
    crc_init();
    uint32_t crc = 0;
    for (uint8_t byte : data)
        crc = (crc << 8) ^ g_crc_table[((crc >> 24) ^ byte) & 0xFFu];
    return crc;
}

void put_u32_le(std::vector<uint8_t> &buf, size_t off, uint32_t v) {
    buf[off + 0] = static_cast<uint8_t>(v & 0xFFu);
    buf[off + 1] = static_cast<uint8_t>((v >> 8) & 0xFFu);
    buf[off + 2] = static_cast<uint8_t>((v >> 16) & 0xFFu);
    buf[off + 3] = static_cast<uint8_t>((v >> 24) & 0xFFu);
}

std::vector<uint8_t> make_single_packet_page(uint32_t serial,
                                             const std::vector<uint8_t> &packet,
                                             bool corrupt_crc = false,
                                             uint8_t version = 0,
                                             uint32_t sequence = 0,
                                             uint8_t header_type = 0x02) {
    assert(packet.size() < 255);
    std::vector<uint8_t> page(27 + 1 + packet.size(), 0);
    page[0] = 'O';
    page[1] = 'g';
    page[2] = 'g';
    page[3] = 'S';
    page[4] = version; // stream structure version
    page[5] = header_type;
    for (int i = 0; i < 8; i++)
        page[6 + i] = 0xFFu;
    put_u32_le(page, 14, serial);
    put_u32_le(page, 18, sequence);
    page[26] = 1;
    page[27] = static_cast<uint8_t>(packet.size());
    if (!packet.empty())
        std::memcpy(page.data() + 28, packet.data(), packet.size());

    uint32_t crc = ogg_crc(page);
    if (corrupt_crc)
        crc ^= 1u;
    put_u32_le(page, 22, crc);
    return page;
}

std::vector<uint8_t> make_unfinished_packet_page(uint32_t serial, uint32_t sequence = 0) {
    std::vector<uint8_t> page(27 + 1 + 255, 0);
    page[0] = 'O';
    page[1] = 'g';
    page[2] = 'g';
    page[3] = 'S';
    page[4] = 0;
    page[5] = 0x02; // BOS
    for (int i = 0; i < 8; ++i)
        page[6 + i] = 0xFFu;
    put_u32_le(page, 14, serial);
    put_u32_le(page, 18, sequence);
    page[26] = 1;
    page[27] = 255; // No terminating lacing value: packet remains partial.
    std::memset(page.data() + 28, static_cast<int>(serial & 0xFFu), 255);
    put_u32_le(page, 22, ogg_crc(page));
    return page;
}

void test_resync_and_serial_zero() {
    std::vector<uint8_t> packet = {1, 'v', 'o', 'r', 'b', 'i', 's'};
    std::vector<uint8_t> data = {'x'};
    std::vector<uint8_t> page = make_single_packet_page(0, packet);
    data.insert(data.end(), page.begin(), page.end());

    ogg_reader_t *reader = ogg_reader_open_mem(data.data(), data.size());
    assert(reader != nullptr);

    const uint8_t *out = nullptr;
    size_t out_len = 0;
    ogg_packet_info_t info{};
    assert(ogg_reader_next_packet_ex(reader, &out, &out_len, &info) == 1);
    assert(out_len == packet.size());
    assert(std::memcmp(out, packet.data(), packet.size()) == 0);
    assert(info.serial_number == 0);
    assert(info.bos == 1);

    ogg_reader_free(reader);
}

void test_crc_mismatch_rejected() {
    std::vector<uint8_t> packet = {'b', 'a', 'd'};
    std::vector<uint8_t> page = make_single_packet_page(123, packet, true);

    ogg_reader_t *reader = ogg_reader_open_mem(page.data(), page.size());
    assert(reader != nullptr);

    const uint8_t *out = nullptr;
    size_t out_len = 0;
    assert(ogg_reader_next_packet(reader, &out, &out_len) == 0);
    assert(out == nullptr);
    assert(out_len == 0);

    ogg_reader_free(reader);
}

void test_nonzero_version_rejected() {
    std::vector<uint8_t> packet = {'v', 'e', 'r'};
    std::vector<uint8_t> page = make_single_packet_page(321, packet, false, 1);

    ogg_reader_t *reader = ogg_reader_open_mem(page.data(), page.size());
    assert(reader != nullptr);

    const uint8_t *out = nullptr;
    size_t out_len = 0;
    assert(ogg_reader_next_packet(reader, &out, &out_len) == 0);

    ogg_reader_free(reader);
}

void test_failed_read_clears_outputs() {
    std::vector<uint8_t> packet = {'b', 'a', 'd'};
    std::vector<uint8_t> page = make_single_packet_page(456, packet, true);

    ogg_reader_t *reader = ogg_reader_open_mem(page.data(), page.size());
    assert(reader != nullptr);

    const uint8_t *out = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(1));
    size_t out_len = 99;
    ogg_packet_info_t info{123, 456, 1, 1};
    assert(ogg_reader_next_packet_ex(reader, &out, &out_len, &info) == 0);
    assert(out == nullptr);
    assert(out_len == 0);
    assert(info.serial_number == 0);
    assert(info.granule_position == 0);
    assert(info.bos == 0);
    assert(info.eos == 0);

    ogg_reader_free(reader);
}

void test_logical_stream_state_is_bounded() {
    constexpr uint32_t kMaxActiveStreams = 64;
    std::vector<uint8_t> data;
    for (uint32_t serial = 1; serial <= kMaxActiveStreams + 1; ++serial) {
        std::vector<uint8_t> page = make_unfinished_packet_page(serial);
        data.insert(data.end(), page.begin(), page.end());
    }
    std::vector<uint8_t> final_page =
        make_single_packet_page(kMaxActiveStreams + 2, std::vector<uint8_t>{'o', 'k'});
    data.insert(data.end(), final_page.begin(), final_page.end());

    ogg_reader_t *reader = ogg_reader_open_mem(data.data(), data.size());
    assert(reader != nullptr);

    const uint8_t *out = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(1));
    size_t out_len = 99;
    assert(ogg_reader_next_packet(reader, &out, &out_len) == 0);
    assert(out == nullptr);
    assert(out_len == 0);

    ogg_reader_free(reader);
}

void test_empty_packet_is_successful_and_preserves_stream_order() {
    std::vector<uint8_t> data = make_single_packet_page(777, {});
    std::vector<uint8_t> nonempty_page =
        make_single_packet_page(888, std::vector<uint8_t>{'n', 'e', 'x', 't'});
    data.insert(data.end(), nonempty_page.begin(), nonempty_page.end());

    ogg_reader_t *reader = ogg_reader_open_mem(data.data(), data.size());
    assert(reader != nullptr);

    const uint8_t *out = nullptr;
    size_t out_len = 99;
    ogg_packet_info_t info{};
    assert(ogg_reader_next_packet_ex(reader, &out, &out_len, &info) == 1);
    assert(out != nullptr);
    assert(out_len == 0);
    assert(info.serial_number == 777);
    assert(info.bos == 1);

    out = nullptr;
    out_len = 0;
    info = {};
    assert(ogg_reader_next_packet_ex(reader, &out, &out_len, &info) == 1);
    assert(out != nullptr);
    assert(out_len == 4);
    assert(std::memcmp(out, "next", 4) == 0);
    assert(info.serial_number == 888);
    assert(info.bos == 1);

    ogg_reader_free(reader);
}

void test_sequence_gap_cannot_splice_partial_packet() {
    std::vector<uint8_t> data = make_unfinished_packet_page(901, 10);
    std::vector<uint8_t> continuation =
        make_single_packet_page(901, std::vector<uint8_t>{'x'}, false, 0, 12, 0x01);
    data.insert(data.end(), continuation.begin(), continuation.end());

    ogg_reader_t *reader = ogg_reader_open_mem(data.data(), data.size());
    assert(reader != nullptr);

    const uint8_t *out = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(1));
    size_t out_len = 99;
    assert(ogg_reader_next_packet(reader, &out, &out_len) == 0);
    assert(out == nullptr);
    assert(out_len == 0);

    ogg_reader_free(reader);
}

void test_missing_continuation_flag_cannot_splice_partial_packet() {
    std::vector<uint8_t> data = make_unfinished_packet_page(902, 0);
    std::vector<uint8_t> non_continuation =
        make_single_packet_page(902, std::vector<uint8_t>{'x'}, false, 0, 1, 0);
    data.insert(data.end(), non_continuation.begin(), non_continuation.end());

    ogg_reader_t *reader = ogg_reader_open_mem(data.data(), data.size());
    assert(reader != nullptr);

    const uint8_t *out = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(1));
    size_t out_len = 99;
    assert(ogg_reader_next_packet(reader, &out, &out_len) == 0);
    assert(out == nullptr);
    assert(out_len == 0);

    ogg_reader_free(reader);
}

void test_duplicate_page_sequence_is_rejected() {
    std::vector<uint8_t> data =
        make_single_packet_page(903, std::vector<uint8_t>{'a'}, false, 0, 0, 0x02);
    std::vector<uint8_t> duplicate =
        make_single_packet_page(903, std::vector<uint8_t>{'b'}, false, 0, 0, 0);
    data.insert(data.end(), duplicate.begin(), duplicate.end());

    ogg_reader_t *reader = ogg_reader_open_mem(data.data(), data.size());
    assert(reader != nullptr);

    const uint8_t *out = nullptr;
    size_t out_len = 0;
    assert(ogg_reader_next_packet(reader, &out, &out_len) == 1);
    assert(out_len == 1);
    assert(out[0] == 'a');

    out = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(1));
    out_len = 99;
    assert(ogg_reader_next_packet(reader, &out, &out_len) == 0);
    assert(out == nullptr);
    assert(out_len == 0);

    ogg_reader_free(reader);
}

void test_page_sequence_wraparound_preserves_continuation() {
    std::vector<uint8_t> data = make_unfinished_packet_page(904, UINT32_MAX);
    std::vector<uint8_t> continuation =
        make_single_packet_page(904, std::vector<uint8_t>{'z'}, false, 0, 0, 0x01);
    data.insert(data.end(), continuation.begin(), continuation.end());

    ogg_reader_t *reader = ogg_reader_open_mem(data.data(), data.size());
    assert(reader != nullptr);

    const uint8_t *out = nullptr;
    size_t out_len = 0;
    assert(ogg_reader_next_packet(reader, &out, &out_len) == 1);
    assert(out != nullptr);
    assert(out_len == 256);
    assert(out[255] == 'z');

    ogg_reader_free(reader);
}

} // namespace

int main() {
    test_resync_and_serial_zero();
    test_crc_mismatch_rejected();
    test_nonzero_version_rejected();
    test_failed_read_clears_outputs();
    test_logical_stream_state_is_bounded();
    test_empty_packet_is_successful_and_preserves_stream_order();
    test_sequence_gap_cannot_splice_partial_packet();
    test_missing_continuation_flag_cannot_splice_partial_packet();
    test_duplicate_page_sequence_is_rejected();
    test_page_sequence_wraparound_preserves_continuation();
    return 0;
}
