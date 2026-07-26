//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgPNG.cpp
// Purpose: PNG read/write and bilinear image resize. Ported from
//          src/runtime/graphics/rt_pixels.c with GC dependencies removed.
//
// Key invariants:
//   - Reader supports 8-bit grayscale, palette, RGB, grayscale-alpha, and RGBA
//     (color_type=0,2,3,4,6), tRNS transparency, and Adam7 interlace.
//   - Reader handles all 5 PNG filter types (None, Sub, Up, Average, Paeth).
//   - Writer always uses RGBA (color_type=6) with filter=0.
//   - Zlib framing: CMF=0x78, FLG=0x01, + DEFLATE + Adler-32.
//   - CRC-32 per chunk uses rt_crc32_compute.
//
// Ownership/Lifetime:
//   - All returned images own their pixel data.
//
// Links: rt_pixels.c (original), PkgDeflate.hpp, rt_crc32.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounded PNG decoding, RGBA encoding, and bilinear resizing.
/// @details Validates chunk order/checksums and zlib framing, supports the five
///          filters and Adam7, and normalizes decoded pixels to owned RGBA storage.

#include "PkgPNG.hpp"
#include "PkgDeflate.hpp"
#include "PkgUtils.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>

extern "C" {
uint32_t rt_crc32_compute(const uint8_t *data, size_t len);
}

namespace zanna::pkg {

//=============================================================================
// PNG Constants
//=============================================================================

static const uint8_t kPNGSignature[8] = {137, 80, 78, 71, 13, 10, 26, 10};

//=============================================================================
// Helpers
//=============================================================================

/// @brief Read a big-endian uint32_t from an arbitrary (possibly unaligned) byte pointer.
/// PNG uses network byte order (big-endian) for all multi-byte fields.
/// @param p Address of at least four readable bytes.
/// @return Decoded 32-bit value.
static uint32_t readBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

/// @brief Write a big-endian uint32_t to an arbitrary (possibly unaligned) byte pointer.
/// @param p Address of at least four writable bytes.
/// @param v Value to encode.
static void writeBE32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>((v >> 24) & 0xFF);
    p[1] = static_cast<uint8_t>((v >> 16) & 0xFF);
    p[2] = static_cast<uint8_t>((v >> 8) & 0xFF);
    p[3] = static_cast<uint8_t>(v & 0xFF);
}

/// @brief PNG CRC-32 over type + data bytes
/// @param data Chunk type followed by chunk payload bytes.
/// @param len Number of bytes included in the checksum.
/// @return PNG CRC-32 value.
static uint32_t pngCRC(const uint8_t *data, size_t len) {
    return rt_crc32_compute(data, len);
}

/// @brief Paeth predictor (RFC 2083)
/// @param a Reconstructed byte immediately to the left.
/// @param b Reconstructed byte immediately above.
/// @param c Reconstructed byte diagonally above-left.
/// @return Neighbor closest to the linear predictor `a + b - c`.
static uint8_t paethPredict(uint8_t a, uint8_t b, uint8_t c) {
    int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
    int pa = p > static_cast<int>(a) ? p - static_cast<int>(a) : static_cast<int>(a) - p;
    int pb = p > static_cast<int>(b) ? p - static_cast<int>(b) : static_cast<int>(b) - p;
    int pc = p > static_cast<int>(c) ? p - static_cast<int>(c) : static_cast<int>(c) - p;
    if (pa <= pb && pa <= pc)
        return a;
    if (pb <= pc)
        return b;
    return c;
}

