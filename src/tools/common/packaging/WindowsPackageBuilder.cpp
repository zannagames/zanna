//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/WindowsPackageBuilder.cpp
// Purpose: Assemble a Windows self-extracting installer .exe.
//
// Key invariants:
//   - Overlay is a structurally valid ZIP archive using stored bootstrap entries.
//     The main payload is a DEFLATE-compressed inner ZIP expanded at install time.
//   - Installer/uninstaller behavior is driven by an explicit package layout,
//     not by parsing installer metadata at runtime.
//   - All package construction happens inside Zanna; no external tools are used.
//
// Ownership/Lifetime:
//   - Single-use builder.
//
// Links: WindowsPackageBuilder.hpp, InstallerStub.hpp, PEBuilder.hpp, ZipWriter.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements native Windows application and toolchain installer assembly.
/// @details The builder validates staged PE payloads and destination layouts,
///          constructs signed nested artifacts and deterministic metadata, then
///          appends a ZIP overlay to an architecture-matched native host.

#include "WindowsPackageBuilder.hpp"
#include "../../../common/Filesystem.hpp"
#include "IconGenerator.hpp"
#include "InstallerStub.hpp"
#include "LnkWriter.hpp"
#include "PEBuilder.hpp"
#include "PkgPNG.hpp"
#include "PkgUtils.hpp"
#include "WindowsInstallerMetadata.hpp"
#include "ZipReader.hpp"
#include "ZipWriter.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace fs = std::filesystem;

