//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/Environment.hpp
// Purpose: Snapshot process environment values using Zanna's UTF-8 text contract.
// Key invariants:
//   - Windows values come from the native UTF-16 environment, never the active code page.
//   - Returned values own their storage and cannot be invalidated by a later environment update.
// Ownership/Lifetime: Returned optional strings own all encoded bytes.
// Links: src/common/Filesystem.hpp, src/common/RunProcess.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file Environment.hpp
 * @brief Provides owned UTF-8 snapshots of process environment variables.
 *
 * The Windows implementation converts names to UTF-16 and reads values through
 * the wide-character environment API. Other hosts copy the bytes returned by
 * `std::getenv`. In every case, successful results own their storage.
 */

#pragma once

#include "PlatformCapabilities.hpp"

#include <climits>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if ZANNA_HOST_WINDOWS
#include <windows.h>
#endif

namespace zanna::environment {

/// @brief Return an owned UTF-8 snapshot of one process environment value.
/// @details An empty but defined value is returned as an engaged empty string.
///          Invalid names, malformed Windows UTF-16, native failures, and absent
///          variables return `std::nullopt`. On Windows, the value buffer is
///          retried up to eight times if concurrent environment changes make
///          the first reported capacity insufficient. On non-Windows hosts,
///          the native environment bytes are copied without transcoding.
/// @param name Variable name without an equals sign or embedded NUL. On
///             Windows, the supplied bytes must also be valid UTF-8 and fit the
///             native conversion API's signed length limit.
/// @return An owning string containing the value when the variable exists and
///         can be read, including an empty string for a defined empty value;
///         otherwise `std::nullopt`.
/// @note The returned snapshot is independent of later environment updates.
inline std::optional<std::string> getUtf8(std::string_view name) {
    if (name.empty() || name.find('\0') != std::string_view::npos ||
        name.find('=') != std::string_view::npos) {
        return std::nullopt;
    }
#if ZANNA_HOST_WINDOWS
    if (name.size() > static_cast<std::size_t>(INT_MAX))
        return std::nullopt;
    const int wideNameLength = MultiByteToWideChar(CP_UTF8,
                                                   MB_ERR_INVALID_CHARS,
                                                   name.data(),
                                                   static_cast<int>(name.size()),
                                                   nullptr,
                                                   0);
    if (wideNameLength <= 0)
        return std::nullopt;
    std::wstring wideName(static_cast<std::size_t>(wideNameLength), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            name.data(),
                            static_cast<int>(name.size()),
                            wideName.data(),
                            wideNameLength) != wideNameLength) {
        return std::nullopt;
    }

    SetLastError(ERROR_SUCCESS);
    DWORD capacity = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (capacity == 0) {
        return GetLastError() == ERROR_SUCCESS ? std::optional<std::string>{std::string{}}
                                               : std::nullopt;
    }
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (capacity > static_cast<DWORD>(INT_MAX))
            return std::nullopt;
        std::vector<wchar_t> wideValue(static_cast<std::size_t>(capacity), L'\0');
        SetLastError(ERROR_SUCCESS);
        const DWORD length =
            GetEnvironmentVariableW(wideName.c_str(), wideValue.data(), capacity);
        if (length == 0) {
            return GetLastError() == ERROR_SUCCESS ? std::optional<std::string>{std::string{}}
                                                   : std::nullopt;
        }
        if (length >= capacity) {
            capacity = length;
            continue;
        }
        if (length > static_cast<DWORD>(INT_MAX))
            return std::nullopt;
        const int utf8Length = WideCharToMultiByte(CP_UTF8,
                                                  WC_ERR_INVALID_CHARS,
                                                  wideValue.data(),
                                                  static_cast<int>(length),
                                                  nullptr,
                                                  0,
                                                  nullptr,
                                                  nullptr);
        if (utf8Length <= 0)
            return std::nullopt;
        std::string value(static_cast<std::size_t>(utf8Length), '\0');
        if (WideCharToMultiByte(CP_UTF8,
                                WC_ERR_INVALID_CHARS,
                                wideValue.data(),
                                static_cast<int>(length),
                                value.data(),
                                utf8Length,
                                nullptr,
                                nullptr) != utf8Length) {
            return std::nullopt;
        }
        return value;
    }
    return std::nullopt;
#else
    const std::string nativeName(name);
    const char *value = std::getenv(nativeName.c_str());
    return value ? std::optional<std::string>{std::string(value)} : std::nullopt;
#endif
}

} // namespace zanna::environment
