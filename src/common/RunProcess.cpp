//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/RunProcess.cpp
// Purpose: Native subprocess launcher shared by Zanna tools and linker helpers.
// Key invariants:
//   - `run_process` launches argv directly without shell interpretation.
//   - stdout and stderr are captured independently.
//   - cwd and environment overrides apply only to the child process.
// Ownership/Lifetime:
//   - Returned output buffers are owned by the caller via RunResult.
//   - Helper-local OS handles/pipes are closed before return.
// Links: src/common/RunProcess.hpp, src/common/Filesystem.hpp
// Cross-platform touchpoints: POSIX spawn + pipe polling, Windows
//                             CreateProcess handle inheritance, cwd/env
//                             behavior across host toolchains.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements shell-free and explicitly shell-mediated subprocess launch.
/// @details The platform adapters validate UTF-8 arguments, construct scoped
///          child environments and working directories, capture stdout/stderr
///          independently, normalize termination status, and release every
///          native pipe, descriptor, thread, and process handle before return.

#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include "common/RunProcess.hpp"

#include "common/Filesystem.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#else
#include <direct.h>
#include <process.h>
#include <wchar.h>
#include <windows.h>
#endif

namespace {

/// @brief Test whether native C process APIs would truncate a byte string.
/// @param text Length-aware command, path, or environment bytes.
/// @return True when @p text contains a NUL before its logical end.
bool contains_embedded_nul(std::string_view text) noexcept {
    return text.find('\0') != std::string_view::npos;
}

/// @brief Test whether Win32's UTF-8 conversion API can represent a byte count.
/// @param length Number of bytes in the length-aware UTF-8 input.
/// @return True when @p length can be passed through the API's signed-int field.
bool windows_utf8_api_length_fits(std::size_t length) noexcept {
    return length <= static_cast<std::size_t>(INT_MAX);
}

/// Maximum CreateProcessW command-line storage, including its terminating NUL.
constexpr std::size_t kWindowsCommandLineMaximumCodeUnits = 32767;
/// Maximum command-line content before CreateProcessW's required terminator.
constexpr std::size_t kWindowsCommandLineMaximumContent = kWindowsCommandLineMaximumCodeUnits - 1;

/// Test-only byte threshold used to exercise post-launch capture failure cleanup.
std::atomic<std::size_t> g_capture_failure_after_bytes{std::numeric_limits<std::size_t>::max()};

/// @brief No-throw sink that retains captured bytes until storage fails.
/// @details Once an append cannot be represented or allocate, later bytes are
///          deliberately discarded while the native pipe continues to drain,
///          preventing a blocked child from deadlocking cleanup.
class CaptureSink {
  public:
    /// @brief Bind a destination and snapshot the current test fault threshold.
    /// @param destination Caller-owned output string.
    explicit CaptureSink(std::string &destination) noexcept
        : destination_(destination),
          failure_after_(g_capture_failure_after_bytes.load(std::memory_order_relaxed)) {}

    /// @brief Append bytes without allowing an exception to cross a native I/O boundary.
    /// @param data Read buffer containing @p length bytes.
    /// @param length Number of bytes to retain.
    void append(const char *data, std::size_t length) noexcept {
        if (storage_failed_ || length == 0)
            return;
        if (retained_ > failure_after_ || length > failure_after_ - retained_) {
            storage_failed_ = true;
            return;
        }
        try {
            destination_.append(data, length);
            retained_ += length;
        } catch (...) {
            storage_failed_ = true;
        }
    }

    /// @brief Report whether any bytes had to be discarded for storage reasons.
    /// @return True after a threshold, allocation, or string-length failure.
    [[nodiscard]] bool storage_failed() const noexcept {
        return storage_failed_;
    }

  private:
    std::string &destination_;   ///< Caller-owned capture buffer.
    std::size_t failure_after_;  ///< Maximum bytes retained by this sink.
    std::size_t retained_{0};    ///< Bytes successfully appended so far.
    bool storage_failed_{false}; ///< Whether this sink has entered discard mode.
};

/// @brief Append one diagnostic without propagating allocation failure.
/// @param destination Existing stderr/diagnostic buffer.
/// @param prefix Fixed diagnostic prefix.
/// @param detail Optional native-error detail.
/// @return True when the complete diagnostic was appended.
bool append_diagnostic_noexcept(std::string &destination,
                                std::string_view prefix,
                                std::string_view detail = {}) noexcept {
    try {
        if (!destination.empty())
            destination.push_back('\n');
        destination.append(prefix.data(), prefix.size());
        destination.append(detail.data(), detail.size());
        return true;
    } catch (...) {
        return false;
    }
}

/// @brief Construct a minimal allocation/length failure result without throwing.
RunResult make_process_storage_failure() noexcept {
    RunResult result;
    result.exit_code = -1;
    result.launch_failed = true;
    append_diagnostic_noexcept(result.err, "process launch/capture storage exhausted");
    return result;
}

/// @brief Determine whether a Windows argument needs command-line quoting.
/// @param arg Argument after UTF-16 conversion.
/// @return True when @p arg contains whitespace or shell-sensitive punctuation.
bool windows_argument_needs_quoting(std::wstring_view arg) noexcept {
    if (arg.empty()) {
        return true;
    }

    for (const wchar_t ch : arg) {
        if (ch == L' ' || ch == L'\t' || ch == L'"' || ch == L'&' || ch == L'|' || ch == L'<' ||
            ch == L'>' || ch == L'^' || ch == L'(' || ch == L')') {
            return true;
        }
    }
    return false;
}

/// @brief Add a command-line fragment without exceeding CreateProcessW's content budget.
/// @param length In/out accumulated content length.
/// @param amount Additional UTF-16 code units to account for.
/// @return True and updates @p length when the fragment fits.
bool windows_command_line_add_length(std::size_t &length, std::size_t amount) noexcept {
    if (length > kWindowsCommandLineMaximumContent ||
        amount > kWindowsCommandLineMaximumContent - length) {
        return false;
    }
    length += amount;
    return true;
}

/// @brief Compute one argument's quoted length under the Microsoft C runtime rules.
/// @details The computation is budgeted as it walks the input, so a long run of
///          backslashes cannot overflow before the caller rejects the command line.
/// @param arg Raw UTF-16 argument.
/// @return Quoted code-unit count, or no value when it alone exceeds the native limit.
std::optional<std::size_t> windows_quoted_argument_length(std::wstring_view arg) noexcept {
    if (!windows_argument_needs_quoting(arg)) {
        if (arg.size() > kWindowsCommandLineMaximumContent) {
            return std::nullopt;
        }
        return arg.size();
    }

    std::size_t length = 1; // Opening quote.
    std::size_t backslash_count = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            if (backslash_count == kWindowsCommandLineMaximumContent) {
                return std::nullopt;
            }
            ++backslash_count;
            continue;
        }

        if (ch == L'"') {
            // Existing backslashes double, one backslash escapes the quote,
            // and the quote itself is emitted.
            if (backslash_count > (kWindowsCommandLineMaximumContent - 2) / 2 ||
                !windows_command_line_add_length(length, backslash_count * 2 + 2)) {
                return std::nullopt;
            }
        } else if (!windows_command_line_add_length(length, backslash_count + 1)) {
            return std::nullopt;
        }
        backslash_count = 0;
    }

    // Backslashes immediately before the closing quote must also double.
    if (backslash_count > (kWindowsCommandLineMaximumContent - 1) / 2 ||
        !windows_command_line_add_length(length, backslash_count * 2 + 1)) {
        return std::nullopt;
    }
    return length;
}

/// @brief Test whether one already-quoted argument fits an aggregate command line.
/// @param current Current content length.
/// @param argument_length Quoted argument length.
/// @param needs_separator Whether a separating space precedes this argument.
/// @return True when the aggregate plus its implicit terminator stays within the native limit.
bool windows_command_line_append_fits(std::size_t current,
                                      std::size_t argument_length,
                                      bool needs_separator) noexcept {
    if (needs_separator && !windows_command_line_add_length(current, 1)) {
        return false;
    }
    return windows_command_line_add_length(current, argument_length);
}

