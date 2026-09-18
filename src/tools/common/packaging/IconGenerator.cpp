//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/IconGenerator.cpp
// Purpose: Generate ICNS, ICO, and multi-size PNG icons from icon source sets.
//
// Key invariants:
//   - ICNS: "icns" magic + big-endian total_size + records. ic04/ic05 hold
//     "ARGB" + four run-length-encoded planes (A, R, G, B) with straight alpha;
//     every other record embeds a PNG.
//   - ICO: ICONDIR header + ICONDIRENTRY array + PNG data blobs.
//   - All numeric fields are little-endian in ICO, big-endian in ICNS.
//   - Sources are never modified; a size either reuses a matching source or is
//     resized from the smallest larger source (the largest when none is larger).
//   - Only loadIconSources performs file I/O.
//
// Ownership/Lifetime:
//   - Functions return caller-owned byte vectors and images.
//
// Links: IconGenerator.hpp, PkgPNG.hpp, PkgUtils.hpp,
//        docs/adr/0372-package-icon-sources-and-dmg-volume-icons.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements icon source sets and platform icon container generation.
/// @details Validates manifest icon sources, picks or resizes the image for every
///          platform size, encodes ICNS (including the ARGB run-length records),
///          ICO, and hicolor payloads, validates user-supplied ICNS files, and
///          supplies the built-in Zanna fallback artwork.

#include "IconGenerator.hpp"
#include "PkgUtils.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <set>
#include <stdexcept>