namespace zanna::pkg {

namespace {

constexpr size_t kInstallerStubPathCharLimit = 32768;
constexpr uint64_t kInstallerStackReserve = 0x200000;
constexpr uint64_t kInstallerStackCommit = 0x100000;
constexpr std::string_view kComponentZannaStudio = "zannastudio";
constexpr std::string_view kComponentSDK = "sdk";
constexpr std::string_view kComponentSamples = "samples";
constexpr std::string_view kComponentVSCode = "vscode";

std::string lowerAscii(std::string text);

/// @brief Return true for staged bootstrap executables consumed by the packager itself.
/// @param relativePath Staged install-relative path.
/// @return true for the installer host or detached cleanup helper paths.
bool isToolchainInstallerBootstrapPath(std::string_view relativePath) {
    std::string normalized(relativePath);
    std::replace(normalized.begin(), normalized.end(), '\\', '/');
    normalized = lowerAscii(std::move(normalized));
    return normalized == "bin/zanna-installer-host.exe" ||
           normalized == "bin/zanna-installer-cleanup.exe";
}

/// @brief Return the optional Windows toolchain component owning an install path.
/// @param relativePath Staged install-relative path.
/// @param packagedVSIX Whether a packaged VS Code extension is available.
/// @return Optional component id, or empty for the required core component.
std::string toolchainComponentForPath(const std::string &relativePath, bool packagedVSIX) {
    const std::string lower = lowerAscii(relativePath);
    if (lower.rfind("bin/zannastudio", 0) == 0 || lower.rfind("share/zanna/zannastudio/", 0) == 0 ||
        lower.rfind("share/zanna/ide/", 0) == 0) {
        return std::string(kComponentZannaStudio);
    }
    if (lower.rfind("share/zanna/samples/", 0) == 0 || lower.rfind("share/zanna/examples/", 0) == 0)
        return std::string(kComponentSamples);
    if (lower.rfind("share/zanna/vscode/", 0) == 0 ||
        lower == "bin/zanna-install-vscode-extension.cmd") {
        return std::string(packagedVSIX ? kComponentVSCode : kComponentSDK);
    }
    if (lower.rfind("include/", 0) == 0 || lower.rfind("lib/", 0) == 0 ||
        lower.rfind("share/zanna/sdk/", 0) == 0 || lower.rfind("share/zanna/cmake/", 0) == 0 ||
        lower.rfind("share/cmake/zanna/", 0) == 0) {
        return std::string(kComponentSDK);
    }
    return {};
}

/// @brief Find and decode one staged PNG by its normalized install-relative path.
/// @param manifest Staged toolchain inventory to search.
/// @param relativePath Case-insensitive path of the desired image.
/// @return Decoded image, or `std::nullopt` when the path is absent.
/// @throws std::runtime_error If the matching file cannot be decoded as PNG.
std::optional<PkgImage> stagedToolchainPng(const ToolchainInstallManifest &manifest,
                                           std::string_view relativePath) {
    const std::string wanted = lowerAscii(std::string(relativePath));
    for (const ToolchainFileEntry &file : manifest.files) {
        if (lowerAscii(file.stagedRelativePath) == wanted)
            return pngRead(zanna::filesystem::pathToUtf8(file.stagedAbsolutePath));
    }
    return std::nullopt;
}

/// @brief Crop an image around its centre to @p targetWidth:@p targetHeight, then resize it.
/// @param source Non-empty source RGBA image.
/// @param targetWidth Required output width in pixels.
/// @param targetHeight Required output height in pixels.
/// @return Center-cropped and resized RGBA image.
/// @throws std::runtime_error If either image dimension or source payload is empty.
PkgImage imageCover(const PkgImage &source, uint32_t targetWidth, uint32_t targetHeight) {
    if (source.width == 0 || source.height == 0 || source.pixels.empty() || targetWidth == 0 ||
        targetHeight == 0) {
        throw std::runtime_error("Windows wizard branding image is empty");
    }

    uint32_t cropWidth = source.width;
    uint32_t cropHeight = source.height;
    const uint64_t sourceScaled = static_cast<uint64_t>(source.width) * targetHeight;
    const uint64_t targetScaled = static_cast<uint64_t>(targetWidth) * source.height;
    if (sourceScaled > targetScaled) {
        cropWidth = static_cast<uint32_t>((static_cast<uint64_t>(source.height) * targetWidth) /
                                          targetHeight);
    } else if (sourceScaled < targetScaled) {
        cropHeight = static_cast<uint32_t>((static_cast<uint64_t>(source.width) * targetHeight) /
                                           targetWidth);
    }
    cropWidth = std::max<uint32_t>(1, std::min(cropWidth, source.width));
    cropHeight = std::max<uint32_t>(1, std::min(cropHeight, source.height));
    const uint32_t originX = (source.width - cropWidth) / 2;
    const uint32_t originY = (source.height - cropHeight) / 2;

    PkgImage crop;
    crop.width = cropWidth;
    crop.height = cropHeight;
    crop.pixels.resize(static_cast<size_t>(cropWidth) * cropHeight * 4u);
    for (uint32_t y = 0; y < cropHeight; ++y) {
        for (uint32_t x = 0; x < cropWidth; ++x)
            std::copy_n(source.at(originX + x, originY + y), 4, crop.at(x, y));
    }
    return imageResize(crop, targetWidth, targetHeight);
}

/// @brief Alpha-compose @p foreground into @p background at the requested pixel origin.
/// @param background Destination RGBA image, modified in place.
/// @param foreground Source RGBA image.
/// @param originX Horizontal destination offset in pixels.
/// @param originY Vertical destination offset in pixels.
void alphaComposite(PkgImage &background,
                    const PkgImage &foreground,
                    uint32_t originX,
                    uint32_t originY) {
    for (uint32_t y = 0; y < foreground.height && originY + y < background.height; ++y) {
        for (uint32_t x = 0; x < foreground.width && originX + x < background.width; ++x) {
            const uint8_t *src = foreground.at(x, y);
            uint8_t *dst = background.at(originX + x, originY + y);
            const uint32_t alpha = src[3];
            const uint32_t inverse = 255u - alpha;
            for (size_t channel = 0; channel < 3; ++channel) {
                dst[channel] =
                    static_cast<uint8_t>((static_cast<uint32_t>(src[channel]) * alpha +
                                          static_cast<uint32_t>(dst[channel]) * inverse + 127u) /
                                         255u);
            }
            dst[3] = 255;
        }
    }
}

/// @brief Compose the wide setup banner from the canonical wallpaper and logo artwork.
/// @param wallpaper Background image center-cropped to the banner aspect ratio.
/// @param logo Logo image resized and composited at the left margin.
/// @return Opaque 960-by-200 RGBA wizard banner.
PkgImage buildZannaWizardBanner(const PkgImage &wallpaper, const PkgImage &logo) {
    constexpr uint32_t kWidth = 960;
    constexpr uint32_t kHeight = 200;
    PkgImage banner = imageCover(wallpaper, kWidth, kHeight);
    for (uint8_t &channel : banner.pixels)
        channel = static_cast<uint8_t>((static_cast<uint32_t>(channel) * 58u) / 100u);
    for (size_t i = 3; i < banner.pixels.size(); i += 4)
        banner.pixels[i] = 255;

    const PkgImage mark = imageResize(logo, 168, 168);
    alphaComposite(banner, mark, 24, 16);
    return banner;
}

/// @brief One Windows runtime dependency to bundle with an app installer.
struct WindowsDllDependency {
    fs::path sourcePath;             ///< Absolute or caller-resolved source DLL path.
    std::string installRelativePath; ///< Path to write under the app install root.
};

/// @brief Apply a larger-than-default stack to all installer/uninstaller PEs.
/// The installer recursively creates directory trees and copies large files;
/// the default 1 MB reserve is too small for deeply nested install paths.
/// @param pe PE build parameters to update in place.
void configureInstallerStack(PEBuildParams &pe) {
    pe.stackReserve = kInstallerStackReserve;
    pe.stackCommit = kInstallerStackCommit;
}

/// @brief Append a directory entry to out only if it has not already been seen.
/// The seen set keys on root+path so the same relative path under different
/// roots (e.g. InstallDir vs StartMenuDir) is treated as two distinct entries.
/// @param out Destination directory list.
/// @param seen Root-qualified sanitized paths already appended.
/// @param root Runtime destination root.
/// @param relativePath Root-relative directory path.
/// @throws std::runtime_error If @p relativePath is unsafe.
void addUniqueDir(std::vector<WindowsPackageDirEntry> &out,
                  std::set<std::string> &seen,
                  WindowsInstallRoot root,
                  const std::string &relativePath) {
    const std::string clean = sanitizePackageRelativePath(relativePath, "windows package path");
    if (clean.empty())
        return;
    const std::string key = std::to_string(static_cast<unsigned long long>(root)) + ":" + clean;
    if (!seen.insert(key).second)
        return;
    out.push_back(WindowsPackageDirEntry{root, clean});
}

/// @brief Validate every path segment in a slash-separated relative Windows path.
/// Each segment must pass validateWindowsFileName to reject reserved device names
/// (CON, NUL, COM1…), illegal characters (<, >, :, |, ?, *), and empty components.
/// @param relativePath Candidate install-relative path.
/// @param fieldName Human-readable metadata field used in diagnostics.
/// @throws std::runtime_error If sanitization or any Windows leaf check fails.
void validateWindowsRelativePath(const std::string &relativePath, const char *fieldName) {
    const std::string clean = sanitizePackageRelativePath(relativePath, fieldName);
    size_t pos = 0;
    while (pos < clean.size()) {
        const size_t next = clean.find('/', pos);
        const std::string segment =
            next == std::string::npos ? clean.substr(pos) : clean.substr(pos, next - pos);
        validateWindowsFileName(segment, fieldName);
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
}

/// @brief Return a conservative environment-expanded probe for a Windows install root.
/// @details The installer resolves known folders at runtime. These probes mirror
///          the longest common expanded locations closely enough for preflight
///          buffer checks, including Desktop and Start Menu entries that are not
///          under the package install directory.
/// @param layout Package layout determining per-user versus machine roots.
/// @param root Root anchor to probe.
/// @return Representative absolute path prefix for @p root.
std::string windowsRootProbeFor(const WindowsPackageLayout &layout, WindowsInstallRoot root) {
    const std::string installDir =
        layout.installDirName.empty() ? layout.displayName : layout.installDirName;
    switch (root) {
        case WindowsInstallRoot::DesktopDir:
            return layout.perUserInstall ? "%UserProfile%\\Desktop" : "%Public%\\Desktop";
        case WindowsInstallRoot::StartMenuDir:
            return layout.perUserInstall
                       ? "%AppData%\\Microsoft\\Windows\\Start Menu\\Programs"
                       : "%ProgramData%\\Microsoft\\Windows\\Start Menu\\Programs";
        case WindowsInstallRoot::InstallDir:
        default:
            return (layout.perUserInstall ? "%LocalAppData%\\" : "%ProgramFiles%\\") + installDir;
    }
}

/// @brief Add a path to a case-insensitive collision set for a Windows root.
/// @details Windows destination paths are case-insensitive on normal NTFS
///          installs. This catches `Readme.txt` vs `README.TXT` collisions
///          before the overlay is built and one entry overwrites another.
/// @param seen Root-qualified normalized path set.
/// @param root Destination root anchor.
/// @param relativePath Root-relative path to validate and insert.
/// @param fieldName Diagnostic field name.
void addWindowsCaseFoldedPath(std::set<std::string> &seen,
                              WindowsInstallRoot root,
                              const std::string &relativePath,
                              const char *fieldName) {
    const std::string clean = sanitizePackageRelativePath(relativePath, fieldName);
    if (clean.empty())
        return;
    std::string folded = clean;
    for (char &c : folded)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const std::string key = std::to_string(static_cast<unsigned long long>(root)) + ":" + folded;
    if (!seen.insert(key).second)
        throw std::runtime_error(
            std::string(fieldName) +
            " collides with another path on case-insensitive Windows: " + clean);
}

/// @brief Ensure that an absolute-expanded Windows path (e.g. %ProgramFiles%\App\bin\zanna.exe)
/// fits within the installer stub's fixed-size WCHAR path buffer (32768 code units).
/// The check uses UTF-16 unit count so multi-byte UTF-8 characters are counted correctly.
/// @param path Representative expanded UTF-8 path.
/// @param fieldName Human-readable context used in diagnostics.
/// @throws std::runtime_error If the path plus terminator exceeds the stub buffer.
void validateStubPathFits(const std::string &path, const char *fieldName) {
    if (utf16CodeUnitCountFromUtf8(path) + 1 > kInstallerStubPathCharLimit)
        throw std::runtime_error(std::string(fieldName) +
                                 " exceeds the Windows installer long-path stub limit: " + path);
}

/// @brief Pre-flight every path that the installer stub will write at runtime to
/// ensure none exceed the stub's WCHAR buffer limit. Checks: install root,
/// all directories, all files, the optional PATH entry, the file association
/// executable path, and all file association ProgID command arguments.
/// @param layout Complete installer layout to validate before code generation.
/// @throws std::runtime_error On unsafe paths, collisions, unknown components,
///         invalid branding, or any stub-capacity violation.
void validateWindowsLayoutFitsStub(const WindowsPackageLayout &layout) {
    const std::string installDir =
        layout.installDirName.empty() ? layout.displayName : layout.installDirName;
    validateWindowsFileName(installDir, "Windows install directory");
    const std::string rootProbe = windowsRootProbeFor(layout, WindowsInstallRoot::InstallDir);
    validateStubPathFits(rootProbe, "Windows install directory");
    std::set<std::string> caseFoldedPaths;
    if (layout.optionalComponents.size() > 8)
        throw std::runtime_error("Windows installer supports at most 8 optional components");
    std::set<std::string> componentIds;
    for (const WindowsOptionalComponent &component : layout.optionalComponents) {
        if (component.id.empty())
            throw std::runtime_error("Windows optional component id must not be empty");
        for (char c : component.id) {
            if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-') {
                throw std::runtime_error("Windows optional component id contains an unsafe "
                                         "character: " +
                                         component.id);
            }
        }
        if (!componentIds.insert(component.id).second)
            throw std::runtime_error("duplicate Windows optional component id: " + component.id);
        validateSingleLineField(component.label, "Windows optional component label");
        validateSingleLineField(component.description, "Windows optional component description");
    }
    const uint64_t wizardPixels =
        static_cast<uint64_t>(layout.wizardImageWidth) * layout.wizardImageHeight;
    if ((layout.wizardImageWidth == 0) != (layout.wizardImageHeight == 0) ||
        wizardPixels > 4u * 1024u * 1024u || wizardPixels * 4u != layout.wizardImageRgba.size()) {
        throw std::runtime_error("Windows wizard branding image has invalid RGBA dimensions");
    }
    /// @brief Verify that a file references a declared optional component.
    /// @param file Package file entry whose component identifier is validated.
    /// @throws std::runtime_error If the entry names an unknown component.
    auto validateComponent = [&](const WindowsPackageFileEntry &file) {
        if (!file.componentId.empty() &&
            componentIds.find(file.componentId) == componentIds.end()) {
            throw std::runtime_error(
                "Windows install file references unknown optional component: " + file.componentId);
        }
    };
    for (const auto &dir : layout.installDirectories) {
        validateWindowsRelativePath(dir.relativePath, "Windows install directory path");
        validateStubPathFits(windowsRootProbeFor(layout, dir.root) + "\\" + dir.relativePath,
                             "Windows install directory path");
        addWindowsCaseFoldedPath(
            caseFoldedPaths, dir.root, dir.relativePath, "Windows install directory path");
    }
    for (const auto &file : layout.installFiles) {
        validateComponent(file);
        validateWindowsRelativePath(file.relativePath, "Windows install file path");
        validateStubPathFits(windowsRootProbeFor(layout, file.root) + "\\" + file.relativePath,
                             "Windows install file path");
        addWindowsCaseFoldedPath(
            caseFoldedPaths, file.root, file.relativePath, "Windows install file path");
    }
    for (const auto &file : layout.installedFiles) {
        validateComponent(file);
        validateWindowsRelativePath(file.relativePath, "Windows installed file path");
        validateStubPathFits(windowsRootProbeFor(layout, file.root) + "\\" + file.relativePath,
                             "Windows installed file path");
        addWindowsCaseFoldedPath(
            caseFoldedPaths, file.root, file.relativePath, "Windows installed file path");
    }
    if (layout.addToPath && !layout.pathRelativePath.empty()) {
        validateWindowsRelativePath(layout.pathRelativePath, "Windows PATH entry path");
        validateStubPathFits(rootProbe + "\\" + layout.pathRelativePath, "Windows PATH entry path");
    }
    if (!layout.fileAssociationExecutableRelativePath.empty()) {
        validateWindowsRelativePath(layout.fileAssociationExecutableRelativePath,
                                    "Windows file association executable path");
        validateStubPathFits(rootProbe + "\\" + layout.fileAssociationExecutableRelativePath,
                             "Windows file association executable path");
    }
    if (!layout.displayIconRelativePath.empty()) {
        validateWindowsRelativePath(layout.displayIconRelativePath, "Windows display icon path");
        validateStubPathFits(rootProbe + "\\" + layout.displayIconRelativePath,
                             "Windows display icon path");
    }
    for (const auto &file : layout.uninstallFiles) {
        validateComponent(file);
        validateWindowsRelativePath(file.relativePath, "Windows uninstall file path");
        validateStubPathFits(windowsRootProbeFor(layout, file.root) + "\\" + file.relativePath,
                             "Windows uninstall file path");
    }
    for (const auto &assoc : layout.fileAssociations) {
        validateSingleLineField(assoc.openCommandArguments,
                                "Windows file association command arguments");
        for (char c : assoc.openCommandArguments) {
            const unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20 || c == '"' || c == '&' || c == '|' || c == '<' || c == '>' || c == '^' ||
                c == '%') {
                throw std::runtime_error("Windows file association command arguments contain "
                                         "unsafe command-line syntax: " +
                                         assoc.openCommandArguments);
            }
        }
    }
}

/// @brief Return text lowercased for case-insensitive filename comparisons (LICENSE, README.MD).
/// Only transforms ASCII letters so it is safe for arbitrary UTF-8 filenames.
/// @param text Text to transform in place.
/// @return Lowercased copy.
std::string lowerAscii(std::string text) {
    for (char &c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

/// @brief Read a little-endian uint16 at @p off (caller must bounds-check).
/// @param data Buffer containing the field.
/// @param off Zero-based field offset.
/// @return Decoded host-order 16-bit value.
uint16_t rd16(const std::vector<uint8_t> &data, size_t off) {
    return static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
}

/// @brief Read a little-endian uint32 at @p off (caller must bounds-check).
/// @param data Buffer containing the field.
/// @param off Zero-based field offset.
/// @return Decoded host-order 32-bit value.
uint32_t rd32(const std::vector<uint8_t> &data, size_t off) {
    return static_cast<uint32_t>(data[off]) | (static_cast<uint32_t>(data[off + 1]) << 8) |
           (static_cast<uint32_t>(data[off + 2]) << 16) |
           (static_cast<uint32_t>(data[off + 3]) << 24);
}

/// @brief Return true if [off, off+len) lies entirely within @p data.
/// @param data Containing byte vector.
/// @param off Candidate range start.
/// @param len Candidate range length.
/// @return true when the range is bounded without overflow.
bool hasBytes(const std::vector<uint8_t> &data, size_t off, size_t len) {
    return off <= data.size() && len <= data.size() - off;
}

/// @brief Rotate a 32-bit value right by @p bits (SHA-256 round operation).
/// @param value Word to rotate.
/// @param bits Rotation distance in `[1, 31]`.
/// @return Circular right rotation of @p value.
uint32_t rotr32(uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

/// @brief Compute the SHA-256 of a buffer as a lowercase hex string.
/// @details Self-contained SHA-256 used to fingerprint payload files in the
///          installer's integrity manifest; kept local to avoid a runtime dep.
/// @param data Input buffer; may be null only when @p len is zero.
/// @param len Number of bytes to hash.
/// @return Digest encoded as 64 lowercase hexadecimal characters.
std::string sha256Hex(const uint8_t *data, size_t len) {
    static constexpr std::array<uint32_t, 64> k = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
        0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
        0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
        0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
        0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
        0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
        0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
        0xc67178f2u};
    uint32_t h[8] = {0x6a09e667u,
                     0xbb67ae85u,
                     0x3c6ef372u,
                     0xa54ff53au,
                     0x510e527fu,
                     0x9b05688cu,
                     0x1f83d9abu,
                     0x5be0cd19u};

    std::vector<uint8_t> msg;
    if (len != 0)
        msg.assign(data, data + len);
    const uint64_t bitLen = static_cast<uint64_t>(len) * 8ull;
    msg.push_back(0x80);
    while ((msg.size() % 64u) != 56u)
        msg.push_back(0);
    for (int i = 7; i >= 0; --i)
        msg.push_back(static_cast<uint8_t>((bitLen >> (i * 8)) & 0xffu));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64] = {};
        for (size_t i = 0; i < 16; ++i) {
            const size_t p = off + i * 4u;
            w[i] = (static_cast<uint32_t>(msg[p]) << 24) |
                   (static_cast<uint32_t>(msg[p + 1]) << 16) |
                   (static_cast<uint32_t>(msg[p + 2]) << 8) | static_cast<uint32_t>(msg[p + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (size_t i = 0; i < 64; ++i) {
            const uint32_t s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
            const uint32_t ch = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + s1 + ch + k[i] + w[i];
            const uint32_t s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::ostringstream os;
    os << std::hex << std::setfill('0');
    for (uint32_t word : h)
        os << std::setw(8) << word;
    return os.str();
}

/// @brief One PE section's RVA/size and file-offset/size, for RVA→offset mapping.
struct PeSectionInfo {
    uint32_t rva{0};         ///< Virtual address of the section.
    uint32_t virtualSize{0}; ///< Virtual size of the section.
    uint32_t rawOffset{0};   ///< File offset of the section's raw data.
    uint32_t rawSize{0};     ///< File size of the section's raw data.
};

/// @brief Minimal identifying info about a PE image: machine type and PE32+ flag.
struct PeExecutableInfo {
    uint16_t machine{0};  ///< COFF Machine field (0x8664 = AMD64, 0xAA64 = ARM64).
    bool pe32Plus{false}; ///< True when the optional header magic is 0x020B (PE32+).
};

/// @brief Parse just enough of a PE image to extract its machine type and bitness.
/// @param data Complete candidate image bytes.
/// @return PeExecutableInfo, or std::nullopt if the bytes are not a valid PE image.
std::optional<PeExecutableInfo> inspectPeExecutable(const std::vector<uint8_t> &data) {
    if (!hasBytes(data, 0, 64) || data[0] != 'M' || data[1] != 'Z')
        return std::nullopt;
    const size_t peOff = rd32(data, 60);
    if (!hasBytes(data, peOff, 24) || data[peOff] != 'P' || data[peOff + 1] != 'E' ||
        data[peOff + 2] != 0 || data[peOff + 3] != 0)
        return std::nullopt;
    const size_t coffOff = peOff + 4;
    const uint16_t optSize = rd16(data, coffOff + 16);
    const size_t optOff = coffOff + 20;
    if (!hasBytes(data, optOff, optSize) || optSize < 2)
        return std::nullopt;
    return PeExecutableInfo{rd16(data, coffOff), rd16(data, optOff) == 0x020B};
}

/// @brief Map an architecture string to its PE COFF Machine value.
/// @param arch Empty/default, `x64`, or `arm64`.
/// @return COFF machine identifier for the selected architecture.
/// @throws std::runtime_error for anything other than "" / "x64" / "arm64".
uint16_t windowsMachineForArch(const std::string &arch) {
    if (arch.empty() || arch == "x64")
        return 0x8664;
    if (arch == "arm64")
        return 0xAA64;
    throw std::runtime_error("unsupported Windows package architecture '" + arch + "'");
}

/// @brief Validate that the payload binary at @p path is a PE32+ for @p arch.
/// @param path Existing executable to read and inspect.
/// @param arch Requested package architecture.
/// @throws std::runtime_error if the file is not PE32+ or its machine type does
///         not match the requested architecture.
void validateWindowsPayloadExecutable(const fs::path &path, const std::string &arch) {
    const auto data = readFile(path);
    const auto info = inspectPeExecutable(data);
    if (!info || !info->pe32Plus)
        throw std::runtime_error("Windows package executable must be a PE32+ executable: " +
                                 zanna::filesystem::pathToUtf8(path));
    const uint16_t expected = windowsMachineForArch(arch);
    if (info->machine != expected) {
        std::ostringstream os;
        os << "Windows package executable architecture does not match selected architecture '"
           << (arch.empty() ? "x64" : arch) << "': " << zanna::filesystem::pathToUtf8(path);
        throw std::runtime_error(os.str());
    }
}

/// @brief Parse an unsigned decimal metadata field without locale or sign ambiguity.
/// @param text Candidate decimal text.
/// @return Parsed value, or `std::nullopt` for empty, invalid, or overflowing input.
std::optional<uint64_t> parseUnsignedDecimal(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    uint64_t value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9')
            return std::nullopt;
        const uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10U)
            return std::nullopt;
        value = value * 10U + digit;
    }
    return value;
}

/// @brief Validate the canonical Zanna Studio binary/buildinfo provenance pair.
/// @details Any staged Studio-owned asset opts into the component, but the component is
///          packageable only when its canonical executable and schema-1 build metadata are
///          present as ordinary files. The metadata must bind the exact PE bytes, size, and
///          selected target architecture.
/// @param manifest Staged toolchain inventory and provenance.
/// @param architecture Expected executable architecture.
/// @return True when the manifest contains a complete, validated Studio component.
/// @throws std::runtime_error If a partial or inconsistent Studio component is present.
bool validateZannaStudioArtifacts(const ToolchainInstallManifest &manifest,
                                  const std::string &architecture) {
    const ToolchainFileEntry *studioExecutable = nullptr;
    const ToolchainFileEntry *studioBuildInfo = nullptr;
    bool hasStudioAsset = false;
    for (const ToolchainFileEntry &file : manifest.files) {
        std::string normalized = lowerAscii(file.stagedRelativePath);
        std::replace(normalized.begin(), normalized.end(), '\\', '/');
        if (toolchainComponentForPath(normalized, false) == kComponentZannaStudio)
            hasStudioAsset = true;
        if (normalized == "bin/zannastudio.exe") {
            if (studioExecutable)
                throw std::runtime_error("duplicate canonical Zanna Studio executable");
            studioExecutable = &file;
        } else if (normalized == "bin/zannastudio.buildinfo") {
            if (studioBuildInfo)
                throw std::runtime_error("duplicate canonical Zanna Studio build metadata");
            studioBuildInfo = &file;
        }
    }
    if (!hasStudioAsset)
        return false;
    if (!studioExecutable || !studioBuildInfo) {
        throw std::runtime_error("Zanna Studio packaging requires bin/zannastudio.exe and "
                                 "bin/zannastudio.buildinfo");
    }
    if (studioExecutable->symlink || !studioExecutable->executable ||
        !fs::is_regular_file(studioExecutable->stagedAbsolutePath)) {
        throw std::runtime_error(
            "bin/zannastudio.exe must be an executable, non-symlink regular file");
    }
    if (studioBuildInfo->symlink || studioBuildInfo->executable ||
        !fs::is_regular_file(studioBuildInfo->stagedAbsolutePath)) {
        throw std::runtime_error(
            "bin/zannastudio.buildinfo must be a non-executable, non-symlink regular file");
    }

    const std::vector<uint8_t> executableBytes = readFile(studioExecutable->stagedAbsolutePath);
    if (executableBytes.empty() || studioExecutable->sizeBytes != executableBytes.size())
        throw std::runtime_error("Zanna Studio executable size disagrees with the stage manifest");
    validateWindowsPayloadExecutable(studioExecutable->stagedAbsolutePath, architecture);

    std::error_code sizeError;
    const uintmax_t buildInfoSize = fs::file_size(studioBuildInfo->stagedAbsolutePath, sizeError);
    if (sizeError || buildInfoSize == 0 || buildInfoSize > 16384U ||
        studioBuildInfo->sizeBytes != buildInfoSize) {
        throw std::runtime_error("Zanna Studio build metadata has an invalid size");
    }
    const std::vector<uint8_t> buildInfoBytes = readFile(studioBuildInfo->stagedAbsolutePath);
    if (std::find(buildInfoBytes.begin(), buildInfoBytes.end(), uint8_t{0}) != buildInfoBytes.end())
        throw std::runtime_error("Zanna Studio build metadata contains a NUL byte");
    const std::string buildInfo(buildInfoBytes.begin(), buildInfoBytes.end());
    std::istringstream input(buildInfo);
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("Zanna Studio build metadata is empty");
    }
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line != "Zanna Studio " + manifest.productVersion) {
        throw std::runtime_error(
            "Zanna Studio build metadata version does not match the toolchain package");
    }

    std::map<std::string, std::string> fields;
    while (std::getline(input, line)) {
        if (line.size() > 4096U)
            throw std::runtime_error("Zanna Studio build metadata contains an oversized line");
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        const size_t separator = line.find(": ");
        if (separator == std::string::npos || separator == 0 || separator + 2U == line.size()) {
            throw std::runtime_error("Zanna Studio build metadata contains an invalid field");
        }
        const std::string key = line.substr(0, separator);
        const std::string value = line.substr(separator + 2U);
        if (!fields.emplace(key, value).second)
            throw std::runtime_error("Zanna Studio build metadata contains a duplicate field");
    }
    for (const std::string_view required :
         {"Schema", "Build", "Source", "Architecture", "Size", "SHA256", "Output", "Zanna"}) {
        if (fields.find(std::string(required)) == fields.end())
            throw std::runtime_error("Zanna Studio build metadata is missing field " +
                                     std::string(required));
    }
    if (fields.size() != 8U)
        throw std::runtime_error("Zanna Studio build metadata contains an unknown field");
    if (fields["Schema"] != "1")
        throw std::runtime_error("Zanna Studio build metadata uses an unsupported schema");
    const std::string expectedArchitecture = architecture.empty() ? "x64" : architecture;
    if (fields["Architecture"] != expectedArchitecture)
        throw std::runtime_error("Zanna Studio build metadata architecture does not match package");
    const std::optional<uint64_t> recordedSize = parseUnsignedDecimal(fields["Size"]);
    if (!recordedSize || *recordedSize != executableBytes.size())
        throw std::runtime_error("Zanna Studio build metadata size does not match executable");
    const std::string &recordedHash = fields["SHA256"];
    if (recordedHash.size() != 64U ||
        !std::all_of(
            recordedHash.begin(),
            recordedHash.end(),
            /// @brief Test whether one metadata hash character is lowercase hexadecimal.
            /// @param ch Character to inspect.
            /// @return `true` for an ASCII decimal digit or a lowercase `a` through `f`.
            [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); }) ||
        recordedHash != sha256Hex(executableBytes.data(), executableBytes.size())) {
        throw std::runtime_error("Zanna Studio build metadata SHA-256 does not match executable");
    }
    return true;
}

/// @brief Rebind validated Studio metadata to the exact PE bytes placed in the payload.
/// @details Nested Authenticode signing can change the executable after the staging buildinfo was
///          produced. Updating only the already-validated Size and SHA256 fields preserves the
///          provenance fields while keeping the installed pair internally consistent.
/// @param original Validated schema-1 build-info bytes.
/// @param packagedExecutable Final executable bytes after optional signing.
/// @return Rewritten build-info bytes with updated size and SHA-256 fields.
/// @throws std::runtime_error If either required field is absent or duplicated.
std::vector<uint8_t> bindZannaStudioBuildInfo(const std::vector<uint8_t> &original,
                                              const std::vector<uint8_t> &packagedExecutable) {
    std::istringstream input(std::string(original.begin(), original.end()));
    std::ostringstream output;
    std::string line;
    bool replacedSize = false;
    bool replacedHash = false;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("Size: ", 0) == 0) {
            if (replacedSize)
                throw std::runtime_error("duplicate Zanna Studio Size metadata during rebinding");
            output << "Size: " << packagedExecutable.size() << '\n';
            replacedSize = true;
        } else if (line.rfind("SHA256: ", 0) == 0) {
            if (replacedHash)
                throw std::runtime_error("duplicate Zanna Studio SHA256 metadata during rebinding");
            output << "SHA256: " << sha256Hex(packagedExecutable.data(), packagedExecutable.size())
                   << '\n';
            replacedHash = true;
        } else {
            output << line << '\n';
        }
    }
    if (!replacedSize || !replacedHash)
        throw std::runtime_error("cannot rebind incomplete Zanna Studio build metadata");
    const std::string rebound = output.str();
    return {rebound.begin(), rebound.end()};
}

