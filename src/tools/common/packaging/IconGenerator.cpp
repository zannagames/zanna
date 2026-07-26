//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/IconGenerator.cpp
// Purpose: Generate ICNS, ICO, and multi-size PNG icons from a source image.
//
// Key invariants:
//   - ICNS: "icns" magic + big-endian total_size + type/size/PNG entries.
//   - ICO: ICONDIR header + ICONDIRENTRY array + PNG data blobs.
//   - All numeric fields are little-endian in ICO, big-endian in ICNS.
//   - Source image is never modified; resized copies are created for each size.
//
// Ownership/Lifetime:
//   - Pure functions returning byte vectors. No file I/O.
//
// Links: IconGenerator.hpp, PkgPNG.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements procedural and resized icon payload generation for package targets.
/// @details The implementation validates RGBA source geometry, encodes PNG-backed
///          ICNS and ICO records with the required byte order, produces hicolor
///          size maps, and supplies the built-in Zanna fallback artwork.

#include "IconGenerator.hpp"

#include <cstdlib>
#include <limits>

namespace zanna::pkg {

//=============================================================================
// ICNS Generation
//=============================================================================

namespace {

/// @brief ICNS icon type codes and their target pixel sizes.
struct IcnsTypeEntry {
    char type[5];  ///< 4-char ICNS type code (NUL-terminated for convenience)
    uint32_t size; ///< Target pixel dimension (square)
};

// Modern ICNS types that embed PNG data directly.
// Each entry is a PNG at the specified pixel size.
static const IcnsTypeEntry kIcnsTypes[] = {
    {"ic11", 32},   // 32x32   (Retina 16)
    {"ic12", 64},   // 64x64   (Retina 32)
    {"ic07", 128},  // 128x128
    {"ic13", 256},  // 256x256 (Retina 128)
    {"ic08", 256},  // 256x256
    {"ic14", 512},  // 512x512 (Retina 256)
    {"ic09", 512},  // 512x512
    {"ic10", 1024}, // 1024x1024 (Retina 512)
};

/// @brief Append a 32-bit big-endian integer to `out`. Used for all ICNS numeric fields.
/// @param out Byte buffer extended by four bytes.
/// @param val Unsigned integer to encode.
void writeBE32(std::vector<uint8_t> &out, uint32_t val) {
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(val & 0xFF));
}

/// @brief Append a 16-bit little-endian integer to `out`. Used for ICO ICONDIR and ICONDIRENTRY
/// fields.
/// @param out Byte buffer extended by two bytes.
/// @param val Unsigned integer to encode.
void writeLE16(std::vector<uint8_t> &out, uint16_t val) {
    out.push_back(static_cast<uint8_t>(val & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
}

/// @brief Append a 32-bit little-endian integer to `out`. Used for ICO SizeInBytes and FileOffset
/// fields.
/// @param out Byte buffer extended by four bytes.
/// @param val Unsigned integer to encode.
void writeLE32(std::vector<uint8_t> &out, uint32_t val) {
    out.push_back(static_cast<uint8_t>(val & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

/// @brief Standard icon sizes for Linux hicolor theme.
static const uint32_t kLinuxIconSizes[] = {16, 32, 48, 128, 256};

/// @brief Standard icon sizes for Windows ICO.
static const uint32_t kIcoSizes[] = {16, 24, 32, 48, 64, 128, 256};

/// @brief Validate that `srcImage` is a well-formed package icon source.
/// @details Throws PNGError if the image is empty, non-square, smaller than the
///          minimum useful source size, or if the pixel buffer size does not
///          match width * height * 4 bytes. Enforcing a square source prevents
///          platform icon generators from silently stretching artwork.
/// @param srcImage Decoded RGBA source image supplied by package metadata.
/// @throws PNGError when dimensions or pixel storage are invalid for icon generation.
void validateSourceImage(const PkgImage &srcImage) {
    if (srcImage.width == 0 || srcImage.height == 0)
        throw PNGError("icon: source image is empty");
    if (srcImage.width != srcImage.height)
        throw PNGError("icon: source image must be square");
    if (srcImage.width < 32)
        throw PNGError("icon: source image must be at least 32x32 pixels");
    const uint64_t pixels = static_cast<uint64_t>(srcImage.width) * srcImage.height;
    if (pixels > std::numeric_limits<size_t>::max() / 4u)
        throw PNGError("icon: source image dimensions overflow");
    const size_t expectedPixels = static_cast<size_t>(pixels) * 4u;
    if (srcImage.pixels.size() != expectedPixels)
        throw PNGError("icon: RGBA pixel buffer size does not match dimensions");
}

/// @brief Safely narrow a `size_t` byte count to `uint32_t` for icon format header fields.
/// @param value Host-sized byte count to narrow.
/// @param format Format label included in the failure message.
/// @return Representable 32-bit byte count.
/// @throws PNGError If @p value exceeds UINT32_MAX; ICNS and ICO cannot encode it.
uint32_t checkedIconSize(size_t value, const char *format) {
    if (value > std::numeric_limits<uint32_t>::max())
        throw PNGError(std::string("icon: ") + format + " entry is too large");
    return static_cast<uint32_t>(value);
}

} // namespace

/// @brief Build a macOS ICNS container embedding PNG data at each standard icon size.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return Complete modern ICNS container bytes.
/// @details Resizes the source to each @ref kIcnsTypes entry, PNG-encodes it,
///          emits type/size/data records, and patches the global total-size field.
/// @throws PNGError When validation, image processing, or size narrowing fails.
std::vector<uint8_t> generateIcns(const PkgImage &srcImage) {
    validateSourceImage(srcImage);
    std::vector<uint8_t> result;

    // ICNS header: magic "icns" (4 bytes) + total_size placeholder (4 bytes)
    result.push_back('i');
    result.push_back('c');
    result.push_back('n');
    result.push_back('s');
    // Reserve 4 bytes for total size — fill in at the end
    result.resize(8, 0);

    for (const auto &entry : kIcnsTypes) {
        // Resize to target size
        PkgImage resized = imageResize(srcImage, entry.size, entry.size);

        // Encode as PNG
        auto png = pngEncode(resized);

        // Write entry: type(4) + entry_size(4, big-endian) + PNG data
        // entry_size includes the 8-byte type+size header
        uint32_t entrySize = checkedIconSize(8 + png.size(), "ICNS");

        result.push_back(static_cast<uint8_t>(entry.type[0]));
        result.push_back(static_cast<uint8_t>(entry.type[1]));
        result.push_back(static_cast<uint8_t>(entry.type[2]));
        result.push_back(static_cast<uint8_t>(entry.type[3]));

        writeBE32(result, entrySize);

        result.insert(result.end(), png.begin(), png.end());
    }

    // Fill in total size (big-endian) at offset 4
    uint32_t totalSize = checkedIconSize(result.size(), "ICNS");
    result[4] = static_cast<uint8_t>((totalSize >> 24) & 0xFF);
    result[5] = static_cast<uint8_t>((totalSize >> 16) & 0xFF);
    result[6] = static_cast<uint8_t>((totalSize >> 8) & 0xFF);
    result[7] = static_cast<uint8_t>(totalSize & 0xFF);

    return result;
}

//=============================================================================
// ICO Generation
//=============================================================================

/// @brief Build a Windows ICO container embedding PNG data at each standard icon size.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return Complete ICO container bytes.
/// @details Generates every @ref kIcoSizes PNG, writes the ICONDIR and directory
///          records, then appends payloads in order. A dimension of 256 is encoded
///          as zero per the ICO specification.
/// @throws PNGError When validation, image processing, or 32-bit offsets fail.
std::vector<uint8_t> generateIco(const PkgImage &srcImage) {
    validateSourceImage(srcImage);

    // First, generate all the PNG blobs for sizes that fit
    struct IcoEntry {
        uint32_t size{0};
        std::vector<uint8_t> pngData;
    };

    std::vector<IcoEntry> entries;

    for (uint32_t sz : kIcoSizes) {
        PkgImage resized = imageResize(srcImage, sz, sz);
        auto png = pngEncode(resized);
        entries.push_back({sz, std::move(png)});
    }

    std::vector<uint8_t> result;

    // ICONDIR header (6 bytes):
    //   Reserved (2) = 0
    //   Type     (2) = 1 (ICO)
    //   Count    (2) = number of entries
    writeLE16(result, 0);                                     // Reserved
    writeLE16(result, 1);                                     // Type = ICO
    writeLE16(result, static_cast<uint16_t>(entries.size())); // Count

    // Calculate data offset: after ICONDIR (6) + all ICONDIRENTRYs (16 each)
    uint32_t dataOffset = 6 + static_cast<uint32_t>(entries.size()) * 16;

    // ICONDIRENTRY array (16 bytes each):
    //   Width      (1) — 0 means 256
    //   Height     (1) — 0 means 256
    //   ColorCount (1) = 0 (no palette)
    //   Reserved   (1) = 0
    //   Planes     (2) = 1
    //   BitCount   (2) = 32 (RGBA)
    //   SizeInBytes(4) = PNG data size
    //   FileOffset (4) = offset from file start to PNG data
    uint32_t currentOffset = dataOffset;
    for (const auto &e : entries) {
        // Width/Height: 0 encodes 256
        uint8_t w = (e.size >= 256) ? 0 : static_cast<uint8_t>(e.size);
        uint8_t h = w;
        result.push_back(w);                                         // Width
        result.push_back(h);                                         // Height
        result.push_back(0);                                         // ColorCount
        result.push_back(0);                                         // Reserved
        writeLE16(result, 1);                                        // Planes
        writeLE16(result, 32);                                       // BitCount (32bpp RGBA)
        writeLE32(result, checkedIconSize(e.pngData.size(), "ICO")); // Size
        writeLE32(result, currentOffset);                            // FileOffset

        const uint32_t pngSize = checkedIconSize(e.pngData.size(), "ICO");
        if (currentOffset > std::numeric_limits<uint32_t>::max() - pngSize)
            throw PNGError("icon: ICO file is too large");
        currentOffset += pngSize;
    }

    // PNG data blocks
    for (const auto &e : entries) {
        result.insert(result.end(), e.pngData.begin(), e.pngData.end());
    }

    return result;
}

/// @brief Build the dependency-free fallback Zanna toolchain icon.
/// @details Draws a high-contrast square icon with the italic Zanna "Z" mark —
///          green top bar, steel diagonal, teal base bar — on a dark
///          charcoal-green field with transparent-free pixels so every
///          platform can resize it predictably for setup programs, launchers,
///          and file association metadata.
/// @return A 256x256 RGBA image.
PkgImage defaultZannaToolchainIconImage() {
    constexpr uint32_t kSize = 256;
    PkgImage img;
    img.width = kSize;
    img.height = kSize;
    img.pixels.resize(static_cast<size_t>(img.width) * img.height * 4u);
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            uint8_t *px = img.at(x, y);
            const int dx = static_cast<int>(x) - 128;
            const int dy = static_cast<int>(y) - 128;
            const bool roundedCorner =
                (x < 24 && y < 24 && (dx + 104) * (dx + 104) + (dy + 104) * (dy + 104) > 24 * 24) ||
                (x >= 232 && y < 24 &&
                 (dx - 104) * (dx - 104) + (dy + 104) * (dy + 104) > 24 * 24) ||
                (x < 24 && y >= 232 &&
                 (dx + 104) * (dx + 104) + (dy - 104) * (dy - 104) > 24 * 24) ||
                (x >= 232 && y >= 232 &&
                 (dx - 104) * (dx - 104) + (dy - 104) * (dy - 104) > 24 * 24);
            const uint8_t shade = static_cast<uint8_t>((x * 34u + y * 22u) / (kSize * 2u));
            px[0] = roundedCorner ? 0 : static_cast<uint8_t>(10 + shade / 3u);
            px[1] = roundedCorner ? 0 : static_cast<uint8_t>(24 + shade / 2u);
            px[2] = roundedCorner ? 0 : static_cast<uint8_t>(28 + shade / 2u);
            px[3] = roundedCorner ? 0 : 255;
        }
    }

    /// @brief Calculate the italic offset shared by every rendered stroke.
    /// @param y Artwork row whose horizontal displacement is required.
    /// @return Horizontal offset, with rows near the top shifted farther right.
    const auto lean = [](uint32_t y) { return static_cast<int>((204u - y) / 8u); };

    // Steel diagonal from the top bar's right end to the base bar's left end.
    for (uint32_t y = 78; y < 178; ++y) {
        const int cx = 180 - static_cast<int>((y - 78) * 104 / 100) + lean(y);
        const uint8_t steel = static_cast<uint8_t>(208 - (y - 78));
        for (int x = cx - 17; x <= cx + 17; ++x) {
            if (x < 0 || x >= static_cast<int>(kSize))
                continue;
            uint8_t *px = img.at(static_cast<uint32_t>(x), y);
            px[0] = steel;
            px[1] = static_cast<uint8_t>(steel + 8);
            px[2] = static_cast<uint8_t>(steel + 6);
            px[3] = 255;
        }
    }

    // Green top bar and teal base bar drawn over the diagonal's ends.
    for (uint32_t y = 52; y < 204; ++y) {
        const bool topBar = y < 88;
        const bool baseBar = y >= 168;
        if (!topBar && !baseBar)
            continue;
        for (int x = 58 + lean(y); x < 198 + lean(y); ++x) {
            if (x < 0 || x >= static_cast<int>(kSize))
                continue;
            uint8_t *px = img.at(static_cast<uint32_t>(x), y);
            const uint8_t fade = static_cast<uint8_t>((x - 58 - lean(y)) / 6);
            if (topBar) {
                px[0] = static_cast<uint8_t>(120 - fade);
                px[1] = static_cast<uint8_t>(200 - fade);
                px[2] = 64;
            } else {
                px[0] = 30;
                px[1] = static_cast<uint8_t>(192 - fade);
                px[2] = static_cast<uint8_t>(188 - fade / 2);
            }
            px[3] = 255;
        }
    }

    return img;
}

//=============================================================================
// Multi-Size PNG Generation
//=============================================================================

/// @brief Produce individual PNG files at each Linux hicolor icon size.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return Ordered map from @ref kLinuxIconSizes dimensions to encoded PNG bytes.
/// @details Each value is a freshly resized and encoded image ready for the
///          corresponding hicolor `<size>x<size>/apps` directory.
/// @throws PNGError When validation, resizing, or encoding fails.
std::map<uint32_t, std::vector<uint8_t>> generateMultiSizePngs(const PkgImage &srcImage) {
    validateSourceImage(srcImage);
    std::map<uint32_t, std::vector<uint8_t>> result;

    for (uint32_t sz : kLinuxIconSizes) {
        PkgImage resized = imageResize(srcImage, sz, sz);
        result[sz] = pngEncode(resized);
    }

    return result;
}

} // namespace zanna::pkg
