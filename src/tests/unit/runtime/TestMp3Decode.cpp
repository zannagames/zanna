//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/runtime/TestMp3Decode.cpp
// Purpose: Unit tests for the MP3 decoder.
// Key invariants:
//   - Decoder rejects invalid/non-MP3 data
//   - Frame header parser extracts correct metadata
//   - ID3v2 tag skipping works correctly
// Ownership/Lifetime:
//   - Test-scoped
// Links: src/runtime/audio/rt_mp3.c
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "runtime/audio/rt_mp3.h"
}

static bool write_temp_file(const char *path, const uint8_t *data, size_t size) {
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(data, 1, size, f) == size;
    fclose(f);
    return ok;
}

TEST(Mp3DecodeTest, CreateAndFree) {
    mp3_decoder_t *dec = mp3_decoder_new();
    ASSERT_TRUE(dec != nullptr);
    mp3_decoder_free(dec);
}

TEST(Mp3DecodeTest, RejectNull) {
    mp3_decoder_t *dec = mp3_decoder_new();
    ASSERT_TRUE(dec != nullptr);

    int16_t *pcm = nullptr;
    int samples = 0, channels = 0, sample_rate = 0;
    int rc = mp3_decode_file(dec, nullptr, 0, &pcm, &samples, &channels, &sample_rate);
    EXPECT_EQ(rc, -1);

    mp3_decoder_free(dec);
}

TEST(Mp3DecodeTest, RejectGarbage) {
    mp3_decoder_t *dec = mp3_decoder_new();
    ASSERT_TRUE(dec != nullptr);

    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    int16_t *pcm = nullptr;
    int samples = 0, channels = 0, sample_rate = 0;
    int rc =
        mp3_decode_file(dec, garbage, sizeof(garbage), &pcm, &samples, &channels, &sample_rate);
    EXPECT_EQ(rc, -1);

    mp3_decoder_free(dec);
}

TEST(Mp3DecodeTest, RejectWavAsMp3) {
    mp3_decoder_t *dec = mp3_decoder_new();
    ASSERT_TRUE(dec != nullptr);

    // WAV RIFF header should not decode as MP3
    const uint8_t wav[] = {
        'R', 'I', 'F', 'F', 0x00, 0x00, 0x00, 0x00, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' '};
    int16_t *pcm = nullptr;
    int samples = 0, channels = 0, sample_rate = 0;
    int rc = mp3_decode_file(dec, wav, sizeof(wav), &pcm, &samples, &channels, &sample_rate);
    EXPECT_EQ(rc, -1);

    mp3_decoder_free(dec);
}

TEST(Mp3DecodeTest, Id3v2SkipComputation) {
    // Verify that an ID3v2 header causes the decoder to skip the correct number of bytes.
    // A file with just an ID3v2 tag and no audio should fail gracefully.
    mp3_decoder_t *dec = mp3_decoder_new();
    ASSERT_TRUE(dec != nullptr);

    // ID3v2 header: "ID3" + version 2.3 + flags=0 + syncsafe size=100
    // syncsafe: 100 = 0x00 0x00 0x00 0x64
    uint8_t id3[120];
    memset(id3, 0, sizeof(id3));
    id3[0] = 'I';
    id3[1] = 'D';
    id3[2] = '3';
    id3[3] = 3;
    id3[4] = 0; // version 2.3.0
    id3[5] = 0; // flags
    // Size: 100 in syncsafe (0x00 0x00 0x00 0x64)
    id3[6] = 0;
    id3[7] = 0;
    id3[8] = 0;
    id3[9] = 100;
    // 100 bytes of padding (tag body), then 10 bytes of garbage (no valid frame)
    // Total: 110 + 10 = 120 bytes

    int16_t *pcm = nullptr;
    int samples = 0, channels = 0, sample_rate = 0;
    int rc = mp3_decode_file(dec, id3, sizeof(id3), &pcm, &samples, &channels, &sample_rate);
    // Should fail because there's no valid MP3 frame after the ID3 tag
    EXPECT_EQ(rc, -1);

    mp3_decoder_free(dec);
}

