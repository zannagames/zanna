//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/common/ScopedProcess.hpp
// Purpose: RAII helpers for process-wide command-line side effects.
// Key invariants:
//   - File-descriptor redirection restores the original descriptor on scope exit.
//   - Environment overrides restore the prior value on scope exit.
// Ownership/Lifetime:
//   - Helpers own duplicated file descriptors and copied environment strings.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines RAII wrappers for temporary process-wide I/O and environment changes.
/// @details Platform adapters normalize descriptor and environment operations,
///          while scoped owners restore prior state and retain the first failure
///          as human-readable text. Because these mutations affect the entire
///          process, callers must coordinate concurrent threads externally.

#pragma once

#include "common/Filesystem.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zanna::tools {

#ifdef _WIN32
/// @brief Duplicate a descriptor using the Windows CRT.
/// @param fd Descriptor to duplicate.
/// @return New descriptor, or a negative value on failure.
inline int scopedDup(int fd) {
    return _dup(fd);
}

/// @brief Replace one descriptor with a duplicate of another on Windows.
/// @param oldFd Descriptor whose open file is copied.
/// @param newFd Descriptor number to replace.
/// @return Zero on success, or a negative value on failure.
inline int scopedDup2(int oldFd, int newFd) {
    return _dup2(oldFd, newFd);
}

/// @brief Close a Windows CRT descriptor.
/// @param fd Descriptor to close.
/// @return Zero on success, or a negative value on failure.
inline int scopedClose(int fd) {
    return _close(fd);
}

/// @brief Open a UTF-8 path for binary, non-inheritable input on Windows.
/// @param path Null-terminated UTF-8 path; null is treated as an empty path.
/// @return Open descriptor, or a negative value on failure.
inline int scopedOpenRead(const char *path) {
    const std::filesystem::path nativePath =
        zanna::filesystem::pathFromUtf8(path ? std::string_view(path) : std::string_view());
    return _wopen(nativePath.c_str(), _O_RDONLY | _O_BINARY | _O_NOINHERIT);
}

/// @brief Open or create a UTF-8 path for truncating binary output on Windows.
/// @param path Null-terminated UTF-8 path; null is treated as an empty path.
/// @return Open descriptor, or a negative value on failure.
inline int scopedOpenWriteTruncate(const char *path) {
    const std::filesystem::path nativePath =
        zanna::filesystem::pathFromUtf8(path ? std::string_view(path) : std::string_view());
    return _wopen(nativePath.c_str(),
                  _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY | _O_NOINHERIT,
                  _S_IREAD | _S_IWRITE);
}

/// @brief Convert the current C runtime error number to text.
/// @return Owned description of the current `errno` value.
inline std::string scopedLastError() {
    return std::strerror(errno);
}

/// @brief Set a process environment variable through the Windows CRT.
/// @param name Null-terminated variable name.
/// @param value Value to assign; null is normalized to an empty string.
/// @return Zero on success or a CRT error code.
inline int scopedSetEnv(const char *name, const char *value) {
    return _putenv_s(name, value ? value : "");
}

/// @brief Remove a process environment variable through the Windows CRT.
/// @param name Null-terminated variable name.
/// @return Zero on success or a CRT error code.
inline int scopedUnsetEnv(const char *name) {
    return _putenv_s(name, "");
}
#else
/// @brief Duplicate a POSIX file descriptor.
/// @param fd Descriptor to duplicate.
/// @return New descriptor, or `-1` on failure.
inline int scopedDup(int fd) {
    return dup(fd);
}

/// @brief Atomically replace one POSIX descriptor with another.
/// @param oldFd Descriptor whose open file description is copied.
/// @param newFd Descriptor number to replace.
/// @return @p newFd on success, or `-1` on failure.
inline int scopedDup2(int oldFd, int newFd) {
    return dup2(oldFd, newFd);
}

/// @brief Close a POSIX file descriptor.
/// @param fd Descriptor to close.
/// @return Zero on success, or `-1` on failure.
inline int scopedClose(int fd) {
    return close(fd);
}

/// @brief Open a path for read-only input on POSIX.
/// @param path Null-terminated filesystem path.
/// @return Open descriptor, or `-1` on failure.
inline int scopedOpenRead(const char *path) {
    return open(path, O_RDONLY);
}