/// @brief Apply a cheap upper bound before converting a UTF-8 process argument.
/// @details A valid UTF-8 sequence consumes at most three source bytes per
///          resulting UTF-16 code unit (supplementary scalars use four bytes
///          and two code units). Inputs above this bound cannot fit regardless
///          of their contents.
/// @param byte_length Length of one UTF-8 argument.
/// @return True when conversion could still fit one native command line.
bool windows_utf8_argument_may_fit_command_line(std::size_t byte_length) noexcept {
    constexpr std::size_t maximum_possible_utf8_bytes = kWindowsCommandLineMaximumContent * 3;
    return byte_length <= maximum_possible_utf8_bytes;
}

#if defined(_WIN32)
/// @brief Format a Win32 error code as a compact UTF-8 diagnostic string.
/// @param error Value returned by @c GetLastError.
/// @return Human-readable message with trailing newlines and full stop removed.
std::string format_windows_error(DWORD error);

/// @brief Convert UTF-8 command text to UTF-16 for Win32 Unicode APIs.
/// @param text UTF-8 encoded input string.
/// @param err Receives a diagnostic when conversion fails.
/// @return UTF-16 string on success, or @c std::nullopt on invalid input.
std::optional<std::wstring> utf8_to_wide(std::string_view text, std::string &err) {
    if (!windows_utf8_api_length_fits(text.size())) {
        err = "UTF-8 text exceeds the Windows conversion length limit";
        return std::nullopt;
    }
    if (text.empty()) {
        return std::wstring{};
    }
    const int input_length = static_cast<int>(text.size());
    const int needed =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_length, nullptr, 0);
    if (needed <= 0) {
        err = "failed to convert UTF-8 text for Windows process launch: " +
              format_windows_error(GetLastError());
        return std::nullopt;
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), input_length, out.data(), needed);
    if (written != needed) {
        err = "failed to convert UTF-8 text for Windows process launch: " +
              format_windows_error(GetLastError());
        return std::nullopt;
    }
    return out;
}

/// @brief Test whether a Windows environment variable name is syntactically valid.
/// @details Empty names are ignored by the launcher for compatibility with the
///          previous POSIX behavior.  Names containing '=' cannot be represented
///          in a Windows or POSIX environment block.
/// @param name Environment variable name to validate.
/// @param err Receives a diagnostic when the name is invalid.
/// @return True when @p name can be used in an environment block.
bool validate_environment_name(std::string_view name, std::string &err) {
    if (name.empty()) {
        return true;
    }
    if (name.find('=') != std::string_view::npos) {
        err = "invalid environment variable name '" + std::string{name} + "'";
        return false;
    }
    return true;
}

/// @brief Quote one UTF-16 command-line argument using the Win32 argv rules.
/// @details Implements the backslash-before-quote algorithm used by the
///          Microsoft C runtime when it reconstructs argv from a command line.
/// @param arg Raw UTF-16 argument.
/// @return Argument text safe to concatenate into a Win32 command line.
std::wstring quote_windows_argument(const std::wstring &arg) {
    if (!windows_argument_needs_quoting(arg)) {
        return arg;
    }

    std::wstring quoted;
    quoted.reserve(arg.size() + 2);
    quoted.push_back(L'"');

    std::size_t backslash_count = 0;
    for (const wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslash_count;
            continue;
        }

        if (ch == L'"') {
            quoted.append(backslash_count * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslash_count = 0;
            continue;
        }

        if (backslash_count != 0) {
            quoted.append(backslash_count, L'\\');
            backslash_count = 0;
        }

        quoted.push_back(ch);
    }

    if (backslash_count != 0) {
        quoted.append(backslash_count * 2, L'\\');
    }

    quoted.push_back(L'"');
    return quoted;
}

/// @brief Format a Win32 error code as a compact UTF-8 diagnostic string.
/// @param error Value returned by @c GetLastError.
/// @return Human-readable message with trailing newlines and full stop removed.
std::string format_windows_error(DWORD error) {
    LPSTR buffer = nullptr;
    const DWORD flags =
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD size = FormatMessageA(flags,
                                      nullptr,
                                      error,
                                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPSTR>(&buffer),
                                      0,
                                      nullptr);
    if (size == 0 || buffer == nullptr) {
        return "Windows error " + std::to_string(static_cast<unsigned long>(error));
    }

    struct LocalBufferGuard {
        LPSTR buffer;

        ~LocalBufferGuard() {
            if (buffer != nullptr)
                LocalFree(buffer);
        }
    } buffer_guard{buffer};

    std::string message(buffer, buffer + size);
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
}

/// @brief Format and append a Win32 diagnostic without propagating allocation failure.
bool append_windows_diagnostic_noexcept(std::string &destination,
                                        std::string_view prefix,
                                        DWORD error) noexcept {
    try {
        const std::string detail = format_windows_error(error);
        return append_diagnostic_noexcept(destination, prefix, detail);
    } catch (...) {
        return false;
    }
}

/// @brief Join argv into the mutable command-line string required by CreateProcessW.
/// @param argv UTF-8 argument vector including the executable at index zero.
/// @param err Receives a diagnostic when UTF-8 conversion fails.
/// @return UTF-16 command line on success, or @c std::nullopt on failure.
std::optional<std::wstring> join_windows_command_line(const std::vector<std::string> &argv,
                                                      std::string &err) {
    std::wstring cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (!windows_utf8_argument_may_fit_command_line(argv[i].size())) {
            err = "process argument " + std::to_string(i) +
                  " exceeds the Windows command-line length limit";
            return std::nullopt;
        }
        auto wide = utf8_to_wide(argv[i], err);
        if (!wide) {
            return std::nullopt;
        }
        const auto quoted_length = windows_quoted_argument_length(*wide);
        if (!quoted_length ||
            !windows_command_line_append_fits(cmdline.size(), *quoted_length, i != 0)) {
            err = "Windows command line exceeds the native length limit at argument " +
                  std::to_string(i);
            return std::nullopt;
        }
        if (i != 0) {
            cmdline.push_back(L' ');
        }
        cmdline += quote_windows_argument(*wide);
    }
    return cmdline;
}

/// @brief Strict weak ordering for case-insensitive Windows environment names.
struct CaseInsensitiveWideLess {
    /// @brief Compare two UTF-16 strings with Windows-style case folding.
    /// @param lhs Left string.
    /// @param rhs Right string.
    /// @return True when @p lhs sorts before @p rhs case-insensitively.
    bool operator()(const std::wstring &lhs, const std::wstring &rhs) const noexcept {
        const std::size_t common = std::min(lhs.size(), rhs.size());
        for (std::size_t i = 0; i < common; ++i) {
            const wchar_t lch = static_cast<wchar_t>(std::towlower(lhs[i]));
            const wchar_t rch = static_cast<wchar_t>(std::towlower(rhs[i]));
            if (lch < rch) {
                return true;
            }
            if (lch > rch) {
                return false;
            }
        }
        return lhs.size() < rhs.size();
    }
};

