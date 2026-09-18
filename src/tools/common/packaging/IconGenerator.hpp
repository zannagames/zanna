//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/IconGenerator.hpp
// Purpose: Generate platform-specific icon formats from author-supplied icon sources.
//          - macOS: ICNS container with 16/32 px ARGB records and PNG records
//            from 32 to 1024 pixels.
//          - Windows: ICO container with embedded PNGs at multiple sizes.
//          - Linux: Individual PNG files at standard sizes.
//
// Key invariants:
//   - An IconSourceSet holds square sources with unique sizes, smallest first;
//     its largest source is at least 32 pixels per side.
//   - A slot whose size matches a supplied source reuses that PNG's bytes (ICO
//     only when they are 8-bit RGBA and non-interlaced); any other slot is
//     area-downscaled from the smallest larger source, or upscaled from the
//     largest source when none is larger.
//   - ICNS uses big-endian fields; ICO uses little-endian fields.
//   - loadIconSources is the only function that reads files.
//
// Ownership/Lifetime:
//   - All output returned as std::vector<uint8_t> or PkgImage (caller-owned).
//
// Links: PkgPNG.hpp (resize + encode), MacOSPackageBuilder.hpp,
//        LinuxPackageBuilder.hpp, WindowsPackageBuilder.hpp,
//        docs/adr/0372-package-icon-sources-and-dmg-volume-icons.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares dependency-free platform icon generation from icon source sets.
/// @details Manifest icon PNGs (or one generated image) form an IconSourceSet. Each
///          platform size is taken verbatim from a matching source or resized from
///          the best neighbour, then packed into macOS ICNS, Windows ICO, or Linux
///          hicolor payloads returned in caller-owned storage.

#pragma once

#include "PkgPNG.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace zanna::pkg {

/// @brief One decoded, validated icon source.
struct IconSource {
    PkgImage image;           ///< Decoded square RGBA pixels.
    std::vector<uint8_t> png; ///< Original PNG bytes; empty for generated artwork.
    std::string label;        ///< Manifest path (or "generated") used in diagnostics.
};

/// @brief Square icon sources with unique sizes, sorted smallest first.
struct IconSourceSet {
    std::vector<IconSource> sources; ///< Validated sources, ascending by size.

    /// @brief Side length of the largest source, or zero for an empty set.
    /// @return Largest source width in pixels.
    uint32_t largestSize() const {
        return sources.empty() ? 0 : sources.back().image.width;
    }
};

/// @brief Decode and validate one PNG icon source.
/// @param png PNG file bytes (kept for verbatim reuse).
/// @param label Manifest path used in diagnostics.
/// @param fieldName Manifest directive used in diagnostics, e.g. "package-icon".
/// @return The decoded source.
/// @throws std::runtime_error If the PNG cannot be decoded, is not square, or is
///         smaller than 16x16 pixels.
IconSource makeIconSource(std::vector<uint8_t> png, std::string label, const char *fieldName);

/// @brief Sort and validate icon sources into a set.
/// @param sources Decoded sources in any order.
/// @param fieldName Manifest directive used in diagnostics.
/// @return Sources sorted by ascending size.
/// @throws std::runtime_error If the list is empty, two sources share a size, or no
///         source is at least 32x32 pixels.
IconSourceSet makeIconSourceSet(std::vector<IconSource> sources, const char *fieldName);

/// @brief Wrap one in-memory image, such as generated artwork, as a one-source set.
/// @param image Square RGBA image at least 32 pixels per side.
/// @return A set whose only source has no original PNG bytes.
/// @throws PNGError When the image is empty, non-square, smaller than 32x32, or malformed.
IconSourceSet iconSourceSetFromImage(const PkgImage &image);

/// @brief Read, decode, and validate manifest icon paths.
/// @param projectRoot Trusted project root the paths are relative to.
/// @param paths Project-relative PNG paths, one per manifest line.
/// @param fieldName Manifest directive used in diagnostics.
/// @return The validated set.
/// @throws std::runtime_error On unsafe, missing, duplicate, or invalid sources.
IconSourceSet loadIconSources(const std::filesystem::path &projectRoot,
                              const std::vector<std::string> &paths,
                              const char *fieldName);