/// @brief Open or create a path for truncating output on POSIX.
/// @param path Null-terminated filesystem path.
/// @return Open descriptor, or `-1` on failure.
/// @details New files use mode `0666`, subject to the process umask.
inline int scopedOpenWriteTruncate(const char *path) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
}

/// @brief Convert the current POSIX error number to text.
/// @return Owned description of the current `errno` value.
inline std::string scopedLastError() {
    return std::strerror(errno);
}

/// @brief Set or replace a POSIX process environment variable.
/// @param name Null-terminated variable name.
/// @param value Value to assign; null is normalized to an empty string.
/// @return Zero on success, or `-1` on failure.
inline int scopedSetEnv(const char *name, const char *value) {
    return setenv(name, value ? value : "", 1);
}

/// @brief Remove a POSIX process environment variable.
/// @param name Null-terminated variable name.
/// @return Zero on success, or `-1` on failure.
inline int scopedUnsetEnv(const char *name) {
    return unsetenv(name);
}
#endif

/// @brief Temporarily replace one process file descriptor with another file.
/// @details The constructor duplicates @p targetFd, opens @p path, and then uses
///          dup2/_dup2 to make the target descriptor reference that file. The
///          original descriptor is restored by finish() or the destructor. The
///          helper is intentionally descriptor-based instead of freopen-based so a
///          failed open cannot leave stdin/stdout closed.
class ScopedFdRedirect {
  public:
    /// @brief Redirect @p targetFd to @p path.
    /// @param targetFd Descriptor to replace, for example fileno(stdin).
    /// @param path File to open.
    /// @param writeMode When true, open for truncating output; otherwise open for input.
    /// @details Construction records failures in @ref errorMessage instead of
    ///          throwing. A failed redirect closes any temporary descriptors and
    ///          leaves the original target descriptor in place.
    ScopedFdRedirect(int targetFd, const std::string &path, bool writeMode)
        : targetFd_(targetFd), path_(path), writeMode_(writeMode) {
        savedFd_ = scopedDup(targetFd_);
        if (savedFd_ < 0) {
            error_ = "failed to duplicate descriptor for " + path_ + ": " + scopedLastError();
            return;
        }

        const int replacementFd =
            writeMode_ ? scopedOpenWriteTruncate(path_.c_str()) : scopedOpenRead(path_.c_str());
        if (replacementFd < 0) {
            error_ = "failed to open " + path_ + ": " + scopedLastError();
            scopedClose(savedFd_);
            savedFd_ = -1;
            return;
        }
        if (scopedDup2(replacementFd, targetFd_) < 0) {
            error_ = "failed to redirect descriptor to " + path_ + ": " + scopedLastError();
            scopedClose(replacementFd);
            scopedClose(savedFd_);
            savedFd_ = -1;
            return;
        }
        scopedClose(replacementFd);
        active_ = true;
    }

    /// @brief Restore the original descriptor if still active.
    /// @details Performs best-effort restoration through @ref finish; callers
    ///          that need to report restore failures should call finish explicitly
    ///          before destruction.
    ~ScopedFdRedirect() {
        (void)finish();
    }

    /// @brief Disallow copying ownership of the saved descriptor.
    ScopedFdRedirect(const ScopedFdRedirect &) = delete;
    /// @brief Disallow copy assignment of descriptor restoration state.
    ScopedFdRedirect &operator=(const ScopedFdRedirect &) = delete;

    /// @brief Return true when redirection succeeded and has not reported restore errors.
    /// @return True when no construction, flush, or restoration error is recorded.
    [[nodiscard]] bool ok() const {
        return error_.empty();
    }

    /// @brief Human-readable error from construction or restoration.
    /// @return Reference to the retained first error, or an empty string.
    /// @note The reference remains valid only for this redirect object's lifetime.
    [[nodiscard]] const std::string &errorMessage() const {
        return error_;
    }

    /// @brief Flush redirected output when needed and restore the original descriptor.
    /// @details The method is idempotent. For stdout/stderr redirection, callers can
    ///          invoke it before returning so flush/restore failures become visible
    ///          diagnostics instead of destructor-only best effort.
    /// @return True when restore completed without error.
    bool finish() {
        if (!active_)
            return error_.empty();
        if (writeMode_ && error_.empty()) {
            FILE *stream = nullptr;
            if (targetFd_ == fileno(stdout))
                stream = stdout;
            else if (targetFd_ == fileno(stderr))
                stream = stderr;
            if (stream != nullptr && std::fflush(stream) != 0)
                error_ = "failed to flush redirected stream for " + path_;
        }
        if (savedFd_ >= 0) {
            if (scopedDup2(savedFd_, targetFd_) < 0 && error_.empty())
                error_ = "failed to restore descriptor after " + path_ + ": " + scopedLastError();
            scopedClose(savedFd_);
            savedFd_ = -1;
        }
        active_ = false;
        return error_.empty();
    }