/// @brief Translate a PE relative virtual address to a file offset.
/// @details Searches @p sections for the one containing @p rva and maps it into
///          that section's raw data. Returns nullopt when no section covers the
///          RVA or the resulting offset would exceed @p fileSize.
/// @param sections Parsed section address/file ranges.
/// @param rva Relative virtual address to translate.
/// @param fileSize Total PE file size used for the final bounds check.
/// @return Corresponding raw-file offset, or `std::nullopt` when unmappable.
std::optional<size_t> peRvaToFileOffset(const std::vector<PeSectionInfo> &sections,
                                        uint32_t rva,
                                        size_t fileSize) {
    for (const auto &sec : sections) {
        if (sec.rawSize == 0)
            continue;
        const uint32_t sectionSpan = std::max(sec.virtualSize, sec.rawSize);
        if (rva >= sec.rva && rva - sec.rva < sectionSpan) {
            const uint32_t delta = rva - sec.rva;
            if (delta >= sec.rawSize)
                return std::nullopt;
            const size_t off = static_cast<size_t>(sec.rawOffset) + static_cast<size_t>(delta);
            if (off < fileSize)
                return off;
        }
    }
    return std::nullopt;
}

/// @brief Read a NUL-terminated printable-ASCII string from @p data at @p off.
/// @param data Buffer containing the candidate string.
/// @param off Zero-based start offset.
/// @return The string, or "" if a non-printable byte or an unterminated run is
///         encountered (used to read DLL name strings from the import table).
std::string readPeAsciiZ(const std::vector<uint8_t> &data, size_t off) {
    std::string out;
    while (off < data.size() && data[off] != 0) {
        const unsigned char ch = data[off++];
        if (ch < 0x20 || ch > 0x7e)
            return {};
        out.push_back(static_cast<char>(ch));
    }
    return off < data.size() ? out : std::string{};
}

/// @brief Parse a PE32+ import directory and return the imported DLL names.
/// @details Walks the import data directory's IMAGE_IMPORT_DESCRIPTOR array,
///          mapping each Name RVA to a file offset and reading the DLL string.
///          Returns an empty vector if the image is not a parseable PE32+.
/// @param data Complete candidate PE image bytes.
/// @return Lowercase imported DLL names, or an empty vector on invalid/absent imports.
std::vector<std::string> importedDllNamesFromPeImpl(const std::vector<uint8_t> &data) {
    if (!hasBytes(data, 0, 64) || data[0] != 'M' || data[1] != 'Z')
        return {};
    const size_t peOff = rd32(data, 60);
    if (!hasBytes(data, peOff, 24) || data[peOff] != 'P' || data[peOff + 1] != 'E' ||
        data[peOff + 2] != 0 || data[peOff + 3] != 0)
        return {};

    const size_t coffOff = peOff + 4;
    const uint16_t numSections = rd16(data, coffOff + 2);
    const uint16_t optSize = rd16(data, coffOff + 16);
    const size_t optOff = coffOff + 20;
    if (!hasBytes(data, optOff, optSize) || rd16(data, optOff) != 0x020B || optSize < 128)
        return {};
    const uint32_t importRva = rd32(data, optOff + 112 + 8);
    const uint32_t importSize = rd32(data, optOff + 112 + 12);
    if (importRva == 0 || importSize == 0)
        return {};

    const size_t secOff = optOff + optSize;
    if (!hasBytes(data, secOff, static_cast<size_t>(numSections) * 40u))
        return {};
    std::vector<PeSectionInfo> sections;
    sections.reserve(numSections);
    for (uint16_t i = 0; i < numSections; ++i) {
        const size_t cur = secOff + static_cast<size_t>(i) * 40u;
        sections.push_back({rd32(data, cur + 12),
                            rd32(data, cur + 8),
                            rd32(data, cur + 20),
                            rd32(data, cur + 16)});
    }

    const auto importOff = peRvaToFileOffset(sections, importRva, data.size());
    if (!importOff)
        return {};
    const uint64_t importEnd64 = static_cast<uint64_t>(*importOff) + importSize;
    if (importSize < 20 || importEnd64 > data.size())
        return {};
    const size_t importEnd = static_cast<size_t>(importEnd64);

    std::vector<std::string> names;
    bool sawTerminator = false;
    for (size_t desc = *importOff; desc <= importEnd - 20; desc += 20) {
        const uint32_t originalThunk = rd32(data, desc);
        const uint32_t nameRva = rd32(data, desc + 12);
        const uint32_t firstThunk = rd32(data, desc + 16);
        if (originalThunk == 0 && nameRva == 0 && firstThunk == 0) {
            sawTerminator = true;
            break;
        }
        const auto nameOff = peRvaToFileOffset(sections, nameRva, data.size());
        if (!nameOff)
            return {};
        std::string dll = lowerAscii(readPeAsciiZ(data, *nameOff));
        if (dll.empty())
            return {};
        names.push_back(std::move(dll));
    }
    if (!sawTerminator)
        return {};
    return names;
}

/// @brief Decide whether a DLL is supplied by the supported Windows operating system.
/// @details Compiler runtimes such as vcruntime140.dll and msvcp140.dll are deliberately
///          excluded: an installer must carry those files app-locally instead of assuming
///          that an unrelated product installed a compatible redistributable globally.
///          API-set forwarders and Windows inbox graphics/input libraries remain system DLLs.
/// @param dll Lowercase DLL filename.
/// @return true when the dependency may be satisfied by supported Windows itself.
bool isKnownWindowsSystemDll(const std::string &dll) {
    static const std::set<std::string> exact = {
        "advapi32.dll",  "bcrypt.dll",      "cfgmgr32.dll",      "combase.dll",  "comctl32.dll",
        "crypt32.dll",   "d3d11.dll",       "d3d12.dll",         "dcomp.dll",    "dwmapi.dll",
        "dxgi.dll",      "gdi32.dll",       "imm32.dll",         "iphlpapi.dll", "kernel32.dll",
        "msvcrt.dll",    "ntdll.dll",       "ole32.dll",         "oleaut32.dll", "propsys.dll",
        "rpcrt4.dll",    "rstrtmgr.dll",    "secur32.dll",       "setupapi.dll", "shell32.dll",
        "shlwapi.dll",   "user32.dll",      "uxtheme.dll",       "version.dll",  "winmm.dll",
        "winhttp.dll",   "winspool.drv",    "ws2_32.dll",        "wtsapi32.dll", "ucrtbase.dll",
        "xinput1_4.dll", "xinput9_1_0.dll", "d3dcompiler_47.dll"};
    if (exact.find(dll) != exact.end())
        return true;
    return dll.rfind("api-ms-win-", 0) == 0 || dll.rfind("ext-ms-win-", 0) == 0;
}

/// @brief Return true when @p dll names an app-local MSVC release or debug runtime.
/// @details Runtime families begin with a stable prefix followed by a toolset number.
///          Requiring the first suffix character to be numeric avoids misclassifying
///          application DLLs such as `msvcp_plugin.dll`.
/// @param dll Lowercase DLL filename.
/// @return true for recognized numbered MSVC runtime families.
bool isAppLocalMsvcRuntimeDll(const std::string &dll) {
    if (dll.size() <= 4 || dll.substr(dll.size() - 4) != ".dll")
        return false;
    const std::string_view stem(dll.data(), dll.size() - 4);
    for (const std::string_view prefix : {std::string_view("vcruntime"),
                                          std::string_view("msvcp"),
                                          std::string_view("concrt"),
                                          std::string_view("vcomp")}) {
        if (stem.size() > prefix.size() && stem.substr(0, prefix.size()) == prefix &&
            std::isdigit(static_cast<unsigned char>(stem[prefix.size()]))) {
            return true;
        }
    }
    return false;
}

/// @brief Return whether a staged file is a Zanna-owned PE eligible for nested signing.
/// @details Microsoft compiler runtime DLLs retain their original Microsoft signatures.
///          Non-PE fixture files and ordinary data with an executable-looking suffix are
///          not passed to the signer.
/// @param logicalName Install-relative payload name.
/// @param data Candidate file bytes.
/// @return true for a Zanna-owned `.exe`/`.dll` containing a parseable PE image.
bool shouldSignWindowsPayloadPe(std::string_view logicalName, const std::vector<uint8_t> &data) {
    const std::string lowerName = lowerAscii(std::string(logicalName));
    const size_t slash = lowerName.find_last_of("/\\");
    const std::string_view leaf = slash == std::string::npos
                                      ? std::string_view(lowerName)
                                      : std::string_view(lowerName).substr(slash + 1U);
    const size_t dot = leaf.find_last_of('.');
    const std::string ext =
        dot == std::string_view::npos ? std::string{} : std::string(leaf.substr(dot));
    if (ext != ".exe" && ext != ".dll")
        return false;
    if (ext == ".dll" && isAppLocalMsvcRuntimeDll(std::string(leaf)))
        return false;
    return inspectPeExecutable(data).has_value();
}

/// @brief Apply @p signer and prove that it preserved a valid PE32+ architecture.
/// @param signer Optional signing callback.
/// @param logicalName Install-relative name passed to the signer and diagnostics.
/// @param unsignedData Original file bytes.
/// @param arch Required output architecture.
/// @return Signed bytes, or the unchanged input when signing is disabled/inapplicable.
/// @throws std::runtime_error when signing fails, returns no bytes, or changes the image type.
std::vector<uint8_t> signWindowsPayloadPe(const WindowsPeSigner &signer,
                                          std::string_view logicalName,
                                          const std::vector<uint8_t> &unsignedData,
                                          const std::string &arch) {
    if (!signer || !shouldSignWindowsPayloadPe(logicalName, unsignedData))
        return unsignedData;

    std::vector<uint8_t> signedData;
    try {
        signedData = signer(logicalName, unsignedData);
    } catch (const std::exception &ex) {
        throw std::runtime_error("failed to Authenticode-sign nested Windows PE '" +
                                 std::string(logicalName) + "': " + ex.what());
    }
    if (signedData.empty()) {
        throw std::runtime_error("nested Windows PE signer returned no bytes for '" +
                                 std::string(logicalName) + "'");
    }
    const auto info = inspectPeExecutable(signedData);
    if (!info || !info->pe32Plus || info->machine != windowsMachineForArch(arch)) {
        throw std::runtime_error("nested Windows PE signer changed the image architecture for '" +
                                 std::string(logicalName) + "'");
    }
    return signedData;
}

/// @brief Require every imported MSVC runtime to exist beside its staged PE importer.
/// @details Windows resolves app-local runtime DLLs from the executable directory. The
///          manifest is case-normalized for comparison, while diagnostics retain the
///          original importer and expected path. Unparseable non-PE fixtures are ignored;
///          staged object-format validation remains responsible for required executables.
/// @param manifest Staged Windows toolchain inventory to inspect.
/// @throws std::runtime_error If an imported MSVC runtime is absent beside its importer.
void validateToolchainMsvcRuntimeClosure(const ToolchainInstallManifest &manifest) {
    std::set<std::string> stagedPaths;
    for (const ToolchainFileEntry &file : manifest.files)
        stagedPaths.insert(lowerAscii(file.stagedRelativePath));

    for (const ToolchainFileEntry &file : manifest.files) {
        if (file.symlink)
            continue;
        const std::string ext =
            lowerAscii(zanna::filesystem::pathToUtf8(file.stagedAbsolutePath.extension()));
        if (ext != ".exe" && ext != ".dll")
            continue;
        const auto imports = importedDllNamesFromPeImpl(readFile(file.stagedAbsolutePath));
        const fs::path parent =
            zanna::filesystem::pathFromUtf8(file.stagedRelativePath).parent_path();
        for (const std::string &dll : imports) {
            if (!isAppLocalMsvcRuntimeDll(dll))
                continue;
            const std::string expected = zanna::filesystem::genericPathToUtf8(parent / dll);
            if (stagedPaths.find(lowerAscii(expected)) == stagedPaths.end()) {
                throw std::runtime_error("Windows toolchain payload '" + file.stagedRelativePath +
                                         "' imports app-local MSVC "
                                         "runtime '" +
                                         dll + "' but '" + expected +
                                         "' is missing from the stage");
            }
        }
    }
}

/// @brief Find @p filename under @p dir, trying exact, adjacent, then bounded recursive search.
/// @details Windows applications often stage plugin or delay-load DLLs in subdirectories.
///          The recursive pass is capped so an accidental large project tree does not make
///          packaging unbounded.
/// @param dir Executable directory and bounded search root.
/// @param filename Lowercase DLL leaf name to locate case-insensitively.
/// @return The matching path, or std::nullopt when no file matches.
std::optional<fs::path> findLocalDllCaseInsensitive(const fs::path &dir,
                                                    const std::string &filename) {
    const fs::path direct = dir / filename;
    std::error_code directEc;
    if (fs::is_regular_file(direct, directEc))
        return direct;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        std::error_code statusEc;
        if (!fs::is_regular_file(entry.path(), statusEc))
            continue;
        if (lowerAscii(zanna::filesystem::genericPathToUtf8(entry.path().filename())) == filename)
            return entry.path();
    }
    constexpr size_t kMaxRecursiveDllSearchEntries = 4096;
    size_t visited = 0;
    ec.clear();
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    while (!ec && it != end && visited++ < kMaxRecursiveDllSearchEntries) {
        const fs::path candidate = it->path();
        std::error_code statusEc;
        if (fs::is_regular_file(candidate, statusEc) &&
            lowerAscii(zanna::filesystem::genericPathToUtf8(candidate.filename())) == filename) {
            return candidate;
        }
        it.increment(ec);
    }
    return std::nullopt;
}

/// @brief Return a stable install path for a discovered DLL dependency.
/// @details Dependencies found under the executable directory keep their
///          relative subdirectory path so plugin folders and similarly named
///          DLLs do not collapse into the install root. Paths outside that tree
///          fall back to the filename, which is used only for explicit manifest
///          dependencies.
/// @param exeDir Directory containing the packaged executable.
/// @param dllPath Resolved DLL source path.
/// @return Sanitized install-relative path.
std::string dllInstallRelativePath(const fs::path &exeDir, const fs::path &dllPath) {
    std::error_code ec;
    fs::path rel = fs::relative(dllPath, exeDir, ec);
    const std::string relText = zanna::filesystem::genericPathToUtf8(rel);
    if (ec || rel.empty() || relText == ".." || relText.rfind("../", 0) == 0)
        rel = dllPath.filename();
    return sanitizePackageRelativePath(zanna::filesystem::genericPathToUtf8(rel),
                                       "Windows DLL dependency path");
}

/// @brief Transitively collect non-system DLLs that ship next to @p exePath.
/// @details Breadth-first walks the import tables of the executable and each
///          discovered local DLL, skipping known redistributable/system DLLs, and
///          returns the adjacent files that must be bundled into the installer
///          together with the install-relative paths they should keep.
/// @param exePath Windows PE executable being packaged.
/// @return Local DLL dependencies with resolved source paths and install targets.
std::vector<WindowsDllDependency> discoverAdjacentDllDependencies(const fs::path &exePath) {
    std::vector<WindowsDllDependency> deps;
    const fs::path dir = exePath.parent_path();
    std::set<std::string> seen;
    std::vector<fs::path> queue{exePath};
    for (size_t index = 0; index < queue.size(); ++index) {
        const fs::path current = queue[index];
        const auto imports = importedDllNamesFromPeImpl(readFile(current));
        for (const auto &dll : imports) {
            if (!seen.insert(dll).second)
                continue;
            if (isKnownWindowsSystemDll(dll))
                continue;
            const auto local = findLocalDllCaseInsensitive(dir, dll);
            if (local) {
                deps.push_back({*local, dllInstallRelativePath(dir, *local)});
                queue.push_back(*local);
                continue;
            }
            throw std::runtime_error("Windows package executable imports non-system DLL '" + dll +
                                     "' but no adjacent DLL was found next to " +
                                     zanna::filesystem::pathToUtf8(exePath));
        }
    }
    return deps;
}