/// @brief Compute Adler-32 checksum
/// @param data Input bytes.
/// @param len Input length.
/// @return Adler-32 checksum used by zlib framing.
static uint32_t adler32(const uint8_t *data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

static constexpr size_t kMaxDecodedPngBytes = 256u * 1024u * 1024u;

/// @brief Return the exact decompressed scanline byte count for an 8-bit PNG image.
/// @details Includes each scanline's leading filter byte. Handles both non-interlaced and Adam7
/// interlaced layouts and rejects any size that would exceed the package image cap.
/// @param width Image width in pixels.
/// @param height Image height in pixels.
/// @param channels Decoded source channels per pixel.
/// @param interlaceMethod Zero for sequential rows or one for Adam7 passes.
/// @return Exact raw zlib payload length expected after decompression.
/// @throws PNGError If dimensions overflow or exceed the 256 MiB image cap.
static size_t expectedPngRawBytes(uint32_t width,
                                  uint32_t height,
                                  int channels,
                                  uint8_t interlaceMethod) {
    /// @brief Compute one filtered PNG row's checked byte count.
    /// @param rowWidth Row width in pixels.
    /// @param rowChannels Decoded channels per pixel.
    /// @return Pixel bytes plus the leading filter byte.
    /// @throws PNGError If the row exceeds the decoded-image budget.
    auto checkedRowBytes = [](uint32_t rowWidth, int rowChannels) -> size_t {
        const size_t pixelBytes = static_cast<size_t>(rowWidth) * static_cast<size_t>(rowChannels);
        if (pixelBytes > kMaxDecodedPngBytes - 1)
            throw PNGError("PNG: image data is too large");
        return pixelBytes + 1;
    };

    if (interlaceMethod == 0) {
        const size_t rowBytes = checkedRowBytes(width, channels);
        if (height > kMaxDecodedPngBytes / rowBytes)
            throw PNGError("PNG: image data is too large");
        return rowBytes * height;
    }

    static constexpr uint32_t xStart[7] = {0, 4, 0, 2, 0, 1, 0};
    static constexpr uint32_t yStart[7] = {0, 0, 4, 0, 2, 0, 1};
    static constexpr uint32_t xStep[7] = {8, 8, 4, 4, 2, 2, 1};
    static constexpr uint32_t yStep[7] = {8, 8, 8, 4, 4, 2, 2};

    size_t total = 0;
    for (int pass = 0; pass < 7; ++pass) {
        if (xStart[pass] >= width || yStart[pass] >= height)
            continue;
        const uint32_t passWidth = (width - xStart[pass] + xStep[pass] - 1) / xStep[pass];
        const uint32_t passHeight = (height - yStart[pass] + yStep[pass] - 1) / yStep[pass];
        if (passWidth == 0 || passHeight == 0)
            continue;
        const size_t rowBytes = checkedRowBytes(passWidth, channels);
        if (passHeight > (kMaxDecodedPngBytes - total) / rowBytes)
            throw PNGError("PNG: image data is too large");
        total += rowBytes * passHeight;
    }
    return total;
}

//=============================================================================
// PNG Reader
//=============================================================================

/// @brief Decode a PNG from a byte array. Validates the signature, iterates all chunks
/// (verifying CRC-32 per chunk), decompresses the IDAT zlib stream, unfilters
/// scanlines with all five PNG filter types, handles both non-interlaced and
/// Adam7-interlaced images, and converts any supported color type (grayscale,
/// palette, RGB, grayscale-alpha, RGBA) to a packed RGBA output buffer.
/// @param data Borrowed complete PNG file bytes.
/// @param len Input length.
/// @return Decoded dimensions and owned row-major RGBA pixels.
/// @throws PNGError If framing, chunks, checksums, zlib data, filters, palette,
///         transparency, dimensions, or pixel data are invalid.
PkgImage pngReadMemory(const uint8_t *data, size_t len) {
    if (!data)
        throw PNGError("PNG: null input buffer");
    if (len < 8 || std::memcmp(data, kPNGSignature, 8) != 0)
        throw PNGError("PNG: invalid signature");

    uint32_t width = 0, height = 0;
    uint8_t colorType = 0;
    uint8_t interlaceMethod = 0;
    std::vector<uint8_t> palette;
    std::vector<uint8_t> transparency;
    std::vector<uint8_t> idatBuf;
    size_t pos = 8;
    bool seenIHDR = false;
    bool seenIEND = false;
    bool seenPLTE = false;
    bool seenTRNS = false;
    bool seenIDAT = false;

    while (pos + 12 <= len) {
        uint32_t chunkLen = readBE32(data + pos);
        const uint8_t *chunkType = data + pos + 4;
        const uint8_t *chunkData = data + pos + 8;

        if (chunkLen > len - pos - 12)
            throw PNGError("PNG: truncated chunk");

        const uint32_t storedCrc = readBE32(data + pos + 8 + chunkLen);
        const uint32_t actualCrc = pngCRC(data + pos + 4, 4 + chunkLen);
        if (storedCrc != actualCrc)
            throw PNGError("PNG: chunk CRC mismatch");

        if (std::memcmp(chunkType, "IHDR", 4) == 0) {
            if (seenIHDR)
                throw PNGError("PNG: duplicate IHDR");
            if (chunkLen != 13)
                throw PNGError("PNG: invalid IHDR length");
            width = readBE32(chunkData);
            height = readBE32(chunkData + 4);
            uint8_t bitDepth = chunkData[8];
            colorType = chunkData[9];
            uint8_t compression = chunkData[10];
            uint8_t filter = chunkData[11];
            interlaceMethod = chunkData[12];
            if (width == 0 || height == 0)
                throw PNGError("PNG: empty image");
            if (bitDepth != 8 || (colorType != 0 && colorType != 2 && colorType != 3 &&
                                  colorType != 4 && colorType != 6))
                throw PNGError("PNG: unsupported format (need 8-bit grayscale, palette, RGB, "
                               "grayscale-alpha, or RGBA)");
            if (compression != 0 || filter != 0 || interlaceMethod > 1)
                throw PNGError("PNG: unsupported IHDR compression/filter/interlace method");
            seenIHDR = true;
        } else if (std::memcmp(chunkType, "PLTE", 4) == 0) {
            if (!seenIHDR)
                throw PNGError("PNG: PLTE before IHDR");
            if (seenIDAT)
                throw PNGError("PNG: PLTE after IDAT");
            if (seenPLTE)
                throw PNGError("PNG: duplicate PLTE");
            if (chunkLen == 0 || chunkLen % 3 != 0 || chunkLen / 3 > 256)
                throw PNGError("PNG: invalid PLTE length");
            palette.assign(chunkData, chunkData + chunkLen);
            seenPLTE = true;
        } else if (std::memcmp(chunkType, "tRNS", 4) == 0) {
            if (!seenIHDR)
                throw PNGError("PNG: tRNS before IHDR");
            if (seenIDAT)
                throw PNGError("PNG: tRNS after IDAT");
            if (seenTRNS)
                throw PNGError("PNG: duplicate tRNS");
            if (colorType == 3 && !seenPLTE)
                throw PNGError("PNG: indexed tRNS before PLTE");
            transparency.assign(chunkData, chunkData + chunkLen);
            seenTRNS = true;
        } else if (std::memcmp(chunkType, "IDAT", 4) == 0) {
            if (!seenIHDR)
                throw PNGError("PNG: IDAT before IHDR");
            if (chunkLen > kMaxDecodedPngBytes ||
                idatBuf.size() > kMaxDecodedPngBytes - static_cast<size_t>(chunkLen)) {
                throw PNGError("PNG: compressed image data is too large");
            }
            idatBuf.insert(idatBuf.end(), chunkData, chunkData + chunkLen);
            seenIDAT = true;
        } else if (std::memcmp(chunkType, "IEND", 4) == 0) {
            if (chunkLen != 0)
                throw PNGError("PNG: invalid IEND length");
            seenIEND = true;
            pos += 12 + chunkLen;
            break;
        }

        pos += 12 + chunkLen;
    }

    if (!seenIHDR || idatBuf.size() < 6)
        throw PNGError("PNG: missing IHDR or IDAT");
    if (!seenIEND)
        throw PNGError("PNG: missing IEND");
    if (pos != len)
        throw PNGError("PNG: trailing data after IEND");

    if (colorType == 3 && palette.empty())
        throw PNGError("PNG: indexed-color image is missing PLTE");
    if (colorType == 3 && !transparency.empty() && transparency.size() > palette.size() / 3)
        throw PNGError("PNG: tRNS palette alpha table is longer than PLTE");
    if (colorType == 0 && !transparency.empty() && transparency.size() != 2)
        throw PNGError("PNG: invalid grayscale tRNS length");
    if (colorType == 0 && !transparency.empty() && transparency[0] != 0)
        throw PNGError("PNG: grayscale tRNS value is out of range for 8-bit PNG");
    if (colorType == 2 && !transparency.empty() && transparency.size() != 6)
        throw PNGError("PNG: invalid RGB tRNS length");
    if (colorType == 2 && !transparency.empty() &&
        (transparency[0] != 0 || transparency[2] != 0 || transparency[4] != 0)) {
        throw PNGError("PNG: RGB tRNS value is out of range for 8-bit PNG");
    }
    if ((colorType == 4 || colorType == 6) && !transparency.empty())
        throw PNGError("PNG: tRNS is not allowed for images with alpha");

    int channels = 0;
    switch (colorType) {
        case 0:
        case 3:
            channels = 1;
            break;
        case 2:
            channels = 3;
            break;
        case 4:
            channels = 2;
            break;
        case 6:
            channels = 4;
            break;
        default:
            throw PNGError("PNG: unsupported color type");
    }
    const size_t expectedRawBytes = expectedPngRawBytes(width, height, channels, interlaceMethod);

    const uint8_t cmf = idatBuf[0];
    const uint8_t flg = idatBuf[1];
    if ((cmf & 0x0F) != 8 || (cmf >> 4) > 7 ||
        (((static_cast<uint16_t>(cmf) << 8) | flg) % 31) != 0)
        throw PNGError("PNG: invalid zlib header");
    if ((flg & 0x20) != 0)
        throw PNGError("PNG: zlib preset dictionaries are not supported");

    size_t deflateLen = idatBuf.size() - 2 - 4;
    auto raw = inflate(idatBuf.data() + 2, deflateLen, expectedRawBytes);
    if (raw.size() != expectedRawBytes)
        throw PNGError("PNG: decompressed data size mismatch");
    const uint32_t expectedAdler = readBE32(idatBuf.data() + idatBuf.size() - 4);
    const uint32_t actualAdler = adler32(raw.data(), raw.size());
    if (expectedAdler != actualAdler)
        throw PNGError("PNG: Adler-32 mismatch");

    if (width > kMaxDecodedPngBytes / static_cast<size_t>(channels))
        throw PNGError("PNG: image dimensions are too large");
    const size_t stride = static_cast<size_t>(width) * channels;
    if (stride == 0 || height > kMaxDecodedPngBytes / stride)
        throw PNGError("PNG: image data is too large");
    std::vector<uint8_t> img(stride * height);

    /// @brief Reverse one PNG scanline filter into decoded pixel bytes.
    /// @param filter PNG filter method numbered zero through four.
    /// @param src Filtered source bytes for the row.
    /// @param dst Destination row receiving reconstructed bytes.
    /// @param prev Previous decoded row, or `nullptr` for the first row.
    /// @param rowBytes Number of bytes to reconstruct.
    /// @throws PNGError If `filter` is not a supported PNG filter method.
    auto unfilterRow = [&](uint8_t filter,
                           const uint8_t *src,
                           uint8_t *dst,
                           const uint8_t *prev,
                           size_t rowBytes) {
        for (size_t i = 0; i < rowBytes; i++) {
            uint8_t rawByte = src[i];
            uint8_t a = (i >= static_cast<size_t>(channels)) ? dst[i - channels] : 0;
            uint8_t b = prev ? prev[i] : 0;
            uint8_t c = (prev && i >= static_cast<size_t>(channels)) ? prev[i - channels] : 0;

            switch (filter) {
                case 0:
                    dst[i] = rawByte;
                    break;
                case 1:
                    dst[i] = rawByte + a;
                    break;
                case 2:
                    dst[i] = rawByte + b;
                    break;
                case 3:
                    dst[i] = rawByte +
                             static_cast<uint8_t>((static_cast<int>(a) + static_cast<int>(b)) / 2);
                    break;
                case 4:
                    dst[i] = rawByte + paethPredict(a, b, c);
                    break;
                default:
                    throw PNGError("PNG: unknown filter type");
            }
        }
    };

    if (interlaceMethod == 0) {
        if (stride + 1 > kMaxDecodedPngBytes || height > kMaxDecodedPngBytes / (stride + 1))
            throw PNGError("PNG: image data is too large");
        for (uint32_t y = 0; y < height; y++) {
            uint8_t filter = raw[y * (stride + 1)];
            const uint8_t *src = raw.data() + y * (stride + 1) + 1;
            uint8_t *dst = img.data() + y * stride;
            const uint8_t *prev = (y > 0) ? img.data() + (y - 1) * stride : nullptr;
            unfilterRow(filter, src, dst, prev, stride);
        }
    } else {
        static constexpr uint32_t xStart[7] = {0, 4, 0, 2, 0, 1, 0};
        static constexpr uint32_t yStart[7] = {0, 0, 4, 0, 2, 0, 1};
        static constexpr uint32_t xStep[7] = {8, 8, 4, 4, 2, 2, 1};
        static constexpr uint32_t yStep[7] = {8, 8, 8, 4, 4, 2, 2};
        size_t rawPos = 0;
        for (int pass = 0; pass < 7; ++pass) {
            if (xStart[pass] >= width || yStart[pass] >= height)
                continue;
            const uint32_t passWidth = (width - xStart[pass] + xStep[pass] - 1) / xStep[pass];
            const uint32_t passHeight = (height - yStart[pass] + yStep[pass] - 1) / yStep[pass];
            if (passWidth == 0 || passHeight == 0)
                continue;
            const size_t passStride = static_cast<size_t>(passWidth) * channels;
            std::vector<uint8_t> prevPassRow(passStride, 0);
            std::vector<uint8_t> curPassRow(passStride, 0);
            for (uint32_t row = 0; row < passHeight; ++row) {
                if (rawPos + 1 + passStride > raw.size())
                    throw PNGError("PNG: interlaced data truncated");
                const uint8_t filter = raw[rawPos++];
                const uint8_t *src = raw.data() + rawPos;
                rawPos += passStride;
                std::fill(curPassRow.begin(), curPassRow.end(), 0);
                const uint8_t *prev = row == 0 ? nullptr : prevPassRow.data();
                unfilterRow(filter, src, curPassRow.data(), prev, passStride);
                const uint32_t y = yStart[pass] + row * yStep[pass];
                for (uint32_t col = 0; col < passWidth; ++col) {
                    const uint32_t x = xStart[pass] + col * xStep[pass];
                    std::memcpy(img.data() + (static_cast<size_t>(y) * width + x) * channels,
                                curPassRow.data() + static_cast<size_t>(col) * channels,
                                static_cast<size_t>(channels));
                }
                prevPassRow.swap(curPassRow);
            }
        }
        if (rawPos != raw.size())
            throw PNGError("PNG: trailing interlaced image data");
    }

    PkgImage result;
    result.width = width;
    result.height = height;
    if (width > kMaxDecodedPngBytes / 4 ||
        height > kMaxDecodedPngBytes / (static_cast<size_t>(width) * 4))
        throw PNGError("PNG: image dimensions are too large");
    result.pixels.resize(width * height * 4);

    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t *px = img.data() + (static_cast<size_t>(y) * stride) + x * channels;
            uint8_t *dst = result.pixels.data() + (static_cast<size_t>(y) * width + x) * 4;
            switch (colorType) {
                case 0:
                    dst[0] = px[0];
                    dst[1] = px[0];
                    dst[2] = px[0];
                    dst[3] = (!transparency.empty() && px[0] == transparency[1]) ? 0 : 0xFF;
                    break;
                case 2:
                    dst[0] = px[0];
                    dst[1] = px[1];
                    dst[2] = px[2];
                    dst[3] = (!transparency.empty() && px[0] == transparency[1] &&
                              px[1] == transparency[3] && px[2] == transparency[5])
                                 ? 0
                                 : 0xFF;
                    break;
                case 3: {
                    const uint8_t index = px[0];
                    if (static_cast<size_t>(index) >= palette.size() / 3)
                        throw PNGError("PNG: palette index out of range");
                    dst[0] = palette[static_cast<size_t>(index) * 3];
                    dst[1] = palette[static_cast<size_t>(index) * 3 + 1];
                    dst[2] = palette[static_cast<size_t>(index) * 3 + 2];
                    dst[3] = static_cast<size_t>(index) < transparency.size() ? transparency[index]
                                                                              : 0xFF;
                    break;
                }
                case 4:
                    dst[0] = px[0];
                    dst[1] = px[0];
                    dst[2] = px[0];
                    dst[3] = px[1];
                    break;
                case 6:
                    dst[0] = px[0];
                    dst[1] = px[1];
                    dst[2] = px[2];
                    dst[3] = px[3];
                    break;
            }
        }
    }

    return result;
}