TEST(Mp3DecodeTest, SyntheticFrameHeader) {
    // Construct a minimal valid MPEG1 Layer III frame header
    // and verify the parser extracts correct fields.
    // This tests the header parsing logic without needing real MP3 data.

    // MPEG1, Layer III, 128kbps, 44100Hz, stereo, no padding
    // Byte 0: 0xFF (sync)
    // Byte 1: 0xFB = 1111 1011 (sync=111, version=11=MPEG1, layer=01=III, CRC=1=no)
    // Byte 2: 0x90 = 1001 0000 (bitrate=1001=128k, srate=00=44100, pad=0, private=0)
    // Byte 3: 0x00 = 0000 0000 (mode=00=stereo, mode_ext=00, copy=0, orig=0, emph=00)
    uint8_t hdr[4] = {0xFF, 0xFB, 0x90, 0x00};

    // We can't call the internal parse function directly from here,
    // but we can verify the decoder doesn't crash on a truncated file
    // with just a valid header.
    mp3_decoder_t *dec = mp3_decoder_new();
    ASSERT_TRUE(dec != nullptr);

    int16_t *pcm = nullptr;
    int samples = 0, channels = 0, sample_rate = 0;
    // Only 4 bytes — header valid but no frame body. Should fail gracefully.
    int rc = mp3_decode_file(dec, hdr, sizeof(hdr), &pcm, &samples, &channels, &sample_rate);
    EXPECT_EQ(rc, -1);

    mp3_decoder_free(dec);
}

TEST(Mp3DecodeTest, StreamRejectsMissingFile) {
    mp3_stream_t *stream = mp3_stream_open("/tmp/zanna_missing_test_stream.mp3");
    EXPECT_EQ(stream, nullptr);
}

TEST(Mp3DecodeTest, StreamRejectsGarbageFile) {
    const char *path = "/tmp/zanna_test_garbage_stream.mp3";
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0xFA, 0xCE, 0xBE, 0xEF};
    ASSERT_TRUE(write_temp_file(path, garbage, sizeof(garbage)));

    mp3_stream_t *stream = mp3_stream_open(path);
    EXPECT_EQ(stream, nullptr);

    remove(path);
}

TEST(Mp3DecodeTest, StreamRejectsId3OnlyFile) {
    const char *path = "/tmp/zanna_test_id3_only_stream.mp3";
    uint8_t id3[120];
    memset(id3, 0, sizeof(id3));
    id3[0] = 'I';
    id3[1] = 'D';
    id3[2] = '3';
    id3[3] = 3;
    id3[9] = 100;
    ASSERT_TRUE(write_temp_file(path, id3, sizeof(id3)));

    mp3_stream_t *stream = mp3_stream_open(path);
    EXPECT_EQ(stream, nullptr);

    remove(path);
}

int main() {
    return zanna_test::run_all_tests();
}

//===----------------------------------------------------------------------===//
// Conformance against real LAME-encoded streams (plan 81 / ZB-37).
//
// Every ISO Huffman codebook was missing or wrong before this ledger entry,
// the synthesis window carried sign errors, and scfsi / frequency inversion /
// the short-block reorder were absent — no real MP3 decoded correctly. These
// tests decode embedded LAME streams covering MPEG-1, MPEG-2 LSF and MPEG-2.5,
// mono and joint stereo, long / short / stop blocks, and verify the tones
// they carry.
//===----------------------------------------------------------------------===//

#include "common/Mp3Fixtures.hpp"

#include <cmath>
#include <vector>

namespace {

/// @brief Goertzel amplitude of @p freq over @p count frames of one channel.
double toneAmplitude(const int16_t *pcm,
                     int channels,
                     int channel,
                     int sampleRate,
                     double freq,
                     int start,
                     int count) {
    const double w = 2.0 * M_PI * freq / sampleRate;
    double re = 0.0;
    double im = 0.0;
    for (int k = 0; k < count; k++) {
        const double v = pcm[(start + k) * channels + channel];
        re += v * std::cos(w * k);
        im -= v * std::sin(w * k);
    }
    return 2.0 * std::sqrt(re * re + im * im) / count;
}

struct DecodedFixture {
    int16_t *pcm = nullptr;
    int samples = 0;
    int channels = 0;
    int sampleRate = 0;
    mp3_decoder_t *dec = nullptr;

