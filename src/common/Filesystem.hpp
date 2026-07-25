//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/Filesystem.hpp
// Purpose: Convert between native filesystem paths and Zanna's UTF-8 path strings.
// Key invariants:
//   - Narrow tool/runtime path strings are UTF-8, never the Windows active code page.
//   - Native path conversion preserves every encoded code point without lossy fallback.
// Ownership/Lifetime: Returned strings and paths own their storage.
// Links: src/tools/common/native_compiler.cpp, src/codegen/common/LinkerSupport.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file Filesystem.hpp
 * @brief Defines lossless conversions between UTF-8 strings and native paths.
 *
 * These helpers centralize Zanna's narrow-path contract. Native paths are
 * decoded or encoded through the C++20 `char8_t` filesystem interfaces so
 * Windows does not interpret UTF-8 bytes using its active code page.
 */

#pragma once

#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

namespace zanna::filesystem {

/// @brief Decode a Zanna UTF-8 path into the host filesystem's native representation.
/// @details Copies the input bytes into a `std::u8string` before constructing
///          the native path, preserving embedded NUL bytes at this conversion
///          layer rather than treating the input as a C string.
/// @param path UTF-8 path bytes to decode.
/// @return Owning native filesystem path representing the supplied bytes.
inline std::filesystem::path pathFromUtf8(std::string_view path) {
    std::u8string encoded(path.size(), u8'\0');
    if (!path.empty())
        std::memcpy(encoded.data(), path.data(), path.size());
    return std::filesystem::path(encoded);
}

/// @brief Encode a native filesystem path using Zanna's UTF-8 path convention.
/// @details Preserves the path's preferred native separator spelling.
/// @param path Native filesystem path to encode.
/// @return Owning UTF-8 byte string produced by `path.u8string()`.
inline std::string pathToUtf8(const std::filesystem::path &path) {
    const std::u8string encoded = path.u8string();
    return std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size());
}

/// @brief Encode a native path as UTF-8 with portable forward-slash separators.
/// @param path Native filesystem path to encode in generic format.
/// @return Owning UTF-8 byte string produced by
///         `path.generic_u8string()`, using forward slashes as separators.
inline std::string genericPathToUtf8(const std::filesystem::path &path) {
    const std::u8string encoded = path.generic_u8string();
    return std::string(reinterpret_cast<const char *>(encoded.data()), encoded.size());
}

} // namespace zanna::filesystem
