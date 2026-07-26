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
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
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
    if (text.empty()) {
        return std::wstring{};
    }
    const int needed = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) {
        err = "failed to convert UTF-8 text for Windows process launch: " +
              format_windows_error(GetLastError());
        return std::nullopt;
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    const int written = MultiByteToWideChar(CP_UTF8,
                                            MB_ERR_INVALID_CHARS,
                                            text.data(),
                                            static_cast<int>(text.size()),
                                            out.data(),
                                            needed);
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

/// @brief Determine whether a Windows argument needs command-line quoting.
/// @param arg Argument after UTF-16 conversion.
/// @return True when @p arg contains whitespace or shell-sensitive punctuation.
bool needs_quoting(const std::wstring &arg) {
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

/// @brief Quote one UTF-16 command-line argument using the Win32 argv rules.
/// @details Implements the backslash-before-quote algorithm used by the
///          Microsoft C runtime when it reconstructs argv from a command line.
/// @param arg Raw UTF-16 argument.
/// @return Argument text safe to concatenate into a Win32 command line.
std::wstring quote_windows_argument(const std::wstring &arg) {
    if (!needs_quoting(arg)) {
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

    std::string message(buffer, buffer + size);
    LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == '\n' || message.back() == '\r' || message.back() == '.')) {
        message.pop_back();
    }
    return message;
}

/// @brief Join argv into the mutable command-line string required by CreateProcessW.
/// @param argv UTF-8 argument vector including the executable at index zero.
/// @param err Receives a diagnostic when UTF-8 conversion fails.
/// @return UTF-16 command line on success, or @c std::nullopt on failure.
std::optional<std::wstring> join_windows_command_line(const std::vector<std::string> &argv,
                                                      std::string &err) {
    std::wstring cmdline;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) {
            cmdline.push_back(L' ');
        }
        auto wide = utf8_to_wide(argv[i], err);
        if (!wide) {
            return std::nullopt;
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
    FreeEnvironmentStringsW(raw_environment);

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
    ///< Caller-owned byte buffer receiving all data read before EOF.
    std::string *buffer = nullptr;
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
            ctx->buffer->append(chunk, chunk + read);
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

/// @brief Destroy POSIX spawn file actions if they were initialised.
/// @param actions File actions object.
/// @param initialised Whether @p actions has been initialised.
void destroy_spawn_file_actions(posix_spawn_file_actions_t &actions, bool initialised) noexcept {
    if (initialised) {
        posix_spawn_file_actions_destroy(&actions);
    }
}

/// @brief Capture stdout and stderr pipes until both reach EOF or fail.
/// @details The function polls both descriptors so one full pipe cannot block
///          the child while the parent reads the other.  Interrupted reads and
///          polls are retried; hard errors are reported through @p capture_error.
/// @param stdout_fd Read end of the stdout pipe; closed before return.
/// @param stderr_fd Read end of the stderr pipe; closed before return.
/// @param out Receives stdout bytes.
/// @param err Receives stderr bytes.
/// @param capture_error Receives diagnostics for pipe-level failures.
/// @return True when capture reached EOF cleanly.
bool capture_posix_pipes(
    int stdout_fd, int stderr_fd, std::string &out, std::string &err, std::string &capture_error) {
    bool stdout_open = stdout_fd >= 0;
    bool stderr_open = stderr_fd >= 0;
    char buffer[4096];
    bool ok = true;

    /// Close a descriptor copy and clear its separately tracked open state.
    auto close_tracked = [](int fd, bool &open_flag) {
        int close_fd = fd;
        close_fd_if_open(close_fd);
        open_flag = false;
    };

    while (stdout_open || stderr_open) {
        pollfd fds[2];
        nfds_t nfds = 0;

        if (stdout_open) {
            fds[nfds].fd = stdout_fd;
            fds[nfds].events = POLLIN | POLLHUP;
            fds[nfds].revents = 0;
            ++nfds;
        }
        if (stderr_open) {
            fds[nfds].fd = stderr_fd;
            fds[nfds].events = POLLIN | POLLHUP;
            fds[nfds].revents = 0;
            ++nfds;
        }

        const int ready = poll(fds, nfds, -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            capture_error =
                "failed to poll child output pipes: " + std::string(std::strerror(errno));
            ok = false;
            if (stdout_open) {
                close_tracked(stdout_fd, stdout_open);
            }
            if (stderr_open) {
                close_tracked(stderr_fd, stderr_open);
            }
            break;
        }

        for (nfds_t i = 0; i < nfds; ++i) {
            if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) {
                continue;
            }

            std::string *target = nullptr;
            bool *open_flag = nullptr;
            if (stdout_open && fds[i].fd == stdout_fd) {
                target = &out;
                open_flag = &stdout_open;
            } else {
                target = &err;
                open_flag = &stderr_open;
            }

            if ((fds[i].revents & POLLNVAL) != 0) {
                capture_error = "child output pipe became invalid";
                ok = false;
                *open_flag = false;
                continue;
            }

            const ssize_t nread = ::read(fds[i].fd, buffer, sizeof(buffer));
            if (nread > 0) {
                target->append(buffer, buffer + nread);
                continue;
            }
            if (nread < 0 && errno == EINTR) {
                continue;
            }
            if (nread < 0) {
                capture_error =
                    "failed to read child output pipe: " + std::string(std::strerror(errno));
                ok = false;
            }

            ::close(fds[i].fd);
            *open_flag = false;
        }
    }
    return ok;
}
#endif

} // namespace