/// @brief Produce the RGBA image for one icon size.
/// @param set Validated icon sources.
/// @param size Target side length in pixels.
/// @return A matching source's pixels, or a resized copy of the best neighbour.
PkgImage iconImageForSize(const IconSourceSet &set, uint32_t size);

/// @brief Produce PNG bytes for one icon size.
/// @param set Validated icon sources.
/// @param size Target side length in pixels.
/// @param requireRgba8 When true, a matching source is reused only if it is 8-bit
///        RGBA and non-interlaced; otherwise its pixels are re-encoded.
/// @return PNG bytes for the size.
std::vector<uint8_t> iconPngForSize(const IconSourceSet &set, uint32_t size, bool requireRgba8);

/// @brief Generate a macOS ICNS container from icon sources.
/// @details Emits `ic04` (16) and `ic05` (32) as `ARGB` records holding four
///          run-length-encoded planes with straight alpha, then PNG records
///          `ic11` (32), `ic12` (64), `ic07` (128), `ic13`/`ic08` (256),
///          `ic14`/`ic09` (512), and `ic10` (1024).
/// @param set Validated icon sources.
/// @return ICNS file data.
/// @throws PNGError When a 32-bit format size is exceeded.
std::vector<uint8_t> generateIcns(const IconSourceSet &set);

/// @brief Generate a macOS ICNS container from one image.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return ICNS file data.
/// @throws PNGError When the source image or a 32-bit format size is invalid.
std::vector<uint8_t> generateIcns(const PkgImage &srcImage);

/// @brief Generate a Windows ICO container from icon sources.
/// @details Produces PNG-backed records at 16, 24, 32, 48, 64, 128, and 256
///          pixels. The output consists of the six-byte ICONDIR, one 16-byte
///          directory entry per image, and the PNG blocks in matching order.
/// @param set Validated icon sources.
/// @return ICO file data.
/// @throws PNGError When a 32-bit format size is exceeded.
std::vector<uint8_t> generateIco(const IconSourceSet &set);

/// @brief Generate a Windows ICO container from one image.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return ICO file data.
/// @throws PNGError When the source image or a 32-bit format size is invalid.
std::vector<uint8_t> generateIco(const PkgImage &srcImage);

/// @brief Validate the framing of an ICNS container.
/// @param data Complete file bytes.
/// @throws std::runtime_error With one of: "missing 'icns' magic", "declared size <n>
///         does not match file size <m>", "entry '<type>' overruns the file", or
///         "contains no icon entries".
void validateIcns(const std::vector<uint8_t> &data);

/// @brief Generate Zanna's built-in fallback toolchain icon as an RGBA image.
///
/// @details The packagers use this when an installer is packaging Zanna itself
///          rather than an end-user project with a manifest-provided icon. The
///          artwork is drawn natively at 1024 pixels with 4x4 supersampling, so
///          every platform size is a downscale.
/// @return A 1024x1024 RGBA image suitable for iconSourceSetFromImage and the
///         single-image generators.
PkgImage defaultZannaToolchainIconImage();

/// @brief Generate Linux hicolor PNG icons from icon sources.
///
/// @details Generates standard Linux hicolor sizes 16, 32, 48, 128, and 256,
///          using the pixel dimension as the ordered map key.
/// @param set Validated icon sources.
/// @return Map of pixel size to PNG file data.
std::map<uint32_t, std::vector<uint8_t>> generateMultiSizePngs(const IconSourceSet &set);

/// @brief Generate Linux hicolor PNG icons from one image.
/// @param srcImage Square RGBA image at least 32 pixels per side.
/// @return Map of pixel size to PNG file data.
/// @throws PNGError When source validation, resizing, or encoding fails.
std::map<uint32_t, std::vector<uint8_t>> generateMultiSizePngs(const PkgImage &srcImage);

} // namespace zanna::pkg