/// @brief Build a CreateProcessW environment block from scoped overrides.
/// @details The block starts from the current process environment, applies
///          case-insensitive overrides without mutating the parent, sorts the
///          resulting entries as expected by Win32, and stores the required
///          double-NUL terminator.  Empty override names are ignored.
/// @param env_overrides UTF-8 key/value override list.
/// @param block Receives the double-NUL-terminated UTF-16 environment block.
/// @param err Receives a diagnostic when conversion or environment capture fails.
/// @return True when the block was built successfully.
bool build_windows_environment_block(
    const std::vector<std::pair<std::string, std::string>> &env_overrides,
    std::vector<wchar_t> &block,
    std::string &err) {
    using Override = std::pair<std::wstring, std::wstring>;
    std::map<std::wstring, Override, CaseInsensitiveWideLess> overrides;
    for (const auto &entry : env_overrides) {
        if (!validate_environment_name(entry.first, err)) {
            return false;
        }
        if (entry.first.empty()) {
            continue;
        }
        auto name = utf8_to_wide(entry.first, err);
        auto value = utf8_to_wide(entry.second, err);
        if (!name || !value) {
            return false;
        }
        overrides[*name] = Override{*name, *value};
    }

    if (overrides.empty()) {
        block.clear();
        return true;
    }

    LPWCH raw_environment = GetEnvironmentStringsW();
    if (raw_environment == nullptr) {
        err = "failed to read Windows environment: " + format_windows_error(GetLastError());
        return false;
    }

    struct EnvironmentBlockGuard {
        LPWCH block;

        ~EnvironmentBlockGuard() {
            if (block != nullptr)
                FreeEnvironmentStringsW(block);
        }
    } environment_guard{raw_environment};

    std::vector<std::wstring> entries;
    std::map<std::wstring, bool, CaseInsensitiveWideLess> emitted_names;
    for (const wchar_t *cursor = raw_environment; *cursor != L'\0';
         cursor += std::wcslen(cursor) + 1) {
        std::wstring current(cursor);
        if (!current.empty() && current.front() == L'=') {
            entries.push_back(std::move(current));
            continue;
        }

        const std::size_t equals = current.find(L'=');
        if (equals == std::wstring::npos) {
            continue;
        }

        const std::wstring name = current.substr(0, equals);
        if (emitted_names.find(name) != emitted_names.end()) {
            continue;
        }

        if (auto override_it = overrides.find(name); override_it != overrides.end()) {
            entries.push_back(override_it->second.first + L"=" + override_it->second.second);
            emitted_names.emplace(override_it->second.first, true);
            overrides.erase(override_it);
        } else {
            emitted_names.emplace(name, true);
            entries.push_back(std::move(current));
        }
    }
    for (const auto &entry : overrides) {
        entries.push_back(entry.second.first + L"=" + entry.second.second);
    }

    std::sort(entries.begin(), entries.end(), CaseInsensitiveWideLess{});

    block.clear();
    for (const auto &entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return true;
}

/// @brief Close a Win32 handle if it currently owns a valid handle value.
/// @param handle Handle slot to close and reset to @c INVALID_HANDLE_VALUE.
void close_handle_if_valid(HANDLE &handle) noexcept {
    if (handle != INVALID_HANDLE_VALUE && handle != nullptr) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
}

void delete_handle_inheritance_list(STARTUPINFOEXW &startup) noexcept;

/// @brief Move-only owner for one closeable Win32 handle.
class UniqueWindowsHandle {
  public:
    UniqueWindowsHandle() noexcept = default;

    explicit UniqueWindowsHandle(HANDLE handle) noexcept : handle_(handle) {}

    ~UniqueWindowsHandle() {
        reset();
    }

    UniqueWindowsHandle(const UniqueWindowsHandle &) = delete;
    UniqueWindowsHandle &operator=(const UniqueWindowsHandle &) = delete;

    UniqueWindowsHandle(UniqueWindowsHandle &&other) noexcept : handle_(other.release()) {}

    UniqueWindowsHandle &operator=(UniqueWindowsHandle &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    /// @brief Return the borrowed native handle.
    [[nodiscard]] HANDLE get() const noexcept {
        return handle_;
    }

    /// @brief Return whether a closeable handle is owned.
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
    }

    /// @brief Close the current handle and optionally adopt a replacement.
    void reset(HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
        close_handle_if_valid(handle_);
        handle_ = replacement;
    }

    /// @brief Relinquish ownership without closing.
    HANDLE release() noexcept {
        const HANDLE handle = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return handle;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE}; ///< Owned handle, or invalid sentinel.
};

/// @brief Joins and closes one `_beginthreadex` capture thread.
class CaptureThreadGuard {
  public:
    CaptureThreadGuard() noexcept = default;

    ~CaptureThreadGuard() {
        join();
    }

    CaptureThreadGuard(const CaptureThreadGuard &) = delete;
    CaptureThreadGuard &operator=(const CaptureThreadGuard &) = delete;

    /// @brief Adopt a native capture-thread handle.
    void reset(HANDLE handle) noexcept {
        join();
        handle_.reset(handle);
    }

    /// @brief Wait for completion and close the thread handle.
    void join() noexcept {
        if (!handle_.valid())
            return;
        (void)WaitForSingleObject(handle_.get(), INFINITE);
        handle_.reset();
    }

    /// @brief Return whether a thread was created.
    [[nodiscard]] bool valid() const noexcept {
        return handle_.valid();
    }

  private:
    UniqueWindowsHandle handle_; ///< Owned native thread handle.
};

/// @brief Owns a created Win32 process and guarantees terminate/wait/close on unwind.
class WindowsChildGuard {
  public:
    WindowsChildGuard() noexcept = default;

    ~WindowsChildGuard() {
        terminate_and_wait();
        close_handles();
    }

    WindowsChildGuard(const WindowsChildGuard &) = delete;
    WindowsChildGuard &operator=(const WindowsChildGuard &) = delete;

    /// @brief Prepare storage for `CreateProcessW`.
    PROCESS_INFORMATION *put() noexcept {
        process_ = {};
        launched_ = false;
        completed_ = false;
        return &process_;
    }

    /// @brief Record whether `CreateProcessW` populated the process handles.
    void set_launched(bool launched) noexcept {
        launched_ = launched;
    }

    /// @brief Borrow the process handle.
    [[nodiscard]] HANDLE process_handle() const noexcept {
        return process_.hProcess;
    }

    /// @brief Wait for child completion.
    /// @return Native wait result.
    DWORD wait() noexcept {
        if (!launched_ || process_.hProcess == nullptr)
            return WAIT_FAILED;
        const DWORD result = WaitForSingleObject(process_.hProcess, INFINITE);
        if (result == WAIT_OBJECT_0)
            completed_ = true;
        return result;
    }

    /// @brief Mark a child known to have reached a terminal wait state.
    void mark_completed() noexcept {
        completed_ = true;
    }

    /// @brief Terminate and reap an outstanding child without throwing.
    void terminate_and_wait() noexcept {
        if (!launched_ || completed_ || process_.hProcess == nullptr)
            return;
        (void)TerminateProcess(process_.hProcess, 127);
        (void)WaitForSingleObject(process_.hProcess, INFINITE);
        completed_ = true;
    }

  private:
    /// @brief Close both process-information handles.
    void close_handles() noexcept {
        close_handle_if_valid(process_.hThread);
        close_handle_if_valid(process_.hProcess);
        launched_ = false;
    }

    PROCESS_INFORMATION process_{}; ///< Handles populated by CreateProcessW.
    bool launched_{false};          ///< Whether the handles name a child.
    bool completed_{false};         ///< Whether a terminal wait completed.
};

/// @brief Deletes one initialized STARTUPINFOEX attribute list on scope exit.
class StartupAttributeListGuard {
  public:
    explicit StartupAttributeListGuard(STARTUPINFOEXW &startup) noexcept : startup_(startup) {}

    ~StartupAttributeListGuard() {
        delete_handle_inheritance_list(startup_);
    }

    StartupAttributeListGuard(const StartupAttributeListGuard &) = delete;
    StartupAttributeListGuard &operator=(const StartupAttributeListGuard &) = delete;

  private:
    STARTUPINFOEXW &startup_;
};

/// @brief Duplicate a handle so it may be inherited by a child process.
/// @param source Source handle in the current process.
/// @return Inheritable duplicate, or @c INVALID_HANDLE_VALUE when duplication fails.
HANDLE duplicate_inheritable_handle(HANDLE source) noexcept {
    if (source == nullptr || source == INVALID_HANDLE_VALUE) {
        return INVALID_HANDLE_VALUE;
    }
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(),
                         source,
                         GetCurrentProcess(),
                         &duplicate,
                         0,
                         TRUE,
                         DUPLICATE_SAME_ACCESS)) {
        return INVALID_HANDLE_VALUE;
    }
    return duplicate;
}

/// @brief Open an inheritable NUL device handle for children without parent stdin.
/// @return Inheritable read handle, or @c INVALID_HANDLE_VALUE on failure.
HANDLE open_inheritable_nul_input_handle() noexcept {
    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    return CreateFileW(L"NUL",
                       GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &security,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
}

/// @brief Initialise a STARTUPINFOEX handle-inheritance allow list.
/// @details CreateProcessW requires @c bInheritHandles to be true when standard
///          handles are redirected.  The attribute list restricts inheritance to
///          the exact pipe/stdin handles needed by the child.
/// @param startup Startup information receiving the attribute list pointer.
/// @param storage Backing storage that must outlive the CreateProcessW call.
/// @param handles Handles allowed to cross into the child process. Its backing
///        array must outlive the CreateProcessW call because the attribute list
///        retains this address instead of copying the values.
/// @param err Receives a diagnostic when attribute-list setup fails.
/// @return True when the handle list is ready for CreateProcessW.
bool initialize_handle_inheritance_list(STARTUPINFOEXW &startup,
                                        std::vector<char> &storage,
                                        std::vector<HANDLE> &handles,
                                        std::string &err) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    if (bytes == 0) {
        err = "failed to size process handle inheritance list: " +
              format_windows_error(GetLastError());
        return false;
    }

    storage.assign(bytes, 0);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) {
        err = "failed to initialise process handle inheritance list: " +
              format_windows_error(GetLastError());
        startup.lpAttributeList = nullptr;
        return false;
    }

    if (!UpdateProcThreadAttribute(startup.lpAttributeList,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles.data(),
                                   handles.size() * sizeof(HANDLE),
                                   nullptr,
                                   nullptr)) {
        err = "failed to update process handle inheritance list: " +
              format_windows_error(GetLastError());
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        startup.lpAttributeList = nullptr;
        return false;
    }
    return true;
}

