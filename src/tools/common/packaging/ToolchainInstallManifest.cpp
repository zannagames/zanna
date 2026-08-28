//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/ToolchainInstallManifest.cpp
// Purpose: Gather and validate the Zanna toolchain staged install tree, classify
//          each file by kind, and map files to platform-specific install paths.
//
// Key invariants:
//   - Paths are always normalized via sanitizePackageRelativePath before use.
//   - Symlinks that escape the stage prefix are rejected at scan time.
//   - Every user-facing binary tool, CMake config, and runtime archive must be
//     present for validateToolchainInstallManifest to succeed.
//
// Ownership/Lifetime:
//   - ToolchainInstallManifest owns its file entries; no external references.
//
// Links: ToolchainInstallManifest.hpp, PkgUtils.hpp,
//        zanna/runtime/RuntimeComponentManifest.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements staged-toolchain discovery, validation, classification,
///        provenance extraction, and platform install-path mapping.
/// @details All filesystem-derived paths are boundary-checked against the staging
///          root before being recorded in an owned, deterministic manifest.

#include "ToolchainInstallManifest.hpp"

#include "PkgUtils.hpp"

#include "zanna/runtime/RuntimeComponentManifest.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace zanna::pkg {
namespace {

/// @brief Encode a native path as UTF-8 for package metadata and diagnostics.
/// @param path Native filesystem path.
/// @return Platform-native path syntax encoded as UTF-8.
std::string pathText(const fs::path &path) {
    return zanna::filesystem::pathToUtf8(path);
}

/// @brief Encode a native path as portable UTF-8 package syntax.
/// @param path Native filesystem path.
/// @return UTF-8 path using generic forward-slash separators.
std::string genericPathText(const fs::path &path) {
    return zanna::filesystem::genericPathToUtf8(path);
}

/// @brief Return a copy of text with all ASCII letters lowercased.
/// @param text Text to normalize in place.
/// @return Lowercase copy; non-ASCII bytes are passed through `std::tolower`.
std::string lowerCopy(std::string text) {
    /// @brief Fold one byte through ctype without signed-char undefined behavior.
    /// @param ch Unsigned source byte.
    /// @return Lowercase byte converted back to plain char.
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

/// @brief Strip the platform-specific extension and "lib" prefix from a filename to
/// get the canonical base name used for manifest lookups.
/// e.g. "libzannagfx.a" → "zannagfx", "zanna.exe" → "zanna".
/// @param filename Filename without directory components.
/// @return Lowercase logical component name without a recognized suffix or `lib` prefix.
std::string toolchainBaseNameFromFilename(std::string filename) {
    filename = lowerCopy(filename);
    if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".lib")
        filename.resize(filename.size() - 4);
    else if (filename.size() > 4 && filename.substr(filename.size() - 4) == ".exe")
        filename.resize(filename.size() - 4);
    else if (filename.size() > 2 && filename.substr(filename.size() - 2) == ".a")
        filename.resize(filename.size() - 2);
    if (filename.rfind("lib", 0) == 0)
        filename.erase(0, 3);
    return filename;
}

/// @brief Return true if base (after toolchainBaseNameFromFilename) matches a known
/// Zanna runtime component archive as listed in RuntimeComponentManifest.hpp.
/// @param base Canonical lowercase archive base name.
/// @return true when @p base occurs in the runtime component registry.
bool isRuntimeArchiveBaseName(const std::string &base) {
    return std::find(runtime_manifest::kRuntimeComponentArchives.begin(),
                     runtime_manifest::kRuntimeComponentArchives.end(),
                     base) != runtime_manifest::kRuntimeComponentArchives.end();
}

/// @brief Return true if base is a Zanna optional support library (graphics, GUI, audio).
/// @param base Canonical lowercase library base name.
/// @return true for the graphics, GUI, or audio support-library names.
bool isSupportLibraryBaseName(const std::string &base) {
    return base == "zannagfx" || base == "zannagui" || base == "zannaaud";
}

/// @brief Thin wrapper around isPathWithin for stage-prefix boundary checks.
/// @param stage Canonical staging root.
/// @param path Canonical candidate path.
/// @return true when @p path is equal to or descends from @p stage.
bool pathWithinStage(const fs::path &stage, const fs::path &path) {
    return isPathWithin(stage, path);
}

/// @brief Verify that path (after resolving any symlink) lies within stagePrefix.
/// Throws std::runtime_error if the path escapes the stage boundary or if
/// canonical resolution fails (e.g. dangling symlink).
/// @param stagePrefix Canonical staging root that owns package inputs.
/// @param path Existing regular file or symlink to validate.
/// @throws std::runtime_error If resolution fails or resolves outside @p stagePrefix.
void validateStagedPathDoesNotEscape(const fs::path &stagePrefix, const fs::path &path) {
    std::error_code ec;
    if (fs::is_symlink(path, ec)) {
        const fs::path resolved = fs::canonical(path, ec);
        if (ec)
            throw std::runtime_error("cannot resolve staged symlink: " + pathText(path));
        if (!pathWithinStage(stagePrefix, resolved))
            throw std::runtime_error("staged symlink escapes install prefix: " + pathText(path));
        return;
    }

    const fs::path resolved = fs::canonical(path, ec);
    if (ec)
        throw std::runtime_error("cannot resolve staged file: " + pathText(path));
    if (!pathWithinStage(stagePrefix, resolved))
        throw std::runtime_error("staged file escapes install prefix: " + pathText(path));
}

/// @brief Compute path's lexical relative path from stagePrefix without touching the
/// filesystem. Throws if the result would start with ".." (escapes the prefix)
/// or if the paths have no common root.
/// @param stagePrefix Staging root used as the lexical base.
/// @param path Candidate path below the staging root.
/// @return Non-empty lexical path relative to @p stagePrefix.
/// @throws std::runtime_error If no contained non-root relative path can be formed.
fs::path stagedLexicalRelativePath(const fs::path &stagePrefix, const fs::path &path) {
    const fs::path normalizedStage = stagePrefix.lexically_normal();
    const fs::path normalizedPath = path.lexically_normal();
    const fs::path rel = normalizedPath.lexically_relative(normalizedStage);
    if (rel.empty() || rel == fs::path("."))
        throw std::runtime_error("failed to compute staged relative path for " + pathText(path));
    auto relIt = rel.begin();
    if (relIt == rel.end() || *relIt == fs::path(".."))
        throw std::runtime_error("staged path escapes install prefix: " + pathText(path));
    return rel;
}

/// @brief Read the real POSIX permission bits from the filesystem and return them as
/// a USTAR-compatible uint32_t (regular file type bits ORed in). Falls back to
/// 0100755 or 0100644 if stat fails so the archive is still well-formed.
/// @param path Existing staged filesystem entry.
/// @param executable Whether fallback permissions should include execute bits.
/// @return POSIX type and permission bits suitable for archive metadata.
uint32_t unixModeFor(const fs::path &path, bool executable) {
    std::error_code ec;
    const fs::file_status status = fs::status(path, ec);
    if (ec)
        return executable ? 0100755u : 0100644u;

    using perms = fs::perms;
    uint32_t mode = fs::is_directory(status) ? 0040000u : 0100000u;
    const auto p = status.permissions();
    if ((p & perms::owner_read) != perms::none)
        mode |= 0400u;
    if ((p & perms::owner_write) != perms::none)
        mode |= 0200u;
    if ((p & perms::owner_exec) != perms::none)
        mode |= 0100u;
    if ((p & perms::group_read) != perms::none)
        mode |= 0040u;
    if ((p & perms::group_write) != perms::none)
        mode |= 0020u;
    if ((p & perms::group_exec) != perms::none)
        mode |= 0010u;
    if ((p & perms::others_read) != perms::none)
        mode |= 0004u;
    if ((p & perms::others_write) != perms::none)
        mode |= 0002u;
    if ((p & perms::others_exec) != perms::none)
        mode |= 0001u;
    return mode;
}

/// @brief Scan text for a version string using two heuristic patterns:
/// CMake: set(PACKAGE_VERSION "<version>"), C++ header: #define ZANNA_VERSION_STR "<version>".
/// Returns the extracted version or empty string if neither pattern is found.
/// @param text CMake or C++ configuration text to inspect.
/// @return First recognized version value, or an empty string when absent/malformed.
std::string parseVersionFromText(const std::string &text) {
    const char *patterns[] = {"set(PACKAGE_VERSION \"", "#define ZANNA_VERSION_STR \""};
    for (const char *pattern : patterns) {
        const std::size_t start = text.find(pattern);
        if (start == std::string::npos)
            continue;
        const std::size_t valueStart = start + std::char_traits<char>::length(pattern);
        const std::size_t end = text.find('"', valueStart);
        if (end != std::string::npos && end > valueStart)
            return text.substr(valueStart, end - valueStart);
    }
    return {};
}

/// @brief Extract one quoted preprocessor definition from configured build metadata.
/// @param text Generated header contents.
/// @param name Exact macro identifier following `#define`.
/// @return Text between the first following quote pair, or empty when unavailable.
std::string parseQuotedDefine(const std::string &text, std::string_view name) {
    const std::string prefix = "#define " + std::string(name);
    const std::size_t define = text.find(prefix);
    if (define == std::string::npos)
        return {};
    const std::size_t quote = text.find('"', define + prefix.size());
    if (quote == std::string::npos)
        return {};
    const std::size_t end = text.find('"', quote + 1U);
    if (end == std::string::npos)
        return {};
    return text.substr(quote + 1U, end - quote - 1U);
}

/// @brief Source identity recovered from the staged generated version header.
struct StagedBuildProvenance {
    std::string productVersion; ///< Exact configured product version.
    std::string snapshot;       ///< Optional descriptive snapshot identity.
    std::string commit;         ///< Optional lowercase source commit hash.
    std::string state{"unknown"}; ///< Source cleanliness state.
};

/// @brief Read immutable source provenance from the installed generated header.
/// @param stagePrefix Staging root expected to contain `include/zanna/version.hpp`.
/// @return Parsed provenance, retaining empty/default fields if the header is absent.
StagedBuildProvenance detectBuildProvenance(const fs::path &stagePrefix) {
    StagedBuildProvenance result;
    const fs::path header = stagePrefix / "include" / "zanna" / "version.hpp";
    std::ifstream input(header, std::ios::binary);
    if (!input)
        return result;
    std::ostringstream bytes;
    bytes << input.rdbuf();
    const std::string text = bytes.str();
    result.productVersion = parseQuotedDefine(text, "ZANNA_VERSION_STR");
    result.snapshot = parseQuotedDefine(text, "ZANNA_SNAPSHOT_STR");
    result.commit = parseQuotedDefine(text, "ZANNA_SOURCE_COMMIT_STR");
    const std::string state = parseQuotedDefine(text, "ZANNA_SOURCE_STATE_STR");
    if (!state.empty())
        result.state = state;
    return result;
}

/// @brief Probe well-known files in the staged install tree to infer the toolchain
/// version without requiring a separate manifest argument. Tries CMake config
/// first, then the C++ version header. Returns empty if neither file exists.
/// @param stagePrefix Staging root containing installed metadata.
/// @return First detected package version, or an empty string when none is readable.
std::string detectManifestVersion(const fs::path &stagePrefix) {
    const fs::path versionCandidates[] = {
        stagePrefix / "lib" / "cmake" / "Zanna" / "ZannaConfigVersion.cmake",
        stagePrefix / "include" / "zanna" / "version.hpp",
    };
    for (const auto &candidate : versionCandidates) {
        std::error_code ec;
        if (!fs::exists(candidate, ec))
            continue;
        std::ifstream in(candidate, std::ios::binary);
        if (!in)
            continue;
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string version = parseVersionFromText(ss.str());
        if (!version.empty())
            return version;
    }
    return {};
}

/// @brief Platform/architecture identity read from a staged executable.
/// @details Toolchain packages may be built from cross-staged trees, so host CPU
///          and OS are not reliable. This structure captures the payload identity
///          detected from the staged `zanna` binary instead.
struct StagedToolchainIdentity {
    std::string platform; ///< Canonical package platform: windows, macos, or linux.
    std::string arch;     ///< Canonical package architecture: x64, arm64, or universal.
};

/// @brief Read up to @p maxBytes from @p path for executable header inspection.
/// @param path Staged executable to open in binary mode.
/// @param maxBytes Maximum prefix size to allocate and read.
/// @return A byte vector containing the prefix; empty only for an empty file.
/// @throws std::runtime_error If the file cannot be opened or read.
std::vector<uint8_t> readBinaryPrefix(const fs::path &path, std::size_t maxBytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read staged executable header: " + pathText(path));
    std::vector<uint8_t> bytes(maxBytes);
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    const std::streamsize got = in.gcount();
    if (got < 0)
        throw std::runtime_error("cannot read staged executable header: " + pathText(path));
    bytes.resize(static_cast<std::size_t>(got));
    return bytes;
}

/// @brief Read a little-endian 16-bit value from @p bytes at @p offset.
/// @param bytes Buffer containing an executable header.
/// @param offset Zero-based field offset.
/// @return Decoded host-order 16-bit value.
/// @throws std::runtime_error If fewer than two bytes remain.
uint16_t readLe16(const std::vector<uint8_t> &bytes, std::size_t offset) {
    if (offset + 2 > bytes.size())
        throw std::runtime_error("truncated staged executable header");
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

/// @brief Read a little-endian 32-bit value from @p bytes at @p offset.
/// @param bytes Buffer containing an executable header.
/// @param offset Zero-based field offset.
/// @return Decoded host-order 32-bit value.
/// @throws std::runtime_error If fewer than four bytes remain.
uint32_t readLe32(const std::vector<uint8_t> &bytes, std::size_t offset) {
    if (offset + 4 > bytes.size())
        throw std::runtime_error("truncated staged executable header");
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

/// @brief Read a big-endian 32-bit value from @p bytes at @p offset.
/// @param bytes Buffer containing an executable header.
/// @param offset Zero-based field offset.
/// @return Decoded host-order 32-bit value.
/// @throws std::runtime_error If fewer than four bytes remain.
uint32_t readBe32(const std::vector<uint8_t> &bytes, std::size_t offset) {
    if (offset + 4 > bytes.size())
        throw std::runtime_error("truncated staged executable header");
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

/// @brief Read an endian-selected 64-bit value from @p bytes at @p offset.
/// @param bytes Buffer containing an executable header.
/// @param offset Zero-based field offset.
/// @param bigEndian true to decode most-significant byte first.
/// @return Decoded host-order 64-bit value.
/// @throws std::runtime_error If fewer than eight bytes remain.
uint64_t readEndian64(const std::vector<uint8_t> &bytes, std::size_t offset, bool bigEndian) {
    if (offset + 8 > bytes.size())
        throw std::runtime_error("truncated staged executable header");
    uint64_t value = 0;
    if (bigEndian) {
        for (std::size_t i = 0; i < 8; ++i)
            value = (value << 8) | bytes[offset + i];
    } else {
        for (std::size_t i = 0; i < 8; ++i)
            value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
    }
    return value;
}

/// @brief Inspect a Mach-O header and return its canonical payload architecture.
/// @return x64, arm64, or universal; nullopt when the bytes are not Mach-O.
/// @details Fat headers are parsed rather than trusted by magic alone. Every declared slice must
///          have a non-empty range inside the file, and `universal` requires both x86_64 and arm64.
/// @param path Full file path used for size queries and diagnostics.
/// @param bytes Prefix containing the complete Mach-O header and fat slice table.
/// @throws std::runtime_error For truncated, unsupported, or out-of-bounds Mach-O metadata.
std::optional<std::string> detectMachOArchitecture(const fs::path &path,
                                                   const std::vector<uint8_t> &bytes) {
    if (bytes.size() < 4)
        return std::nullopt;

    /// @brief Compare the file prefix against one four-byte executable magic.
    /// @param a Expected first byte.
    /// @param b Expected second byte.
    /// @param c Expected third byte.
    /// @param d Expected fourth byte.
    /// @return `true` when the prefix matches exactly.
    const auto hasMagic = [&](uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
        return bytes[0] == a && bytes[1] == b && bytes[2] == c && bytes[3] == d;
    };
    bool headerBigEndian = false;
    bool thin = false;
    if (hasMagic(0xCF, 0xFA, 0xED, 0xFE) || hasMagic(0xCE, 0xFA, 0xED, 0xFE)) {
        thin = true;
    } else if (hasMagic(0xFE, 0xED, 0xFA, 0xCF) || hasMagic(0xFE, 0xED, 0xFA, 0xCE)) {
        thin = true;
        headerBigEndian = true;
    }
    if (thin) {
        if (bytes.size() < 8)
            throw std::runtime_error("truncated Mach-O executable header: " + pathText(path));
        const uint32_t cpuType = headerBigEndian ? readBe32(bytes, 4) : readLe32(bytes, 4);
        if (cpuType == 0x01000007)
            return std::string("x64");
        if (cpuType == 0x0100000c)
            return std::string("arm64");
        throw std::runtime_error("unsupported Mach-O executable architecture in '" +
                                 pathText(path) + "': CPU type " + std::to_string(cpuType));
    }

    bool fat64 = false;
    if (hasMagic(0xCA, 0xFE, 0xBA, 0xBE)) {
        headerBigEndian = true;
    } else if (hasMagic(0xBE, 0xBA, 0xFE, 0xCA)) {
        headerBigEndian = false;
    } else if (hasMagic(0xCA, 0xFE, 0xBA, 0xBF)) {
        headerBigEndian = true;
        fat64 = true;
    } else if (hasMagic(0xBF, 0xBA, 0xFE, 0xCA)) {
        headerBigEndian = false;
        fat64 = true;
    } else {
        return std::nullopt;
    }

    if (bytes.size() < 8)
        throw std::runtime_error("truncated universal Mach-O header: " + pathText(path));
    /// @brief Read one 32-bit fat-header field using its detected byte order.
    /// @param offset Byte offset within @p bytes.
    /// @return Decoded unsigned field value.
    const auto read32 = [&](std::size_t offset) {
        return headerBigEndian ? readBe32(bytes, offset) : readLe32(bytes, offset);
    };
    const uint32_t sliceCount = read32(4);
    constexpr uint32_t kMaxFatSlices = 32;
    if (sliceCount == 0 || sliceCount > kMaxFatSlices)
        throw std::runtime_error("invalid universal Mach-O slice count in '" + pathText(path) +
                                 "': " + std::to_string(sliceCount));
    const std::size_t entrySize = fat64 ? 32u : 20u;
    if (sliceCount > (bytes.size() - 8) / entrySize)
        throw std::runtime_error("truncated universal Mach-O architecture table: " +
                                 pathText(path));

    std::error_code ec;
    const uint64_t fileSize = fs::file_size(path, ec);
    if (ec)
        throw std::runtime_error("cannot inspect universal Mach-O size for '" + pathText(path) +
                                 "': " + ec.message());
    const uint64_t headerSize = 8u + static_cast<uint64_t>(sliceCount) * entrySize;
    bool hasX64 = false;
    bool hasArm64 = false;
    for (uint32_t i = 0; i < sliceCount; ++i) {
        const std::size_t entry = 8u + static_cast<std::size_t>(i) * entrySize;
        const uint32_t cpuType = read32(entry);
        const uint64_t sliceOffset =
            fat64 ? readEndian64(bytes, entry + 8, headerBigEndian) : read32(entry + 8);
        const uint64_t sliceSize =
            fat64 ? readEndian64(bytes, entry + 16, headerBigEndian) : read32(entry + 12);
        if (sliceSize == 0 || sliceOffset < headerSize || sliceOffset > fileSize ||
            sliceSize > fileSize - sliceOffset) {
            throw std::runtime_error("invalid universal Mach-O slice bounds in '" + pathText(path) +
                                     "'");
        }
        if (cpuType == 0x01000007)
            hasX64 = true;
        else if (cpuType == 0x0100000c)
            hasArm64 = true;
        else
            throw std::runtime_error("unsupported universal Mach-O architecture in '" +
                                     pathText(path) + "': CPU type " + std::to_string(cpuType));
    }
    if (hasX64 && hasArm64)
        return std::string("universal");
    if (hasX64)
        return std::string("x64");
    if (hasArm64)
        return std::string("arm64");
    throw std::runtime_error("universal Mach-O has no supported slices: " + pathText(path));
}

/// @brief Detect payload platform and architecture from the staged `zanna` binary.
/// @details Supports PE (Windows), ELF (Linux), thin Mach-O, and universal Mach-O
///          headers. Throws when the executable format or CPU is unsupported so
///          cross-packaging cannot silently inherit the build host identity.
/// @param path Staged executable to inspect.
/// @return Canonical platform and architecture inferred from its binary header.
/// @throws std::runtime_error If the header is unreadable, malformed, or unsupported.
StagedToolchainIdentity detectStagedExecutableIdentity(const fs::path &path) {
    const std::vector<uint8_t> bytes = readBinaryPrefix(path, 4096);
    if (bytes.size() >= 64 && bytes[0] == 'M' && bytes[1] == 'Z') {
        const uint32_t peOffset = readLe32(bytes, 0x3c);
        if (peOffset <= bytes.size() - 6 && bytes[peOffset] == 'P' && bytes[peOffset + 1] == 'E' &&
            bytes[peOffset + 2] == 0 && bytes[peOffset + 3] == 0) {
            const uint16_t machine = readLe16(bytes, peOffset + 4);
            if (machine == 0x8664)
                return {"windows", "x64"};
            if (machine == 0xAA64)
                return {"windows", "arm64"};
        }
        throw std::runtime_error("unsupported Windows zanna executable architecture: " +
                                 pathText(path));
    }
    if (bytes.size() >= 20 && bytes[0] == 0x7f && bytes[1] == 'E' && bytes[2] == 'L' &&
        bytes[3] == 'F') {
        const uint16_t machine = readLe16(bytes, 18);
        if (machine == 62)
            return {"linux", "x64"};
        if (machine == 183)
            return {"linux", "arm64"};
        throw std::runtime_error("unsupported ELF zanna executable architecture: " +
                                 pathText(path));
    }
    if (const auto arch = detectMachOArchitecture(path, bytes))
        return {"macos", *arch};
    throw std::runtime_error("cannot determine staged zanna executable platform: " +
                             pathText(path));
}

/// @brief Require every Mach-O payload file to match the staged zanna architecture.
/// @param stagePrefix Canonical staging root used to report relative paths.
/// @param files Gathered staged entries to inspect when they are regular files.
/// @param expectedArch Architecture detected from the primary `zanna` executable.
/// @throws std::runtime_error If inspection fails or any Mach-O payload differs.
void validateMacOSPayloadArchitectures(const fs::path &stagePrefix,
                                       const std::vector<fs::path> &files,
                                       const std::string &expectedArch) {
    for (const fs::path &file : files) {
        std::error_code ec;
        if (!fs::is_regular_file(file, ec)) {
            if (ec)
                throw std::runtime_error("cannot inspect staged macOS payload file '" +
                                         pathText(file) + "': " + ec.message());
            continue;
        }
        const std::vector<uint8_t> bytes = readBinaryPrefix(file, 4096);
        const std::optional<std::string> arch = detectMachOArchitecture(file, bytes);
        if (!arch || *arch == expectedArch)
            continue;
        const fs::path relative = stagedLexicalRelativePath(stagePrefix, file);
        throw std::runtime_error("macOS payload architecture mismatch: '" +
                                 genericPathText(relative) + "' is " + *arch +
                                 " but staged zanna is " + expectedArch);
    }
}

/// @brief Find the staged `zanna` executable and return its payload identity.
/// @param stagePrefix Canonical staging root.
/// @param files Files gathered from the stage or install manifest.
/// @return Platform and architecture detected from the executable header.
StagedToolchainIdentity detectStagedToolchainIdentity(const fs::path &stagePrefix,
                                                      const std::vector<fs::path> &files) {
    for (const auto &file : files) {
        const fs::path rel = stagedLexicalRelativePath(stagePrefix, file);
        const std::string relText = lowerCopy(genericPathText(rel));
        const std::string filename = lowerCopy(pathText(file.filename()));
        if (relText == "bin/zanna" || relText == "bin/zanna.exe" || filename == "zanna" ||
            filename == "zanna.exe") {
            return detectStagedExecutableIdentity(file);
        }
    }
    throw std::runtime_error("staged toolchain is missing a detectable zanna executable");
}

/// @brief Map a staged relative path to a ToolchainFileKind by inspecting its directory
/// prefix, then its lowercased filename base when the prefix alone is ambiguous
/// (lib/ can contain runtime archives, support libs, or generic libraries).
/// The order of checks matters: more specific prefixes must come before general ones.
/// @param relativePath Sanitized staged path using forward slashes.
/// @return Most specific install-manifest classification for the entry.
ToolchainFileKind classifyFileKind(const std::string &relativePath) {
    const std::string rel = lowerCopy(relativePath);
    const fs::path relPath = zanna::filesystem::pathFromUtf8(relativePath);
    const std::string filenameBase = toolchainBaseNameFromFilename(pathText(relPath.filename()));

    if (rel.rfind("bin/", 0) == 0 && rel.size() >= 10U &&
        rel.substr(rel.size() - 10U) == ".buildinfo") {
        return ToolchainFileKind::Extra;
    }
    if (rel.rfind("bin/", 0) == 0)
        return ToolchainFileKind::Binary;
    if (rel.rfind("include/", 0) == 0)
        return ToolchainFileKind::Header;
    if (rel.rfind("lib/cmake/zanna/", 0) == 0)
        return ToolchainFileKind::CMakeConfig;
    if (rel.rfind("share/man/", 0) == 0)
        return ToolchainFileKind::ManPage;
    if (rel == "license" || rel == "readme.md" || rel.rfind("share/doc/", 0) == 0)
        return ToolchainFileKind::Doc;
    if (rel.rfind("share/zanna/", 0) == 0 || rel.rfind("share/", 0) == 0)
        return ToolchainFileKind::Extra;
    if ((rel.rfind("lib/", 0) == 0 || rel.find('/') == std::string::npos) &&
        isRuntimeArchiveBaseName(filenameBase)) {
        return ToolchainFileKind::RuntimeArchive;
    }
    if ((rel.rfind("lib/", 0) == 0 || rel.find('/') == std::string::npos) &&
        isSupportLibraryBaseName(filenameBase)) {
        return ToolchainFileKind::SupportLibrary;
    }
    if (rel.rfind("lib/", 0) == 0)
        return ToolchainFileKind::Library;
    return ToolchainFileKind::Extra;
}

/// @brief Return path's lexical relative path from prefix, or std::nullopt if path is
/// not under prefix. Used to remap cmake_install.cmake paths that were written
/// with a build-time alias prefix instead of the final staging root.
/// @param prefix Candidate lexical ancestor.
/// @param path Path to express relative to @p prefix.
/// @return Non-root relative path, or `std::nullopt` if containment is not lexical.
std::optional<fs::path> lexicalRelativeIfUnder(const fs::path &prefix, const fs::path &path) {
    const fs::path rel = path.lexically_normal().lexically_relative(prefix.lexically_normal());
    if (rel.empty() || rel == fs::path("."))
        return std::nullopt;
    auto it = rel.begin();
    if (it == rel.end() || *it == fs::path(".."))
        return std::nullopt;
    return rel;
}

/// @brief Parse cmake_install.cmake's install_manifest.txt and return the absolute
/// paths to all regular files and symlinks it lists. Handles both absolute paths
/// (emitted by cmake) and paths written with a build-alias prefix by remapping
/// them through stageAlias → stagePrefix. Duplicate entries are silently dropped.
/// @param stagePrefix Canonical root containing actual installed entries.
/// @param stageAliasPrefix Build-time prefix that manifest lines may reference.
/// @param installManifestPath Text manifest to read one path per line.
/// @return Unique existing regular-file and symlink paths in manifest order.
/// @throws std::runtime_error If the manifest is unreadable or lists unsafe entries.
std::vector<fs::path> gatherFromInstallManifest(const fs::path &stagePrefix,
                                                const fs::path &stageAliasPrefix,
                                                const fs::path &installManifestPath) {
    std::ifstream in(installManifestPath);
    if (!in)
        throw std::runtime_error("cannot read install manifest: " + pathText(installManifestPath));

    std::vector<fs::path> files;
    std::set<std::string> seen;
    const fs::path normalizedStage = fs::weakly_canonical(stagePrefix);
    const fs::path normalizedAlias = stageAliasPrefix.lexically_normal();
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty())
            continue;
        fs::path filePath = zanna::filesystem::pathFromUtf8(line);
        if (filePath.is_relative()) {
            filePath = normalizedStage / filePath;
        } else if (auto relViaAlias = lexicalRelativeIfUnder(normalizedAlias, filePath)) {
            filePath = normalizedStage / *relViaAlias;
        }
        filePath = filePath.lexically_normal();
        std::error_code ec;
        if (!fs::is_regular_file(filePath, ec) && !fs::is_symlink(filePath, ec)) {
            throw std::runtime_error("install manifest lists a missing or unsupported path: " +
                                     pathText(filePath));
        }
        validateStagedPathDoesNotEscape(normalizedStage, filePath);
        fs::path rel;
        rel = stagedLexicalRelativePath(normalizedStage, filePath);
        const std::string relKey =
            sanitizePackageRelativePath(genericPathText(rel), "staged install path");
        if (seen.insert(relKey).second)
            files.push_back(filePath);
    }
    return files;
}

/// @brief Fallback when no install manifest is provided: recursively enumerate all
/// regular files and symlinks under stagePrefix. Traversal errors are fatal so
/// packaging cannot silently omit unreadable files from a release installer.
/// @param stagePrefix Existing staging directory to traverse.
/// @return Unique contained regular-file and symlink paths in traversal order.
/// @throws std::runtime_error On traversal, inspection, resolution, or safety failure.
std::vector<fs::path> gatherFromStageWalk(const fs::path &stagePrefix) {
    std::vector<fs::path> files;
    std::set<std::string> seen;
    std::error_code ec;
    const fs::path normalizedStage = fs::weakly_canonical(stagePrefix);
    for (fs::recursive_directory_iterator it(stagePrefix, ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec)
            throw std::runtime_error("cannot enumerate staged install tree: " + ec.message());
        std::error_code typeEc;
        const bool regular = it->is_regular_file(typeEc);
        if (typeEc)
            throw std::runtime_error("cannot inspect staged path: " + pathText(it->path()));
        const bool symlink = it->is_symlink(typeEc);
        if (typeEc)
            throw std::runtime_error("cannot inspect staged path: " + pathText(it->path()));
        if (!regular && !symlink)
            continue;
        validateStagedPathDoesNotEscape(normalizedStage, it->path());
        fs::path rel;
        try {
            rel = stagedLexicalRelativePath(normalizedStage, it->path());
        } catch (const std::runtime_error &) {
            continue;
        }
        const std::string relKey =
            sanitizePackageRelativePath(genericPathText(rel), "staged install path");
        if (seen.insert(relKey).second)
            files.push_back(it->path());
    }
    if (ec)
        throw std::runtime_error("cannot enumerate staged install tree: " + ec.message());
    return files;
}

/// @brief Construct a ToolchainFileEntry for filePath by computing its relative path,
/// classifying its kind, reading its POSIX mode, and — for symlinks — rebasing
/// absolute link targets to relative ones so they stay valid after installation.
/// @param stagePrefix Canonical staging root.
/// @param filePath Contained regular file or symlink to describe.
/// @return Fully populated owned manifest entry.
/// @throws std::runtime_error If metadata cannot be read or normalized safely.
ToolchainFileEntry makeEntry(const fs::path &stagePrefix, const fs::path &filePath) {
    std::error_code ec;
    const fs::path rel = stagedLexicalRelativePath(stagePrefix, filePath);

    ToolchainFileEntry entry;
    entry.stagedAbsolutePath = filePath;
    entry.stagedRelativePath =
        sanitizePackageRelativePath(genericPathText(rel), "staged install path");
    entry.kind = classifyFileKind(entry.stagedRelativePath);
    entry.symlink = fs::is_symlink(filePath, ec);
    ec.clear();
    if (entry.symlink) {
        const fs::path rawTarget = fs::read_symlink(filePath, ec);
        if (ec)
            throw std::runtime_error("cannot read staged symlink target: " + pathText(filePath));
        const fs::path resolved = fs::canonical(filePath, ec);
        if (ec)
            throw std::runtime_error("cannot resolve staged symlink: " + pathText(filePath));
        const fs::path parent = fs::canonical(filePath.parent_path(), ec);
        if (ec)
            throw std::runtime_error("cannot resolve staged symlink parent: " + pathText(filePath));
        if (rawTarget.is_absolute()) {
            entry.symlinkTarget = genericPathText(fs::relative(resolved, parent, ec));
            if (ec)
                throw std::runtime_error("cannot compute staged symlink target: " +
                                         pathText(filePath));
        } else {
            entry.symlinkTarget = genericPathText(rawTarget);
        }
        validateSingleLineField(entry.symlinkTarget, "staged symlink target");
        if (entry.symlinkTarget.empty() || entry.symlinkTarget.front() == '/' ||
            (entry.symlinkTarget.size() >= 2 &&
             std::isalpha(static_cast<unsigned char>(entry.symlinkTarget[0])) &&
             entry.symlinkTarget[1] == ':')) {
            throw std::runtime_error("staged symlink target must be relative: " +
                                     pathText(filePath));
        }
        entry.sizeBytes = 0;
    } else {
        entry.sizeBytes = static_cast<uint64_t>(fs::file_size(filePath, ec));
        if (ec)
            throw std::runtime_error("cannot determine staged file size: " + pathText(filePath));
    }
    const std::string lower = lowerCopy(entry.stagedRelativePath);
    entry.executable = entry.kind == ToolchainFileKind::Binary ||
                       (lower.size() > 4 && lower.substr(lower.size() - 4) == ".exe");
    entry.unixMode = entry.symlink ? 0120777u : unixModeFor(filePath, entry.executable);
    return entry;
}

/// @brief Return true if the manifest contains an entry whose stagedRelativePath equals
/// relPath. Used by validateToolchainInstallManifest to check for specific
/// required files like ZannaConfig.cmake and ZannaTargets.cmake.
/// @param manifest Manifest inventory to search.
/// @param relPath Case-insensitive staged-relative path to locate.
/// @return true when an entry's path matches @p relPath.
bool manifestHasRelativePath(const ToolchainInstallManifest &manifest, std::string_view relPath) {
    const std::string needle = lowerCopy(std::string(relPath));
    /// @brief Match one manifest entry against the normalized relative path.
    /// @param entry Candidate manifest file.
    /// @return `true` when its staged path equals @c needle.
    return std::any_of(
        manifest.files.begin(), manifest.files.end(), [&](const ToolchainFileEntry &entry) {
            return lowerCopy(entry.stagedRelativePath) == needle;
        });
}

/// @brief Return true if any manifest entry has both the given kind and the given base
/// name (after stripping extension and "lib" prefix). Used to detect that all
/// required runtime archives and support libraries are present regardless of
/// platform-specific naming ("librt_core.a" vs "rt_core.lib").
/// @param manifest Manifest inventory to search.
/// @param kind Required file classification.
/// @param baseName Canonical logical name without platform prefix or extension.
/// @return true when one entry matches both classification and base name.
bool manifestHasBaseNameKind(const ToolchainInstallManifest &manifest,
                             ToolchainFileKind kind,
                             std::string_view baseName) {
    /// @brief Match one manifest entry by classification and logical base name.
    /// @param entry Candidate manifest file.
    /// @return `true` when both requested attributes match.
    return std::any_of(
        manifest.files.begin(), manifest.files.end(), [&](const ToolchainFileEntry &entry) {
            if (entry.kind != kind)
                return false;
            return toolchainBaseNameFromFilename(pathText(
                       zanna::filesystem::pathFromUtf8(entry.stagedRelativePath).filename())) ==
                   baseName;
        });
}

/// @brief Scan every CMakeConfig entry in the manifest for needle (case-insensitive).
/// Used to detect optional support-library components declared in ZannaTargets.cmake
/// so validation can require their corresponding library archives to be present.
/// @param manifest Manifest whose readable CMake metadata should be searched.
/// @param needle Case-insensitive text fragment to locate.
/// @return true when any non-symlink CMake configuration contains @p needle.
/// @throws std::runtime_error If a listed metadata file cannot be read fully.
bool stagedCMakeMetadataMentions(const ToolchainInstallManifest &manifest,
                                 std::string_view needle) {
    const std::string lowerNeedle = lowerCopy(std::string(needle));
    for (const auto &entry : manifest.files) {
        if (entry.kind != ToolchainFileKind::CMakeConfig || entry.symlink)
            continue;
        std::ifstream in(entry.stagedAbsolutePath, std::ios::binary);
        if (!in)
            throw std::runtime_error("cannot read staged CMake metadata: " +
                                     pathText(entry.stagedAbsolutePath));
        std::ostringstream ss;
        ss << in.rdbuf();
        if (!in.good() && !in.eof())
            throw std::runtime_error("failed while reading staged CMake metadata: " +
                                     pathText(entry.stagedAbsolutePath));
        if (lowerCopy(ss.str()).find(lowerNeedle) != std::string::npos)
            return true;
    }
    return false;
}

/// @brief Validate the per-file invariants every package builder depends on.
/// @details Checks normalized unique relative paths, absolute staged source paths,
///          file/symlink existence, symlink target safety, and size consistency for
///          regular files. This catches hand-built or stale manifests before a
///          platform builder copies or dereferences their entries.
/// @param manifest Manifest whose file entries should be validated.
void validateManifestFileEntries(const ToolchainInstallManifest &manifest) {
    std::set<std::string> seenPaths;
    for (const auto &entry : manifest.files) {
        const std::string clean =
            sanitizePackageRelativePath(entry.stagedRelativePath, "staged install path");
        if (clean.empty())
            throw std::runtime_error("toolchain manifest contains an empty file path");
        if (clean != entry.stagedRelativePath) {
            throw std::runtime_error("toolchain manifest path is not normalized: " +
                                     entry.stagedRelativePath);
        }
        if (!seenPaths.insert(clean).second)
            throw std::runtime_error("duplicate toolchain manifest path: " + clean);
        if (entry.stagedAbsolutePath.empty() || !entry.stagedAbsolutePath.is_absolute()) {
            throw std::runtime_error("toolchain manifest source path must be absolute for " +
                                     clean);
        }

        std::error_code ec;
        const fs::file_status symlinkStatus = fs::symlink_status(entry.stagedAbsolutePath, ec);
        if (ec)
            throw std::runtime_error("cannot stat staged manifest path '" +
                                     pathText(entry.stagedAbsolutePath) + "': " + ec.message());
        if (entry.symlink) {
            if (!fs::is_symlink(symlinkStatus)) {
                throw std::runtime_error("toolchain manifest marks a non-symlink as symlink: " +
                                         clean);
            }
            if (entry.sizeBytes != 0)
                throw std::runtime_error("toolchain manifest symlink size must be zero: " + clean);
            validateSingleLineField(entry.symlinkTarget, "staged symlink target");
            if (entry.symlinkTarget.empty())
                throw std::runtime_error("toolchain manifest symlink target is empty: " + clean);
            const std::string normalizedTarget =
                genericPathText(zanna::filesystem::pathFromUtf8(entry.symlinkTarget));
            if (normalizedTarget.front() == '/' ||
                (normalizedTarget.size() >= 2 &&
                 std::isalpha(static_cast<unsigned char>(normalizedTarget[0])) &&
                 normalizedTarget[1] == ':')) {
                throw std::runtime_error("toolchain manifest symlink target must be relative: " +
                                         clean);
            }
            const fs::path resolved = (zanna::filesystem::pathFromUtf8(clean).parent_path() /
                                       zanna::filesystem::pathFromUtf8(normalizedTarget))
                                          .lexically_normal();
            const std::string resolvedText = genericPathText(resolved);
            if (resolvedText.empty() || resolvedText == "." || resolvedText == ".." ||
                resolvedText.rfind("../", 0) == 0) {
                throw std::runtime_error(
                    "toolchain manifest symlink target escapes install root: " + clean);
            }
            ec.clear();
            const fs::path canonicalTarget = fs::canonical(entry.stagedAbsolutePath, ec);
            if (ec) {
                throw std::runtime_error("toolchain manifest symlink target cannot be resolved: " +
                                         clean);
            }
            ec.clear();
            const fs::file_status targetStatus = fs::status(canonicalTarget, ec);
            if (ec || (!fs::is_regular_file(targetStatus) && !fs::is_directory(targetStatus))) {
                throw std::runtime_error(
                    "toolchain manifest symlink must resolve to a file or directory: " + clean);
            }
            continue;
        }

        if (!fs::is_regular_file(symlinkStatus)) {
            throw std::runtime_error("toolchain manifest entry is not a regular file: " + clean);
        }
        if (!entry.symlinkTarget.empty())
            throw std::runtime_error("toolchain manifest regular file has a symlink target: " +
                                     clean);
        ec.clear();
        const uint64_t actualSize =
            static_cast<uint64_t>(fs::file_size(entry.stagedAbsolutePath, ec));
        if (ec) {
            throw std::runtime_error("cannot determine staged file size for '" + clean +
                                     "': " + ec.message());
        }
        if (actualSize != entry.sizeBytes) {
            throw std::runtime_error("toolchain manifest file size mismatch for '" + clean + "'");
        }
    }
}

} // namespace

/// @brief Return the sum of sizeBytes across all non-symlink entries. Used by package
/// builders to populate the "Installed-Size" field in control files and to
/// estimate required disk space in installer UI dialogs.
/// @return Sum of all recorded entry sizes; symlinks contribute their stored zero size.
/// @throws std::overflow_error If the total exceeds `uint64_t`.
uint64_t ToolchainInstallManifest::totalSizeBytes() const {
    uint64_t total = 0;
    for (const auto &file : files) {
        if (file.sizeBytes > std::numeric_limits<uint64_t>::max() - total)
            throw std::overflow_error("toolchain manifest total size overflow");
        total += file.sizeBytes;
    }
    return total;
}

/// @brief Return the canonical set of file type associations for the Zanna toolchain:
/// .zia (Zia source), .bas (BASIC source), .il (Zanna IL module). These are
/// registered with the OS by all platform package builders (deb, pkg, msi).
/// @return Owned association descriptors in stable Zia, BASIC, then IL order.
std::vector<FileAssoc> defaultToolchainFileAssociations() {
    return {
        {".zia", "Zia Source File", "text/x-zia", ""},
        {".bas", "BASIC Source File", "text/x-basic", ""},
        {".il", "Zanna IL Module", "text/x-zanna-il", ""},
    };
}

/// @brief Walk stagePrefix and build a ToolchainInstallManifest.
/// If installManifestPath names a cmake install_manifest.txt, its listed files
/// are used directly (faster, avoids scanning the entire tree). If that path is
/// absent or yields no files, falls back to a full recursive walk. After gathering,
/// each entry is classified, validated against stage-escape rules, and the
/// manifest is sorted by stagedRelativePath before being returned.
/// @param stagePrefix Existing staged installation root.
/// @param installManifestPath Optional CMake install manifest used instead of a walk.
/// @return Validated, path-sorted, self-contained toolchain manifest.
/// @throws std::runtime_error If discovery, identity detection, metadata parsing,
///         containment checks, or final validation fails.
ToolchainInstallManifest gatherToolchainInstallManifest(
    const fs::path &stagePrefix, std::optional<fs::path> installManifestPath) {
    std::error_code ec;
    const fs::path absoluteStagePrefix = fs::absolute(stagePrefix, ec);
    const fs::path stageAlias = (ec ? stagePrefix : absoluteStagePrefix).lexically_normal();
    ec.clear();
    const fs::path normalizedStage = fs::weakly_canonical(stagePrefix, ec);
    const fs::path stage = ec ? stagePrefix.lexically_normal() : normalizedStage;
    if (!fs::exists(stage) || !fs::is_directory(stage))
        throw std::runtime_error("staged install prefix does not exist: " + pathText(stage));

    std::vector<fs::path> files;
    if (installManifestPath && fs::exists(*installManifestPath))
        files = gatherFromInstallManifest(stage, stageAlias, *installManifestPath);
    if (files.empty())
        files = gatherFromStageWalk(stage);

    ToolchainInstallManifest manifest;
    const StagedToolchainIdentity identity = detectStagedToolchainIdentity(stage, files);
    const StagedBuildProvenance provenance = detectBuildProvenance(stage);
    if (identity.platform == "macos")
        validateMacOSPayloadArchitectures(stage, files, identity.arch);
    manifest.version = detectManifestVersion(stage);
    manifest.productVersion = provenance.productVersion;
    manifest.snapshot = provenance.snapshot;
    manifest.sourceCommit = provenance.commit;
    manifest.sourceState = provenance.state;
    manifest.arch = identity.arch;
    manifest.platform = identity.platform;
    manifest.fileAssociations = defaultToolchainFileAssociations();
    manifest.files.reserve(files.size());
    for (const auto &file : files)
        manifest.files.push_back(makeEntry(stage, file));

    std::sort(manifest.files.begin(),
              manifest.files.end(),
              /// @brief Order manifest entries by staged relative path.
              /// @param a First entry.
              /// @param b Second entry.
              /// @return `true` when @p a has the earlier path.
              [](const ToolchainFileEntry &a, const ToolchainFileEntry &b) {
                  return a.stagedRelativePath < b.stagedRelativePath;
              });

    validateToolchainInstallManifest(manifest);
    return manifest;
}

/// @brief Return the canonical binary tools that every Zanna toolchain installer ships.
/// @return Stable list of logical binary base names without platform extensions.
std::vector<std::string> requiredToolchainBinaryNames() {
    return {"zanna",
            "zia",
            "zbasic",
            "ilrun",
            "il-verify",
            "il-dis",
            "zia-server",
            "zbasic-server",
            "basic-ast-dump",
            "basic-lex-dump",
            "zannastudio"};
}

/// @brief Validate that the manifest contains a complete, shippable toolchain.
/// Checks arch/platform/version strings, required binaries and cmake configs, all
/// runtime archives from RuntimeComponentManifest, and any support libraries
/// (gfx/gui/aud) referenced by CMake metadata. Throws on the first violation.
/// @param manifest Candidate staged-toolchain manifest.
/// @throws std::runtime_error If metadata, file entries, required components,
///         provenance, architecture, or associations violate the contract.
void validateToolchainInstallManifest(const ToolchainInstallManifest &manifest) {
    validateToolchainPlatform(manifest.platform);
    if (manifest.arch == "universal") {
        if (manifest.platform != "macos") {
            throw std::runtime_error("universal toolchain architecture is only valid for macOS "
                                     "toolchain manifests");
        }
    } else {
        validateToolchainArchitecture(manifest.arch);
    }
    validateDebVersion(manifest.version, "toolchain package version");
    if (manifest.productVersion.empty())
        throw std::runtime_error("toolchain product version is missing from staged metadata");
    validateSingleLineField(manifest.productVersion, "toolchain product version");
    validateSingleLineField(manifest.snapshot, "toolchain snapshot identity");
    if (!manifest.sourceCommit.empty() &&
        (manifest.sourceCommit.size() < 7U || manifest.sourceCommit.size() > 64U ||
         !std::all_of(
             /// @brief Validate one source-commit byte as lowercase hexadecimal.
             /// @param ch Candidate byte.
             /// @return `true` for `0-9` or `a-f`.
             manifest.sourceCommit.begin(), manifest.sourceCommit.end(), [](unsigned char ch) {
                 return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
             }))) {
        throw std::runtime_error(
            "toolchain source commit must be 7-64 lowercase hexadecimal characters");
    }
    if (manifest.sourceState != "clean" && manifest.sourceState != "dirty" &&
        manifest.sourceState != "unknown") {
        throw std::runtime_error("toolchain source state must be clean, dirty, or unknown");
    }
    validatePackageFileAssociations(manifest.fileAssociations);
    validateManifestFileEntries(manifest);

    /// @brief Test whether the manifest contains one required binary base name.
    /// @param nameNoExt Logical binary name without a platform extension.
    /// @return `true` when a matching binary entry exists.
    auto hasBinary = [&](const char *nameNoExt) {
        /// @brief Match one manifest entry against the requested binary base name.
        /// @param entry Candidate manifest file.
        /// @return `true` when it is the requested binary.
        return std::any_of(
            manifest.files.begin(), manifest.files.end(), [&](const ToolchainFileEntry &entry) {
                if (entry.kind != ToolchainFileKind::Binary)
                    return false;
                const std::string base = toolchainBaseNameFromFilename(
                    pathText(zanna::filesystem::pathFromUtf8(entry.stagedRelativePath).filename()));
                return base == nameNoExt;
            });
    };

    for (const std::string &binary : requiredToolchainBinaryNames()) {
        if (!hasBinary(binary.c_str())) {
            if (binary == "zannastudio") {
                throw std::runtime_error("staged toolchain is missing required binary zannastudio "
                                         "(configure installer build trees with "
                                         "ZANNA_INSTALL_ZANNASTUDIO=ON)");
            }
            throw std::runtime_error("staged toolchain is missing required binary " + binary);
        }
    }
    if (!manifestHasRelativePath(manifest, "lib/cmake/Zanna/ZannaConfig.cmake"))
        throw std::runtime_error("staged toolchain is missing lib/cmake/Zanna/ZannaConfig.cmake");
    if (!manifestHasRelativePath(manifest, "lib/cmake/Zanna/ZannaTargets.cmake"))
        throw std::runtime_error("staged toolchain is missing lib/cmake/Zanna/ZannaTargets.cmake");

    for (std::string_view archive : runtime_manifest::kRuntimeComponentArchives) {
        if (!manifestHasBaseNameKind(manifest, ToolchainFileKind::RuntimeArchive, archive)) {
            throw std::runtime_error("staged toolchain is missing runtime archive " +
                                     std::string(archive));
        }
    }

    if (stagedCMakeMetadataMentions(manifest, "zannagfx") &&
        !manifestHasBaseNameKind(manifest, ToolchainFileKind::SupportLibrary, "zannagfx")) {
        throw std::runtime_error("staged toolchain is missing support library zannagfx");
    }
    if (stagedCMakeMetadataMentions(manifest, "zannagui") &&
        !manifestHasBaseNameKind(manifest, ToolchainFileKind::SupportLibrary, "zannagui")) {
        throw std::runtime_error("staged toolchain is missing support library zannagui");
    }
    if (stagedCMakeMetadataMentions(manifest, "zannaaud") &&
        !manifestHasBaseNameKind(manifest, ToolchainFileKind::SupportLibrary, "zannaaud")) {
        throw std::runtime_error("staged toolchain is missing support library zannaaud");
    }
}

/// @brief Map a file's staged relative path to its final install destination under policy.
/// Windows: "bin/zanna.exe" → "C:\Program Files\Zanna\bin\zanna.exe";
/// macOS: "bin/zanna" → "/usr/local/zanna/bin/zanna";
/// Linux: "bin/zanna" → "/usr/bin/zanna" (FHS merge); PortableArchive: unchanged.
/// @param file Manifest entry whose staged-relative path should be mapped.
/// @param policy Destination root and layout convention.
/// @return Sanitized platform destination path under the selected policy.
/// @throws std::runtime_error If the staged path is unsafe.
std::string mapInstallPath(const ToolchainFileEntry &file, InstallPathPolicy policy) {
    const std::string rel =
        sanitizePackageRelativePath(file.stagedRelativePath, "staged install path");
    switch (policy) {
        case InstallPathPolicy::WindowsProgramFilesRoot: {
            std::string path = "C:\\Program Files\\Zanna";
            if (!rel.empty()) {
                path.push_back('\\');
                for (char ch : rel)
                    path.push_back(ch == '/' ? '\\' : ch);
            }
            return path;
        }
        case InstallPathPolicy::MacOSUsrLocalZannaRoot:
            return rel.empty() ? "/usr/local/zanna" : "/usr/local/zanna/" + rel;
        case InstallPathPolicy::LinuxUsrRoot:
            if (file.kind == ToolchainFileKind::Doc && rel.rfind("share/doc/", 0) != 0) {
                const std::string name =
                    genericPathText(zanna::filesystem::pathFromUtf8(rel).filename());
                return name.empty() ? "/usr/share/doc/zanna" : "/usr/share/doc/zanna/" + name;
            }
            return rel.empty() ? "/usr" : "/usr/" + rel;
        case InstallPathPolicy::PortableArchive:
        default:
            return rel;
    }
}

} // namespace zanna::pkg