    ~DecodedFixture() {
        free(pcm);
        if (dec)
            mp3_decoder_free(dec);
    }
};

bool decodeFixture(const uint8_t *data, size_t size, DecodedFixture &out) {
    out.dec = mp3_decoder_new();
    if (!out.dec)
        return false;
    return mp3_decode_file(
               out.dec, data, size, &out.pcm, &out.samples, &out.channels, &out.sampleRate) == 0 &&
           out.pcm != nullptr;
}

} // namespace

TEST(Mp3DecodeTest, HuffmanTablesAreCompleteIsoCodes) {
    // 0 = every pair table and both count1 quad tables are complete prefix
    // codes over exactly the value square the standard defines.
    EXPECT_EQ(mp3_huffman_self_check(), 0);
}

TEST(Mp3DecodeTest, DecodesMpeg1StereoToneExactly) {
    using namespace zanna_test_mp3;
    DecodedFixture d;
    ASSERT_TRUE(decodeFixture(kMp3ToneMpeg1Stereo48k, kMp3ToneMpeg1Stereo48kSize, d));
    EXPECT_EQ(d.sampleRate, 48000);
    EXPECT_EQ(d.channels, 2);
    // 44 frames x 1152 (the LAME Info frame decodes as silence).
    EXPECT_EQ(d.samples, 50688);

    const int start = 15000;
    const int count = 24000;
    const double l440 = toneAmplitude(d.pcm, 2, 0, 48000, 440.0, start, count);
    const double l1320 = toneAmplitude(d.pcm, 2, 0, 48000, 1320.0, start, count);
    const double l880 = toneAmplitude(d.pcm, 2, 0, 48000, 880.0, start, count);
    const double r880 = toneAmplitude(d.pcm, 2, 1, 48000, 880.0, start, count);
    const double r3000 = toneAmplitude(d.pcm, 2, 1, 48000, 3000.0, start, count);
    const double r440 = toneAmplitude(d.pcm, 2, 1, 48000, 440.0, start, count);
    // Encoded amplitudes: L 12000 + 6000, R 12000 + 4000.
    EXPECT_NEAR(l440, 12000.0, 1500.0);
    EXPECT_NEAR(l1320, 6000.0, 900.0);
    EXPECT_NEAR(r880, 12000.0, 1500.0);
    EXPECT_NEAR(r3000, 4000.0, 800.0);
    // Channel separation: the other channel's tone stays 20 dB down.
    EXPECT_LT(l880, 1200.0);
    EXPECT_LT(r440, 1200.0);
}

TEST(Mp3DecodeTest, DecodesMpeg2LsfJointStereo) {
    using namespace zanna_test_mp3;
    DecodedFixture d;
    ASSERT_TRUE(decodeFixture(kMp3ToneMpeg2Joint24k, kMp3ToneMpeg2Joint24kSize, d));
    EXPECT_EQ(d.sampleRate, 24000);
    EXPECT_EQ(d.channels, 2);
    EXPECT_EQ(d.samples, 25344);

    const int start = 6000;
    const int count = 12000;
    const double l440 = toneAmplitude(d.pcm, 2, 0, 24000, 440.0, start, count);
    const double r880 = toneAmplitude(d.pcm, 2, 1, 24000, 880.0, start, count);
    const double l880 = toneAmplitude(d.pcm, 2, 0, 24000, 880.0, start, count);
    const double r440 = toneAmplitude(d.pcm, 2, 1, 24000, 440.0, start, count);
    EXPECT_NEAR(l440, 12000.0, 1800.0);
    EXPECT_NEAR(r880, 12000.0, 1800.0);
    // M/S decoding restores separation; a broken stereo stage leaks the mix.
    EXPECT_LT(l880, 1500.0);
    EXPECT_LT(r440, 1500.0);
}