/// @brief Convert a file association extension into a ProgID-safe component.
/// @details Windows ProgIDs allow alphanumerics plus '.', '_', and '-' as
///          separators/components. File extensions can contain characters such
///          as '+', so this helper preserves the association while replacing
///          unsupported ProgID component characters with '_'.
/// @param assoc File association whose extension has already passed package validation.
/// @return Non-empty identifier component without a leading dot.
std::string windowsProgIdExtensionComponent(const FileAssoc &assoc) {
    std::string ext = assoc.extension;
    while (!ext.empty() && ext.front() == '.')
        ext.erase(ext.begin());
    if (ext.empty())
        throw std::runtime_error("Windows file association extension must not be empty");
    for (char &c : ext) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-')
            c = '_';
    }
    return ext;
}

/// @brief Return an 8-character stable hexadecimal FNV-1a hash.
/// @details Used only to shorten generated Windows ProgID bases while keeping
///          a deterministic suffix that avoids obvious collisions between long
///          executable names.
/// @param text Input text to hash.
/// @return Lowercase hexadecimal hash suffix.
std::string shortStableHexHash(std::string_view text) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : text) {
        hash ^= c;
        hash *= 16777619u;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(8) << hash;
    return out.str();
}

/// @brief Build a valid default Windows ProgID base for an executable.
/// @details Windows ProgID bases are capped at 39 characters by Zanna's registry
///          policy. The generated `zanna.<exe>` base is truncated and hash-
///          suffixed when needed, while preserving alphanumeric/underscore/dash
///          characters already produced by normalizeExecName().
/// @param exec Normalized executable stem.
/// @return Valid ProgID base that passes validateWindowsProgIdBase().
std::string defaultWindowsProgIdBase(const std::string &exec) {
    std::string component = exec.empty() ? "app" : exec;
    constexpr size_t kPrefixLen = 6; // "zanna."
    constexpr size_t kHashLen = 9;   // "-" + 8 hex chars
    constexpr size_t kMaxBaseLen = 39;
    if (kPrefixLen + component.size() > kMaxBaseLen) {
        const size_t keep = kMaxBaseLen - kPrefixLen - kHashLen;
        component = component.substr(0, keep) + "-" + shortStableHexHash(component);
    }
    std::string base = "zanna." + component;
    validateWindowsProgIdBase(base, "Windows file association ProgID base");
    return base;
}

/// @brief Build the Windows ProgID string for a file association in app packages.
/// Format: "<pkg.identifier>.<ext>" — e.g. "com.example.myapp.zia".
/// ProgIDs are registered in HKEY_CLASSES_ROOT and link the extension to the app.
/// @param pkg Package metadata supplying an optional identifier override.
/// @param exec Normalized executable stem used for a generated default base.
/// @param assoc Validated file association.
/// @return Validated full ProgID for the association.
std::string windowsProgIdFor(const PackageConfig &pkg,
                             const std::string &exec,
                             const FileAssoc &assoc) {
    std::string base = pkg.identifier.empty() ? defaultWindowsProgIdBase(exec) : pkg.identifier;
    validateWindowsProgIdBase(base, "Windows file association ProgID base");
    validateFileAssociation(assoc.extension, assoc.description, assoc.mimeType);
    const std::string progId = base + "." + windowsProgIdExtensionComponent(assoc);
    validateWindowsProgIdBase(progId, "Windows file association ProgID");
    return progId;
}

/// @brief Build a %ProgramFiles%\<installDir>[\<leaf>] path string for use in .lnk
/// shortcut files and installer metadata. The %ProgramFiles% token is expanded
/// by Windows at shortcut resolution time, so the path is machine-independent.
std::string windowsInstallEnvPath(const std::string &installDir,
                                  bool perUserInstall,
                                  const std::string &leaf = {}) {
    std::string path = perUserInstall ? "%LocalAppData%\\" : "%ProgramFiles%\\";
    path += installDir;
    if (!leaf.empty())
        path += "\\" + leaf;
    return path;
}

/// @brief Wrap @p path in double quotes for embedding in a command line.
/// @param path Literal path that must not contain quotes.
/// @return Double-quoted path.
/// @throws std::runtime_error if @p path itself contains a double quote.
std::string windowsQuotedPath(const std::string &path) {
    if (path.find('"') != std::string::npos)
        throw std::runtime_error("Windows shortcut path must not contain quotes: " + path);
    return "\"" + path + "\"";
}

/// @brief Generate the "Zanna Developer Prompt" .bat with CLI and CMake discovery enabled.
std::string toolchainDeveloperPromptScript() {
    std::ostringstream os;
    os << "@echo off\r\n"
       << "for %%I in (\"%~dp0..\") do set \"ZANNA_HOME=%%~fI\"\r\n"
       << "set \"PATH=%ZANNA_HOME%\\bin;%PATH%\"\r\n"
       << "set \"Zanna_DIR=%ZANNA_HOME%\\lib\\cmake\\Zanna\"\r\n"
       << "set \"CMAKE_PREFIX_PATH=%ZANNA_HOME%;%CMAKE_PREFIX_PATH%\"\r\n"
       << "if not exist \"%USERPROFILE%\" goto prompt\r\n"
       << "cd /d \"%USERPROFILE%\"\r\n"
       << ":prompt\r\n"
       << "echo Zanna developer environment\r\n"
       << "echo ZANNA_HOME=%ZANNA_HOME%\r\n"
       << "echo Zanna_DIR=%Zanna_DIR%\r\n"
       << "zanna --version\r\n";
    return os.str();
}

/// @brief Generate the .bat that installs the bundled VS Code (.vsix) extension.
std::string toolchainVSCodeInstallScript() {
    return "@echo off\r\n"
           "setlocal\r\n"
           "set \"ZANNA_HOME=%~dp0..\"\r\n"
           "set \"VSIX_DIR=%ZANNA_HOME%\\share\\zanna\\vscode\"\r\n"
           "set \"VSIX=\"\r\n"
           "for %%F in (\"%VSIX_DIR%\\*.vsix\") do set \"VSIX=%%~fF\"\r\n"
           "if not defined VSIX (\r\n"
           "  echo No packaged VS Code extension was found under \"%VSIX_DIR%\".\r\n"
           "  echo Build misc\\editors\\vscode\\zia\\zia-language-*.vsix before packaging to "
           "install it here.\r\n"
           "  exit /b 2\r\n"
           ")\r\n"
           "set \"CODE_CMD=\"\r\n"
           "for %%F in (code.cmd code.exe code) do if not defined CODE_CMD (\r\n"
           "  for /f \"delims=\" %%C in ('where %%F 2^>nul') do if not defined CODE_CMD set "
           "\"CODE_CMD=%%C\"\r\n"
           ")\r\n"
           "if not defined CODE_CMD (\r\n"
           "  echo Visual Studio Code command-line launcher was not found on PATH.\r\n"
           "  exit /b 3\r\n"
           ")\r\n"
           "\"%CODE_CMD%\" --install-extension \"%VSIX%\" --force\r\n";
}

/// @brief Generate the prerequisites README text bundled with the installer.
/// @param installDirName Validated default installation-directory leaf.
/// @return CRLF-terminated guidance with the selected user/machine root examples.
std::string toolchainWindowsPrerequisitesReadme(std::string_view installDirName) {
    std::string readme =
        "Zanna Windows developer installation\r\n"
        "\r\n"
        "The native setup program is self-contained: it does not invoke PowerShell, fetch "
        "packages, or require a separately installed Microsoft C++ redistributable. "
        "Architecture-matched runtime DLLs are installed next to the Zanna tools.\r\n"
        "\r\n"
        "Git, CMake, Ninja, Visual Studio C++, the Windows SDK, VS Code, and Windows "
        "Terminal are optional companions. Setup reports what it detects but never downloads "
        "or changes those products. Native code generation still requires an appropriate "
        "compiler, linker, and Windows SDK for the selected backend.\r\n"
        "\r\n"
        "After setup, open a new terminal before relying on PATH changes. The default "
        "per-user install root for this package is %LOCALAPPDATA%\\Programs\\";
    readme += installDirName;
    readme += ", and its machine-scope root is %ProgramFiles%\\";
    readme += installDirName;
    readme +=
        ". Setup also supports a validated custom folder.\r\n"
        "\r\n"
        "The Zanna Developer Prompt configures ZANNA_HOME, PATH, Zanna_DIR, and "
        "CMAKE_PREFIX_PATH so CMake projects can use find_package(Zanna CONFIG REQUIRED).\r\n"
        "\r\n"
        "Start Menu shortcuts include a Zanna developer prompt, Zanna Studio, and the VS Code "
        "extension installer when a verified .vsix was packaged. Settings > Apps supports "
        "Modify, Repair, and Uninstall. Direct uninstall.exe removal safely hands off to the "
        "verified maintenance cache before deleting the install directory.\r\n";
    return readme;
}

/// @brief Estimate Add/Remove Programs installed size from files installed on disk.
/// @param layout Final package layout and file sizes.
/// @return Rounded-up KiB total, saturated at `UINT32_MAX`.
uint32_t estimatedInstalledSizeKb(const WindowsPackageLayout &layout) {
    uint64_t total = 0;
    const auto &files = layout.installedFiles.empty() ? layout.installFiles : layout.installedFiles;
    for (const auto &file : files) {
        if (file.root == WindowsInstallRoot::InstallDir)
            checkedAddU64(total, file.sizeBytes, "Windows installed size");
    }
    const uint64_t kb = roundedKiB(total, "Windows installed size");
    return kb > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(kb);
}

/// @brief Pick the embedded UAC manifest: asInvoker for per-user installs,
///        admin-elevation otherwise.
/// @param layout Package scope determining the requested execution level.
/// @param minOsWindows Minimum supported Windows version encoded in the manifest.
/// @return Complete generated application-manifest XML.
std::string windowsManifestForLayout(const WindowsPackageLayout &layout,
                                     const std::string &minOsWindows) {
    return layout.perUserInstall ? generateAsInvokerManifest(minOsWindows)
                                 : generateUacManifest(minOsWindows);
}

/// @brief Parse a Windows VERSIONINFO numeric core into a {a,b,c,d} array.
/// @details Package versions may include a semver-style suffix such as
///          `1.2.3-beta`; the PE VERSIONINFO numeric fields can only store the
///          leading dotted numeric core. This parser accepts one to four numeric
///          components followed either by end-of-string or by `-`/`+` suffix
///          metadata. Other malformed versions are rejected instead of being
///          silently truncated.
/// @param version Package version text.
/// @return Four 16-bit numeric components, zero-filling omitted trailing parts.
/// @throws std::runtime_error If the numeric core is malformed or out of range.
std::array<uint16_t, 4> windowsVersionPartsForResource(const std::string &version) {
    std::array<uint16_t, 4> parts{0, 0, 0, 0};
    if (version.empty())
        throw std::runtime_error("Windows VERSIONINFO version must not be empty");
    std::vector<uint32_t> parsed;
    size_t pos = 0;
    while (pos < version.size()) {
        if (parsed.size() == parts.size()) {
            if (version[pos] == '-' || version[pos] == '+')
                break;
            throw std::runtime_error("Windows VERSIONINFO version must have at most 4 numeric "
                                     "components: " +
                                     version);
        }
        if (!std::isdigit(static_cast<unsigned char>(version[pos]))) {
            if (!parsed.empty() && (version[pos] == '-' || version[pos] == '+'))
                break;
            throw std::runtime_error("Windows VERSIONINFO version must start with a dotted "
                                     "numeric core: " +
                                     version);
        }
        uint32_t value = 0;
        while (pos < version.size() && std::isdigit(static_cast<unsigned char>(version[pos]))) {
            const uint32_t digit = static_cast<uint32_t>(version[pos] - '0');
            if (value > (65535u - digit) / 10u) {
                throw std::runtime_error("Windows VERSIONINFO version component exceeds 65535: " +
                                         version);
            }
            value = value * 10u + digit;
            ++pos;
        }
        parsed.push_back(value);
        if (pos >= version.size())
            break;
        if (version[pos] == '.') {
            ++pos;
            if (pos >= version.size() || !std::isdigit(static_cast<unsigned char>(version[pos]))) {
                throw std::runtime_error("Windows VERSIONINFO version has an empty numeric "
                                         "component: " +
                                         version);
            }
            continue;
        }
        if (version[pos] == '-' || version[pos] == '+')
            break;
        throw std::runtime_error("Windows VERSIONINFO version has invalid suffix: " + version);
    }
    if (parsed.empty()) {
        throw std::runtime_error("Windows VERSIONINFO version must start with a numeric core: " +
                                 version);
    }
    if (parsed.size() > parts.size()) {
        throw std::runtime_error("Windows VERSIONINFO version must have at most 4 numeric "
                                 "components: " +
                                 version);
    }
    for (size_t i = 0; i < parsed.size(); ++i)
        parts[i] = static_cast<uint16_t>(parsed[i]);
    return parts;
}

/// @brief Build a PEVersionInfo (RT_VERSION resource) from a package layout.
/// @details Populates file/product versions from the layout version and the
///          publisher/display-name strings; the description is the display name
///          optionally suffixed (e.g. " Uninstaller").
/// @param layout Package layout providing version, publisher, and display name.
/// @param filename Internal/original filename to record.
/// @param descriptionSuffix Optional suffix appended to the file description.
/// @return A populated, enabled PEVersionInfo.
PEVersionInfo windowsVersionInfoForLayout(const WindowsPackageLayout &layout,
                                          const std::string &filename,
                                          const std::string &descriptionSuffix) {
    const std::string version = layout.version.empty() ? "0.0.0" : layout.version;
    PEVersionInfo info;
    info.enabled = true;
    info.fileVersion = windowsVersionPartsForResource(version);
    info.productVersion = info.fileVersion;
    info.companyName = layout.publisher;
    info.fileDescription = descriptionSuffix.empty() ? layout.displayName
                                                     : layout.displayName + " " + descriptionSuffix;
    info.fileVersionText = version;
    info.internalName = filename;
    info.originalFilename = filename;
    info.productName = layout.displayName;
    info.productVersionText = version;
    return info;
}

/// @brief Convert a byte buffer to a string without transcoding.
/// @details Installer metadata files are treated as byte-preserving text. This
///          helper keeps existing newline and encoding bytes intact for license
///          display and fallback metadata.
/// @param data Raw file bytes.
/// @return String containing the same bytes, or an empty string for empty input.
std::string textFromBytes(const std::vector<uint8_t> &data) {
    if (data.empty())
        return {};
    return std::string(reinterpret_cast<const char *>(data.data()), data.size());
}

/// @brief Resolve the license text shown by the Windows installer wizard.
/// @details Prefers the explicit package-license-file manifest entry, then
///          common project LICENSE files, then the SPDX package-license field.
///          A GPL-3.0-only fallback keeps existing installer metadata non-empty.
/// @param projectRoot Root used to resolve project-relative metadata files.
/// @param pkg Package metadata.
/// @return License or license-summary text for installer display.
std::string appLicenseTextFor(const std::string &projectRoot, const PackageConfig &pkg) {
    const fs::path root = zanna::filesystem::pathFromUtf8(projectRoot);
    if (!pkg.licenseFilePath.empty())
        return readPackageTextFile(root, pkg.licenseFilePath, "package license file");
    for (const char *name : {"LICENSE", "LICENSE.txt"}) {
        const fs::path candidate = root / name;
        if (fs::is_regular_file(candidate))
            return textFromBytes(readFile(candidate));
    }
    if (!pkg.license.empty())
        return pkg.license;
    return "GPL-3.0-only";
}

/// @brief Resolve the Windows publisher shown in ARP and VERSIONINFO.
/// @details The manifest can provide a Windows-specific publisher override.
///          Otherwise Zanna falls back to package-author, then the display name
///          so release installers do not emit blank Publisher/Company fields.
/// @param pkg Package metadata.
/// @param displayName User-visible app name.
/// @return Non-empty publisher text.
std::string windowsPublisherFor(const PackageConfig &pkg, const std::string &displayName) {
    if (!pkg.windowsPublisher.empty())
        return pkg.windowsPublisher;
    if (!pkg.author.empty())
        return pkg.author;
    return displayName;
}

/// @brief Build the Windows ProgID for a toolchain file association.
/// Equivalent to windowsProgIdFor but takes an explicit identifier string instead
/// of a full PackageConfig; used for toolchain installer builds.
/// @param identifier Validated product identifier used as the ProgID base.
/// @param assoc File association to validate and append.
/// @return Full validated ProgID.
std::string toolchainProgIdFor(const std::string &identifier, const FileAssoc &assoc) {
    validateWindowsProgIdBase(identifier, "Windows file association ProgID base");
    if (identifier.empty())
        throw std::runtime_error("Windows file association ProgID base must not be empty");
    validateFileAssociation(assoc.extension, assoc.description, assoc.mimeType);
    const std::string progId = identifier + "." + windowsProgIdExtensionComponent(assoc);
    validateWindowsProgIdBase(progId, "Windows file association ProgID");
    return progId;
}

