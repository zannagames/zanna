//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/Utf8CommandLine.hpp
// Purpose: Present native process arguments to command-line tools as strict UTF-8.
// Key invariants:
//   - Windows arguments are rebuilt from GetCommandLineW, never the active code page.
//   - Invalid UTF-16 fails tool startup instead of substituting or redirecting a path.
// Ownership/Lifetime: The adapter owns copied strings and argv pointers until destruction.
// Links: src/common/Environment.hpp, src/common/RunProcess.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file Utf8CommandLine.hpp
 * @brief Defines a strict UTF-8 command-line adapter for Zanna tools.
 *
 * On Windows, the adapter reparses the native UTF-16 command line and owns
 * converted UTF-8 strings. On other hosts it publishes the original narrow
 * arguments unchanged.
 */

#pragma once

#include "PlatformCapabilities.hpp"

#include <climits>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#if ZANNA_HOST_WINDOWS
#include <windows.h>
#endif

namespace zanna::tools {

/// @brief Replace CRT narrow argv with a strict UTF-8 snapshot on Windows.
/// @details Windows conversion uses the operating system's command-line parser
///          and rejects malformed UTF-16. Non-Windows instances simply retain
///          the caller's existing argc/argv pair. Any published pointers remain
///          valid only as long as this adapter and, on non-Windows hosts, the
///          original argument storage remain alive.
class Utf8CommandLine {
  public:
    /// @brief Capture or borrow the process argument vector for UTF-8 use.
    /// @param argc Native argument count supplied to the tool entry point.
    /// @param argv Native argument vector supplied to the tool entry point.
    /// @details Windows ignores the narrow values and rebuilds the vector from
    ///          `GetCommandLineW`; other hosts retain them verbatim.
    Utf8CommandLine(int argc, char **argv) : argc_(argc), argv_(argv) {
#if ZANNA_HOST_WINDOWS
        argc_ = 0;
        argv_ = nullptr;
        captureWindows();
#endif
    }

    /// Copy construction is disabled because argv pointers refer to instance storage.
    Utf8CommandLine(const Utf8CommandLine &) = delete;
    /// Copy assignment is disabled because argv pointers refer to instance storage.
    Utf8CommandLine &operator=(const Utf8CommandLine &) = delete;

    /// @brief Publish the captured argument vector, printing a startup error on failure.
    /// @param argc Destination argument count, unchanged on failure.
    /// @param argv Destination argument vector, unchanged on failure.
    /// @return True when @p argc and @p argv now reference a valid UTF-8 vector.
    /// @post On success, @p argv contains @p argc arguments followed by a null
    ///       sentinel. On failure, one diagnostic line has been written to stderr.
    /// @warning Successful output pointers are borrowed from this object on
    ///          Windows and must not outlive it.
    bool applyOrReport(int &argc, char **&argv) const noexcept {
        if (!error_.empty()) {
            std::fprintf(stderr, "error: %s\n", error_.c_str());
            return false;
        }
        argc = argc_;
        argv = argv_;
        return true;
    }

  private:
#if ZANNA_HOST_WINDOWS
    /// Signature of the dynamically resolved system CommandLineToArgvW routine.
    using CommandLineToArgvWFn = LPWSTR *(WINAPI *)(LPCWSTR, int *);

    /// @brief Acquire the system Shell32 module without unsafe search paths.
    /// @param owned Receives true only when this function loaded a new module
    ///              reference that the caller must release with FreeLibrary.
    /// @return Module handle on success, or `nullptr` if the system directory
    ///         cannot be resolved or the module cannot be loaded.
    static HMODULE loadShell32(bool &owned) {
        owned = false;
        if (HMODULE module = GetModuleHandleW(L"shell32.dll"))
            return module;

        wchar_t systemDirectory[32768] = {};
        const UINT length =
            GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
        if (length == 0 || length >= std::size(systemDirectory))
            return nullptr;
        std::wstring path(systemDirectory, length);
        if (!path.empty() && path.back() != L'\\')
            path.push_back(L'\\');
        path += L"shell32.dll";
        HMODULE module = LoadLibraryW(path.c_str());
        owned = module != nullptr;
        return module;
    }