namespace zanna::pkg {

namespace {

/// @brief One ICNS record: its type code, pixel size, and payload encoding.
struct IcnsSlot {
    char type[5];  ///< 4-char ICNS type code (NUL-terminated for convenience).
    uint32_t size; ///< Target pixel dimension (square).
    bool argb;     ///< True for an ARGB run-length record, false for an embedded PNG.
};

// The record set Apple's own tools emit: 1x 16/32 as ARGB, everything else as PNG.
static const IcnsSlot kIcnsSlots[] = {
    {"ic04", 16, true},    // 16x16 ARGB
    {"ic05", 32, true},    // 32x32 ARGB
    {"ic11", 32, false},   // 32x32   (Retina 16)
    {"ic12", 64, false},   // 64x64   (Retina 32)
    {"ic07", 128, false},  // 128x128
    {"ic13", 256, false},  // 256x256 (Retina 128)
    {"ic08", 256, false},  // 256x256
    {"ic14", 512, false},  // 512x512 (Retina 256)
    {"ic09", 512, false},  // 512x512
    {"ic10", 1024, false}, // 1024x1024 (Retina 512)
};

/// @brief Standard icon sizes for Linux hicolor theme.
static const uint32_t kLinuxIconSizes[] = {16, 32, 48, 128, 256};

/// @brief Standard icon sizes for Windows ICO.
static const uint32_t kIcoSizes[] = {16, 24, 32, 48, 64, 128, 256};

/// @brief Append a 32-bit big-endian integer to `out`. Used for all ICNS numeric fields.
/// @param out Byte buffer extended by four bytes.
/// @param val Unsigned integer to encode.
void writeBE32(std::vector<uint8_t> &out, uint32_t val) {
    out.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(val & 0xFF));
}

/// @brief Read a 32-bit big-endian integer.
/// @param p Address of at least four readable bytes.
/// @return Decoded value.
uint32_t readBE32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
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

/// @brief Validate that `srcImage` is a well-formed single-image icon source.
/// @details Throws PNGError if the image is empty, non-square, smaller than the
///          minimum useful source size, or if the pixel buffer size does not
///          match width * height * 4 bytes. Enforcing a square source prevents
///          platform icon generators from silently stretching artwork.
/// @param srcImage Decoded RGBA source image.
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

/// @brief Format a square size as "<n>x<n>".
/// @param size Side length in pixels.
/// @return Human-readable dimensions.
std::string squareText(uint32_t size) {
    return std::to_string(size) + "x" + std::to_string(size);
}

/// @brief Find the first source at least @p size pixels wide.
/// @param set Sources sorted by ascending size.
/// @param size Target side length.
/// @return The matching or smallest larger source, or null when every source is smaller.
const IconSource *firstSourceAtLeast(const IconSourceSet &set, uint32_t size) {
    for (const IconSource &source : set.sources) {
        if (source.image.width >= size)
            return &source;
    }
    return nullptr;
}

/// @brief Append one channel plane with the run-length scheme ICNS uses.
/// @details A control byte of 0x80 or more repeats the next byte (control - 125)
///          times (3..130); a smaller control byte copies the next (control + 1)
///          bytes literally (1..128). Runs of three or more identical bytes are
///          always encoded as repeats.
/// @param out Destination buffer.
/// @param plane Channel bytes.
/// @param count Number of channel bytes.
void appendIcnsRunLength(std::vector<uint8_t> &out, const uint8_t *plane, size_t count) {
    size_t i = 0;
    while (i < count) {
        size_t run = 1;
        while (i + run < count && run < 130 && plane[i + run] == plane[i])
            ++run;
        if (run >= 3) {
            out.push_back(static_cast<uint8_t>(0x80 + run - 3));
            out.push_back(plane[i]);
            i += run;
            continue;
        }
        const size_t start = i;
        size_t literal = 0;
        while (i < count && literal < 128) {
            if (i + 2 < count && plane[i] == plane[i + 1] && plane[i] == plane[i + 2])
                break;
            ++i;
            ++literal;
        }
        out.push_back(static_cast<uint8_t>(literal - 1));
        out.insert(out.end(), plane + start, plane + start + literal);
    }
}

/// @brief Encode an image as an ICNS ARGB record payload.
/// @param image Straight-alpha RGBA image.
/// @return "ARGB" followed by the run-length-encoded A, R, G, and B planes.
std::vector<uint8_t> encodeIcnsArgb(const PkgImage &image) {
    std::vector<uint8_t> out = {'A', 'R', 'G', 'B'};
    const size_t count = static_cast<size_t>(image.width) * image.height;
    std::vector<uint8_t> plane(count);
    for (const size_t channel : {size_t{3}, size_t{0}, size_t{1}, size_t{2}}) {
        for (size_t i = 0; i < count; ++i)
            plane[i] = image.pixels[i * 4 + channel];
        appendIcnsRunLength(out, plane.data(), count);
    }
    return out;
}

/// @brief Colour of the procedural Zanna mark at one sample point.
/// @details Positions are in 1/32 units of the mark's 256-unit design space. The
///          design is a charcoal-green field with rounded corners, a steel
///          diagonal, and green top and teal base bars that lean to the right.
/// @param u Horizontal sample position.
/// @param v Vertical sample position.
/// @param rgba Receives straight RGBA; transparent outside the rounded field.
void sampleZannaMark(int64_t u, int64_t v, int64_t rgba[4]) {
    constexpr int64_t kUnit = 32;
    constexpr int64_t kNear = 24 * kUnit;
    constexpr int64_t kFar = 232 * kUnit;
    constexpr int64_t kRadius = 24 * kUnit;
    const auto outsideCorner = [&](int64_t cx, int64_t cy) {
        const int64_t dx = u - cx;
        const int64_t dy = v - cy;
        return dx * dx + dy * dy > kRadius * kRadius;
    };
    const bool corner = (u < kNear && v < kNear && outsideCorner(kNear, kNear)) ||
                        (u >= kFar && v < kNear && outsideCorner(kFar, kNear)) ||
                        (u < kNear && v >= kFar && outsideCorner(kNear, kFar)) ||
                        (u >= kFar && v >= kFar && outsideCorner(kFar, kFar));
    if (corner) {
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0;
        return;
    }
    rgba[3] = 255;

    // Green top bar and teal base bar, drawn over the diagonal's ends.
    if (v >= 52 * kUnit && v < 204 * kUnit) {
        const int64_t lean = (204 * kUnit - v) / 8;
        const bool topBar = v < 88 * kUnit;
        const bool baseBar = v >= 168 * kUnit;
        if ((topBar || baseBar) && u >= 58 * kUnit + lean && u < 198 * kUnit + lean) {
            const int64_t fade = (u - 58 * kUnit - lean) / (6 * kUnit);
            if (topBar) {
                rgba[0] = 120 - fade;
                rgba[1] = 200 - fade;
                rgba[2] = 64;
            } else {
                rgba[0] = 30;
                rgba[1] = 192 - fade;
                rgba[2] = 188 - fade / 2;
            }
            return;
        }
    }

    // Steel diagonal from the top bar's right end to the base bar's left end.
    if (v >= 78 * kUnit && v < 178 * kUnit) {
        const int64_t lean = (204 * kUnit - v) / 8;
        const int64_t center = 180 * kUnit - (v - 78 * kUnit) * 104 / 100 + lean + kUnit / 2;
        const int64_t distance = u >= center ? u - center : center - u;
        if (distance * 2 <= 35 * kUnit) {
            const int64_t steel = 208 - (v - 78 * kUnit) / kUnit;
            rgba[0] = steel;
            rgba[1] = steel + 8;
            rgba[2] = steel + 6;
            return;
        }
    }

    const int64_t shade = (u * 34 + v * 22) / (512 * kUnit);
    rgba[0] = 10 + shade / 3;
    rgba[1] = 24 + shade / 2;
    rgba[2] = 28 + shade / 2;
}

/// @brief Render the procedural Zanna mark at 1024x1024 with 4x4 supersampling.
/// @return The rendered image; samples are averaged with alpha weighting.
PkgImage renderZannaMark() {
    constexpr uint32_t kSize = 1024;
    PkgImage img;
    img.width = kSize;
    img.height = kSize;
    img.pixels.assign(static_cast<size_t>(kSize) * kSize * 4u, 0);
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            int64_t sumA = 0;
            int64_t sumR = 0;
            int64_t sumG = 0;
            int64_t sumB = 0;
            for (int64_t j = 0; j < 4; ++j) {
                for (int64_t i = 0; i < 4; ++i) {
                    int64_t rgba[4];
                    // One output pixel spans 8/32 design units; sample at subpixel centers.
                    sampleZannaMark(8 * static_cast<int64_t>(x) + 2 * i + 1,
                                    8 * static_cast<int64_t>(y) + 2 * j + 1,
                                    rgba);
                    sumA += rgba[3];
                    sumR += rgba[0] * rgba[3];
                    sumG += rgba[1] * rgba[3];
                    sumB += rgba[2] * rgba[3];
                }
            }
            if (sumA == 0)
                continue;
            uint8_t *px = img.at(x, y);
            px[0] = static_cast<uint8_t>((sumR + sumA / 2) / sumA);
            px[1] = static_cast<uint8_t>((sumG + sumA / 2) / sumA);
            px[2] = static_cast<uint8_t>((sumB + sumA / 2) / sumA);
            px[3] = static_cast<uint8_t>((sumA + 8) / 16);
        }
    }
    return img;
}

} // namespace