namespace zanna::test_support {
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
                      const std::vector<std::pair<std::string, std::string>> &env) {
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

    HANDLE stdout_read = INVALID_HANDLE_VALUE;
    HANDLE stdout_write = INVALID_HANDLE_VALUE;
    HANDLE stderr_read = INVALID_HANDLE_VALUE;
    HANDLE stderr_write = INVALID_HANDLE_VALUE;

    if (!CreatePipe(&stdout_read, &stdout_write, &security, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &security, 0)) {
        const std::string message =
            "failed to create process pipes: " + format_windows_error(GetLastError());
        close_handle_if_valid(stdout_read);
        close_handle_if_valid(stdout_write);
        close_handle_if_valid(stderr_read);
        close_handle_if_valid(stderr_write);
        return fail_launch(message);
    }

    if (!SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        const std::string message =
            "failed to configure process pipe inheritance: " + format_windows_error(GetLastError());
        close_handle_if_valid(stdout_read);
        close_handle_if_valid(stdout_write);
        close_handle_if_valid(stderr_read);
        close_handle_if_valid(stderr_write);
        return fail_launch(message);
    }

    HANDLE stdin_for_child = duplicate_inheritable_handle(GetStdHandle(STD_INPUT_HANDLE));
    if (stdin_for_child == INVALID_HANDLE_VALUE) {
        stdin_for_child = open_inheritable_nul_input_handle();
    }

    if (stdin_for_child == INVALID_HANDLE_VALUE) {
        close_handle_if_valid(stdin_for_child);
        close_handle_if_valid(stdout_read);
        close_handle_if_valid(stdout_write);
        close_handle_if_valid(stderr_read);
        close_handle_if_valid(stderr_write);
        return fail_launch("failed to open child stdin handle: " +
                           format_windows_error(GetLastError()));
    }

    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_for_child;
    startup.StartupInfo.hStdOutput = stdout_write;
    startup.StartupInfo.hStdError = stderr_write;
    std::vector<char> startup_attribute_storage;
    std::vector<HANDLE> inherited_handles = {stdin_for_child, stdout_write, stderr_write};
    if (!initialize_handle_inheritance_list(
            startup, startup_attribute_storage, inherited_handles, rr.err)) {
        close_handle_if_valid(stdin_for_child);
        close_handle_if_valid(stdout_read);
        close_handle_if_valid(stdout_write);
        close_handle_if_valid(stderr_read);
        close_handle_if_valid(stderr_write);
        return fail_launch(rr.err);
    }

    PROCESS_INFORMATION process = {};
    std::vector<wchar_t> mutable_cmdline(cmdline->begin(), cmdline->end());
    mutable_cmdline.push_back(L'\0');

    DWORD creation_flags = EXTENDED_STARTUPINFO_PRESENT;
    LPVOID environment_ptr = nullptr;
    if (!environment_block.empty()) {
        creation_flags |= CREATE_UNICODE_ENVIRONMENT;
        environment_ptr = environment_block.data();
    }

    const BOOL created = CreateProcessW(nullptr,
                                        mutable_cmdline.data(),
                                        nullptr,
                                        nullptr,
                                        TRUE,
                                        creation_flags,
                                        environment_ptr,
                                        wide_cwd ? wide_cwd->c_str() : nullptr,
                                        &startup.StartupInfo,
                                        &process);
    const DWORD create_error = GetLastError();
    delete_handle_inheritance_list(startup);

    close_handle_if_valid(stdin_for_child);
    close_handle_if_valid(stdout_write);
    close_handle_if_valid(stderr_write);

    if (!created) {
        const std::string message =
            "failed to launch process: " + format_windows_error(create_error);
        close_handle_if_valid(stdout_read);
        close_handle_if_valid(stderr_read);
        return fail_launch(message);
    }
    rr.launched = true;

    CaptureThreadContext stdout_ctx{stdout_read, &rr.out, ERROR_SUCCESS};
    CaptureThreadContext stderr_ctx{stderr_read, &rr.err, ERROR_SUCCESS};
    const uintptr_t stdout_thread_value =
        _beginthreadex(nullptr, 0, capture_handle_thread_proc, &stdout_ctx, 0, nullptr);
    const int stdout_thread_error = errno;
    const uintptr_t stderr_thread_value =
        _beginthreadex(nullptr, 0, capture_handle_thread_proc, &stderr_ctx, 0, nullptr);
    const int stderr_thread_error = errno;
    HANDLE stdout_thread = reinterpret_cast<HANDLE>(stdout_thread_value);
    HANDLE stderr_thread = reinterpret_cast<HANDLE>(stderr_thread_value);

    if (stdout_thread == nullptr || stderr_thread == nullptr) {
        const int thread_error =
            stdout_thread == nullptr ? stdout_thread_error : stderr_thread_error;
        const std::string message =
            "failed to create process capture thread: " + std::string(std::strerror(thread_error));
        TerminateProcess(process.hProcess, 127);
        WaitForSingleObject(process.hProcess, INFINITE);
        if (stdout_thread != nullptr) {
            WaitForSingleObject(stdout_thread, INFINITE);
            CloseHandle(stdout_thread);
        }
        if (stderr_thread != nullptr) {
            WaitForSingleObject(stderr_thread, INFINITE);
            CloseHandle(stderr_thread);
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        close_handle_if_valid(stdout_read);
        close_handle_if_valid(stderr_read);
        return fail_launch(message);
    }

    const DWORD process_wait = WaitForSingleObject(process.hProcess, INFINITE);
    if (process_wait != WAIT_OBJECT_0) {
        const DWORD wait_error = process_wait == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        TerminateProcess(process.hProcess, 127);
        WaitForSingleObject(process.hProcess, INFINITE);
        rr.err += (rr.err.empty() ? "" : "\n");
        rr.err += "failed while waiting for child process: " + format_windows_error(wait_error);
    }
    WaitForSingleObject(stdout_thread, INFINITE);
    CloseHandle(stdout_thread);
    WaitForSingleObject(stderr_thread, INFINITE);
    CloseHandle(stderr_thread);
    if (stdout_ctx.error != ERROR_SUCCESS) {
        rr.err += (rr.err.empty() ? "" : "\n");
        rr.err += "failed while capturing child stdout: " + format_windows_error(stdout_ctx.error);
    }
    if (stderr_ctx.error != ERROR_SUCCESS) {
        rr.err += (rr.err.empty() ? "" : "\n");
        rr.err += "failed while capturing child stderr: " + format_windows_error(stderr_ctx.error);
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        const DWORD exit_error = GetLastError();
        rr.err += (rr.err.empty() ? "" : "\n");
        rr.err += "failed to query child exit code: " + format_windows_error(exit_error);
        exit_code = static_cast<DWORD>(INT_MAX);
    }
    rr.native_exit_code = static_cast<std::uint32_t>(exit_code);
    rr.exit_code = normalize_windows_exit_code(exit_code);

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    close_handle_if_valid(stdout_read);
    close_handle_if_valid(stderr_read);
    return rr;
#else
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (!create_cloexec_pipe(stdout_pipe, rr.err) || !create_cloexec_pipe(stderr_pipe, rr.err)) {
        close_fd_if_open(stdout_pipe[0]);
        close_fd_if_open(stdout_pipe[1]);
        close_fd_if_open(stderr_pipe[0]);
        close_fd_if_open(stderr_pipe[1]);
        return fail_launch(rr.err);
    }

    std::vector<std::string> env_strings;
    if (!build_posix_environment_strings(env, env_strings, rr.err)) {
        close_fd_if_open(stdout_pipe[0]);
        close_fd_if_open(stdout_pipe[1]);
        close_fd_if_open(stderr_pipe[0]);
        close_fd_if_open(stderr_pipe[1]);
        return fail_launch(rr.err);
    }

    std::vector<char *> exec_argv = build_spawn_argv(argv);
    std::vector<char *> envp_storage = build_spawn_envp(env_strings);
    char *const *spawn_envp = env_strings.empty() ? environ : envp_storage.data();

    posix_spawn_file_actions_t actions{};
    bool actions_initialised = false;
    if (const int code = posix_spawn_file_actions_init(&actions); code != 0) {
        close_fd_if_open(stdout_pipe[0]);
        close_fd_if_open(stdout_pipe[1]);
        close_fd_if_open(stderr_pipe[0]);
        close_fd_if_open(stderr_pipe[1]);
        return fail_launch("failed to initialise process file actions: " +
                           std::string(std::strerror(code)));
    }
    actions_initialised = true;

    if (!add_spawn_file_actions(&actions, stdout_pipe, stderr_pipe, cwd, rr.err)) {
        destroy_spawn_file_actions(actions, actions_initialised);
        close_fd_if_open(stdout_pipe[0]);
        close_fd_if_open(stdout_pipe[1]);
        close_fd_if_open(stderr_pipe[0]);
        close_fd_if_open(stderr_pipe[1]);
        return fail_launch(rr.err);
    }

    pid_t pid = -1;
    const int spawn_error =
        posix_spawnp(&pid, argv[0].c_str(), &actions, nullptr, exec_argv.data(), spawn_envp);
    destroy_spawn_file_actions(actions, actions_initialised);
    close_fd_if_open(stdout_pipe[1]);
    close_fd_if_open(stderr_pipe[1]);

    if (spawn_error != 0) {
        close_fd_if_open(stdout_pipe[0]);
        close_fd_if_open(stderr_pipe[0]);
        return fail_launch("failed to launch process '" + argv[0] +
                           "': " + std::string(std::strerror(spawn_error)));
    }
    rr.launched = true;

    std::string capture_error;
    if (!capture_posix_pipes(stdout_pipe[0], stderr_pipe[0], rr.out, rr.err, capture_error)) {
        rr.err += (rr.err.empty() ? "" : "\n");
        rr.err += capture_error;
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            rr.exit_code = -1;
            rr.err += (rr.err.empty() ? "" : "\n");
            rr.err += "failed to wait for child: " + std::string(std::strerror(errno));
            return rr;
        }
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