/// @brief Load a PNG file from disk and decode it via pngReadMemory.
/// Rethrows PNGError as-is; wraps other I/O exceptions in a PNGError.
/// @param path Input filesystem path.
/// @return Decoded dimensions and owned RGBA pixels.
/// @throws PNGError If file reading or PNG decoding fails.
PkgImage pngRead(const std::string &path) {
    try {
        auto data = readFile(path);
        return pngReadMemory(data.data(), data.size());
    } catch (const PNGError &) {
        throw;
    } catch (const std::exception &ex) {
        throw PNGError(std::string("PNG: ") + ex.what());
    }
}

//=============================================================================
// PNG Writer
//=============================================================================

/// @brief Write a PNG chunk to a buffer.
/// @param buf Destination PNG buffer.
/// @param type Four-byte ASCII chunk type.
/// @param data Optional chunk payload.
/// @param len Payload length.
/// @throws PNGError If `len` exceeds the 32-bit chunk length field.
static void writeChunk(std::vector<uint8_t> &buf,
                       const char *type,
                       const uint8_t *data,
                       size_t len) {
    if (len > std::numeric_limits<uint32_t>::max())
        throw PNGError("PNG: chunk too large");

    // Length (big-endian)
    uint8_t lenBuf[4];
    writeBE32(lenBuf, static_cast<uint32_t>(len));
    buf.insert(buf.end(), lenBuf, lenBuf + 4);

    // Type + data (used for CRC calculation)
    size_t tdStart = buf.size();
    buf.insert(buf.end(),
               reinterpret_cast<const uint8_t *>(type),
               reinterpret_cast<const uint8_t *>(type) + 4);
    if (len > 0)
        buf.insert(buf.end(), data, data + len);

    // CRC over type + data
    uint32_t crc = pngCRC(buf.data() + tdStart, 4 + len);
    uint8_t crcBuf[4];
    writeBE32(crcBuf, crc);
    buf.insert(buf.end(), crcBuf, crcBuf + 4);
}