//=============================================================================
// Icon Sources
//=============================================================================

/// @brief Decode and validate one PNG icon source.
/// @param png PNG file bytes, retained for verbatim reuse.
/// @param label Manifest path used in diagnostics.
/// @param fieldName Manifest directive used in diagnostics.
/// @return The decoded source.
/// @throws std::runtime_error If decoding fails, the image is not square, or it is
///         smaller than 16x16 pixels.
IconSource makeIconSource(std::vector<uint8_t> png, std::string label, const char *fieldName) {
    IconSource source;
    try {
        source.image = pngReadMemory(png.data(), png.size());
    } catch (const PNGError &error) {
        throw std::runtime_error(std::string(fieldName) + " '" + label +
                                 "' is not a readable PNG: " + error.what());
    }
    if (source.image.width != source.image.height) {
        throw std::runtime_error(
            std::string(fieldName) + " '" + label + "' must be a square PNG (got " +
            std::to_string(source.image.width) + "x" + std::to_string(source.image.height) + ")");
    }
    if (source.image.width < 16) {
        throw std::runtime_error(std::string(fieldName) + " '" + label +
                                 "' must be at least 16x16 pixels");
    }
    source.png = std::move(png);
    source.label = std::move(label);
    return source;
}

/// @brief Sort and validate icon sources into a set.
/// @param sources Decoded sources in any order.
/// @param fieldName Manifest directive used in diagnostics.
/// @return Sources sorted by ascending size.
/// @throws std::runtime_error On an empty list, duplicate sizes, or no source of at
///         least 32x32 pixels.
IconSourceSet makeIconSourceSet(std::vector<IconSource> sources, const char *fieldName) {
    if (sources.empty())
        throw std::runtime_error(std::string(fieldName) + " lists no icon files");
    std::stable_sort(sources.begin(), sources.end(), [](const IconSource &a, const IconSource &b) {
        return a.image.width < b.image.width;
    });
    for (size_t i = 1; i < sources.size(); ++i) {
        if (sources[i].image.width == sources[i - 1].image.width) {
            throw std::runtime_error(std::string(fieldName) + " sources '" + sources[i - 1].label +
                                     "' and '" + sources[i].label + "' are both " +
                                     squareText(sources[i].image.width) +
                                     "; each size may appear once");
        }
    }
    IconSourceSet set;
    set.sources = std::move(sources);
    if (set.largestSize() < 32) {
        throw std::runtime_error(std::string(fieldName) +
                                 " needs a source of at least 32x32 pixels (largest is " +
                                 squareText(set.largestSize()) + ")");
    }
    return set;
}