/// @brief Return Zanna Studio arguments used before the quoted source path.
/// @details Opening a source association must never execute the file implicitly.
/// @param assoc Association being converted; currently does not alter the safe default.
/// @return Empty fixed-argument prefix.
std::string toolchainOpenCommandArgsFor(const FileAssoc &assoc) {
    (void)assoc;
    return {};
}

/// @brief Walk every parent directory segment of relativeFilePath and ensure each one
/// is present in out. Directories are added shallowest-first so the installer
/// creates parent directories before their children. Duplicate entries are
/// suppressed via the seen set.
/// @param out Destination directory inventory.
/// @param seen Root-qualified paths already emitted.
/// @param root Runtime destination root for new directories.
/// @param relativeFilePath File path whose parents should be registered.
void addParentDirs(std::vector<WindowsPackageDirEntry> &out,
                   std::set<std::string> &seen,
                   WindowsInstallRoot root,
                   const std::string &relativeFilePath) {
    fs::path rel = zanna::filesystem::pathFromUtf8(relativeFilePath);
    fs::path cur = rel.parent_path();
    if (cur.empty())
        return;

    std::vector<std::string> dirs;
    while (!cur.empty() && cur != ".") {
        dirs.push_back(sanitizePackageRelativePath(zanna::filesystem::genericPathToUtf8(cur),
                                                   "windows package path"));
        cur = cur.parent_path();
    }
    std::reverse(dirs.begin(), dirs.end());
    for (const auto &dir : dirs)
        addUniqueDir(out, seen, root, dir);
}

/// @brief Add a file to the ZIP overlay and register corresponding install/uninstall
/// entries in layout. The layout entry captures the local-data offset and CRC-32
/// from the freshly-written ZIP entry so the installer stub can locate and verify
/// each file without parsing the central directory at runtime.
/// @param payloadManifest Optional SHA-256 manifest stream; null disables emission.
/// @param overlayName ZIP entry path to sanitize and record.
/// @param data Entry bytes; may be null only when @p len is zero.
/// @param len Number of bytes to hash.
void appendPayloadManifestEntry(std::ostringstream *payloadManifest,
                                const std::string &overlayName,
                                const uint8_t *data,
                                size_t len) {
    if (payloadManifest == nullptr)
        return;
    *payloadManifest << sha256Hex(data, len) << "  "
                     << sanitizePackageRelativePath(overlayName, "Windows package manifest path")
                     << "\n";
}

/// @brief Record a file for removal at uninstall time, when @p deleteOnUninstall.
/// @details Appends an uninstall entry to @p layout so the generated uninstaller
///          knows to delete this path; no-op when the file should be left behind.
void registerInstalledFile(WindowsPackageLayout &layout,
                           WindowsInstallRoot root,
                           const std::string &installRelativePath,
                           uint64_t sizeBytes,
                           bool deleteOnUninstall,
                           const std::string &componentId = {}) {
    if (deleteOnUninstall) {
        layout.uninstallFiles.push_back(
            WindowsPackageFileEntry{root, installRelativePath, 0, sizeBytes, 0, {}, componentId});
    }
}

/// @brief Add a STORED (uncompressed) file to the overlay ZIP and register it.
/// @details Stored entries are mandatory for bootstrap files the stub reads
///          directly: it captures the entry's local-data offset and CRC-32 into
///          @p layout so the stub can locate and verify the bytes without parsing
///          the central directory. Throws if the writer compressed the entry.
///          Optionally appends a SHA-256 line to @p payloadManifest.
void addStoredOverlayFile(ZipWriter &zip,
                          const std::string &overlayName,
                          const uint8_t *data,
                          size_t len,
                          uint32_t unixMode,
                          WindowsPackageLayout &layout,
                          WindowsInstallRoot root,
                          const std::string &installRelativePath,
                          bool deleteOnUninstall,
                          std::ostringstream *payloadManifest = nullptr,
                          const std::string &componentId = {}) {
    zip.addFile(overlayName, data, len, unixMode);
    const auto &entry = zip.layoutEntries().back();
    if (entry.method != 0 || entry.compressedSize != entry.uncompressedSize) {
        throw std::runtime_error("Windows installer overlay entries must be stored without "
                                 "compression: " +
                                 overlayName);
    }
    layout.installFiles.push_back(WindowsPackageFileEntry{root,
                                                          installRelativePath,
                                                          entry.localDataOffset,
                                                          entry.uncompressedSize,
                                                          entry.crc32,
                                                          sha256Hex(data, len),
                                                          componentId,
                                                          overlayName});
    appendPayloadManifestEntry(payloadManifest, overlayName, data, len);
    registerInstalledFile(
        layout, root, installRelativePath, entry.uncompressedSize, deleteOnUninstall, componentId);
}

/// @brief Add a file to the compressed inner payload ZIP and register it.
/// @details Unlike addStoredOverlayFile, entries here may be DEFLATE-compressed
///          because the stub extracts the whole inner ZIP before use, so no
///          stored-offset capture is needed. Records the install/uninstall entry
///          and, for InstallDir files, appends to @p installManifestPaths.
void addCompressedPayloadFile(ZipWriter &payloadZip,
                              const std::string &payloadName,
                              const uint8_t *data,
                              size_t len,
                              uint32_t unixMode,
                              WindowsPackageLayout &layout,
                              WindowsInstallRoot root,
                              const std::string &installRelativePath,
                              bool deleteOnUninstall,
                              std::vector<std::string> *installManifestPaths = nullptr,
                              const std::string &componentId = {}) {
    payloadZip.addFile(payloadName, data, len, unixMode);
    layout.installedFiles.push_back(WindowsPackageFileEntry{
        root, installRelativePath, 0, len, 0, sha256Hex(data, len), componentId, payloadName});
    registerInstalledFile(layout, root, installRelativePath, len, deleteOnUninstall, componentId);
    if (installManifestPaths != nullptr && root == WindowsInstallRoot::InstallDir) {
        installManifestPaths->push_back(
            sanitizePackageRelativePath(installRelativePath, "Windows installed manifest path"));
    }
}

/// @brief Render the newline-separated, sorted, de-duplicated installed-file manifest.
/// @details Adds the manifest's own path, sorts case-insensitively, removes
///          case-insensitive duplicates, and joins with newlines. The installer
///          writes this manifest so a later upgrade/uninstall knows exactly which
///          files it placed.
/// @param paths Install-relative paths to include (taken by value; mutated).
/// @param manifestRelativePath Path of the manifest file itself (also recorded).
/// @return The manifest text.
std::string buildWindowsInstalledManifest(std::vector<std::string> paths,
                                          const std::string &manifestRelativePath) {
    paths.push_back(
        sanitizePackageRelativePath(manifestRelativePath, "Windows installed manifest path"));
    /// @brief Order manifest paths by their case-insensitive spelling.
    /// @param a Left-hand path.
    /// @param b Right-hand path.
    /// @return `true` when the normalized form of `a` precedes that of `b`.
    std::sort(paths.begin(), paths.end(), [](const std::string &a, const std::string &b) {
        return lowerAscii(a) < lowerAscii(b);
    });
    paths.erase(std::unique(paths.begin(),
                            paths.end(),
                            /// @brief Identify paths that differ only by ASCII letter case.
                            /// @param a Left-hand path.
                            /// @param b Right-hand path.
                            /// @return `true` when the normalized paths are equal.
                            [](const std::string &a, const std::string &b) {
                                return lowerAscii(a) == lowerAscii(b);
                            }),
                paths.end());
    std::ostringstream os;
    for (const auto &path : paths)
        os << path << "\n";
    return os.str();
}

/// @brief Populate uninstallDirectories from installDirectories, then sort deepest-first
/// so the uninstaller removes leaf directories before their parents. Sorting by
/// depth first, then path length, ensures correct order when depths are equal.
/// @param layout Package layout whose uninstall directory order is rebuilt.
void finalizeUninstallDirs(WindowsPackageLayout &layout) {
    layout.uninstallDirectories = layout.installDirectories;
    std::stable_sort(layout.uninstallDirectories.begin(),
                     layout.uninstallDirectories.end(),
                     /// @brief Order directories from deepest to shallowest for safe removal.
                     /// @param a Left-hand directory entry.
                     /// @param b Right-hand directory entry.
                     /// @return `true` when `a` should be removed before `b`.
                     [](const WindowsPackageDirEntry &a, const WindowsPackageDirEntry &b) {
                         /// @brief Count the directory separators in a package-relative path.
                         /// @param path Path whose nesting depth is measured.
                         /// @return Number of forward-slash separators in `path`.
                         auto depth = [](const std::string &path) {
                             return static_cast<int>(std::count(path.begin(), path.end(), '/'));
                         };
                         const int da = depth(a.relativePath);
                         const int db = depth(b.relativePath);
                         if (da != db)
                             return da > db;
                         return a.relativePath.size() > b.relativePath.size();
                     });
}

/// @brief Normalize a validated Windows package path for the ZIP/metadata protocol.
/// @param value Validated path to normalize in place.
/// @return Path with backslashes replaced by forward slashes.
std::string metadataPath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

/// @brief Convert the package layout into the deterministic native-host contract.
/// @param layout Validated package destinations, payloads, components, and integrations.
/// @param productKind Metadata product kind (`application` or `toolchain`).
/// @param packageMode Initial contract mode.
/// @param arch Target architecture; empty selects x64.
/// @return Complete metadata derived from the layout.
/// @throws std::runtime_error On installed-size overflow.
WindowsInstallerMetadata nativeMetadataForLayout(const WindowsPackageLayout &layout,
                                                 const std::string &productKind,
                                                 const std::string &packageMode,
                                                 const std::string &arch) {
    WindowsInstallerMetadata metadata;
    metadata.packageMode = packageMode;
    metadata.productKind = productKind;
    metadata.identifier = layout.identifier;
    metadata.displayName = layout.displayName;
    metadata.version = layout.version;
    metadata.publisher = layout.publisher;
    metadata.description = layout.description;
    metadata.contact = layout.contact;
    metadata.homepage = layout.homepage;
    metadata.documentationUrl =
        layout.documentationUrl.empty() ? layout.homepage : layout.documentationUrl;
    metadata.updateManifestUrl = layout.updateManifestUrl;
    metadata.updateRsaModulus = layout.updateRsaModulus;
    metadata.updateRsaExponent = layout.updateRsaExponent;
    metadata.architecture = arch.empty() ? "x64" : arch;
    metadata.channel = layout.releaseChannel;
    metadata.commit = layout.sourceCommit;
    metadata.defaultScope = layout.perUserInstall ? "user" : "machine";
    metadata.defaultInstallDir = layout.installDirName;
    metadata.executableName = metadataPath(
        productKind == "toolchain" ? "bin/" + layout.executableName : layout.executableName);
    metadata.associationExecutable = metadataPath(layout.fileAssociationExecutableRelativePath);
    metadata.pathRelativePath = metadataPath(layout.pathRelativePath);
    metadata.displayIconRelativePath = metadataPath(layout.displayIconRelativePath);
    metadata.installedManifestRelativePath = metadataPath(layout.installedManifestRelativePath);
    metadata.minimumWindowsVersion = layout.minimumWindowsVersion;
    metadata.addToPath = layout.addToPath;
    metadata.registerFileAssociations = !layout.fileAssociations.empty();

    std::map<std::string, uint64_t> componentSizes;
    for (const auto &file : layout.installedFiles) {
        if (file.root != WindowsInstallRoot::InstallDir || file.sourcePath.empty())
            continue;
        if (file.sizeBytes > std::numeric_limits<uint64_t>::max() - metadata.installedSizeBytes)
            throw std::runtime_error("Windows native installer payload size overflow");
        metadata.installedSizeBytes += file.sizeBytes;
        componentSizes[file.componentId] += file.sizeBytes;
        metadata.payloadFiles.push_back(
            {metadataPath(file.sourcePath), file.sha256, file.sizeBytes, file.componentId});
    }
    metadata.components.push_back(
        {"core",
         productKind == "toolchain" ? "Core developer tools" : "Application files",
         productKind == "toolchain" ? "Compiler, virtual machine, command-line tools, runtime, "
                                      "documentation, and developer prompt"
                                    : "Files required to run the application",
         true,
         true,
         componentSizes[""]});
    for (const auto &component : layout.optionalComponents) {
        metadata.components.push_back({component.id,
                                       component.label,
                                       component.description,
                                       false,
                                       component.defaultSelected,
                                       componentSizes[component.id]});
    }
    for (const auto &shortcut : layout.nativeShortcuts) {
        metadata.shortcuts.push_back(
            {shortcut.root == WindowsInstallRoot::DesktopDir ? "desktop" : "start-menu",
             metadataPath(shortcut.relativePath),
             shortcut.targetRoot,
             metadataPath(shortcut.targetPath),
             shortcut.workingRoot,
             metadataPath(shortcut.workingPath),
             shortcut.argumentPrefix,
             metadataPath(shortcut.argumentPath),
             shortcut.description,
             shortcut.iconRoot,
             metadataPath(shortcut.iconPath),
             shortcut.iconIndex,
             shortcut.componentId});
    }
    metadata.createShortcuts = !metadata.shortcuts.empty();
    for (const auto &assoc : layout.fileAssociations) {
        metadata.associations.push_back({assoc.extension,
                                         assoc.description,
                                         assoc.mimeType,
                                         assoc.progId,
                                         assoc.openCommandArguments});
    }
    return metadata;
}

/// @brief Prove that @p path is an unsigned, static-runtime native host for @p arch.
/// @param path Existing native host template.
/// @param arch Required PE architecture.
/// @return Complete unsigned host bytes.
/// @throws std::runtime_error If missing, malformed, wrong-architecture, signed,
///         or dependent on a non-system DLL.
std::vector<uint8_t> loadNativeInstallerHost(const fs::path &path, const std::string &arch) {
    if (!fs::is_regular_file(path))
        throw std::runtime_error("Windows native installer host is missing: " +
                                 zanna::filesystem::pathToUtf8(path));
    std::vector<uint8_t> bytes = readFile(path);
    const auto info = inspectPeExecutable(bytes);
    if (!info || !info->pe32Plus || info->machine != windowsMachineForArch(arch)) {
        throw std::runtime_error("Windows native installer host architecture does not match '" +
                                 (arch.empty() ? std::string("x64") : arch) +
                                 "': " + zanna::filesystem::pathToUtf8(path));
    }
    for (const std::string &dependency : importedDllNamesFromPeImpl(bytes)) {
        const std::string lower = lowerAscii(dependency);
        if (!isKnownWindowsSystemDll(lower)) {
            throw std::runtime_error(
                "Windows native installer host is not self-contained; imports " + dependency);
        }
    }

    const size_t peOff = rd32(bytes, 60);
    const size_t coffOff = peOff + 4;
    const uint16_t optSize = rd16(bytes, coffOff + 16);
    const size_t optOff = coffOff + 20;
    constexpr size_t kPe32PlusDataDirectory = 112;
    constexpr size_t kSecurityDirectoryIndex = 4;
    const size_t security = optOff + kPe32PlusDataDirectory + kSecurityDirectoryIndex * 8;
    if (optSize >= kPe32PlusDataDirectory + (kSecurityDirectoryIndex + 1) * 8 &&
        hasBytes(bytes, security, 8) &&
        (rd32(bytes, security) != 0 || rd32(bytes, security + 4) != 0)) {
        throw std::runtime_error(
            "Windows native installer host template must be unsigned before package assembly");
    }
    return bytes;
}

/// @brief Append a completed ZIP overlay to an unsigned native host template.
/// @param host Complete native host PE bytes.
/// @param overlay Final ZIP bytes.
/// @return Concatenated self-extracting executable.
/// @throws std::runtime_error If the combined size overflows `size_t`.
std::vector<uint8_t> appendNativeHostOverlay(const std::vector<uint8_t> &host,
                                             const std::vector<uint8_t> &overlay) {
    if (overlay.size() > std::numeric_limits<size_t>::max() - host.size())
        throw std::runtime_error("Windows native installer executable size overflow");
    std::vector<uint8_t> result;
    result.reserve(host.size() + overlay.size());
    result.insert(result.end(), host.begin(), host.end());
    result.insert(result.end(), overlay.begin(), overlay.end());
    return result;
}