/// @brief Encode a PkgImage as a PNG byte stream. Always writes RGBA (color_type=6) with
/// filter=None on every scanline, then wraps the DEFLATE output in a zlib envelope
/// (CMF=0x78, FLG=0x01, Adler-32) and emits IHDR/IDAT/IEND chunks.
/// @param img Source dimensions and row-major RGBA pixels.
/// @return Complete PNG file bytes.
/// @throws PNGError If dimensions or pixel storage are invalid or exceed limits.
/// @throws DeflateError If IDAT compression fails.
std::vector<uint8_t> pngEncode(const PkgImage &img) {
    if (img.width == 0 || img.height == 0)
        throw PNGError("PNG: empty image");
    if (img.width > kMaxDecodedPngBytes / 4 ||
        img.height > kMaxDecodedPngBytes / (static_cast<size_t>(img.width) * 4))
        throw PNGError("PNG: image dimensions are too large");
    const size_t pixelBytes = static_cast<size_t>(img.width) * img.height * 4;
    if (img.pixels.size() != pixelBytes)
        throw PNGError("PNG: RGBA pixel buffer size does not match dimensions");

    std::vector<uint8_t> result;
    result.reserve(pixelBytes + 1024);

    // PNG signature
    result.insert(result.end(), kPNGSignature, kPNGSignature + 8);

    // IHDR
    uint8_t ihdr[13];
    writeBE32(ihdr, img.width);
    writeBE32(ihdr + 4, img.height);
    ihdr[8] = 8;  // bit depth
    ihdr[9] = 6;  // color type RGBA
    ihdr[10] = 0; // compression
    ihdr[11] = 0; // filter
    ihdr[12] = 0; // interlace
    writeChunk(result, "IHDR", ihdr, 13);

    // Build raw scanlines with filter=0
    size_t stride = static_cast<size_t>(img.width) * 4;
    size_t rawLen = (stride + 1) * img.height;
    std::vector<uint8_t> raw(rawLen);

    for (uint32_t y = 0; y < img.height; y++) {
        raw[y * (stride + 1)] = 0; // Filter: None
        std::memcpy(raw.data() + y * (stride + 1) + 1, img.pixels.data() + y * stride, stride);
    }

    // Compress with DEFLATE
    auto deflated = deflate(raw.data(), rawLen);

    // Wrap in zlib stream: CMF + FLG + DEFLATE + Adler-32
    size_t zlibLen = 2 + deflated.size() + 4;
    std::vector<uint8_t> zlibData(zlibLen);
    zlibData[0] = 0x78; // CMF: deflate, window=32K
    zlibData[1] = 0x01; // FLG: no dict, check bits
    std::memcpy(zlibData.data() + 2, deflated.data(), deflated.size());

    uint32_t adler = adler32(raw.data(), rawLen);
    writeBE32(zlibData.data() + 2 + deflated.size(), adler);

    // IDAT
    writeChunk(result, "IDAT", zlibData.data(), zlibLen);

    // IEND
    writeChunk(result, "IEND", nullptr, 0);

    return result;
}