/// @brief Wrap one in-memory image as a one-source set.
/// @param image Square RGBA image at least 32 pixels per side.
/// @return Set holding a copy of the image and no original PNG bytes.
/// @throws PNGError When the image is empty, non-square, too small, or malformed.
IconSourceSet iconSourceSetFromImage(const PkgImage &image) {
    validateSourceImage(image);
    IconSourceSet set;
    set.sources.push_back({image, {}, "generated"});
    return set;
}

/// @brief Read, decode, and validate manifest icon paths.
/// @param projectRoot Trusted project root the paths are relative to.
/// @param paths Project-relative PNG paths.
/// @param fieldName Manifest directive used in diagnostics.
/// @return The validated set.
/// @throws std::runtime_error On unsafe, missing, duplicate, or invalid sources.
IconSourceSet loadIconSources(const std::filesystem::path &projectRoot,
                              const std::vector<std::string> &paths,
                              const char *fieldName) {
    std::vector<IconSource> sources;
    std::set<std::string> seen;
    for (const std::string &raw : paths) {
        if (!seen.insert(sanitizePackageRelativePath(raw, fieldName)).second)
            throw std::runtime_error("duplicate " + std::string(fieldName) + " path '" + raw + "'");
        const std::filesystem::path resolved =
            resolvePackageSourcePath(projectRoot, raw, fieldName);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(resolved, ec))
            throw std::runtime_error(std::string(fieldName) + " not found: " + raw);
        sources.push_back(makeIconSource(readFile(resolved), raw, fieldName));
    }
    return makeIconSourceSet(std::move(sources), fieldName);
}

/// @brief Produce the RGBA image for one icon size.
/// @param set Validated icon sources.
/// @param size Target side length in pixels.
/// @return A matching source's pixels, or a resized copy of the best neighbour.
PkgImage iconImageForSize(const IconSourceSet &set, uint32_t size) {
    const IconSource *source = firstSourceAtLeast(set, size);
    if (!source)
        source = &set.sources.back();
    if (source->image.width == size)
        return source->image;
    return imageResize(source->image, size, size);
}

/// @brief Produce PNG bytes for one icon size, reusing a matching source verbatim.
/// @param set Validated icon sources.
/// @param size Target side length in pixels.
/// @param requireRgba8 Reuse a matching source only when it is 8-bit, RGBA, and
///        non-interlaced.
/// @return PNG bytes for the size.
std::vector<uint8_t> iconPngForSize(const IconSourceSet &set, uint32_t size, bool requireRgba8) {
    const IconSource *source = firstSourceAtLeast(set, size);
    if (source && source->image.width == size && !source->png.empty()) {
        if (!requireRgba8)
            return source->png;
        const PngInfo info = pngReadInfo(source->png.data(), source->png.size());
        if (info.bitDepth == 8 && info.colorType == 6 && info.interlace == 0)
            return source->png;
    }
    return pngEncode(iconImageForSize(set, size));
}

//=============================================================================
// ICNS Generation
//=============================================================================

/// @brief Build a macOS ICNS container from icon sources.
/// @param set Validated icon sources.
/// @return Complete ICNS container bytes.
/// @details Emits every @ref kIcnsSlots record in order; a size needed by two
///          records is produced once. The global size field is patched last.
/// @throws PNGError When a size cannot be represented in 32 bits.
std::vector<uint8_t> generateIcns(const IconSourceSet &set) {
    std::vector<uint8_t> result = {'i', 'c', 'n', 's', 0, 0, 0, 0};
    std::map<uint32_t, std::vector<uint8_t>> pngs;
    for (const auto &slot : kIcnsSlots) {
        std::vector<uint8_t> argb;
        const std::vector<uint8_t> *payload = nullptr;
        if (slot.argb) {
            argb = encodeIcnsArgb(iconImageForSize(set, slot.size));
            payload = &argb;
        } else {
            auto it = pngs.find(slot.size);
            if (it == pngs.end())
                it = pngs.emplace(slot.size, iconPngForSize(set, slot.size, false)).first;
            payload = &it->second;
        }
        result.insert(result.end(), slot.type, slot.type + 4);
        writeBE32(result, checkedIconSize(8 + payload->size(), "ICNS"));
        result.insert(result.end(), payload->begin(), payload->end());
    }
    const uint32_t totalSize = checkedIconSize(result.size(), "ICNS");
    result[4] = static_cast<uint8_t>((totalSize >> 24) & 0xFF);
    result[5] = static_cast<uint8_t>((totalSize >> 16) & 0xFF);
    result[6] = static_cast<uint8_t>((totalSize >> 8) & 0xFF);
    result[7] = static_cast<uint8_t>(totalSize & 0xFF);
    return result;
}