TEST(Mp3DecodeTest, DecodesMpeg25JointStereoWithShortBlocks) {
    using namespace zanna_test_mp3;
    DecodedFixture d;
    ASSERT_TRUE(decodeFixture(kMp3ToneMpeg25Joint11k, kMp3ToneMpeg25Joint11kSize, d));
    EXPECT_EQ(d.sampleRate, 11025);
    EXPECT_EQ(d.channels, 2);
    EXPECT_EQ(d.samples, 12672);

    // The early frames switch to short / stop blocks: analyse the whole body
    // so a wrong band table (the MPEG-2.5 11025 Hz convention) shows up.
    const int start = 1200;
    const int count = 10000;
    const double l440 = toneAmplitude(d.pcm, 2, 0, 11025, 440.0, start, count);
    const double r880 = toneAmplitude(d.pcm, 2, 1, 11025, 880.0, start, count);
    const double l880 = toneAmplitude(d.pcm, 2, 0, 11025, 880.0, start, count);
    const double r440 = toneAmplitude(d.pcm, 2, 1, 11025, 440.0, start, count);
    EXPECT_GT(l440, 8000.0);
    EXPECT_GT(r880, 8000.0);
    EXPECT_LT(l880, 2500.0);
    EXPECT_LT(r440, 2500.0);
}

TEST(Mp3DecodeTest, DecodesMpeg25Mono) {
    using namespace zanna_test_mp3;
    DecodedFixture d;
    ASSERT_TRUE(decodeFixture(kMp3ToneMpeg25Mono8k, kMp3ToneMpeg25Mono8kSize, d));
    EXPECT_EQ(d.sampleRate, 8000);
    EXPECT_EQ(d.channels, 1);
    EXPECT_EQ(d.samples, 9216);
    const double m440 = toneAmplitude(d.pcm, 1, 0, 8000, 440.0, 1200, 7000);
    const double m880 = toneAmplitude(d.pcm, 1, 0, 8000, 880.0, 1200, 7000);
    const double m2500 = toneAmplitude(d.pcm, 1, 0, 8000, 2500.0, 1200, 7000);
    // The mono mix carries both fundamentals at ~6000; 2500 Hz is silence.
    EXPECT_GT(m440, 4000.0);
    EXPECT_GT(m880, 4000.0);
    EXPECT_LT(m2500, 800.0);
}

TEST(Mp3DecodeTest, StreamDecodesEveryFrameWithoutErrors) {
    using namespace zanna_test_mp3;
    const char *path = "/tmp/zanna_test_mp3_tone_stream.mp3";
    ASSERT_TRUE(write_temp_file(path, kMp3ToneMpeg1Stereo48k, kMp3ToneMpeg1Stereo48kSize));
    mp3_stream_t *stream = mp3_stream_open(path);
    ASSERT_TRUE(stream != nullptr);
    EXPECT_EQ(mp3_stream_sample_rate(stream), 48000);
    EXPECT_EQ(mp3_stream_channels(stream), 2);
    EXPECT_EQ(mp3_stream_total_samples(stream), 50688);

    int frames = 0;
    int errors = 0;
    long total = 0;
    for (;;) {
        int16_t *pcm = nullptr;
        int rc = mp3_stream_decode_frame(stream, &pcm);
        if (rc == 0)
            break;
        if (rc < 0) {
            errors++;
            if (errors > 8)
                break;
            continue;
        }
        EXPECT_EQ(rc, 1152);
        EXPECT_TRUE(pcm != nullptr);
        frames++;
        total += rc;
    }
    // Before the codebook fix every real stream stopped after the Info
    // frame with -1 ("unsupported Huffman table").
    EXPECT_EQ(errors, 0);
    EXPECT_EQ(frames, 44);
    EXPECT_EQ(total, 50688L);

    mp3_stream_rewind(stream);
    int16_t *first = nullptr;
    EXPECT_EQ(mp3_stream_decode_frame(stream, &first), 1152);
    mp3_stream_free(stream);
    remove(path);
}