/// @brief Delete a STARTUPINFOEX attribute list when one was initialised.
/// @param startup Startup information whose @c lpAttributeList should be freed.
void delete_handle_inheritance_list(STARTUPINFOEXW &startup) noexcept {
    if (startup.lpAttributeList != nullptr) {
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        startup.lpAttributeList = nullptr;
    }
}

/// @brief Convert a native Windows process exit code to the legacy int field.
/// @param exit_code Exact unsigned Win32 exit code.
/// @return @p exit_code when it fits in @c int, otherwise @c INT_MAX.
int normalize_windows_exit_code(DWORD exit_code) noexcept {
    if (exit_code > static_cast<DWORD>(INT_MAX)) {
        return INT_MAX;
    }
    return static_cast<int>(exit_code);
}

struct CaptureThreadContext {
    ///< Parent-side pipe handle consumed by the capture thread.
    HANDLE handle = INVALID_HANDLE_VALUE;
    ///< No-throw caller-owned sink receiving all data read before EOF.
    CaptureSink *sink = nullptr;
    ///< First non-broken-pipe ReadFile failure, or ERROR_SUCCESS.
    DWORD error = ERROR_SUCCESS;
};

/// @brief Read one redirected Win32 pipe until EOF on a background thread.
/// @param param Pointer to a @ref CaptureThreadContext owned by the caller.
/// @return Always zero; unexpected read errors are retained in the context.
/// @pre @p param, its context's buffer, and handle remain valid until this
///      thread returns.
unsigned __stdcall capture_handle_thread_proc(void *param) {
    auto *ctx = static_cast<CaptureThreadContext *>(param);
    char chunk[4096];
    for (;;) {
        DWORD read = 0;
        if (ReadFile(ctx->handle, chunk, sizeof(chunk), &read, nullptr)) {
            if (read == 0)
                return 0;
            ctx->sink->append(chunk, static_cast<std::size_t>(read));
            continue;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_BROKEN_PIPE)
            ctx->error = error;
        return 0;
    }
}
#endif

/// @brief Temporarily override one process environment variable.
/// @details Captures the previous value, applies the requested replacement,
///          and restores the original state at destruction. Move operations
///          transfer restoration responsibility so exactly one live guard owns
///          each saved state. Empty names are inert.
/// @warning This helper mutates process-global state and is not safe against
///          concurrent environment access.
class ScopedEnvironmentAssignment {
  public:
    /// @brief Capture a variable and apply a temporary replacement.
    /// @param name Environment variable name; an empty name makes the guard inert.
    /// @param value Replacement value retained for move reassignment.
    ScopedEnvironmentAssignment(std::string name, std::string value)
        : name_(std::move(name)), previous_(capture_existing()), override_(std::move(value)) {
        apply_override();
    }

    /// Copying is disabled because restoration ownership must remain unique.
    ScopedEnvironmentAssignment(const ScopedEnvironmentAssignment &) = delete;
    /// Copy assignment is disabled because restoration ownership must remain unique.
    ScopedEnvironmentAssignment &operator=(const ScopedEnvironmentAssignment &) = delete;

    /// @brief Transfer responsibility for restoring another guard's variable.
    /// @param other Guard relinquishing its captured name and values.
    /// @post @p other is inert and this guard owns its former restoration state.
    ScopedEnvironmentAssignment(ScopedEnvironmentAssignment &&other) noexcept
        : name_(std::move(other.name_)), previous_(std::move(other.previous_)),
          override_(std::move(other.override_)) {
        other.name_.clear();
        other.previous_.reset();
        other.override_.reset();
    }

    /// @brief Restore the current assignment, then take another guard's state.
    /// @param other Guard whose restoration responsibility is transferred.
    /// @return This guard.
    /// @post The prior variable owned by this guard is restored, @p other is
    ///       inert, and the transferred override is reapplied.
    ScopedEnvironmentAssignment &operator=(ScopedEnvironmentAssignment &&other) noexcept {
        if (this == &other) {
            return *this;
        }

        restore();
        name_ = std::move(other.name_);
        previous_ = std::move(other.previous_);
        override_ = std::move(other.override_);
        other.name_.clear();
        other.previous_.reset();
        other.override_.reset();
        apply_override();
        return *this;
    }

    /// @brief Restore the captured value or remove a previously absent variable.
    ~ScopedEnvironmentAssignment() {
        restore();
    }

  private:
    /// @brief Snapshot the current value of @c name_.
    /// @return Owning prior value, or `std::nullopt` when it is absent.
    std::optional<std::string> capture_existing() const {
        const char *existing = std::getenv(name_.c_str());
        if (existing == nullptr) {
            return std::nullopt;
        }
        return std::string(existing);
    }