/// @brief Build a macOS ICNS container from one image.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return Complete ICNS container bytes.
/// @throws PNGError When validation or size narrowing fails.
std::vector<uint8_t> generateIcns(const PkgImage &srcImage) {
    return generateIcns(iconSourceSetFromImage(srcImage));
}

/// @brief Validate the framing of an ICNS container.
/// @param data Complete file bytes.
/// @throws std::runtime_error With the first framing problem found.
void validateIcns(const std::vector<uint8_t> &data) {
    if (data.size() < 8 || data[0] != 'i' || data[1] != 'c' || data[2] != 'n' || data[3] != 's')
        throw std::runtime_error("missing 'icns' magic");
    const uint32_t declared = readBE32(data.data() + 4);
    if (declared != data.size()) {
        throw std::runtime_error("declared size " + std::to_string(declared) +
                                 " does not match file size " + std::to_string(data.size()));
    }
    size_t pos = 8;
    size_t iconEntries = 0;
    while (pos < data.size()) {
        std::string type;
        for (size_t i = pos; i < pos + 4 && i < data.size(); ++i) {
            const unsigned char c = data[i];
            type.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '?');
        }
        if (data.size() - pos < 8)
            throw std::runtime_error("entry '" + type + "' overruns the file");
        const uint32_t length = readBE32(data.data() + pos + 4);
        if (length < 8 || length > data.size() - pos)
            throw std::runtime_error("entry '" + type + "' overruns the file");
        if (type != "TOC " && type != "info" && type != "name")
            ++iconEntries;
        pos += length;
    }
    if (iconEntries == 0)
        throw std::runtime_error("contains no icon entries");
}

//=============================================================================
// ICO Generation
//=============================================================================

/// @brief Build a Windows ICO container embedding PNG data at each standard icon size.
/// @param set Validated icon sources.
/// @return Complete ICO container bytes.
/// @details Generates every @ref kIcoSizes PNG, writes the ICONDIR and directory
///          records, then appends payloads in order. A dimension of 256 is encoded
///          as zero per the ICO specification. Matching sources are reused only when
///          they are 8-bit RGBA and non-interlaced.
/// @throws PNGError When image processing or 32-bit offsets fail.
std::vector<uint8_t> generateIco(const IconSourceSet &set) {
    struct IcoEntry {
        uint32_t size{0};
        std::vector<uint8_t> pngData;
    };

    std::vector<IcoEntry> entries;
    for (uint32_t sz : kIcoSizes)
        entries.push_back({sz, iconPngForSize(set, sz, true)});

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

/// @brief Build a Windows ICO container from one image.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return Complete ICO container bytes.
/// @throws PNGError When validation, image processing, or 32-bit offsets fail.
std::vector<uint8_t> generateIco(const PkgImage &srcImage) {
    return generateIco(iconSourceSetFromImage(srcImage));
}

//=============================================================================
// Built-in Artwork
//=============================================================================

/// @brief Build the dependency-free fallback Zanna toolchain icon.
/// @details Draws the italic Zanna "Z" mark — green top bar, steel diagonal, teal
///          base bar — on a dark charcoal-green field with rounded corners. The
///          artwork is rendered once per process at 1024 pixels with 4x4 integer
///          supersampling, so every platform icon size is a downscale.
/// @return A 1024x1024 RGBA image.
PkgImage defaultZannaToolchainIconImage() {
    static const PkgImage icon = renderZannaMark();
    return icon;
}

//=============================================================================
// Multi-Size PNG Generation
//=============================================================================

/// @brief Produce individual PNG files at each Linux hicolor icon size.
/// @param set Validated icon sources.
/// @return Ordered map from @ref kLinuxIconSizes dimensions to encoded PNG bytes.
/// @details Matching sources are reused verbatim; other sizes are resized.
std::map<uint32_t, std::vector<uint8_t>> generateMultiSizePngs(const IconSourceSet &set) {
    std::map<uint32_t, std::vector<uint8_t>> result;
    for (uint32_t sz : kLinuxIconSizes)
        result[sz] = iconPngForSize(set, sz, false);
    return result;
}

/// @brief Produce individual PNG files at each Linux hicolor icon size from one image.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return Ordered map from @ref kLinuxIconSizes dimensions to encoded PNG bytes.
/// @throws PNGError When validation, resizing, or encoding fails.
std::map<uint32_t, std::vector<uint8_t>> generateMultiSizePngs(const PkgImage &srcImage) {
    return generateMultiSizePngs(iconSourceSetFromImage(srcImage));
}

} // namespace zanna::pkg