/// @brief Finish a native setup + maintenance pair from the prepared payload and outer entries.
/// @details The maintenance executable carries the repair payload but not itself. The signed
///          maintenance image is then added as a non-recursive outer-file record to setup.
/// @param outer Prepared stored-entry writer for maintenance overlay controls.
/// @param payload Finished compressed inner payload ZIP.
/// @param metadata Base validated native-host contract.
/// @param host Unsigned architecture-matched native host bytes.
/// @param unsignedCleanup Unsigned detached cleanup-helper PE bytes.
/// @param signer Optional Authenticode signing callback.
/// @param arch Required architecture for signed images.
/// @param licenseText Optional installer license content.
/// @param readmeText Optional installer readme content.
/// @return Final unsigned setup executable carrying a signed maintenance executable.
/// @throws std::runtime_error On signing, size, metadata, ZIP, or host assembly failure.
std::vector<uint8_t> buildNativeInstallerPair(ZipWriter &outer,
                                              const std::vector<uint8_t> &payload,
                                              WindowsInstallerMetadata metadata,
                                              const std::vector<uint8_t> &host,
                                              const std::vector<uint8_t> &unsignedCleanup,
                                              const WindowsPeSigner &signer,
                                              const std::string &arch,
                                              std::string_view licenseText,
                                              std::string_view readmeText) {
    outer.addFile("meta/payload.zip", payload.data(), payload.size(), 0100644);
    const std::vector<uint8_t> cleanup =
        signWindowsPayloadPe(signer, "installer-cleanup.exe", unsignedCleanup, arch);
    metadata.cleanupSha256 = sha256Hex(cleanup.data(), cleanup.size());
    outer.addFile("meta/cleanup.exe", cleanup.data(), cleanup.size(), 0100755);
    if (!licenseText.empty()) {
        outer.addFile("meta/license.txt",
                      reinterpret_cast<const uint8_t *>(licenseText.data()),
                      licenseText.size(),
                      0100644);
    }
    if (!readmeText.empty()) {
        outer.addFile("meta/readme.txt",
                      reinterpret_cast<const uint8_t *>(readmeText.data()),
                      readmeText.size(),
                      0100644);
    }

    metadata.packageMode = "maintenance";
    std::string maintenanceText = serializeWindowsInstallerMetadata(metadata);
    outer.addFile("meta/installer-v2.txt",
                  reinterpret_cast<const uint8_t *>(maintenanceText.data()),
                  maintenanceText.size(),
                  0100644);
    const std::vector<uint8_t> maintenanceOverlay = outer.finishToVector();
    std::vector<uint8_t> maintenanceExe = appendNativeHostOverlay(host, maintenanceOverlay);
    maintenanceExe = signWindowsPayloadPe(signer, "uninstall.exe", maintenanceExe, arch);

    metadata.packageMode = "setup";
    metadata.outerFiles.push_back({"meta/uninstall.exe",
                                   metadata.uninstallerRelativePath,
                                   sha256Hex(maintenanceExe.data(), maintenanceExe.size()),
                                   maintenanceExe.size(),
                                   {}});
    if (maintenanceExe.size() >
        std::numeric_limits<uint64_t>::max() - metadata.installedSizeBytes) {
        throw std::runtime_error("Windows native installer size overflow");
    }
    metadata.installedSizeBytes += maintenanceExe.size();
    const auto core = std::find_if(metadata.components.begin(),
                                   metadata.components.end(),
                                   /// @brief Determine whether metadata describes the core component.
                                   /// @param component Component metadata to inspect.
                                   /// @return `true` when the normalized component identifier is `core`.
                                   [](const WindowsInstallerComponentMetadata &component) {
                                       return lowerAscii(component.id) == "core";
                                   });
    if (core == metadata.components.end())
        throw std::runtime_error("Windows native installer metadata has no core component");
    if (maintenanceExe.size() > std::numeric_limits<uint64_t>::max() - core->sizeBytes) {
        throw std::runtime_error("Windows native installer component size overflow");
    }
    core->sizeBytes += maintenanceExe.size();
    const std::string setupText = serializeWindowsInstallerMetadata(metadata);

    ZipReader maintenanceReader(maintenanceOverlay.data(), maintenanceOverlay.size());
    ZipWriter setupOuter;
    setupOuter.setCompressionEnabled(false);
    for (const ZipEntry &entry : maintenanceReader.entries()) {
        if (entry.name == "meta/installer-v2.txt")
            continue;
        if (!entry.name.empty() && entry.name.back() == '/') {
            setupOuter.addDirectory(entry.name);
            continue;
        }
        const std::vector<uint8_t> data = maintenanceReader.extract(entry);
        setupOuter.addFile(entry.name, data.data(), data.size(), 0100644);
    }
    setupOuter.addFile("meta/uninstall.exe", maintenanceExe.data(), maintenanceExe.size(), 0100755);
    setupOuter.addFile("meta/installer-v2.txt",
                       reinterpret_cast<const uint8_t *>(setupText.data()),
                       setupText.size(),
                       0100644);
    return appendNativeHostOverlay(host, setupOuter.finishToVector());
}

/// @brief Locate a staged host template for a toolchain package.
/// @param params Toolchain build parameters and staged inventory.
/// @return Explicit or discovered host path; empty when unavailable.
fs::path toolchainNativeHostPath(const WindowsToolchainBuildParams &params) {
    if (!params.installerHostPath.empty())
        return zanna::filesystem::pathFromUtf8(params.installerHostPath);
    for (const ToolchainFileEntry &file : params.manifest.files) {
        if (lowerAscii(metadataPath(file.stagedRelativePath)) == "bin/zanna-installer-host.exe") {
            return file.stagedAbsolutePath;
        }
    }
    return {};
}

/// @brief Locate the staged detached-cleanup helper for a toolchain package.
/// @param params Toolchain build parameters and staged inventory.
/// @return Explicit or discovered cleanup path; empty when unavailable.
fs::path toolchainNativeCleanupPath(const WindowsToolchainBuildParams &params) {
    if (!params.installerCleanupPath.empty())
        return zanna::filesystem::pathFromUtf8(params.installerCleanupPath);
    for (const ToolchainFileEntry &file : params.manifest.files) {
        if (lowerAscii(metadataPath(file.stagedRelativePath)) ==
            "bin/zanna-installer-cleanup.exe") {
            return file.stagedAbsolutePath;
        }
    }
    return {};
}

} // namespace

/// @brief Extract imported DLL names from a safely parseable PE32+ import table.
/// @param data Complete candidate PE bytes.
/// @return Lowercase DLL names, or an empty vector for invalid/unsupported images.
std::vector<std::string> importedDllNamesFromPe(const std::vector<uint8_t> &data) {
    return importedDllNamesFromPeImpl(data);
}

/// @brief Build a Windows self-extracting installer for a single application binary.
/// Assembly is a two-pass process: Pass 1 measures the exact overlay offset with
/// overlayFileOffset=0; Pass 2 bakes the measured offset into the stub and produces
/// the final PE. The uninstaller is built first so it can be included in the ZIP.
/// @param params Application inputs, package metadata, host templates, output, and signer.
/// @throws std::runtime_error On validation, dependency discovery, signing, I/O,
///         ZIP construction, PE assembly, or output failure.
void buildWindowsPackage(const WindowsBuildParams &params) {
    const auto &pkg = params.pkgConfig;
    const fs::path executablePath = zanna::filesystem::pathFromUtf8(params.executablePath);
    const fs::path projectRoot = zanna::filesystem::pathFromUtf8(params.projectRoot);
    const fs::path outputPath = zanna::filesystem::pathFromUtf8(params.outputPath);
    const std::string displayName = pkg.displayName.empty() ? params.projectName : pkg.displayName;
    const std::string exec = normalizeExecName(params.projectName);
    const std::string installDir =
        pkg.windowsInstallDir.empty() ? displayName : pkg.windowsInstallDir;
    const std::string version = params.version.empty() ? "0.0.0" : params.version;
    validateWindowsFileName(displayName, "Windows display name");
    validateWindowsFileName(installDir, "Windows install directory");
    validateWindowsProgIdBase(pkg.identifier, "Windows package identifier");
    validateSingleLineField(version, "Windows package version");
    if (!pkg.windowsInstallScope.empty() && pkg.windowsInstallScope != "user" &&
        pkg.windowsInstallScope != "machine") {
        throw std::runtime_error("Windows install scope must be 'user' or 'machine'");
    }
    if (!pkg.minOsWindows.empty())
        validateDottedNumericVersion(pkg.minOsWindows, "minimum Windows version");
    validateSingleLineField(pkg.author, "Windows package author");
    validateSingleLineField(pkg.windowsPublisher, "Windows publisher");
    validateSingleLineField(pkg.windowsWizardSummary, "Windows wizard summary");
    validateSingleLineField(pkg.description, "Windows package description");
    validatePackageUrl(pkg.homepage, "package homepage");
    validatePackageFileAssociations(pkg.fileAssociations);
    if (!fs::is_regular_file(executablePath))
        throw std::runtime_error("Windows package executable is not a regular file: " +
                                 params.executablePath);
    validateWindowsPayloadExecutable(executablePath, params.archStr);

    WindowsPackageLayout layout;
    layout.displayName = displayName;
    layout.installDirName = installDir;
    layout.version = version;
    layout.identifier = pkg.identifier;
    layout.publisher = windowsPublisherFor(pkg, displayName);
    layout.description = pkg.description;
    layout.contact = pkg.maintainerEmail.empty() ? layout.publisher : pkg.maintainerEmail;
    layout.licenseText = appLicenseTextFor(params.projectRoot, pkg);
    layout.wizardSummary =
        pkg.windowsWizardSummary.empty() ? pkg.welcomeText : pkg.windowsWizardSummary;
    layout.executableName = exec + ".exe";
    layout.fileAssociationExecutableRelativePath = exec + ".exe";
    layout.perUserInstall = pkg.windowsInstallScope.empty() || pkg.windowsInstallScope == "user";
    layout.homepage = pkg.homepage;
    layout.minimumWindowsVersion =
        pkg.minOsWindows.empty() ? std::string("10.0.17763") : pkg.minOsWindows;
    layout.createDesktopShortcut = pkg.shortcutDesktop;
    layout.createStartMenuShortcut = pkg.shortcutMenu;
    layout.cleanInstallRootBeforeInstall = false;
    for (const auto &assoc : pkg.fileAssociations) {
        layout.fileAssociations.push_back({assoc.extension,
                                           assoc.description,
                                           assoc.mimeType,
                                           windowsProgIdFor(pkg, exec, assoc),
                                           assoc.openCommandArguments});
    }

    std::set<std::string> installDirSet;
    std::set<std::string> installFileSet;
    /// @brief Validate and reserve one case-insensitive installation-file path.
    /// @param relativePath Candidate path relative to the installation root.
    /// @return Sanitized package-relative path.
    /// @throws std::runtime_error If the path is invalid or collides with a prior file.
    auto noteInstallFile = [&](const std::string &relativePath) {
        const std::string clean = sanitizePackageRelativePath(relativePath, "Windows install path");
        const std::string key = lowerAscii(clean);
        if (!installFileSet.insert(key).second)
            throw std::runtime_error("Windows package install path collision: " + clean);
        return clean;
    };

    ZipWriter zip;
    zip.setCompressionEnabled(false);
    zip.addDirectory("app/");
    zip.addDirectory("meta/");
    ZipWriter payloadZip;
    payloadZip.setCompressionEnabled(true);
    std::vector<std::string> installedManifestPaths;
    layout.compressedPayloadRelativePath = ".zanna-payload.zip";
    layout.compressedPayloadManifestRelativePath = ".zanna-install-manifest.next";
    layout.installedManifestRelativePath = ".zanna-install-manifest.txt";
    std::ostringstream payloadManifest;

    std::vector<uint8_t> icoData;
    if (!pkg.iconPath.empty()) {
        fs::path iconSrc = resolvePackageSourcePath(projectRoot, pkg.iconPath, "package icon");
        if (!fs::is_regular_file(iconSrc))
            throw std::runtime_error("package icon not found: " + pkg.iconPath);
        auto srcImage = pngRead(zanna::filesystem::pathToUtf8(iconSrc));
        icoData = generateIco(srcImage);
    } else {
        icoData = generateIco(defaultZannaToolchainIconImage());
    }

    const auto execData = signWindowsPayloadPe(
        params.peSigner, exec + ".exe", readFile(executablePath), params.archStr);
    noteInstallFile(exec + ".exe");
    addCompressedPayloadFile(payloadZip,
                             exec + ".exe",
                             execData.data(),
                             execData.size(),
                             0100755,
                             layout,
                             WindowsInstallRoot::InstallDir,
                             exec + ".exe",
                             true,
                             &installedManifestPaths);

    std::vector<WindowsDllDependency> dllDependencies =
        discoverAdjacentDllDependencies(executablePath);
    for (const auto &dllRelPath : pkg.windowsDlls) {
        const fs::path dllPath =
            resolvePackageSourcePath(projectRoot, dllRelPath, "Windows DLL dependency");
        if (!fs::is_regular_file(dllPath))
            throw std::runtime_error("Windows DLL dependency is not a regular file: " + dllRelPath);
        dllDependencies.push_back(
            {dllPath, sanitizePackageRelativePath(dllRelPath, "Windows DLL dependency path")});
    }
    for (const auto &dll : dllDependencies) {
        const std::string dllName = noteInstallFile(dll.installRelativePath);
        validateWindowsRelativePath(dllName, "Windows DLL dependency path");
        addParentDirs(
            layout.installDirectories, installDirSet, WindowsInstallRoot::InstallDir, dllName);
        const auto dllData = signWindowsPayloadPe(
            params.peSigner, dllName, readFile(dll.sourcePath), params.archStr);
        addCompressedPayloadFile(payloadZip,
                                 dllName,
                                 dllData.data(),
                                 dllData.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::InstallDir,
                                 dllName,
                                 true,
                                 &installedManifestPaths);
    }

    if (!icoData.empty()) {
        noteInstallFile(exec + ".ico");
        addCompressedPayloadFile(payloadZip,
                                 exec + ".ico",
                                 icoData.data(),
                                 icoData.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::InstallDir,
                                 exec + ".ico",
                                 true,
                                 &installedManifestPaths);
    }
    layout.displayIconRelativePath = !icoData.empty() ? exec + ".ico" : exec + ".exe";

    for (const auto &asset : pkg.assets) {
        const std::string sourceRel =
            sanitizePackageRelativePath(asset.sourcePath, "asset source path");
        const std::string sourceLeaf = zanna::filesystem::genericPathToUtf8(
            zanna::filesystem::pathFromUtf8(sourceRel).filename());
        fs::path srcPath =
            resolvePackageSourcePath(projectRoot, asset.sourcePath, "asset source path");
        const std::string targetDir =
            sanitizePackageRelativePath(asset.targetPath, "asset target path");
        validateWindowsRelativePath(targetDir, "asset target path");

        if (!fs::exists(srcPath))
            throw std::runtime_error("asset not found: " + asset.sourcePath);

        if (fs::is_directory(srcPath)) {
            if (!targetDir.empty()) {
                addUniqueDir(layout.installDirectories,
                             installDirSet,
                             WindowsInstallRoot::InstallDir,
                             targetDir);
            }
            /// @brief Add one safely resolved asset-tree entry to the Windows payload layout.
            /// @param entry Directory entry with verified physical and logical paths.
            /// @throws std::runtime_error If an entry path or payload operation is invalid.
            safeDirectoryIterateResolved(
                srcPath, projectRoot, [&](const SafeDirectoryEntry &entry) {
                    const auto relPath = sanitizePackageRelativePath(
                        zanna::filesystem::genericPathToUtf8(
                            entry.logicalPath.lexically_relative(srcPath)),
                        "asset path");
                    if (entry.directory) {
                        const std::string relInstall =
                            joinPackageRelativePath(targetDir, relPath, "asset path");
                        addUniqueDir(layout.installDirectories,
                                     installDirSet,
                                     WindowsInstallRoot::InstallDir,
                                     relInstall);
                        return;
                    }
                    if (!entry.regularFile)
                        return;

                    const std::string relInstall =
                        joinPackageRelativePath(targetDir, relPath, "asset path");
                    noteInstallFile(relInstall);
                    addParentDirs(layout.installDirectories,
                                  installDirSet,
                                  WindowsInstallRoot::InstallDir,
                                  relInstall);
                    const auto data = readFile(entry.resolvedPath);
                    addCompressedPayloadFile(payloadZip,
                                             relInstall,
                                             data.data(),
                                             data.size(),
                                             0100644,
                                             layout,
                                             WindowsInstallRoot::InstallDir,
                                             relInstall,
                                             true,
                                             &installedManifestPaths);
                });
        } else if (fs::is_regular_file(srcPath)) {
            const std::string relInstall =
                joinPackageRelativePath(targetDir, sourceLeaf, "asset path");
            noteInstallFile(relInstall);
            addParentDirs(layout.installDirectories,
                          installDirSet,
                          WindowsInstallRoot::InstallDir,
                          relInstall);
            const auto data = readFile(srcPath);
            addCompressedPayloadFile(payloadZip,
                                     relInstall,
                                     data.data(),
                                     data.size(),
                                     0100644,
                                     layout,
                                     WindowsInstallRoot::InstallDir,
                                     relInstall,
                                     true,
                                     &installedManifestPaths);
        } else {
            throw std::runtime_error("asset is not a regular file or directory: " +
                                     asset.sourcePath);
        }
    }

    if (pkg.shortcutMenu) {
        layout.nativeShortcuts.push_back({WindowsInstallRoot::StartMenuDir,
                                          displayName + ".lnk",
                                          "install",
                                          exec + ".exe",
                                          "install",
                                          {},
                                          {},
                                          {},
                                          displayName,
                                          icoData.empty() ? std::string{} : "install",
                                          icoData.empty() ? std::string{} : exec + ".ico",
                                          0,
                                          {}});
        LnkParams lnk;
        lnk.targetPath = windowsInstallEnvPath(installDir, layout.perUserInstall, exec + ".exe");
        lnk.workingDir = windowsInstallEnvPath(installDir, layout.perUserInstall);
        lnk.description = displayName;
        if (!icoData.empty())
            lnk.iconPath = windowsInstallEnvPath(installDir, layout.perUserInstall, exec + ".ico");
        const auto lnkData = generateLnk(lnk);
        if (params.installerHostPath.empty()) {
            addStoredOverlayFile(zip,
                                 "meta/start_menu.lnk",
                                 lnkData.data(),
                                 lnkData.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::StartMenuDir,
                                 displayName + ".lnk",
                                 true,
                                 &payloadManifest);
        }
    }

    if (pkg.shortcutDesktop) {
        layout.nativeShortcuts.push_back({WindowsInstallRoot::DesktopDir,
                                          displayName + ".lnk",
                                          "install",
                                          exec + ".exe",
                                          "install",
                                          {},
                                          {},
                                          {},
                                          displayName,
                                          icoData.empty() ? std::string{} : "install",
                                          icoData.empty() ? std::string{} : exec + ".ico",
                                          0,
                                          {}});
        LnkParams lnk;
        lnk.targetPath = windowsInstallEnvPath(installDir, layout.perUserInstall, exec + ".exe");
        lnk.workingDir = windowsInstallEnvPath(installDir, layout.perUserInstall);
        lnk.description = displayName;
        if (!icoData.empty())
            lnk.iconPath = windowsInstallEnvPath(installDir, layout.perUserInstall, exec + ".ico");
        const auto lnkData = generateLnk(lnk);
        if (params.installerHostPath.empty()) {
            addStoredOverlayFile(zip,
                                 "meta/desktop.lnk",
                                 lnkData.data(),
                                 lnkData.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::DesktopDir,
                                 displayName + ".lnk",
                                 true,
                                 &payloadManifest);
        }
    }

    if (!params.installerHostPath.empty()) {
        finalizeUninstallDirs(layout);
        const std::vector<uint8_t> compressedPayload = payloadZip.finishToVector();
        WindowsInstallerMetadata metadata =
            nativeMetadataForLayout(layout, "application", "setup", params.archStr);
        const std::vector<uint8_t> host = loadNativeInstallerHost(
            zanna::filesystem::pathFromUtf8(params.installerHostPath), params.archStr);
        if (params.installerCleanupPath.empty())
            throw std::runtime_error("Windows native installer cleanup helper path is required");
        const std::vector<uint8_t> cleanup = loadNativeInstallerHost(
            zanna::filesystem::pathFromUtf8(params.installerCleanupPath), params.archStr);
        const std::vector<uint8_t> nativeInstaller = buildNativeInstallerPair(zip,
                                                                              compressedPayload,
                                                                              std::move(metadata),
                                                                              host,
                                                                              cleanup,
                                                                              params.peSigner,
                                                                              params.archStr,
                                                                              layout.licenseText,
                                                                              {});
        writePEToFile(nativeInstaller, params.outputPath);
        return;
    }

    noteInstallFile("uninstall.exe");
    noteInstallFile(layout.installedManifestRelativePath);
    registerInstalledFile(
        layout, WindowsInstallRoot::InstallDir, layout.installedManifestRelativePath, 0, true);
    finalizeUninstallDirs(layout);

    auto uninstStub = buildUninstallerStub(layout, params.archStr);
    PEBuildParams uninstPe;
    uninstPe.arch = uninstStub.peArch;
    uninstPe.textSection = uninstStub.textSection;
    uninstPe.rdataSection = uninstStub.stubData;
    uninstPe.imports = uninstStub.imports;
    uninstPe.manifest = windowsManifestForLayout(layout, pkg.minOsWindows);
    uninstPe.iconData = icoData;
    uninstPe.versionInfo = windowsVersionInfoForLayout(layout, "uninstall.exe", "Uninstaller");
    configureInstallerStack(uninstPe);
    auto uninstBytes =
        signWindowsPayloadPe(params.peSigner, "uninstall.exe", buildPE(uninstPe), params.archStr);
    addCompressedPayloadFile(payloadZip,
                             "uninstall.exe",
                             uninstBytes.data(),
                             uninstBytes.size(),
                             0100755,
                             layout,
                             WindowsInstallRoot::InstallDir,
                             "uninstall.exe",
                             false,
                             &installedManifestPaths);

    const std::string installedManifestText =
        buildWindowsInstalledManifest(installedManifestPaths, layout.installedManifestRelativePath);
    addCompressedPayloadFile(payloadZip,
                             layout.installedManifestRelativePath,
                             reinterpret_cast<const uint8_t *>(installedManifestText.data()),
                             installedManifestText.size(),
                             0100644,
                             layout,
                             WindowsInstallRoot::InstallDir,
                             layout.installedManifestRelativePath,
                             false,
                             nullptr);
    const auto compressedPayload = payloadZip.finishToVector();
    addStoredOverlayFile(zip,
                         "meta/payload.zip",
                         compressedPayload.data(),
                         compressedPayload.size(),
                         0100644,
                         layout,
                         WindowsInstallRoot::InstallDir,
                         layout.compressedPayloadRelativePath,
                         true,
                         &payloadManifest);
    addStoredOverlayFile(zip,
                         "meta/install_manifest.next",
                         reinterpret_cast<const uint8_t *>(installedManifestText.data()),
                         installedManifestText.size(),
                         0100644,
                         layout,
                         WindowsInstallRoot::InstallDir,
                         layout.compressedPayloadManifestRelativePath,
                         true,
                         &payloadManifest);
    layout.estimatedSizeKb = estimatedInstalledSizeKb(layout);
    validateWindowsLayoutFitsStub(layout);
    const std::string manifestText = payloadManifest.str();
    zip.addFile("meta/manifest.sha256",
                reinterpret_cast<const uint8_t *>(manifestText.data()),
                manifestText.size(),
                0100644);

    const auto zipPayload = zip.finishToVector();

    layout.overlayFileOffset = 0;
    auto provisionalStub = buildInstallerStub(layout, params.archStr);
    PEBuildParams provisionalPe;
    provisionalPe.arch = provisionalStub.peArch;
    provisionalPe.textSection = provisionalStub.textSection;
    provisionalPe.rdataSection = provisionalStub.stubData;
    provisionalPe.imports = provisionalStub.imports;
    provisionalPe.manifest = windowsManifestForLayout(layout, pkg.minOsWindows);
    provisionalPe.iconData = icoData;
    provisionalPe.overlay = zipPayload;
    provisionalPe.versionInfo = windowsVersionInfoForLayout(
        layout, zanna::filesystem::genericPathToUtf8(outputPath.filename()), "Setup");
    configureInstallerStack(provisionalPe);
    const auto provisionalBytes = buildPE(provisionalPe);
    layout.overlayFileOffset = static_cast<uint64_t>(provisionalBytes.size() - zipPayload.size());

    std::vector<uint8_t> peBytes;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        auto instStub = buildInstallerStub(layout, params.archStr);
        PEBuildParams pe;
        pe.arch = instStub.peArch;
        pe.textSection = instStub.textSection;
        pe.rdataSection = instStub.stubData;
        pe.imports = instStub.imports;
        pe.manifest = windowsManifestForLayout(layout, pkg.minOsWindows);
        pe.iconData = icoData;
        pe.overlay = zipPayload;
        pe.versionInfo = windowsVersionInfoForLayout(
            layout, zanna::filesystem::genericPathToUtf8(outputPath.filename()), "Setup");
        configureInstallerStack(pe);

        peBytes = buildPE(pe);
        const uint64_t finalOverlayOffset =
            static_cast<uint64_t>(peBytes.size() - zipPayload.size());
        if (finalOverlayOffset == layout.overlayFileOffset)
            break;
        if (attempt == 3) {
            throw std::runtime_error("Windows installer overlay offset did not converge");
        }
        layout.overlayFileOffset = finalOverlayOffset;
    }
    writePEToFile(peBytes, params.outputPath);
}

