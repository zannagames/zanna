//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/MachOBuildVersion.hpp
// Purpose: Shared Mach-O LC_BUILD_VERSION defaults and environment overrides
//          for object and executable writers.
// Key invariants:
//   - Version words use Apple's packed nibbles: major << 16 | minor << 8 | patch.
//   - Missing or malformed environment overrides fall back to conservative defaults.
//   - No dynamic state is cached, keeping tests free to set per-process variables.
// Cross-platform touchpoints:
//   - Uses only the C library environment API, so non-Darwin hosts can still
//     cross-emit Mach-O metadata without platform-specific code.
// Ownership/Lifetime:
//   - Stateless inline helpers; returned version words are plain values.
// Links: codegen/common/objfile/MachOWriter.cpp,
//        codegen/common/linker/MachOExeWriter.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string_view>

/// @file
/// @brief Provides packed Mach-O build-version parsing and environment overrides.

namespace zanna::codegen::macho {

inline constexpr uint32_t kPlatformMacOS = 1; ///< LC_BUILD_VERSION platform value.
inline constexpr uint32_t kDefaultMacOSMinVersion = 0x000E0000; ///< macOS 14.0.0.
inline constexpr uint32_t kDefaultMacOSSDKVersion = 0x000F0000; ///< macOS SDK 15.0.0.

/// @brief Parse one unsigned decimal component from a dotted Mach-O version string.
/// @details Consumes decimal digits from @p pos and rejects empty components,
///          values outside the requested limit, and non-decimal characters.
///          The caller owns dot handling so all diagnostics collapse to a
///          simple std::nullopt return for environment fallback.
/// @param text Source string, for example "14.4.1".
/// @param pos Current parse position, advanced past the component on success.
/// @param limit Inclusive maximum allowed for this component.
/// @return Parsed component, or std::nullopt if the component is malformed.
[[nodiscard]] inline std::optional<uint32_t> parseVersionComponent(std::string_view text,
                                                                   std::size_t &pos,
                                                                   uint32_t limit) noexcept {
    if (pos >= text.size() || text[pos] < '0' || text[pos] > '9')
        return std::nullopt;

    uint32_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        const uint32_t digit = static_cast<uint32_t>(text[pos] - '0');
        if (value > (limit - digit) / 10U)
            return std::nullopt;
        value = value * 10U + digit;
        ++pos;
    }
    if (value > limit)
        return std::nullopt;
    return value;
}

/// @brief Parse a Mach-O packed version from "major.minor.patch" text.
/// @details The minor and patch components are optional and default to zero,
///          so "14", "14.2", and "14.2.1" are all valid. Major is limited
///          to 16 bits; minor and patch are limited to 8 bits, matching the
///          LC_BUILD_VERSION field layout.
/// @param text Environment-provided version string.
/// @return Packed version word, or std::nullopt for malformed input.
[[nodiscard]] inline std::optional<uint32_t> parsePackedVersion(std::string_view text) noexcept {
    std::size_t pos = 0;
    const auto major = parseVersionComponent(text, pos, 0xFFFFU);
    if (!major)
        return std::nullopt;

    uint32_t minor = 0;
    uint32_t patch = 0;
    if (pos < text.size()) {
        if (text[pos++] != '.')
            return std::nullopt;
        const auto parsedMinor = parseVersionComponent(text, pos, 0xFFU);
        if (!parsedMinor)
            return std::nullopt;
        minor = *parsedMinor;
    }
    if (pos < text.size()) {
        if (text[pos++] != '.')
            return std::nullopt;
        const auto parsedPatch = parseVersionComponent(text, pos, 0xFFU);
        if (!parsedPatch)
            return std::nullopt;
        patch = *parsedPatch;
    }
    if (pos != text.size())
        return std::nullopt;

    return (*major << 16U) | (minor << 8U) | patch;
}

/// @brief Read a packed Mach-O version override from the process environment.
/// @details Missing variables and malformed values are intentionally non-fatal:
///          object writers do not have a warning channel at every call site, and
///          reproducible fallback metadata is preferable to rejecting unrelated
///          compilation work because of a bad developer shell variable.
/// @param name Environment variable name.
/// @param fallback Packed version returned when no valid override is present.
/// @return Either the parsed override or @p fallback.
[[nodiscard]] inline uint32_t versionFromEnvironment(const char *name, uint32_t fallback) noexcept {
    const char *raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0')
        return fallback;
    const auto parsed = parsePackedVersion(raw);
    return parsed.value_or(fallback);
}

/// @brief Return the LC_BUILD_VERSION minimum macOS version.
/// @details Defaults to macOS 14.0.0 and can be overridden with
///          ZANNA_MACHO_MINOS=major.minor.patch for tests or SDK targeting.
/// @return Packed `major << 16 | minor << 8 | patch` version word.
[[nodiscard]] inline uint32_t minimumMacOSVersion() noexcept {
    return versionFromEnvironment("ZANNA_MACHO_MINOS", kDefaultMacOSMinVersion);
}

/// @brief Return the LC_BUILD_VERSION SDK version.
/// @details Defaults to macOS SDK 15.0.0 and can be overridden with
///          ZANNA_MACHO_SDK=major.minor.patch for tests or SDK targeting.
/// @return Packed `major << 16 | minor << 8 | patch` version word.
[[nodiscard]] inline uint32_t macOSSDKVersion() noexcept {
    return versionFromEnvironment("ZANNA_MACHO_SDK", kDefaultMacOSSDKVersion);
}

} // namespace zanna::codegen::macho