/// @brief Encode img as PNG via pngEncode and write the result to a file.
/// Throws PNGError if the file cannot be created or the write fails.
/// @param path Destination filesystem path.
/// @param img Source dimensions and RGBA pixels.
/// @throws PNGError If encoding or the atomic write fails.
void pngWrite(const std::string &path, const PkgImage &img) {
    try {
        writeFileAtomic(path, pngEncode(img));
    } catch (const std::exception &error) {
        throw PNGError(std::string("PNG: ") + error.what());
    }
}

//=============================================================================
// Bilinear Image Resize
//=============================================================================

/// @brief Resize an RGBA image to newWidth×newHeight using bilinear interpolation.
/// Each output channel is computed as a weighted blend of the four nearest
/// source pixels (top-left, top-right, bottom-left, bottom-right), with
/// 8-bit fractional coordinates scaled by 256 to avoid floating-point math.
/// Edge pixels clamp rather than wrap.
/// @param src Source dimensions and row-major RGBA pixels.
/// @param newWidth Requested output width; zero is normalized to one.
/// @param newHeight Requested output height; zero is normalized to one.
/// @return Resized owned RGBA image, or transparent pixels for an empty source.
/// @throws PNGError If source storage is inconsistent or output dimensions exceed limits.
PkgImage imageResize(const PkgImage &src, uint32_t newWidth, uint32_t newHeight) {
    if (newWidth == 0)
        newWidth = 1;
    if (newHeight == 0)
        newHeight = 1;

    PkgImage result;
    result.width = newWidth;
    result.height = newHeight;
    if (newWidth > kMaxDecodedPngBytes / 4 ||
        newHeight > kMaxDecodedPngBytes / (static_cast<size_t>(newWidth) * 4))
        throw PNGError("PNG: resized image dimensions are too large");
    result.pixels.resize(static_cast<size_t>(newWidth) * newHeight * 4);

    if (src.width == 0 || src.height == 0) {
        std::memset(result.pixels.data(), 0, result.pixels.size());
        return result;
    }
    if (src.width > kMaxDecodedPngBytes / 4 ||
        src.height > kMaxDecodedPngBytes / (static_cast<size_t>(src.width) * 4) ||
        src.pixels.size() != static_cast<size_t>(src.width) * src.height * 4)
        throw PNGError("PNG: source RGBA pixel buffer size does not match dimensions");

    for (uint32_t y = 0; y < newHeight; y++) {
        // Map dest y to source y with 8-bit fractional part
        int64_t srcY256 = (static_cast<int64_t>(y) * src.height * 256) / newHeight;
        int64_t srcY = srcY256 >> 8;
        int64_t fracY = srcY256 & 0xFF;

        if (srcY >= src.height)
            srcY = src.height - 1;
        if (srcY < 0)
            srcY = 0;
        int64_t sy1 = (srcY + 1 < src.height) ? srcY + 1 : srcY;
        if (srcY >= static_cast<int64_t>(src.height) - 1)
            fracY = 255;

        for (uint32_t x = 0; x < newWidth; x++) {
            int64_t srcX256 = (static_cast<int64_t>(x) * src.width * 256) / newWidth;
            int64_t srcX = srcX256 >> 8;
            int64_t fracX = srcX256 & 0xFF;

            if (srcX >= src.width)
                srcX = src.width - 1;
            if (srcX < 0)
                srcX = 0;
            int64_t sx1 = (srcX + 1 < src.width) ? srcX + 1 : srcX;
            if (srcX >= static_cast<int64_t>(src.width) - 1)
                fracX = 255;

            // Four neighboring pixels (RGBA bytes)
            const uint8_t *p00 = src.at(srcX, srcY);
            const uint8_t *p10 = src.at(sx1, srcY);
            const uint8_t *p01 = src.at(srcX, sy1);
            const uint8_t *p11 = src.at(sx1, sy1);

            int64_t invFracX = 256 - fracX;
            int64_t invFracY = 256 - fracY;

            uint8_t *dst = result.at(x, y);
            for (int ch = 0; ch < 4; ch++) {
                int64_t v = (p00[ch] * invFracX * invFracY + p10[ch] * fracX * invFracY +
                             p01[ch] * invFracX * fracY + p11[ch] * fracX * fracY) >>
                            16;
                dst[ch] = static_cast<uint8_t>(v & 0xFF);
            }
        }
    }

    return result;
}

} // namespace zanna::pkg