/// @brief Build a Windows toolchain installer from a pre-validated staged manifest.
/// Staged files are installed as-is, while generated developer helper scripts and
/// Start Menu shortcuts are added by the packager. Symlinks are dereferenced.
/// @brief Build a native Windows installer for a validated staged Zanna toolchain.
/// @details Packages all non-bootstrap staged files, binds optional Studio
///          provenance, enforces runtime-DLL closure, generates integration
///          metadata and branding, then assembles setup/maintenance native hosts.
/// @param params Toolchain manifest, identity, integration options, hosts, output, and signer.
/// @throws std::runtime_error On manifest/layout validation, signing, I/O,
///         metadata, ZIP, PE, or output failure.
void buildWindowsToolchainInstaller(const WindowsToolchainBuildParams &params) {
    const fs::path outputPath = zanna::filesystem::pathFromUtf8(params.outputPath);
    validateToolchainInstallManifest(params.manifest);
    if (params.manifest.platform != "windows") {
        throw std::runtime_error("Windows toolchain installer requires a Windows staged "
                                 "toolchain manifest, got '" +
                                 params.manifest.platform + "'");
    }
    validateToolchainMsvcRuntimeClosure(params.manifest);
    validateWindowsFileName(params.displayName, "Windows display name");
    validateWindowsProgIdBase(params.identifier, "Windows package identifier");
    validateSingleLineField(params.publisher, "Windows package publisher");
    validateWindowsFileName(params.installDirName, "Windows install directory");
    validatePackageUrl(params.homepage, "Windows package homepage");
    if (params.installScope != "user" && params.installScope != "machine") {
        throw std::runtime_error("Windows toolchain install scope must be 'user' or 'machine'");
    }
    if (params.manifest.version.empty())
        throw std::runtime_error("toolchain package version is required");
    validateSingleLineField(params.manifest.version, "Windows package version");
    const fs::path nativeHostPath = toolchainNativeHostPath(params);
    const bool useNativeHost = !nativeHostPath.empty();
    WindowsPackageLayout layout;
    layout.displayName = params.displayName;
    layout.installDirName = params.installDirName;
    layout.version = params.manifest.version;
    layout.identifier = params.identifier;
    layout.publisher = params.publisher;
    layout.description = "Zanna compiler toolchain";
    layout.contact = params.manifest.maintainerEmail.empty() ? params.publisher
                                                             : params.manifest.maintainerEmail;
    layout.licenseText = params.manifest.license;
    layout.wizardSummary =
        "Choose the developer tools you want, review the license, and install Zanna.";
    layout.executableName = "zanna.exe";
    layout.homepage = params.homepage;
    layout.documentationUrl = params.documentationUrl;
    layout.updateManifestUrl = params.updateManifestUrl;
    layout.updateRsaModulus = params.updateRsaModulus;
    layout.updateRsaExponent = params.updateRsaExponent;
    layout.releaseChannel = params.releaseChannel;
    layout.sourceCommit = params.sourceCommit;
    layout.displayIconRelativePath = "share\\zanna\\zanna.ico";
    layout.minimumWindowsVersion = "10.0.17763";
    layout.createDesktopShortcut = false;
    layout.createStartMenuShortcut = params.createStartMenuShortcuts;
    layout.addToPath = params.addToPath;
    layout.cleanInstallRootBeforeInstall = false;
    layout.pathRelativePath = "bin";
    layout.fileAssociationExecutableRelativePath = "bin\\zannastudio.exe";
    layout.perUserInstall = params.installScope == "user";
    if (params.registerFileAssociations) {
        if (params.identifier.empty())
            throw std::runtime_error(
                "Windows file associations require a non-empty package identifier");
        for (const auto &assoc : params.manifest.fileAssociations) {
            layout.fileAssociations.push_back({assoc.extension,
                                               assoc.description,
                                               assoc.mimeType,
                                               toolchainProgIdFor(params.identifier, assoc),
                                               toolchainOpenCommandArgsFor(assoc)});
        }
    }

    std::set<std::string> installDirSet;
    ZipWriter zip;
    zip.setCompressionEnabled(false);
    zip.addDirectory("app/");
    zip.addDirectory("meta/");
    ZipWriter payloadZip;
    payloadZip.setCompressionEnabled(true);
    std::vector<std::string> installedManifestPaths;
    layout.compressedPayloadRelativePath = ".zanna-payload.zip";
    layout.compressedPayloadManifestRelativePath = ".zanna-install-manifest.next";
    layout.installedManifestRelativePath = ".zanna-install-manifest.txt";
    std::ostringstream payloadManifest;
    std::string stagedLicenseText;
    std::string windowsReadmeText;
    bool hasLicenseOverlay = false;
    bool hasReadmeOverlay = false;
    bool hasPackagedVSIX = false;
    const auto stagedLogo =
        stagedToolchainPng(params.manifest, "share/zanna/branding/zannalogo2.png");
    const auto stagedWallpaper =
        stagedToolchainPng(params.manifest, "share/zanna/branding/zannawallpaper2.png");
    const PkgImage toolchainIconImage =
        stagedLogo.has_value() ? *stagedLogo : defaultZannaToolchainIconImage();
    const std::vector<uint8_t> toolchainIcon = generateIco(toolchainIconImage);
    if (stagedLogo.has_value() && stagedWallpaper.has_value()) {
        const PkgImage banner = buildZannaWizardBanner(*stagedWallpaper, *stagedLogo);
        layout.wizardImageWidth = banner.width;
        layout.wizardImageHeight = banner.height;
        layout.wizardImageRgba = banner.pixels;
    }

    for (const ToolchainFileEntry &file : params.manifest.files) {
        const std::string lowerRel = lowerAscii(file.stagedRelativePath);
        if (lowerRel.rfind("share/zanna/vscode/", 0) != 0 || lowerRel.size() < 5U ||
            lowerRel.substr(lowerRel.size() - 5U) != ".vsix") {
            continue;
        }
        try {
            const std::vector<uint8_t> bytes = readFile(file.stagedAbsolutePath);
            ZipReader vsix(bytes.data(), bytes.size());
            if (!vsix.find("[Content_Types].xml") || !vsix.find("extension.vsixmanifest") ||
                !vsix.find("extension/package.json")) {
                throw std::runtime_error("required VSIX metadata is missing");
            }
            for (const ZipEntry &entry : vsix.entries()) {
                if (entry.name.empty() || entry.name.back() == '/')
                    continue;
                static_cast<void>(vsix.extract(entry));
            }
        } catch (const std::exception &error) {
            throw std::runtime_error("invalid staged VS Code extension '" +
                                     file.stagedRelativePath + "': " + error.what());
        }
        hasPackagedVSIX = true;
    }

    const bool hasZannaStudioComponent =
        validateZannaStudioArtifacts(params.manifest, params.archStr);
    bool hasSDKComponent = false;
    bool hasSamplesComponent = false;
    bool hasVSCodeComponent = false;
    for (const ToolchainFileEntry &file : params.manifest.files) {
        if (isToolchainInstallerBootstrapPath(file.stagedRelativePath))
            continue;
        const std::string component =
            toolchainComponentForPath(file.stagedRelativePath, hasPackagedVSIX);
        hasSDKComponent = hasSDKComponent || component == kComponentSDK;
        hasSamplesComponent = hasSamplesComponent || component == kComponentSamples;
        hasVSCodeComponent = hasVSCodeComponent || component == kComponentVSCode;
    }
    if (!hasZannaStudioComponent)
        layout.fileAssociations.clear();
    if (hasZannaStudioComponent) {
        layout.optionalComponents.push_back(
            {std::string(kComponentZannaStudio),
             "Zanna Studio",
             "Native editor, project workflow, debugger, and language services",
             true});
    }
    if (hasSDKComponent) {
        layout.optionalComponents.push_back(
            {std::string(kComponentSDK),
             "Zanna SDK",
             "Headers, libraries, CMake integration, and extension development files",
             true});
    }
    if (hasSamplesComponent) {
        layout.optionalComponents.push_back(
            {std::string(kComponentSamples),
             "Samples and example projects",
             "Install the complete source-level examples under share/zanna/samples",
             false});
    }
    if (hasVSCodeComponent) {
        layout.optionalComponents.push_back(
            {std::string(kComponentVSCode),
             "VS Code integration",
             "Zia extension sources, packaged extension, and installation helper",
             true});
    }

    std::optional<std::vector<uint8_t>> packagedStudioExecutable;
    if (hasZannaStudioComponent) {
        for (const ToolchainFileEntry &file : params.manifest.files) {
            std::string normalized = lowerAscii(file.stagedRelativePath);
            std::replace(normalized.begin(), normalized.end(), '\\', '/');
            if (normalized == "bin/zannastudio.exe") {
                packagedStudioExecutable = signWindowsPayloadPe(params.peSigner,
                                                                "bin/zannastudio.exe",
                                                                readFile(file.stagedAbsolutePath),
                                                                params.archStr);
                break;
            }
        }
        if (!packagedStudioExecutable)
            throw std::runtime_error("validated Zanna Studio executable disappeared from manifest");
    }

    for (const auto &file : params.manifest.files) {
        if (isToolchainInstallerBootstrapPath(file.stagedRelativePath))
            continue;
        const std::string relInstall =
            sanitizePackageRelativePath(file.stagedRelativePath, "windows toolchain path");
        validateWindowsRelativePath(relInstall, "windows toolchain path");
        const std::string lowerRel = lowerAscii(relInstall);
        if (file.symlink && !fs::is_regular_file(file.stagedAbsolutePath)) {
            throw std::runtime_error("Windows toolchain installers can only dereference "
                                     "symlinks to regular files: " +
                                     file.stagedRelativePath);
        }
        addParentDirs(
            layout.installDirectories, installDirSet, WindowsInstallRoot::InstallDir, relInstall);
        std::vector<uint8_t> data;
        if (lowerRel == "bin/zannastudio.exe" && packagedStudioExecutable) {
            data = *packagedStudioExecutable;
        } else {
            const std::vector<uint8_t> stagedData = readFile(file.stagedAbsolutePath);
            if (lowerRel == "bin/zannastudio.buildinfo" && packagedStudioExecutable) {
                data = bindZannaStudioBuildInfo(stagedData, *packagedStudioExecutable);
            } else {
                data =
                    signWindowsPayloadPe(params.peSigner, relInstall, stagedData, params.archStr);
            }
        }
        const std::string componentId = toolchainComponentForPath(relInstall, hasPackagedVSIX);
        addCompressedPayloadFile(payloadZip,
                                 relInstall,
                                 data.data(),
                                 data.size(),
                                 file.executable ? 0100755 : 0100644,
                                 layout,
                                 WindowsInstallRoot::InstallDir,
                                 relInstall,
                                 true,
                                 &installedManifestPaths,
                                 componentId);

        const bool isCanonicalLicense =
            lowerRel == "license" || lowerRel == "share/doc/zanna/license";
        const bool isCanonicalReadme =
            lowerRel == "readme.md" || lowerRel == "share/doc/zanna/readme.md";
        if ((isCanonicalLicense && !hasLicenseOverlay) ||
            (isCanonicalReadme && !hasReadmeOverlay)) {
            const std::string overlayName =
                isCanonicalLicense ? "meta/license.txt" : "meta/readme.txt";
            // The native host pair owns its metadata overlay and inserts these
            // files once in buildNativeInstallerPair().  The legacy generated
            // stub still needs them added to its outer ZIP here.
            if (!useNativeHost) {
                zip.addFile(overlayName, data.data(), data.size(), 0100644);
                appendPayloadManifestEntry(&payloadManifest, overlayName, data.data(), data.size());
            }
            if (isCanonicalLicense) {
                stagedLicenseText = textFromBytes(data);
                hasLicenseOverlay = true;
            } else {
                hasReadmeOverlay = true;
            }
        }
    }
    if (!stagedLicenseText.empty())
        layout.licenseText = stagedLicenseText;

    addUniqueDir(layout.installDirectories, installDirSet, WindowsInstallRoot::InstallDir, "bin");
    {
        const std::string script = toolchainDeveloperPromptScript();
        addCompressedPayloadFile(payloadZip,
                                 "bin/zanna-dev.cmd",
                                 reinterpret_cast<const uint8_t *>(script.data()),
                                 script.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::InstallDir,
                                 "bin/zanna-dev.cmd",
                                 true,
                                 &installedManifestPaths);
    }
    if (hasPackagedVSIX) {
        const std::string script = toolchainVSCodeInstallScript();
        addCompressedPayloadFile(payloadZip,
                                 "bin/zanna-install-vscode-extension.cmd",
                                 reinterpret_cast<const uint8_t *>(script.data()),
                                 script.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::InstallDir,
                                 "bin/zanna-install-vscode-extension.cmd",
                                 true,
                                 &installedManifestPaths,
                                 std::string(kComponentVSCode));
    }
    {
        addUniqueDir(layout.installDirectories,
                     installDirSet,
                     WindowsInstallRoot::InstallDir,
                     "share/zanna");
        addCompressedPayloadFile(payloadZip,
                                 "share/zanna/zanna.ico",
                                 toolchainIcon.data(),
                                 toolchainIcon.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::InstallDir,
                                 "share/zanna/zanna.ico",
                                 true,
                                 &installedManifestPaths);
        if (hasZannaStudioComponent) {
            addCompressedPayloadFile(payloadZip,
                                     "bin/zannastudio.ico",
                                     toolchainIcon.data(),
                                     toolchainIcon.size(),
                                     0100644,
                                     layout,
                                     WindowsInstallRoot::InstallDir,
                                     "bin/zannastudio.ico",
                                     true,
                                     &installedManifestPaths,
                                     std::string(kComponentZannaStudio));
        }
        const std::string readme = toolchainWindowsPrerequisitesReadme(params.installDirName);
        windowsReadmeText = readme;
        addCompressedPayloadFile(payloadZip,
                                 "share/zanna/README.windows-prerequisites.txt",
                                 reinterpret_cast<const uint8_t *>(readme.data()),
                                 readme.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::InstallDir,
                                 "share/zanna/README.windows-prerequisites.txt",
                                 true,
                                 &installedManifestPaths);
    }

    if (params.createStartMenuShortcuts) {
        layout.nativeShortcuts.push_back({WindowsInstallRoot::StartMenuDir,
                                          "Zanna Developer Prompt.lnk",
                                          "windows",
                                          "System32/cmd.exe",
                                          "profile",
                                          {},
                                          "/k",
                                          "bin/zanna-dev.cmd",
                                          params.displayName + " Developer Prompt",
                                          "install",
                                          metadataPath(layout.displayIconRelativePath),
                                          0,
                                          {}});
        LnkParams promptLnk;
        promptLnk.targetPath = "%SystemRoot%\\System32\\cmd.exe";
        promptLnk.workingDir = "%USERPROFILE%";
        promptLnk.arguments =
            "/k " + windowsQuotedPath(windowsInstallEnvPath(
                        params.installDirName, layout.perUserInstall, "bin\\zanna-dev.cmd"));
        promptLnk.description = params.displayName + " Developer Prompt";
        promptLnk.iconPath = windowsInstallEnvPath(
            params.installDirName, layout.perUserInstall, layout.displayIconRelativePath);
        const auto promptData = generateLnk(promptLnk);
        if (!useNativeHost) {
            addStoredOverlayFile(zip,
                                 "meta/zanna_developer_prompt.lnk",
                                 promptData.data(),
                                 promptData.size(),
                                 0100644,
                                 layout,
                                 WindowsInstallRoot::StartMenuDir,
                                 "Zanna Developer Prompt.lnk",
                                 true,
                                 &payloadManifest);
        }

        if (hasPackagedVSIX) {
            layout.nativeShortcuts.push_back(
                {WindowsInstallRoot::StartMenuDir,
                 "Install VS Code Extension.lnk",
                 "windows",
                 "System32/cmd.exe",
                 "profile",
                 {},
                 "/c",
                 "bin/zanna-install-vscode-extension.cmd",
                 "Install " + params.displayName + " VS Code extension",
                 "install",
                 metadataPath(layout.displayIconRelativePath),
                 0,
                 std::string(kComponentVSCode)});
            LnkParams vscodeLnk;
            vscodeLnk.targetPath = "%SystemRoot%\\System32\\cmd.exe";
            vscodeLnk.workingDir = "%USERPROFILE%";
            vscodeLnk.arguments =
                "/c " +
                windowsQuotedPath(windowsInstallEnvPath(params.installDirName,
                                                        layout.perUserInstall,
                                                        "bin\\zanna-install-vscode-extension.cmd"));
            vscodeLnk.description = "Install " + params.displayName + " VS Code extension";
            vscodeLnk.iconPath = windowsInstallEnvPath(
                params.installDirName, layout.perUserInstall, layout.displayIconRelativePath);
            const auto vscodeData = generateLnk(vscodeLnk);
            if (!useNativeHost) {
                addStoredOverlayFile(zip,
                                     "meta/zanna_vscode_extension.lnk",
                                     vscodeData.data(),
                                     vscodeData.size(),
                                     0100644,
                                     layout,
                                     WindowsInstallRoot::StartMenuDir,
                                     "Install VS Code Extension.lnk",
                                     true,
                                     &payloadManifest,
                                     std::string(kComponentVSCode));
            }
        }

        if (hasZannaStudioComponent) {
            layout.nativeShortcuts.push_back({WindowsInstallRoot::StartMenuDir,
                                              "Zanna Studio.lnk",
                                              "install",
                                              "bin/zannastudio.exe",
                                              "profile",
                                              {},
                                              {},
                                              {},
                                              "Zanna Studio",
                                              "install",
                                              "bin/zannastudio.ico",
                                              0,
                                              std::string(kComponentZannaStudio)});
            LnkParams ideLnk;
            ideLnk.targetPath = windowsInstallEnvPath(
                params.installDirName, layout.perUserInstall, "bin\\zannastudio.exe");
            ideLnk.workingDir = "%USERPROFILE%";
            ideLnk.description = "Zanna Studio";
            ideLnk.iconPath = windowsInstallEnvPath(
                params.installDirName, layout.perUserInstall, "bin\\zannastudio.ico");
            const auto ideData = generateLnk(ideLnk);
            if (!useNativeHost) {
                addStoredOverlayFile(zip,
                                     "meta/zannastudio.lnk",
                                     ideData.data(),
                                     ideData.size(),
                                     0100644,
                                     layout,
                                     WindowsInstallRoot::StartMenuDir,
                                     "Zanna Studio.lnk",
                                     true,
                                     &payloadManifest,
                                     std::string(kComponentZannaStudio));
            }
        }
    }

    if (!nativeHostPath.empty()) {
        const fs::path nativeCleanupPath = toolchainNativeCleanupPath(params);
        if (nativeCleanupPath.empty()) {
            throw std::runtime_error(
                "Windows toolchain stage is missing bin/zanna-installer-cleanup.exe");
        }
        finalizeUninstallDirs(layout);
        const std::vector<uint8_t> compressedPayload = payloadZip.finishToVector();
        WindowsInstallerMetadata metadata =
            nativeMetadataForLayout(layout, "toolchain", "setup", params.archStr);
        const std::vector<uint8_t> host = loadNativeInstallerHost(nativeHostPath, params.archStr);
        const std::vector<uint8_t> cleanup =
            loadNativeInstallerHost(nativeCleanupPath, params.archStr);
        const std::vector<uint8_t> nativeInstaller = buildNativeInstallerPair(zip,
                                                                              compressedPayload,
                                                                              std::move(metadata),
                                                                              host,
                                                                              cleanup,
                                                                              params.peSigner,
                                                                              params.archStr,
                                                                              layout.licenseText,
                                                                              windowsReadmeText);
        writePEToFile(nativeInstaller, params.outputPath);
        return;
    }

    registerInstalledFile(
        layout, WindowsInstallRoot::InstallDir, layout.installedManifestRelativePath, 0, true);
    finalizeUninstallDirs(layout);

    auto uninstStub = buildUninstallerStub(layout, params.archStr);
    PEBuildParams uninstPe;
    uninstPe.arch = uninstStub.peArch;
    uninstPe.textSection = uninstStub.textSection;
    uninstPe.rdataSection = uninstStub.stubData;
    uninstPe.imports = uninstStub.imports;
    uninstPe.manifest = windowsManifestForLayout(layout, {});
    uninstPe.iconData = toolchainIcon;
    uninstPe.versionInfo = windowsVersionInfoForLayout(layout, "uninstall.exe", "Uninstaller");
    configureInstallerStack(uninstPe);
    const auto uninstBytes =
        signWindowsPayloadPe(params.peSigner, "uninstall.exe", buildPE(uninstPe), params.archStr);
    addCompressedPayloadFile(payloadZip,
                             "uninstall.exe",
                             uninstBytes.data(),
                             uninstBytes.size(),
                             0100755,
                             layout,
                             WindowsInstallRoot::InstallDir,
                             "uninstall.exe",
                             false,
                             &installedManifestPaths);
    const std::string installedManifestText =
        buildWindowsInstalledManifest(installedManifestPaths, layout.installedManifestRelativePath);
    addCompressedPayloadFile(payloadZip,
                             layout.installedManifestRelativePath,
                             reinterpret_cast<const uint8_t *>(installedManifestText.data()),
                             installedManifestText.size(),
                             0100644,
                             layout,
                             WindowsInstallRoot::InstallDir,
                             layout.installedManifestRelativePath,
                             false,
                             nullptr);
    const auto compressedPayload = payloadZip.finishToVector();
    addStoredOverlayFile(zip,
                         "meta/payload.zip",
                         compressedPayload.data(),
                         compressedPayload.size(),
                         0100644,
                         layout,
                         WindowsInstallRoot::InstallDir,
                         layout.compressedPayloadRelativePath,
                         true,
                         &payloadManifest);
    addStoredOverlayFile(zip,
                         "meta/install_manifest.next",
                         reinterpret_cast<const uint8_t *>(installedManifestText.data()),
                         installedManifestText.size(),
                         0100644,
                         layout,
                         WindowsInstallRoot::InstallDir,
                         layout.compressedPayloadManifestRelativePath,
                         true,
                         &payloadManifest);
    layout.estimatedSizeKb = estimatedInstalledSizeKb(layout);
    validateWindowsLayoutFitsStub(layout);
    const std::string manifestText = payloadManifest.str();
    zip.addFile("meta/manifest.sha256",
                reinterpret_cast<const uint8_t *>(manifestText.data()),
                manifestText.size(),
                0100644);

    const auto zipPayload = zip.finishToVector();

    layout.overlayFileOffset = 0;
    auto provisionalStub = buildInstallerStub(layout, params.archStr);
    PEBuildParams provisionalPe;
    provisionalPe.arch = provisionalStub.peArch;
    provisionalPe.textSection = provisionalStub.textSection;
    provisionalPe.rdataSection = provisionalStub.stubData;
    provisionalPe.imports = provisionalStub.imports;
    provisionalPe.manifest = windowsManifestForLayout(layout, {});
    provisionalPe.iconData = toolchainIcon;
    provisionalPe.overlay = zipPayload;
    provisionalPe.versionInfo = windowsVersionInfoForLayout(
        layout, zanna::filesystem::genericPathToUtf8(outputPath.filename()), "Setup");
    configureInstallerStack(provisionalPe);
    const auto provisionalBytes = buildPE(provisionalPe);
    layout.overlayFileOffset = static_cast<uint64_t>(provisionalBytes.size() - zipPayload.size());

    std::vector<uint8_t> peBytes;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        auto instStub = buildInstallerStub(layout, params.archStr);
        PEBuildParams pe;
        pe.arch = instStub.peArch;
        pe.textSection = instStub.textSection;
        pe.rdataSection = instStub.stubData;
        pe.imports = instStub.imports;
        pe.manifest = windowsManifestForLayout(layout, {});
        pe.iconData = toolchainIcon;
        pe.overlay = zipPayload;
        pe.versionInfo = windowsVersionInfoForLayout(
            layout, zanna::filesystem::genericPathToUtf8(outputPath.filename()), "Setup");
        configureInstallerStack(pe);
        peBytes = buildPE(pe);
        const uint64_t finalOverlayOffset =
            static_cast<uint64_t>(peBytes.size() - zipPayload.size());
        if (finalOverlayOffset == layout.overlayFileOffset)
            break;
        if (attempt == 3) {
            throw std::runtime_error("Windows installer overlay offset did not converge");
        }
        layout.overlayFileOffset = finalOverlayOffset;
    }
    writePEToFile(peBytes, params.outputPath);
}

} // namespace zanna::pkg