    /// @brief Assign one value to @c name_ through the host C runtime.
    /// @param value Bytes to install as the variable's value.
    /// @note Native assignment errors are intentionally ignored.
    void apply(const std::string &value) const {
#ifdef _WIN32
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    /// @brief Apply the retained override when this guard is active.
    void apply_override() const {
        if (!override_.has_value() || name_.empty()) {
            return;
        }
        apply(*override_);
    }

    /// @brief Restore the original environment state owned by this guard.
    /// @details If the variable was absent, POSIX removes it while Windows
    ///          assigns an empty value through `_putenv_s`.
    /// @note Native restoration errors are intentionally ignored.
    void restore() const {
        if (name_.empty()) {
            return;
        }

        if (previous_.has_value()) {
#ifdef _WIN32
            _putenv_s(name_.c_str(), previous_->c_str());
#else
            setenv(name_.c_str(), previous_->c_str(), 1);
#endif
            return;
        }

#ifdef _WIN32
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }

    ///< Variable whose previous state this guard owns; empty means inert.
    std::string name_;
    ///< Value captured before the guard applied its replacement.
    std::optional<std::string> previous_;
    ///< Replacement value reapplied after move assignment.
    std::optional<std::string> override_;
};

/// @brief Validate an optional UTF-8 child working directory.
/// @param cwd Directory text, or `std::nullopt` to inherit the parent's directory.
/// @param err Receives a stable launch diagnostic when validation fails.
/// @return True when no directory was requested or the converted path exists
///         and is a directory.
bool validate_working_directory(const std::optional<std::string> &cwd, std::string &err) {
    if (!cwd.has_value()) {
        return true;
    }

    std::error_code ec;
    const std::filesystem::path path = zanna::filesystem::pathFromUtf8(*cwd);
    if (!std::filesystem::exists(path, ec) || ec) {
        err = "failed to change working directory to '" + *cwd + "'";
        return false;
    }
    if (!std::filesystem::is_directory(path, ec) || ec) {
        err = "failed to change working directory to '" + *cwd + "'";
        return false;
    }
    return true;
}

#ifndef _WIN32
/// @brief Test whether an environment variable name is representable.
/// @details Empty names are ignored for compatibility with the historical
///          implementation.  Names containing '=' are rejected because neither
///          POSIX nor Windows environment blocks can represent them as keys.
/// @param name Environment variable name to validate.
/// @param err Receives a diagnostic when the name is invalid.
/// @return True when @p name can be represented or intentionally ignored.
bool validate_environment_name(std::string_view name, std::string &err) {
    if (name.empty()) {
        return true;
    }
    if (name.find('=') != std::string_view::npos) {
        err = "invalid environment variable name '" + std::string{name} + "'";
        return false;
    }
    return true;
}

/// @brief Close a POSIX file descriptor when it is open.
/// @param fd Descriptor slot to close and reset to -1.
void close_fd_if_open(int &fd) noexcept {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

/// @brief Move-only owner for one POSIX file descriptor.
class UniqueFd {
  public:
    UniqueFd() noexcept = default;

    explicit UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}

    UniqueFd &operator=(UniqueFd &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    /// @brief Return the borrowed descriptor number.
    [[nodiscard]] int get() const noexcept {
        return fd_;
    }

    /// @brief Return whether this object owns an open descriptor.
    [[nodiscard]] bool valid() const noexcept {
        return fd_ >= 0;
    }

    /// @brief Close the current descriptor and optionally adopt a replacement.
    /// @param replacement New descriptor, or -1 to become empty.
    void reset(int replacement = -1) noexcept {
        close_fd_if_open(fd_);
        fd_ = replacement;
    }

    /// @brief Relinquish ownership without closing.
    /// @return Former descriptor number.
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

  private:
    int fd_{-1}; ///< Owned descriptor, or -1.
};

/// @brief RAII owner for a POSIX spawn file-action object.
class ScopedSpawnFileActions {
  public:
    ScopedSpawnFileActions() noexcept = default;

    ~ScopedSpawnFileActions() {
        if (initialised_)
            posix_spawn_file_actions_destroy(&actions_);
    }

    ScopedSpawnFileActions(const ScopedSpawnFileActions &) = delete;
    ScopedSpawnFileActions &operator=(const ScopedSpawnFileActions &) = delete;

    /// @brief Initialise the native action list exactly once.
    /// @return Zero on success or a POSIX error number.
    int initialize() noexcept {
        if (initialised_)
            return EALREADY;
        const int result = posix_spawn_file_actions_init(&actions_);
        initialised_ = result == 0;
        return result;
    }

    /// @brief Borrow the initialized native action list.
    posix_spawn_file_actions_t *get() noexcept {
        return &actions_;
    }

  private:
    posix_spawn_file_actions_t actions_{};
    bool initialised_{false};
};

/// @brief Ensures a successfully spawned POSIX child is never left running or unreaped.
class PosixChildGuard {
  public:
    explicit PosixChildGuard(pid_t pid) noexcept : pid_(pid) {}

    ~PosixChildGuard() {
        terminate_and_reap();
    }

    PosixChildGuard(const PosixChildGuard &) = delete;
    PosixChildGuard &operator=(const PosixChildGuard &) = delete;

    /// @brief Wait for normal child completion, retrying interruption.
    /// @param status Receives the native wait status on success.
    /// @return Zero on success or the first non-interruption errno.
    int wait(int &status) noexcept {
        if (pid_ <= 0)
            return ECHILD;
        for (;;) {
            const pid_t waited = waitpid(pid_, &status, 0);
            if (waited == pid_) {
                pid_ = -1;
                return 0;
            }
            if (waited < 0 && errno == EINTR)
                continue;
            const int error = waited < 0 ? errno : ECHILD;
            if (error == ECHILD)
                pid_ = -1;
            return error;
        }
    }

    /// @brief Kill and reap an outstanding child without throwing or allocating.
    void terminate_and_reap() noexcept {
        if (pid_ <= 0)
            return;
        (void)::kill(pid_, SIGKILL);
        int status = 0;
        for (;;) {
            const pid_t waited = waitpid(pid_, &status, 0);
            if (waited == pid_ || (waited < 0 && errno == ECHILD))
                break;
            if (waited < 0 && errno == EINTR)
                continue;
            break;
        }
        pid_ = -1;
    }

  private:
    pid_t pid_{-1}; ///< Outstanding child PID, or -1 once reaped.
};

/// @brief Mark one descriptor close-on-exec.
/// @param fd Descriptor to update.
/// @param err Receives a diagnostic when @c fcntl fails.
/// @return True when the descriptor will close across exec.
bool set_close_on_exec(int fd, std::string &err) {
    const int flags = ::fcntl(fd, F_GETFD);
    if (flags < 0) {
        err = "failed to query pipe flags: " + std::string(std::strerror(errno));
        return false;
    }
    if (::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        err = "failed to set close-on-exec on pipe: " + std::string(std::strerror(errno));
        return false;
    }
    return true;
}

/// @brief Move a descriptor out of the standard-stream fd range when necessary.
/// @details POSIX file actions operate by descriptor number.  If @c pipe()
///          returns fd 0, 1, or 2 because the parent closed a standard stream,
///          later close actions could accidentally close the child's redirected
///          stdio.  Duplicating such descriptors to fd >= 3 keeps the action
///          sequence unambiguous.
/// @param fd Descriptor slot to update in place.
/// @param err Receives a diagnostic when duplication fails.
/// @return True when @p fd is safely outside the standard descriptor range.
bool move_fd_above_standard_streams(int &fd, std::string &err) {
    if (fd < 0 || fd > STDERR_FILENO) {
        return true;
    }

#ifdef F_DUPFD_CLOEXEC
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
#else
    const int moved = ::fcntl(fd, F_DUPFD, STDERR_FILENO + 1);
#endif
    if (moved < 0) {
        err = "failed to move pipe descriptor above stdio range: " +
              std::string(std::strerror(errno));
        return false;
    }
#ifndef F_DUPFD_CLOEXEC
    if (!set_close_on_exec(moved, err)) {
        int moved_copy = moved;
        close_fd_if_open(moved_copy);
        return false;
    }
#endif
    close_fd_if_open(fd);
    fd = moved;
    return true;
}

/// @brief Create a pipe whose descriptors are close-on-exec by default.
/// @param pipe_fds Two-element descriptor array receiving read and write ends.
/// @param err Receives a diagnostic when pipe creation or flag setup fails.
/// @return True when both descriptors were created and marked close-on-exec.
bool create_cloexec_pipe(int pipe_fds[2], std::string &err) {
    pipe_fds[0] = -1;
    pipe_fds[1] = -1;
    if (::pipe(pipe_fds) != 0) {
        err = "failed to create process pipes: " + std::string(std::strerror(errno));
        return false;
    }
    if (!set_close_on_exec(pipe_fds[0], err) || !set_close_on_exec(pipe_fds[1], err)) {
        close_fd_if_open(pipe_fds[0]);
        close_fd_if_open(pipe_fds[1]);
        return false;
    }
    if (!move_fd_above_standard_streams(pipe_fds[0], err) ||
        !move_fd_above_standard_streams(pipe_fds[1], err)) {
        close_fd_if_open(pipe_fds[0]);
        close_fd_if_open(pipe_fds[1]);
        return false;
    }
    return true;
}

/// @brief Build mutable argv pointers for @c posix_spawnp.
/// @details The returned pointers reference strings owned by @p argv, so the
///          caller must keep @p argv alive until the spawn call returns.
/// @param argv Stable argument vector including the executable.
/// @return NUL-terminated mutable pointer vector accepted by POSIX spawn APIs.
std::vector<char *> build_spawn_argv(const std::vector<std::string> &argv) {
    std::vector<char *> exec_argv;
    exec_argv.reserve(argv.size() + 1);
    for (const auto &arg : argv) {
        exec_argv.push_back(const_cast<char *>(arg.c_str()));
    }
    exec_argv.push_back(nullptr);
    return exec_argv;
}

/// @brief Build a POSIX environment vector with scoped overrides applied.
/// @details The current process environment is copied in the parent, duplicate
///          names are collapsed, and the last override for a key wins.  Empty
///          override names are ignored.
/// @param env_overrides Key/value override list.
/// @param env_strings Receives owned @c NAME=VALUE strings.
/// @param err Receives a diagnostic when an override name is invalid.
/// @return True when @p env_strings is ready for pointer conversion.
bool build_posix_environment_strings(
    const std::vector<std::pair<std::string, std::string>> &env_overrides,
    std::vector<std::string> &env_strings,
    std::string &err) {
    std::map<std::string, std::string> overrides;
    for (const auto &entry : env_overrides) {
        if (!validate_environment_name(entry.first, err)) {
            return false;
        }
        if (entry.first.empty()) {
            continue;
        }
        overrides[entry.first] = entry.second;
    }

    env_strings.clear();
    if (overrides.empty()) {
        return true;
    }

    std::map<std::string, bool> emitted;
    for (char **cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor) {
        std::string current(*cursor);
        const std::size_t equals = current.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string name = current.substr(0, equals);
        if (emitted.find(name) != emitted.end()) {
            continue;
        }

        if (auto override_it = overrides.find(name); override_it != overrides.end()) {
            env_strings.push_back(override_it->first + "=" + override_it->second);
            emitted.emplace(override_it->first, true);
            overrides.erase(override_it);
        } else {
            env_strings.push_back(std::move(current));
            emitted.emplace(name, true);
        }
    }

    for (const auto &entry : overrides) {
        env_strings.push_back(entry.first + "=" + entry.second);
    }
    return true;
}

/// @brief Build mutable envp pointers for @c posix_spawnp.
/// @details The returned pointers reference strings owned by @p env_strings, so
///          the caller must keep that vector alive until the spawn call returns.
/// @param env_strings Stable @c NAME=VALUE strings.
/// @return NUL-terminated mutable pointer vector accepted by POSIX spawn APIs.
std::vector<char *> build_spawn_envp(std::vector<std::string> &env_strings) {
    std::vector<char *> envp;
    envp.reserve(env_strings.size() + 1);
    for (auto &entry : env_strings) {
        envp.push_back(entry.data());
    }
    envp.push_back(nullptr);
    return envp;
}

/// @brief Add a working-directory change to POSIX spawn file actions.
/// @details macOS exposes the standardized
///          @c posix_spawn_file_actions_addchdir spelling, while glibc and
///          several BSD libcs expose the non-standard @c *_np extension.
///          Zanna uses these APIs to preserve cwd support without running
///          allocation-heavy code after @c fork.
/// @param actions File actions object being prepared for @c posix_spawnp.
/// @param cwd Working directory path.
/// @return Zero on success, or a POSIX error number on failure/unsupported hosts.
int add_spawn_chdir_action(posix_spawn_file_actions_t *actions, const std::string &cwd) {
#if defined(__APPLE__)
    return posix_spawn_file_actions_addchdir(actions, cwd.c_str());
#elif defined(__linux__) || defined(__FreeBSD__)
    return posix_spawn_file_actions_addchdir_np(actions, cwd.c_str());
#else
    (void)actions;
    (void)cwd;
    return ENOTSUP;
#endif
}

/// @brief Add stdio redirection and optional cwd actions for @c posix_spawnp.
/// @param actions Initialised POSIX file actions object.
/// @param stdout_pipe Pipe used to capture child stdout.
/// @param stderr_pipe Pipe used to capture child stderr.
/// @param cwd Optional working directory.
/// @param err Receives a diagnostic when an action cannot be added.
/// @return True when all required actions were added.
bool add_spawn_file_actions(posix_spawn_file_actions_t *actions,
                            const int stdout_pipe[2],
                            const int stderr_pipe[2],
                            const std::optional<std::string> &cwd,
                            std::string &err) {
    /// Convert a POSIX action error code into the caller's diagnostic channel.
    auto fail = [&](int code, const char *what) {
        err = std::string(what) + ": " + std::strerror(code);
        return false;
    };

    if (cwd.has_value()) {
        if (const int code = add_spawn_chdir_action(actions, *cwd); code != 0) {
            return fail(code, "failed to add child working directory action");
        }
    }
    if (const int code = posix_spawn_file_actions_adddup2(actions, stdout_pipe[1], STDOUT_FILENO);
        code != 0) {
        return fail(code, "failed to add stdout redirection action");
    }
    if (const int code = posix_spawn_file_actions_adddup2(actions, stderr_pipe[1], STDERR_FILENO);
        code != 0) {
        return fail(code, "failed to add stderr redirection action");
    }
    if (const int code = posix_spawn_file_actions_addclose(actions, stdout_pipe[0]); code != 0) {
        return fail(code, "failed to add stdout read-close action");
    }
    if (const int code = posix_spawn_file_actions_addclose(actions, stderr_pipe[0]); code != 0) {
        return fail(code, "failed to add stderr read-close action");
    }
    if (const int code = posix_spawn_file_actions_addclose(actions, stdout_pipe[1]); code != 0) {
        return fail(code, "failed to add stdout write-close action");
    }
    if (const int code = posix_spawn_file_actions_addclose(actions, stderr_pipe[1]); code != 0) {
        return fail(code, "failed to add stderr write-close action");
    }
    return true;
}

enum class PosixCaptureErrorKind {
    None,
    Poll,
    InvalidDescriptor,
    Read,
};

struct PosixCaptureStatus {
    PosixCaptureErrorKind error_kind{PosixCaptureErrorKind::None};
    int native_error{0};
    bool storage_failed{false};
};

/// @brief Capture stdout and stderr pipes until both reach EOF or fail.
/// @details The function polls both descriptors so one full pipe cannot block
///          the child while the parent reads the other. Interrupted reads and
///          polls are retried. Capture storage failure switches only that sink
///          to discard mode so both pipes are still drained to EOF.
/// @param stdout_fd Owned read end of the stdout pipe; reset before return.
/// @param stderr_fd Owned read end of the stderr pipe; reset before return.
/// @param out Receives stdout bytes.
/// @param err Receives stderr bytes.
/// @return Allocation-free status describing the first native error and any
///         discarded output.
PosixCaptureStatus capture_posix_pipes(UniqueFd &stdout_fd,
                                       UniqueFd &stderr_fd,
                                       std::string &out,
                                       std::string &err) noexcept {
    PosixCaptureStatus status;
    CaptureSink stdout_sink(out);
    CaptureSink stderr_sink(err);
    char buffer[4096];

    while (stdout_fd.valid() || stderr_fd.valid()) {
        pollfd fds[2];
        nfds_t nfds = 0;

        if (stdout_fd.valid()) {
            fds[nfds].fd = stdout_fd.get();
            fds[nfds].events = POLLIN | POLLHUP;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (stderr_fd.valid()) {
            fds[nfds].fd = stderr_fd.get();
            fds[nfds].events = POLLIN | POLLHUP;
            fds[nfds].revents = 0;
            ++nfds;
        }

        const int ready = poll(fds, nfds, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            status.error_kind = PosixCaptureErrorKind::Poll;
            status.native_error = errno;
            stdout_fd.reset();
            stderr_fd.reset();
            break;
        }

        for (nfds_t i = 0; i < nfds; ++i) {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) {
                continue;
            }

            UniqueFd *source = nullptr;
            CaptureSink *sink = nullptr;
            if (stdout_fd.valid() && fds[i].fd == stdout_fd.get()) {
                source = &stdout_fd;
                sink = &stdout_sink;
            } else {
                source = &stderr_fd;
                sink = &stderr_sink;
            }

            if ((fds[i].revents & POLLNVAL) != 0) {
                if (status.error_kind == PosixCaptureErrorKind::None)
                    status.error_kind = PosixCaptureErrorKind::InvalidDescriptor;
                source->reset();
                continue;
            }

            const ssize_t nread = ::read(fds[i].fd, buffer, sizeof(buffer));
            if (nread > 0) {
                sink->append(buffer, static_cast<std::size_t>(nread));
                continue;
            }
            if (nread < 0 && errno == EINTR) {
                continue;
            }
            if (nread < 0) {
                if (status.error_kind == PosixCaptureErrorKind::None) {
                    status.error_kind = PosixCaptureErrorKind::Read;
                    status.native_error = errno;
                }
            }

            source->reset();
        }
    }
    status.storage_failed = stdout_sink.storage_failed() || stderr_sink.storage_failed();
    return status;
}
#endif

} // namespace

namespace zanna::test_support {
/// @brief Expose the Win32 UTF-8 input-length policy to host-neutral tests.
/// @param length Candidate UTF-8 byte count.
/// @return True when the signed Win32 API length field can represent it.
bool windows_utf8_length_is_representable(std::size_t length) noexcept {
    return windows_utf8_api_length_fits(length);
}

/// @brief Expose bounded Windows quoting arithmetic to host-neutral tests.
/// @param argument Synthetic wide argument whose code-unit count is examined.
/// @return Quoted length, or no value when it exceeds CreateProcessW's budget.
std::optional<std::size_t> windows_quoted_length(std::wstring_view argument) noexcept {
    return windows_quoted_argument_length(argument);
}

/// @brief Test whether a quoted argument fits after existing command-line content.
/// @param current Existing command-line content length.
/// @param argument Synthetic raw wide argument.
/// @param needs_separator Whether a separating space is required.
/// @return True when content plus quoting, separator, and terminator fit.
bool windows_command_line_argument_fits(std::size_t current,
                                        std::wstring_view argument,
                                        bool needs_separator) noexcept {
    const auto quoted_length = windows_quoted_argument_length(argument);
    return quoted_length &&
           windows_command_line_append_fits(current, *quoted_length, needs_separator);
}

/// @brief Expose the lossless UTF-8 preflight ceiling to host-neutral tests.
/// @param byte_length Candidate UTF-8 argument size.
/// @return True when the argument could still fit after conversion.
bool windows_utf8_argument_may_fit(std::size_t byte_length) noexcept {
    return windows_utf8_argument_may_fit_command_line(byte_length);
}

/// @brief Configure deterministic capture-storage failure for focused tests.
/// @param limit Per-stream bytes retained before capture enters discard mode;
///        `SIZE_MAX` disables fault injection.
void set_run_process_capture_failure_after_bytes(std::size_t limit) noexcept {
    g_capture_failure_after_bytes.store(limit, std::memory_order_relaxed);
}

/// @brief Observations produced by the scoped-environment move probe.
/// @details This private test-support record lets a separate test translation
///          unit verify move construction, move assignment, and final
///          restoration without exposing the guard implementation itself.
struct ScopedEnvironmentAssignmentMoveResult {
    ///< Whether the source override survived move construction.
    bool value_visible_after_move_ctor;
    ///< Whether the transferred override survived move assignment.
    bool value_visible_after_move_assign;
    ///< Whether leaving both scopes restored the pre-probe environment.
    bool restored;
    ///< Value observed immediately after move assignment, if defined.
    std::optional<std::string> move_assigned_value;
};

/// @brief Exercise restoration ownership across both guard move operations.
/// @details Snapshots @p name, applies @p source_value, move-constructs a new
///          guard, then move-assigns that guard over a receiver holding
///          @p receiver_value. After the scope exits, records whether the
///          original environment state was restored.
/// @param name Environment variable used by the probe.
/// @param source_value Override whose visibility should survive both moves.
/// @param receiver_value Temporary value owned by the move-assignment receiver.
/// @return Visibility and restoration observations collected during the probe.
/// @warning Mutates process-global environment state and is not thread-safe.
ScopedEnvironmentAssignmentMoveResult scoped_environment_assignment_move_preserves(
    const std::string &name, const std::string &source_value, const std::string &receiver_value) {
    const char *original_raw = std::getenv(name.c_str());
    std::optional<std::string> original_value;
    if (original_raw != nullptr) {
        original_value = std::string(original_raw);
    }

    ScopedEnvironmentAssignmentMoveResult result{false, false, false, std::nullopt};

    {
        ScopedEnvironmentAssignment guard(name, source_value);
        ScopedEnvironmentAssignment moved(std::move(guard));
        const char *current = std::getenv(name.c_str());
        result.value_visible_after_move_ctor =
            (current != nullptr) && std::string(current) == source_value;

        ScopedEnvironmentAssignment receiver(name, receiver_value);
        receiver = std::move(moved);
        current = std::getenv(name.c_str());
        if (current != nullptr) {
            result.move_assigned_value = std::string(current);
            result.value_visible_after_move_assign = *result.move_assigned_value == source_value;
        } else {
            result.value_visible_after_move_assign = false;
        }
    }

    const char *restored_raw = std::getenv(name.c_str());
    if (original_value.has_value()) {
        result.restored = (restored_raw != nullptr) && std::string(restored_raw) == *original_value;
    } else {
        result.restored = (restored_raw == nullptr) || std::string(restored_raw).empty();
    }

    return result;
}
} // namespace zanna::test_support

/// @brief Launch a subprocess directly from an argument vector.
/// @details Applies child-only environment and working-directory configuration,
///          captures stdout and stderr independently, and reports both setup
///          failures and normalized process termination in the returned result.
/// @param argv Arguments including the executable at index zero.
/// @param cwd Optional existing directory used only by the child.
/// @param env Child environment overrides; later duplicate names win.
/// @return Captured output, native/normalized status, and launch diagnostics.
RunResult run_process(const std::vector<std::string> &argv,
                      std::optional<std::string> cwd,
                      const std::vector<std::pair<std::string, std::string>> &env) try {
    RunResult rr{};
    /// @brief Mark a pre-launch failure and replace stderr with its diagnostic.
    /// @param message Launcher diagnostic to retain.
    /// @return Reference-independent copy of the updated process result.
    auto fail_launch = [&](std::string message) {
        rr.exit_code = -1;
        rr.launch_failed = true;
        rr.err = std::move(message);
        return rr;
    };

    if (argv.empty()) {
        return fail_launch("empty argv");
    }
    for (std::size_t index = 0; index < argv.size(); ++index) {
        if (contains_embedded_nul(argv[index])) {
            return fail_launch("process argument " + std::to_string(index) +
                               " contains embedded NUL");
        }
    }
    if (cwd.has_value() && contains_embedded_nul(*cwd)) {
        return fail_launch("working directory contains embedded NUL");
    }
    for (const auto &entry : env) {
        if (contains_embedded_nul(entry.first)) {
            return fail_launch("environment variable name contains embedded NUL");
        }
        if (contains_embedded_nul(entry.second)) {
            return fail_launch("environment variable value contains embedded NUL");
        }
    }

    if (!validate_working_directory(cwd, rr.err)) {
        rr.exit_code = -1;
        rr.launch_failed = true;
        return rr;
    }

#ifdef _WIN32
    auto cmdline = join_windows_command_line(argv, rr.err);
    if (!cmdline) {
        return fail_launch(rr.err);
    }

    std::optional<std::wstring> wide_cwd;
    if (cwd.has_value()) {
        wide_cwd = utf8_to_wide(*cwd, rr.err);
        if (!wide_cwd) {
            return fail_launch(rr.err);
        }
    }

    std::vector<wchar_t> environment_block;
    if (!build_windows_environment_block(env, environment_block, rr.err)) {
        return fail_launch(rr.err);
    }

    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE stdout_read_raw = INVALID_HANDLE_VALUE;
    HANDLE stdout_write_raw = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&stdout_read_raw, &stdout_write_raw, &security, 0)) {
        const DWORD pipe_error = GetLastError();
        return fail_launch("failed to create process stdout pipe: " +
                           format_windows_error(pipe_error));
    }
    UniqueWindowsHandle stdout_read(stdout_read_raw);
    UniqueWindowsHandle stdout_write(stdout_write_raw);

    HANDLE stderr_read_raw = INVALID_HANDLE_VALUE;
    HANDLE stderr_write_raw = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&stderr_read_raw, &stderr_write_raw, &security, 0)) {
        const DWORD pipe_error = GetLastError();
        return fail_launch("failed to create process stderr pipe: " +
                           format_windows_error(pipe_error));
    }
    UniqueWindowsHandle stderr_read(stderr_read_raw);
    UniqueWindowsHandle stderr_write(stderr_write_raw);

    if (!SetHandleInformation(stdout_read.get(), HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0)) {
        const DWORD inheritance_error = GetLastError();
        const std::string message = "failed to configure process pipe inheritance: " +
                                    format_windows_error(inheritance_error);
        return fail_launch(message);
    }

    UniqueWindowsHandle stdin_for_child(
        duplicate_inheritable_handle(GetStdHandle(STD_INPUT_HANDLE)));
    if (!stdin_for_child.valid()) {
        stdin_for_child.reset(open_inheritable_nul_input_handle());
    }

    if (!stdin_for_child.valid()) {
        const DWORD stdin_error = GetLastError();
        return fail_launch("failed to open child stdin handle: " +
                           format_windows_error(stdin_error));
    }

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_for_child.get();
    startup.StartupInfo.hStdOutput = stdout_write.get();
    startup.StartupInfo.hStdError = stderr_write.get();
    StartupAttributeListGuard startup_guard(startup);
    std::vector<char> startup_attribute_storage;
    std::vector<HANDLE> inherited_handles = {
        stdin_for_child.get(), stdout_write.get(), stderr_write.get()};
    if (!initialize_handle_inheritance_list(
            startup, startup_attribute_storage, inherited_handles, rr.err)) {
        return fail_launch(rr.err);
    }

    std::vector<wchar_t> mutable_cmdline(cmdline->begin(), cmdline->end());
    mutable_cmdline.push_back(L'\0');

    DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT;
    LPVOID environment_ptr = nullptr;
    if (!environment_block.empty()) {
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
        environment_ptr = environment_block.data();
    }

    CaptureSink stdout_sink(rr.out);
    CaptureSink stderr_sink(rr.err);
    CaptureThreadContext stdout_ctx{stdout_read.get(), &stdout_sink, ERROR_SUCCESS};
    CaptureThreadContext stderr_ctx{stderr_read.get(), &stderr_sink, ERROR_SUCCESS};
    CaptureThreadGuard stdout_thread;
    CaptureThreadGuard stderr_thread;
    WindowsChildGuard child;

    const BOOL created = CreateProcessW(nullptr,
                                        mutable_cmdline.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        creation_flags,
                                        environment_ptr,
                                        wide_cwd ? wide_cwd->c_str() : nullptr,
                                        &startup.StartupInfo,
                                        child.put());
    const DWORD create_error = GetLastError();
    child.set_launched(created != FALSE);
    delete_handle_inheritance_list(startup);

    stdin_for_child.reset();
    stdout_write.reset();
    stderr_write.reset();

    if (!created) {
        const std::string message =
            "failed to launch process: " + format_windows_error(create_error);
        return fail_launch(message);
    }
    rr.launched = true;

    const uintptr_t stdout_thread_value =
        _beginthreadex(nullptr, 0, capture_handle_thread_proc, &stdout_ctx, 0, nullptr);
    const int stdout_thread_error = errno;
    const uintptr_t stderr_thread_value =
        _beginthreadex(nullptr, 0, capture_handle_thread_proc, &stderr_ctx, 0, nullptr);
    const int stderr_thread_error = errno;
    stdout_thread.reset(reinterpret_cast<HANDLE>(stdout_thread_value));
    stderr_thread.reset(reinterpret_cast<HANDLE>(stderr_thread_value));

    if (!stdout_thread.valid() || !stderr_thread.valid()) {
        const int thread_error = !stdout_thread.valid() ? stdout_thread_error : stderr_thread_error;
        child.terminate_and_wait();
        stdout_thread.join();
        stderr_thread.join();
        rr.capture_failed = true;
        rr.exit_code = 127;
        rr.native_exit_code = 127;
        append_diagnostic_noexcept(
            rr.err, "failed to create process capture thread: ", std::strerror(thread_error));
        return rr;
    }

    const DWORD process_wait = child.wait();
    if (process_wait != WAIT_OBJECT_0) {
        const DWORD wait_error = process_wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        child.terminate_and_wait();
        append_windows_diagnostic_noexcept(
            rr.err, "failed while waiting for child process: ", wait_error);
    }
    stdout_thread.join();
    stderr_thread.join();
    if (stdout_sink.storage_failed() || stderr_sink.storage_failed()) {
        rr.capture_failed = true;
        append_diagnostic_noexcept(rr.err, "process capture storage exhausted; output discarded");
    }
    if (stdout_ctx.error != ERROR_SUCCESS) {
        rr.capture_failed = true;
        append_windows_diagnostic_noexcept(
            rr.err, "failed while capturing child stdout: ", stdout_ctx.error);
    }
    if (stderr_ctx.error != ERROR_SUCCESS) {
        rr.capture_failed = true;
        append_windows_diagnostic_noexcept(
            rr.err, "failed while capturing child stderr: ", stderr_ctx.error);
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(child.process_handle(), &exit_code)) {
        const DWORD exit_error = GetLastError();
        append_windows_diagnostic_noexcept(rr.err, "failed to query child exit code: ", exit_error);
        exit_code = static_cast<DWORD>(INT_MAX);
    }
    rr.native_exit_code = static_cast<std::uint32_t>(exit_code);
    rr.exit_code = normalize_windows_exit_code(exit_code);

    return rr;