    /// @brief Strictly encode one NUL-terminated UTF-16 argument as UTF-8.
    /// @param storage Destination vector receiving one new owning string.
    /// @param wide UTF-16 argument to encode.
    /// @return True and appends exactly one string on success; false for a null
    ///         pointer, excessive length, malformed UTF-16, or native failure.
    static bool appendUtf8(std::vector<std::string> &storage, const wchar_t *wide) {
        if (!wide)
            return false;
        const std::size_t length = std::wcslen(wide);
        if (length > static_cast<std::size_t>(INT_MAX))
            return false;
        if (length == 0) {
            storage.emplace_back();
            return true;
        }
        const int required = WideCharToMultiByte(CP_UTF8,
                                                 WC_ERR_INVALID_CHARS,
                                                 wide,
                                                 static_cast<int>(length),
                                                 nullptr,
                                                 0,
                                                 nullptr,
                                                 nullptr);
        if (required <= 0)
            return false;
        std::string encoded(static_cast<std::size_t>(required), '\0');
        if (WideCharToMultiByte(CP_UTF8,
                                WC_ERR_INVALID_CHARS,
                                wide,
                                static_cast<int>(length),
                                encoded.data(),
                                required,
                                nullptr,
                                nullptr) != required) {
            return false;
        }
        storage.push_back(std::move(encoded));
        return true;
    }

    /// @brief Rebuild this instance from the native Windows command line.
    /// @details Dynamically resolves CommandLineToArgvW from the system
    ///          Shell32 module, converts every parsed argument strictly, and
    ///          creates a null-terminated pointer vector. Every native resource
    ///          acquired during capture is released before return.
    /// @post On success, argc_/argv_ describe storage_/pointers_. On failure,
    ///       error_ contains a user-facing startup diagnostic.
    void captureWindows() {
        bool shellOwned = false;
        HMODULE shell32 = loadShell32(shellOwned);
        if (!shell32) {
            error_ = "cannot load the system command-line parser";
            return;
        }
        auto parse =
            reinterpret_cast<CommandLineToArgvWFn>(GetProcAddress(shell32, "CommandLineToArgvW"));
        if (!parse) {
            if (shellOwned)
                FreeLibrary(shell32);
            error_ = "cannot resolve the system command-line parser";
            return;
        }
        int count = 0;
        LPWSTR *wideArgs = parse(GetCommandLineW(), &count);
        if (!wideArgs || count <= 0) {
            if (wideArgs)
                LocalFree(wideArgs);
            if (shellOwned)
                FreeLibrary(shell32);
            error_ = "cannot parse the native Windows command line";
            return;
        }

        storage_.reserve(static_cast<std::size_t>(count));
        bool converted = true;
        for (int index = 0; index < count; ++index) {
            if (!appendUtf8(storage_, wideArgs[index])) {
                converted = false;
                break;
            }
        }
        LocalFree(wideArgs);
        if (shellOwned)
            FreeLibrary(shell32);
        if (!converted) {
            storage_.clear();
            error_ = "the native Windows command line contains invalid Unicode";
            return;
        }

        pointers_.reserve(storage_.size() + 1U);
        for (std::string &argument : storage_)
            pointers_.push_back(argument.data());
        pointers_.push_back(nullptr);
        argc_ = count;
        argv_ = pointers_.data();
    }
#endif

    ///< Captured argument count, or zero after failed Windows capture.
    int argc_{0};
    ///< Published argument pointers, borrowed or backed by pointers_.
    char **argv_{nullptr};
    ///< Startup diagnostic; empty when capture succeeded.
    std::string error_;
#if ZANNA_HOST_WINDOWS
    ///< Owning UTF-8 argument strings in native order.
    std::vector<std::string> storage_;
    ///< Null-terminated pointers into storage_.
    std::vector<char *> pointers_;
#endif
};

} // namespace zanna::tools