  private:
    int targetFd_{-1};
    int savedFd_{-1};
    std::string path_;
    bool writeMode_{false};
    bool active_{false};
    std::string error_;
};

/// @brief Redirect stdin from a file for the lifetime of this object.
/// @details This thin wrapper supplies the standard input descriptor to
///          ScopedFdRedirect while preserving the same error-reporting API.
class ScopedStdinRedirect : public ScopedFdRedirect {
  public:
    /// @brief Open @p path for reading and make it the process stdin descriptor.
    /// @param path File whose bytes should replace standard input.
    explicit ScopedStdinRedirect(const std::string &path)
        : ScopedFdRedirect(fileno(stdin), path, false) {}
};

/// @brief Redirect stdout to a truncating file for the lifetime of this object.
/// @details Call finish() before emitting further terminal output so buffered data
///          is flushed and stdout is restored deterministically.
class ScopedStdoutRedirect : public ScopedFdRedirect {
  public:
    /// @brief Open @p path for writing and make it the process stdout descriptor.
    /// @param path File to create or truncate for standard output.
    explicit ScopedStdoutRedirect(const std::string &path)
        : ScopedFdRedirect(fileno(stdout), path, true) {}
};

/// @brief Temporarily set or clear a process environment variable.
/// @details The prior value is copied at construction. If the variable was absent,
///          destruction removes it again. Constructor and restore failures are
///          retained for callers that need to reject silent environment changes.
class ScopedEnvVar {
  public:
    /// @brief Set @p name to @p value, or unset it when @p value is null.
    /// @param name Environment variable name; null or empty records an error.
    /// @param value New value, or null to remove the variable temporarily.
    /// @details Copies the prior value before mutation so restoration does not
    ///          depend on the environment's storage remaining stable.
    ScopedEnvVar(const char *name, const char *value) : name_(name ? name : "") {
        if (name_.empty()) {
            error_ = "environment variable name must not be empty";
            return;
        }
        if (const char *current = std::getenv(name_.c_str()))
            oldValue_ = current;
        const int rc = value ? scopedSetEnv(name_.c_str(), value) : scopedUnsetEnv(name_.c_str());
        if (rc != 0)
            error_ = "failed to update environment variable " + name_ + ": " + scopedLastError();
    }

    /// @brief Restore the prior environment state.
    /// @details Calls @ref restore as a best-effort fallback. Callers that need
    ///          to surface restoration errors should restore explicitly.
    ~ScopedEnvVar() {
        (void)restore();
    }

    /// @brief Disallow copying ownership of environment restoration state.
    ScopedEnvVar(const ScopedEnvVar &) = delete;
    /// @brief Disallow copy assignment of environment restoration state.
    ScopedEnvVar &operator=(const ScopedEnvVar &) = delete;

    /// @brief Return true when construction and restoration have not failed.
    /// @return True when no update or restoration error is recorded.
    [[nodiscard]] bool ok() const {
        return error_.empty();
    }

    /// @brief Return the first environment update failure, if any.
    /// @return Reference to retained error text, or an empty string.
    /// @note The reference remains valid only for this object's lifetime.
    [[nodiscard]] const std::string &errorMessage() const {
        return error_;
    }

    /// @brief Restore the environment immediately.
    /// @return True when the environment was restored or no change was active.
    /// @details Idempotently reinstates the copied value or removes a variable
    ///          that was absent at construction. The first failure is retained.
    bool restore() {
        if (restored_)
            return error_.empty();
        restored_ = true;
        if (name_.empty())
            return false;
        const int rc = oldValue_ ? scopedSetEnv(name_.c_str(), oldValue_->c_str())
                                 : scopedUnsetEnv(name_.c_str());
        if (rc != 0 && error_.empty())
            error_ = "failed to restore environment variable " + name_ + ": " + scopedLastError();
        return error_.empty();
    }

  private:
    std::string name_;
    std::optional<std::string> oldValue_;
    bool restored_{false};
    std::string error_;
};

} // namespace zanna::tools