#else
    int stdout_pipe[2] = {-1, -1};
    if (!create_cloexec_pipe(stdout_pipe, rr.err)) {
        return fail_launch(rr.err);
    }
    UniqueFd stdout_read(stdout_pipe[0]);
    UniqueFd stdout_write(stdout_pipe[1]);

    int stderr_pipe[2] = {-1, -1};
    if (!create_cloexec_pipe(stderr_pipe, rr.err))
        return fail_launch(rr.err);
    UniqueFd stderr_read(stderr_pipe[0]);
    UniqueFd stderr_write(stderr_pipe[1]);

    std::vector<std::string> env_strings;
    if (!build_posix_environment_strings(env, env_strings, rr.err))
        return fail_launch(rr.err);

    std::vector<char *> exec_argv = build_spawn_argv(argv);
    std::vector<char *> envp_storage = build_spawn_envp(env_strings);
    char *const *spawn_envp = env_strings.empty() ? environ : envp_storage.data();

    ScopedSpawnFileActions actions;
    if (const int code = actions.initialize(); code != 0) {
        return fail_launch("failed to initialise process file actions: " +
                           std::string(std::strerror(code)));
    }

    const int stdout_fds[2] = {stdout_read.get(), stdout_write.get()};
    const int stderr_fds[2] = {stderr_read.get(), stderr_write.get()};
    if (!add_spawn_file_actions(actions.get(), stdout_fds, stderr_fds, cwd, rr.err))
        return fail_launch(rr.err);

    pid_t pid = -1;
    const int spawn_error =
        posix_spawnp(&pid, argv[0].c_str(), actions.get(), nullptr, exec_argv.data(), spawn_envp);
    stdout_write.reset();
    stderr_write.reset();

    if (spawn_error != 0) {
        return fail_launch("failed to launch process '" + argv[0] +
                           "': " + std::string(std::strerror(spawn_error)));
    }
    PosixChildGuard child(pid);
    rr.launched = true;

    const PosixCaptureStatus capture =
        capture_posix_pipes(stdout_read, stderr_read, rr.out, rr.err);
    if (capture.storage_failed) {
        rr.capture_failed = true;
        append_diagnostic_noexcept(rr.err, "process capture storage exhausted; output discarded");
    }
    if (capture.error_kind != PosixCaptureErrorKind::None) {
        rr.capture_failed = true;
        switch (capture.error_kind) {
            case PosixCaptureErrorKind::Poll:
                append_diagnostic_noexcept(rr.err,
                                           "failed to poll child output pipes: ",
                                           std::strerror(capture.native_error));
                break;
            case PosixCaptureErrorKind::InvalidDescriptor:
                append_diagnostic_noexcept(rr.err, "child output pipe became invalid");
                break;
            case PosixCaptureErrorKind::Read:
                append_diagnostic_noexcept(rr.err,
                                           "failed to read child output pipe: ",
                                           std::strerror(capture.native_error));
                break;
            case PosixCaptureErrorKind::None:
                break;
        }
    }

    int status = 0;
    if (const int wait_error = child.wait(status); wait_error != 0) {
        rr.exit_code = -1;
        append_diagnostic_noexcept(rr.err, "failed to wait for child: ", std::strerror(wait_error));
        return rr;
    }

    if (WIFEXITED(status)) {
        rr.exit_code = WEXITSTATUS(status);
        rr.native_exit_code = static_cast<std::uint32_t>(rr.exit_code);
    } else if (WIFSIGNALED(status)) {
        rr.exit_code = 128 + WTERMSIG(status);
        rr.native_exit_code = static_cast<std::uint32_t>(rr.exit_code);
    } else {
        rr.exit_code = status;
        rr.native_exit_code = static_cast<std::uint32_t>(rr.exit_code);
    }
    return rr;
#endif
} catch (const std::bad_alloc &) {
    return make_process_storage_failure();
} catch (const std::length_error &) {
    return make_process_storage_failure();
}

/// @brief Launch command text through the host's explicit command shell.
/// @details Delegates to @ref run_process using @c cmd /C on Windows or
///          @c sh -c on POSIX platforms, so shell parsing and expansion apply.
/// @param command Shell command text.
/// @param cwd Optional existing directory used only by the child.
/// @param env Child environment overrides.
/// @return Captured subprocess result from the shell invocation.
RunResult run_shell_command(const std::string &command,
                            std::optional<std::string> cwd,
                            const std::vector<std::pair<std::string, std::string>> &env) {
#ifdef _WIN32
    return run_process({"cmd", "/C", command}, std::move(cwd), env);
#else
    return run_process({"sh", "-c", command}, std::move(cwd), env);
#endif
}
