//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgUtils.hpp
// Purpose: Shared utility functions for all platform package builders —
//          file reading, name normalization, and common helpers.
//
// Key invariants:
//   - readFile() throws std::runtime_error on I/O failure.
//   - Name normalizers produce lowercase ASCII with no spaces.
//   - Reproducible-build epochs are nonnegative signed 64-bit values.
//
// Ownership/Lifetime:
//   - Pure functions, no state.
//
// Links: MacOSPackageBuilder.cpp, LinuxPackageBuilder.cpp,
//        WindowsPackageBuilder.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines shared validation, filesystem, encoding, and traversal utilities for packagers.
/// @details All helpers are inline and cross-platform. They enforce package size,
///          metadata, path-containment, atomic-output, and UTF conversion contracts.

#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../../../common/Filesystem.hpp"
#include "../../../common/PlatformCapabilities.hpp"
#include "PackageConfig.hpp"

#if ZANNA_HOST_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zanna::pkg {

/// @brief Largest individual input representable by the package formats' 32-bit size fields.
inline constexpr uint64_t kMaxPackageFileBytes = 0xFFFFFFFFull;

/// @brief Parse a reproducible-build Unix epoch without accepting partial input.
/// @param text Decimal SOURCE_DATE_EPOCH text.
/// @param fieldName Human-readable field name used in diagnostics.
/// @return Parsed nonnegative epoch in the signed 64-bit range.
/// @throws std::runtime_error if @p text is empty, malformed, or out of range.
inline uint64_t parseSourceDateEpoch(std::string_view text,
                                     const char *fieldName = "SOURCE_DATE_EPOCH") {
    if (text.empty())
        throw std::runtime_error(std::string(fieldName) + " must be a non-negative integer");
    uint64_t value = 0;
    constexpr uint64_t kMaxEpoch = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    for (char c : text) {
        if (c < '0' || c > '9')
            throw std::runtime_error(std::string(fieldName) + " must be a non-negative integer");
        const uint64_t digit = static_cast<uint64_t>(c - '0');
        if (value > (kMaxEpoch - digit) / UINT64_C(10))
            throw std::runtime_error(std::string(fieldName) + " is out of range");
        value = value * UINT64_C(10) + digit;
    }
    return value;
}

/// @brief Add @p value to @p total, throwing if the result would overflow size_t.
/// @details Package writers commonly pre-compute buffer capacities before serializing archives.
///          This helper keeps those estimates from wrapping on pathological inputs, which would
///          otherwise turn a size validation failure into an under-sized allocation.
/// @param total Accumulator to update in place.
/// @param value Amount to add.
/// @param context Human-readable component name used in diagnostics.
inline void checkedAddSize(size_t &total, size_t value, const char *context) {
    if (value > std::numeric_limits<size_t>::max() - total)
        throw std::runtime_error(std::string(context) + ": size overflow");
    total += value;
}

/// @brief Add @p value to @p total, throwing if the result would overflow uint64_t.
/// @details Used for installed-size accounting where the source byte counts may be
///          platform-sized but the emitted package metadata is computed in 64-bit space.
/// @param total Accumulator to update in place.
/// @param value Amount to add.
/// @param context Human-readable component name used in diagnostics.
inline void checkedAddU64(uint64_t &total, uint64_t value, const char *context) {
    if (value > std::numeric_limits<uint64_t>::max() - total)
        throw std::runtime_error(std::string(context) + ": size overflow");
    total += value;
}

/// @brief Return @p bytes rounded up to KiB, throwing when adding the rounding bias would overflow.
/// @details Debian and macOS metadata store installed size in KiB. This helper keeps
///          "(bytes + 1023) / 1024" from wrapping for maximal byte totals.
/// @param bytes Byte count to round.
/// @param context Human-readable component name used in diagnostics.
/// @return KiB count rounded toward positive infinity.
inline uint64_t roundedKiB(uint64_t bytes, const char *context) {
    if (bytes > std::numeric_limits<uint64_t>::max() - 1023u)
        throw std::runtime_error(std::string(context) + ": installed size overflow");
    return (bytes + 1023u) / 1024u;
}

/// @brief Build a best-effort unique suffix for temporary files and directories.
/// @details Combines a steady-clock tick, a random_device sample, and an attempt counter.
///          Callers must still create the path exclusively and retry on collisions.
/// @param attempt Retry counter included in the suffix.
/// @return A filesystem-friendly ASCII suffix.
inline std::string uniqueTempSuffix(unsigned attempt) {
    const auto tick = static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::random_device rd;
    const unsigned random = rd();
    return std::to_string(tick) + "-" + std::to_string(random) + "-" + std::to_string(attempt);
}

/// @brief Create a new unique directory under @p parent using an exclusive create_directory loop.
/// @details The returned directory already exists. The function never removes a pre-existing path;
///          it retries with a different randomized suffix until creation succeeds or the attempt
///          budget is exhausted.
/// @param parent Directory in which to create the temporary child.
/// @param stem Prefix for the generated child name.
/// @return Path to the newly-created directory.
/// @throws std::runtime_error when @p parent cannot be created or no unique child can be made.
inline std::filesystem::path createUniqueTempDirectory(const std::filesystem::path &parent,
                                                       std::string_view stem) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec)
        throw std::runtime_error("cannot create temp directory parent '" +
                                 zanna::filesystem::pathToUtf8(parent) + "': " + ec.message());
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const fs::path candidate = parent / (std::string(stem) + "-" + uniqueTempSuffix(attempt));
        ec.clear();
        if (fs::create_directory(candidate, ec))
            return candidate;
        if (ec && ec != std::errc::file_exists) {
            throw std::runtime_error("cannot create temp directory '" +
                                     zanna::filesystem::pathToUtf8(candidate) +
                                     "': " + ec.message());
        }
    }
    throw std::runtime_error("cannot create a unique temp directory under " +
                             zanna::filesystem::pathToUtf8(parent));
}

namespace detail {

#if ZANNA_HOST_WINDOWS
/// @brief Exclusively create a non-inheritable binary temporary file on Windows.
/// @param path Native temporary path.
/// @return Writable CRT file descriptor, or a negative value on failure.
inline int openExclusiveTempFile(const std::filesystem::path &path) {
    return _wopen(path.native().c_str(),
                  _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                  _S_IREAD | _S_IWRITE);
}

/// @brief Write an entire byte sequence to a Windows CRT descriptor.
/// @param fd Writable file descriptor.
/// @param data Source bytes.
/// @param size Number of bytes to write.
/// @return Zero on success or -1 on a short/failed write.
inline int writeTempFileBytes(int fd, const uint8_t *data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const unsigned chunk = static_cast<unsigned>(std::min<size_t>(size - written, 1u << 20));
        const int n = _write(fd, data + written, chunk);
        if (n <= 0)
            return -1;
        written += static_cast<size_t>(n);
    }
    return 0;
}

/// @brief Flush a Windows temporary file to stable storage.
/// @param fd Open file descriptor.
/// @return CRT commit result.
inline int syncTempFile(int fd) {
    return _commit(fd);
}

/// @brief Close a Windows temporary file descriptor.
/// @param fd Open file descriptor.
/// @return CRT close result.
inline int closeTempFile(int fd) {
    return _close(fd);
}

/// @brief Describe the most recent CRT I/O error.
/// @return Human-readable errno text.
inline std::string lastIoError() {
    return std::strerror(errno);
}

/// @brief Atomically replace @p finalPath with @p tempPath on Windows.
/// @details Uses MoveFileExW with MOVEFILE_REPLACE_EXISTING so overwriting an
///          existing destination behaves consistently with POSIX rename(2).
///          MOVEFILE_WRITE_THROUGH asks Windows to flush metadata before the
///          operation returns.
/// @param tempPath Same-directory temporary file containing the final bytes.
/// @param finalPath Destination path to create or replace.
/// @param ec Receives the Windows system error when replacement fails.
inline void replaceFileAtomic(const std::filesystem::path &tempPath,
                              const std::filesystem::path &finalPath,
                              std::error_code &ec) {
    if (MoveFileExW(tempPath.native().c_str(),
                    finalPath.native().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ec.clear();
        return;
    }
    ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
}

#else
/// @brief Exclusively create a temporary file on a POSIX-like host.
/// @param path Native temporary path.
/// @return Writable file descriptor, or a negative value on failure.
inline int openExclusiveTempFile(const std::filesystem::path &path) {
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    return open(path.c_str(), flags, 0666);
}

/// @brief Write an entire byte sequence, retrying interrupted POSIX writes.
/// @param fd Writable file descriptor.
/// @param data Source bytes.
/// @param size Number of bytes to write.
/// @return Zero on success or -1 on a short/failed write.
inline int writeTempFileBytes(int fd, const uint8_t *data, size_t size) {
    size_t written = 0;
    while (written < size) {
        const ssize_t n = write(fd, data + written, size - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        written += static_cast<size_t>(n);
    }
    return 0;
}

/// @brief Flush a POSIX temporary file to stable storage.
/// @param fd Open file descriptor.
/// @return fsync result.
inline int syncTempFile(int fd) {
    return fsync(fd);
}

/// @brief Close a POSIX temporary file descriptor.
/// @param fd Open file descriptor.
/// @return close result.
inline int closeTempFile(int fd) {
    return close(fd);
}

/// @brief Best-effort flush of directory metadata after an atomic rename.
/// @param parent Parent directory containing the renamed output.
inline void syncParentDirectoryBestEffort(const std::filesystem::path &parent) {
    const int fd = open(parent.c_str(), O_RDONLY);
    if (fd < 0)
        return;
    (void)fsync(fd);
    (void)close(fd);
}

/// @brief Describe the most recent POSIX I/O error.
/// @return Human-readable errno text.
inline std::string lastIoError() {
    return std::strerror(errno);
}

/// @brief Atomically replace @p finalPath with @p tempPath on POSIX-like hosts.
/// @details std::filesystem::rename maps to rename(2) for normal files on these
///          hosts, which atomically replaces an existing same-filesystem target.
/// @param tempPath Same-directory temporary file containing the final bytes.
/// @param finalPath Destination path to create or replace.
/// @param ec Receives the filesystem error when replacement fails.
inline void replaceFileAtomic(const std::filesystem::path &tempPath,
                              const std::filesystem::path &finalPath,
                              std::error_code &ec) {
    std::filesystem::rename(tempPath, finalPath, ec);
}
#endif

/// @brief Same-directory temporary file reserved with O_EXCL.
/// @details The descriptor is closed and the path is removed on destruction unless
///          release() is called after the final rename succeeds.
class ExclusiveTempFile {
  public:
    /// @brief Reserve a unique hidden temp file next to @p finalPath.
    /// @param finalPath Destination whose parent and leaf name seed the temporary path.
    /// @throws std::runtime_error If exclusive creation fails or collisions exhaust retries.
    explicit ExclusiveTempFile(const std::filesystem::path &finalPath) {
        namespace fs = std::filesystem;
        const fs::path parent =
            finalPath.parent_path().empty() ? fs::current_path() : finalPath.parent_path();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            fs::path temporaryName{"."};
            temporaryName += finalPath.filename().native();
            temporaryName +=
                zanna::filesystem::pathFromUtf8(".tmp-" + uniqueTempSuffix(attempt)).native();
            path_ = parent / temporaryName;
            fd_ = openExclusiveTempFile(path_);
            if (fd_ >= 0)
                return;
            if (errno != EEXIST) {
                throw std::runtime_error("cannot create temporary output file '" +
                                         zanna::filesystem::pathToUtf8(path_) +
                                         "': " + lastIoError());
            }
        }
        throw std::runtime_error("cannot find a unique temporary output path for " +
                                 zanna::filesystem::pathToUtf8(finalPath));
    }

    /// @brief Close and remove the temp file unless ownership was released.
    ~ExclusiveTempFile() {
        if (fd_ >= 0)
            (void)closeTempFile(fd_);
        if (!released_ && !path_.empty()) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }

    ExclusiveTempFile(const ExclusiveTempFile &) = delete;
    ExclusiveTempFile &operator=(const ExclusiveTempFile &) = delete;

    /// @brief Return the reserved path.
    /// @return Const reference valid for the guard's lifetime.
    [[nodiscard]] const std::filesystem::path &path() const {
        return path_;
    }

    /// @brief Return the writable file descriptor.
    /// @return Open descriptor, or -1 after flushAndClose().
    [[nodiscard]] int fd() const {
        return fd_;
    }

    /// @brief Close the descriptor after forcing file contents to stable storage.
    /// @throws std::runtime_error If flushing or closing fails.
    void flushAndClose() {
        if (fd_ < 0)
            return;
        if (syncTempFile(fd_) != 0)
            throw std::runtime_error("failed to flush temporary output file '" +
                                     zanna::filesystem::pathToUtf8(path_) + "': " + lastIoError());
        if (closeTempFile(fd_) != 0) {
            fd_ = -1;
            throw std::runtime_error("failed to close temporary output file '" +
                                     zanna::filesystem::pathToUtf8(path_) + "': " + lastIoError());
        }
        fd_ = -1;
    }

    /// @brief Prevent destructor cleanup after the temp file has been renamed.
    void release() {
        released_ = true;
    }

  private:
    std::filesystem::path path_;
    int fd_{-1};
    bool released_{false};
};

} // namespace detail

/// @brief Write bytes to @p path via an exclusive same-directory temporary file and rename.
/// @details Parent directories are created as needed. The temporary path is opened
///          with exclusive-create semantics to avoid TOCTOU races, data is flushed
///          before rename, and the destination is never removed as a fallback. On
///          POSIX, the parent directory is fsync'd best-effort after rename.
/// @param path Final output path.
/// @param data Bytes to write.
/// @throws std::runtime_error on directory creation, file open, write, close, or rename failure.
inline void writeFileAtomic(const std::filesystem::path &path, const std::vector<uint8_t> &data) {
    namespace fs = std::filesystem;
    const fs::path parent = path.parent_path().empty() ? fs::current_path() : path.parent_path();
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec)
        throw std::runtime_error("cannot create output directory '" +
                                 zanna::filesystem::pathToUtf8(parent) + "': " + ec.message());

    detail::ExclusiveTempFile temp(path);
    if (!data.empty() && detail::writeTempFileBytes(temp.fd(), data.data(), data.size()) != 0)
        throw std::runtime_error("failed to write temporary output file '" +
                                 zanna::filesystem::pathToUtf8(temp.path()) +
                                 "': " + detail::lastIoError());
    temp.flushAndClose();

    detail::replaceFileAtomic(temp.path(), path, ec);
    if (ec) {
        throw std::runtime_error("cannot move temporary output into place at '" +
                                 zanna::filesystem::pathToUtf8(path) + "': " + ec.message());
    }
#if !ZANNA_HOST_WINDOWS
    detail::syncParentDirectoryBestEffort(parent);
#endif
    temp.release();
}

/// @brief Decode a UTF-8 package output path and write it without using the Windows ACP.
/// @param path UTF-8 destination path.
/// @param data Bytes to write atomically.
/// @throws std::runtime_error If path decoding or atomic output fails.
inline void writeFileAtomic(const std::string &path, const std::vector<uint8_t> &data) {
    writeFileAtomic(zanna::filesystem::pathFromUtf8(path), data);
}

/// @brief Write UTF-8/text content atomically using the package byte writer.
/// @details Converts @p text to bytes without adding or removing newlines, then
///          delegates to writeFileAtomic() so text outputs get the same exclusive
///          temp-file and rename behavior as binary package artifacts.
/// @param path Final output path.
/// @param text Complete file contents.
/// @throws std::runtime_error on directory creation, write, close, or rename failure.
inline void writeTextFileAtomic(const std::filesystem::path &path, std::string_view text) {
    const auto *begin = reinterpret_cast<const uint8_t *>(text.data());
    std::vector<uint8_t> bytes;
    if (!text.empty())
        bytes.assign(begin, begin + text.size());
    writeFileAtomic(path, bytes);
}

/// @brief Decode a UTF-8 package text-output path and write it atomically.
/// @param path UTF-8 destination path.
/// @param text Exact text bytes to write.
/// @throws std::runtime_error If path decoding or atomic output fails.
inline void writeTextFileAtomic(const std::string &path, std::string_view text) {
    writeTextFileAtomic(zanna::filesystem::pathFromUtf8(path), text);
}

/// @brief Read a native filesystem path into a byte vector.
/// @param path Input file path.
/// @return Complete owned file contents.
/// @throws std::runtime_error on open or read failure.
inline std::vector<uint8_t> readFile(const std::filesystem::path &path) {
    const std::string displayPath = zanna::filesystem::pathToUtf8(path);
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("cannot read file: " + displayPath);
    auto size = f.tellg();
    if (size < 0)
        throw std::runtime_error("cannot determine file size: " + displayPath);
    const auto size64 = static_cast<uint64_t>(size);
    if (size64 > kMaxPackageFileBytes)
        throw std::runtime_error("file is too large for ZAPS package formats: " + displayPath);
    if (size64 > static_cast<uint64_t>(std::vector<uint8_t>().max_size()))
        throw std::runtime_error("file is too large to read into memory: " + displayPath);
    if (size64 > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
        throw std::runtime_error("file is too large for a single stream read: " + displayPath);
    f.seekg(0);
    if (!f)
        throw std::runtime_error("cannot seek file: " + displayPath);
    std::vector<uint8_t> data(static_cast<size_t>(size64));
    if (data.empty())
        return data;
    f.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f || f.gcount() != static_cast<std::streamsize>(data.size()))
        throw std::runtime_error("incomplete read of: " + displayPath);
    return data;
}

/// @brief Decode a UTF-8 package path and read it without using the Windows ACP.
/// @param path UTF-8 input path.
/// @return Complete owned file contents.
/// @throws std::runtime_error If path decoding or file reading fails.
inline std::vector<uint8_t> readFile(const std::string &path) {
    return readFile(zanna::filesystem::pathFromUtf8(path));
}

/// @brief Normalize a project name to a lowercase executable name.
///
/// Spaces become underscores, all chars lowered.
/// e.g. "Zanna IDE" -> "zanna_ide"
/// @param name Project name to normalize.
/// @return Lowercase executable leaf name.
/// @throws std::runtime_error If the name is empty, special, or contains unsupported characters.
inline std::string normalizeExecName(const std::string &name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == ' ')
            result.push_back('_');
        else if (std::isalnum(uc) || c == '_' || c == '-' || c == '.')
            result.push_back(static_cast<char>(std::tolower(uc)));
        else
            throw std::runtime_error("executable name contains an invalid character: '" + name +
                                     "'");
    }
    if (result.empty() || result == "." || result == "..")
        throw std::runtime_error("executable name must not be empty or special: '" + name + "'");
    return result;
}

/// @brief Normalize a project name to a Debian-style package name.
///
/// Spaces and underscores become hyphens, all chars lowered.
/// e.g. "Zanna IDE" -> "zanna-ide"
/// @param name Project name to normalize.
/// @return Debian-compatible lowercase package name.
/// @throws std::runtime_error If the result violates Debian name syntax.
inline std::string normalizeDebName(const std::string &name) {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (c == ' ' || c == '_')
            result.push_back('-');
        else if (std::isalnum(uc) || c == '+' || c == '-' || c == '.')
            result.push_back(static_cast<char>(std::tolower(uc)));
        else
            throw std::runtime_error("Debian package name contains an invalid character: '" + name +
                                     "'");
    }
    if (result.size() < 2 || !std::isalnum(static_cast<unsigned char>(result.front())))
        throw std::runtime_error("Debian package name must start with an alphanumeric character "
                                 "and contain at least two characters: '" +
                                 name + "'");
    if (result.back() == '-' || result.back() == '.')
        throw std::runtime_error("Debian package name must not end with '-' or '.': '" + name +
                                 "'");
    return result;
}

/// @brief Strip leading and trailing ASCII whitespace (space, tab, CR, LF).
/// @param value Text to trim.
/// @return Trimmed copy, or an empty string when no non-whitespace remains.
inline std::string trimAsciiWhitespace(std::string_view value) {
    const std::size_t start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
        return {};
    const std::size_t end = value.find_last_not_of(" \t\r\n");
    return std::string(value.substr(start, end - start + 1));
}

/// @brief Return a copy of value with all ASCII letters converted to lowercase.
/// @param value Text to normalize.
/// @return Lowercase copy.
inline std::string lowerAsciiCopy(std::string value) {
    /// @brief Fold one byte through ctype without signed-char undefined behavior.
    /// @param c Unsigned source byte.
    /// @return Lowercase byte converted back to plain char.
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

/// @brief Throw if value contains any ASCII control character (0x00-0x1F or 0x7F).
/// Used to keep metadata strings safe for embedding in package control files.
/// @param value Metadata text to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If a control character is present.
inline void rejectControlChars(const std::string &value, const char *fieldName) {
    for (char c : value) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F) {
            throw std::runtime_error(std::string(fieldName) +
                                     " must not contain control characters");
        }
    }
}

/// @brief Throw if value contains CR or LF characters.
/// Single-line metadata fields (Name, Version, etc.) must not span multiple lines.
/// @param value Metadata text to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If a line break is present.
inline void rejectLineBreaks(const std::string &value, const char *fieldName) {
    if (value.find('\n') != std::string::npos || value.find('\r') != std::string::npos) {
        throw std::runtime_error(std::string(fieldName) + " must not contain line breaks");
    }
}

/// @brief Validate a string that must be a single, control-character-free line.
/// Combines rejectLineBreaks + rejectControlChars in one call.
/// @param value Metadata text to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If a line break or control character is present.
inline void validateSingleLineField(const std::string &value, const char *fieldName) {
    rejectLineBreaks(value, fieldName);
    rejectControlChars(value, fieldName);
}

/// @brief Validate a portable archive version string.
/// @details Portable tarball names do not need Debian package-version syntax. This
///          accepts a non-empty single-line string made of alphanumerics plus '.', '_',
///          '+', '~', and '-', while rejecting special "."/".." path components.
/// @param version Version text from project metadata.
/// @param fieldName Name used in diagnostics.
inline void validatePortableArchiveVersion(const std::string &version,
                                           const char *fieldName = "package version") {
    if (version.empty())
        throw std::runtime_error(std::string(fieldName) + " must not be empty");
    validateSingleLineField(version, fieldName);
    if (version == "." || version == "..")
        throw std::runtime_error(std::string(fieldName) + " must not be a special path segment");
    for (char c : version) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '.' && c != '_' && c != '+' && c != '~' && c != '-') {
            throw std::runtime_error(std::string(fieldName) +
                                     " contains a character invalid for portable archives: '" +
                                     version + "'");
        }
    }
}

/// @brief Validate that arch is one of the two supported Zanna target architectures ("x64",
/// "arm64").
/// @param arch Architecture text to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If the value is not `x64` or `arm64`.
inline void validateToolchainArchitecture(const std::string &arch,
                                          const char *fieldName = "toolchain architecture") {
    validateSingleLineField(arch, fieldName);
    if (arch != "x64" && arch != "arm64")
        throw std::runtime_error(std::string(fieldName) + " must be x64 or arm64: '" + arch + "'");
}

/// @brief Validate that platform is one of "windows", "macos", or "linux".
/// @param platform Platform text to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If the value is unsupported.
inline void validateToolchainPlatform(const std::string &platform,
                                      const char *fieldName = "toolchain platform") {
    validateSingleLineField(platform, fieldName);
    if (platform != "windows" && platform != "macos" && platform != "linux") {
        throw std::runtime_error(std::string(fieldName) + " must be windows, macos, or linux: '" +
                                 platform + "'");
    }
}

/// @brief Validate a Debian package version string per Policy Manual §5.6.12.
/// Accepts the full [epoch:]upstream[-revision] format. Throws on any invalid
/// character, empty component, non-numeric epoch, or wrong starting digit.
/// @param version Version text to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If Debian version syntax is violated.
inline void validateDebVersion(const std::string &version, const char *fieldName = "version") {
    /// @brief Throw a contextual Debian-version validation error.
    /// @param why Specific grammar failure.
    /// @throws std::runtime_error Always.
    const auto fail = [&](const std::string &why) {
        throw std::runtime_error(std::string(fieldName) + " is not a valid Debian version: '" +
                                 version + "' (" + why + ")");
    };
    if (version.empty())
        fail("empty");

    int colonCount = 0;
    std::size_t colon = std::string::npos;
    for (std::size_t i = 0; i < version.size(); ++i) {
        const char c = version[i];
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '.' && c != '+' && c != '~' && c != '-' && c != ':') {
            fail("invalid character");
        }
        if (c == ':') {
            ++colonCount;
            colon = i;
        }
    }
    if (colonCount > 1)
        fail("multiple epochs");
    std::size_t upstreamStart = 0;
    if (colon != std::string::npos) {
        if (colon == 0)
            fail("empty epoch");
        for (std::size_t i = 0; i < colon; ++i) {
            if (!std::isdigit(static_cast<unsigned char>(version[i])))
                fail("epoch must be numeric");
        }
        upstreamStart = colon + 1;
    }
    if (upstreamStart >= version.size())
        fail("empty upstream version");
    const std::size_t dash = version.find('-', upstreamStart);
    const std::size_t upstreamEnd = dash == std::string::npos ? version.size() : dash;
    if (upstreamEnd == upstreamStart)
        fail("empty upstream version");
    if (!std::isdigit(static_cast<unsigned char>(version[upstreamStart])))
        fail("upstream version must start with a digit");
    for (std::size_t i = upstreamStart; i < upstreamEnd; ++i) {
        const char c = version[i];
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '.' && c != '+' && c != '~' && c != ':')
            fail("invalid upstream character");
    }
    if (dash != std::string::npos) {
        if (dash + 1 >= version.size())
            fail("empty Debian revision");
        for (std::size_t i = dash + 1; i < version.size(); ++i) {
            const char c = version[i];
            unsigned char uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '.' && c != '+' && c != '~')
                fail("invalid Debian revision character");
        }
    }
}

/// @brief Parse a dotted-numeric version string ("1.2.3.4") into a uint32_t vector.
/// Each component is validated to be purely numeric and ≤ 65535.
/// Throws on empty input, non-numeric characters, or a trailing dot.
/// @param version Version text to parse.
/// @param fieldName Human-readable field name used in diagnostics.
/// @return Ordered numeric components.
/// @throws std::runtime_error If syntax is invalid or a component exceeds 65535.
inline std::vector<uint32_t> parseDottedNumericVersionParts(const std::string &version,
                                                            const char *fieldName) {
    if (version.empty())
        throw std::runtime_error(std::string(fieldName) + " must not be empty");
    std::vector<uint32_t> parts;
    uint32_t current = 0;
    bool sawDigit = false;
    for (char c : version) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isdigit(uc)) {
            sawDigit = true;
            const uint32_t digit = static_cast<uint32_t>(c - '0');
            if (current > (65535u - digit) / 10u)
                throw std::runtime_error(std::string(fieldName) + " component exceeds 65535: '" +
                                         version + "'");
            current = current * 10u + digit;
            continue;
        }
        if (c == '.' && sawDigit) {
            parts.push_back(current);
            current = 0;
            sawDigit = false;
            continue;
        }
        throw std::runtime_error(std::string(fieldName) + " must be a dotted numeric version: '" +
                                 version + "'");
    }
    if (!sawDigit)
        throw std::runtime_error(std::string(fieldName) + " must be a dotted numeric version: '" +
                                 version + "'");
    parts.push_back(current);
    return parts;
}

/// @brief Validate a dotted-numeric version string with 2-4 components (e.g. "1.0" or "1.2.3.4").
/// @param version Version text to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If parsing fails or the component count is outside 2–4.
inline void validateDottedNumericVersion(const std::string &version, const char *fieldName) {
    const auto parts = parseDottedNumericVersionParts(version, fieldName);
    if (parts.size() < 2 || parts.size() > 4)
        throw std::runtime_error(std::string(fieldName) +
                                 " must have 2 to 4 numeric components: '" + version + "'");
}

/// @brief Validate a Debian dependency specification per Policy Manual §7.1.
/// Accepts the full pkgname[:arch] [(op version)] [arch-list] [profile-list] | alt ...
/// syntax. Throws with a descriptive error on any violation.
/// @param dependency Dependency expression to validate.
/// @throws std::runtime_error If the expression is empty, unsafe, or syntactically invalid.
inline void validateDebDependency(const std::string &dependency) {
    const std::string dep = trimAsciiWhitespace(dependency);
    if (dep.empty())
        throw std::runtime_error("package dependency must not be empty");
    validateSingleLineField(dep, "package dependency");

    /// @brief Test one byte for Debian package-name membership.
    /// @param c Candidate byte.
    /// @return `true` for lowercase alphanumeric or `+.-`.
    auto isPkgChar = [](char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        return std::islower(uc) || std::isdigit(uc) || c == '+' || c == '-' || c == '.';
    };
    /// @brief Test one byte for Debian architecture-name membership.
    /// @param c Candidate byte.
    /// @return `true` for lowercase alphanumeric or hyphen.
    auto isArchChar = [](char c) {
        unsigned char uc = static_cast<unsigned char>(c);
        return std::islower(uc) || std::isdigit(uc) || c == '-';
    };
    /// @brief Advance a parser position past horizontal ASCII whitespace.
    /// @param[in,out] pos Current dependency offset.
    auto skipSpaces = [&](size_t &pos) {
        while (pos < dep.size() && (dep[pos] == ' ' || dep[pos] == '\t'))
            ++pos;
    };
    /// @brief Parse and validate one Debian package name.
    /// @param[in,out] pos Current dependency offset, advanced past the name.
    /// @throws std::runtime_error On invalid name syntax.
    auto parseName = [&](size_t &pos) {
        const size_t start = pos;
        if (pos >= dep.size() || (!std::islower(static_cast<unsigned char>(dep[pos])) &&
                                  !std::isdigit(static_cast<unsigned char>(dep[pos]))))
            throw std::runtime_error("package dependency has invalid package name: '" + dep + "'");
        while (pos < dep.size() && isPkgChar(dep[pos]))
            ++pos;
        if (pos - start < 2)
            throw std::runtime_error("package dependency package name is too short: '" + dep + "'");
        if (dep[pos - 1] == '-' || dep[pos - 1] == '.')
            throw std::runtime_error("package dependency package name has invalid suffix: '" + dep +
                                     "'");
    };
    /// @brief Parse an optional Debian architecture qualifier.
    /// @param[in,out] pos Current dependency offset, advanced when a qualifier exists.
    /// @throws std::runtime_error On an empty qualifier.
    auto parseArchQualifier = [&](size_t &pos) {
        if (pos >= dep.size() || dep[pos] != ':')
            return;
        ++pos;
        const size_t start = pos;
        while (pos < dep.size() && isArchChar(dep[pos]))
            ++pos;
        if (start == pos)
            throw std::runtime_error("package dependency has empty architecture qualifier: '" +
                                     dep + "'");
    };
    /// @brief Parse an optional parenthesized Debian version relation.
    /// @param[in,out] pos Current dependency offset, advanced past the constraint.
    /// @throws std::runtime_error On invalid or unterminated syntax.
    auto parseVersionConstraint = [&](size_t &pos) {
        skipSpaces(pos);
        if (pos >= dep.size() || dep[pos] != '(')
            return;
        ++pos;
        skipSpaces(pos);
        const char *ops[] = {"<<", "<=", "=", ">=", ">>"};
        bool matched = false;
        for (const char *op : ops) {
            const size_t len = std::char_traits<char>::length(op);
            if (dep.compare(pos, len, op) == 0) {
                pos += len;
                matched = true;
                break;
            }
        }
        if (!matched)
            throw std::runtime_error("package dependency has invalid version relation: '" + dep +
                                     "'");
        skipSpaces(pos);
        const size_t versionStart = pos;
        while (pos < dep.size() && dep[pos] != ')') {
            unsigned char uc = static_cast<unsigned char>(dep[pos]);
            if (!std::isalnum(uc) && dep[pos] != '.' && dep[pos] != '+' && dep[pos] != '~' &&
                dep[pos] != '-' && dep[pos] != ':')
                throw std::runtime_error("package dependency has invalid version: '" + dep + "'");
            ++pos;
        }
        if (versionStart == pos || pos >= dep.size() || dep[pos] != ')')
            throw std::runtime_error("package dependency has unterminated version constraint: '" +
                                     dep + "'");
        ++pos;
    };
    /// @brief Parse an optional bracketed architecture or profile restriction list.
    /// @param[in,out] pos Current dependency offset, advanced past the list.
    /// @param open Opening delimiter.
    /// @param close Closing delimiter.
    /// @param what Human-readable list kind for diagnostics.
    /// @throws std::runtime_error On invalid or unterminated syntax.
    auto parseBracketList = [&](size_t &pos, char open, char close, const char *what) {
        skipSpaces(pos);
        if (pos >= dep.size() || dep[pos] != open)
            return;
        ++pos;
        bool sawToken = false;
        while (pos < dep.size() && dep[pos] != close) {
            unsigned char uc = static_cast<unsigned char>(dep[pos]);
            if (std::islower(uc) || std::isdigit(uc) || dep[pos] == '-' || dep[pos] == '!' ||
                dep[pos] == ' ' || dep[pos] == '\t') {
                if (std::islower(uc) || std::isdigit(uc))
                    sawToken = true;
                ++pos;
                continue;
            }
            throw std::runtime_error(std::string("package dependency has invalid ") + what + ": '" +
                                     dep + "'");
        }
        if (!sawToken || pos >= dep.size() || dep[pos] != close)
            throw std::runtime_error(std::string("package dependency has unterminated ") + what +
                                     ": '" + dep + "'");
        ++pos;
    };

    size_t pos = 0;
    bool expectAlternative = true;
    while (pos < dep.size()) {
        skipSpaces(pos);
        if (pos >= dep.size())
            break;
        if (!expectAlternative && dep[pos] == '|') {
            expectAlternative = true;
            ++pos;
            continue;
        }
        if (!expectAlternative)
            throw std::runtime_error("package dependency alternatives must be separated by '|': '" +
                                     dep + "'");
        parseName(pos);
        parseArchQualifier(pos);
        parseVersionConstraint(pos);
        parseBracketList(pos, '[', ']', "architecture restriction");
        parseBracketList(pos, '<', '>', "build profile restriction");
        expectAlternative = false;
        skipSpaces(pos);
        if (pos < dep.size() && dep[pos] != '|')
            throw std::runtime_error("package dependency contains trailing invalid syntax: '" +
                                     dep + "'");
    }
    if (expectAlternative)
        throw std::runtime_error("package dependency has empty alternative: '" + dep + "'");
}

/// @brief Validate a single RPM `Requires:` dependency expression.
/// @details RPM dependency lines are emitted directly into the generated `.spec`
///          file, so this accepts a conservative single-line expression suitable
///          for common package names plus version relations while rejecting
///          commas (the manifest separator) and `%` macro expansion. Full RPM
///          dependency grammar is intentionally not reimplemented here; rpmbuild
///          remains the final arbiter after this injection-safety pass.
/// @param dependency One dependency term from `package-rpm-depends`.
/// @throws std::runtime_error when the dependency is empty or unsafe for a spec.
inline void validateRpmDependency(const std::string &dependency) {
    const std::string dep = trimAsciiWhitespace(dependency);
    if (dep.empty())
        throw std::runtime_error("RPM dependency must not be empty");
    validateSingleLineField(dep, "RPM dependency");
    for (char c : dep) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (c == ',' || c == '%') {
            throw std::runtime_error("RPM dependency contains an unsupported character: '" + dep +
                                     "'");
        }
        if (!std::isprint(uc)) {
            throw std::runtime_error("RPM dependency contains a non-printable character: '" + dep +
                                     "'");
        }
    }
}

/// @brief Return true if category is a registered freedesktop.org desktop category.
/// Used by normalizeDesktopCategories to reject unknown tokens before writing
/// the .desktop Categories= field.
/// @param category One un-delimited category token.
/// @return Whether the token appears in the registered category set.
inline bool isKnownDesktopCategory(std::string_view category) {
    static constexpr std::string_view known[] = {"AudioVideo",
                                                 "Audio",
                                                 "Video",
                                                 "Development",
                                                 "Education",
                                                 "Game",
                                                 "Graphics",
                                                 "Network",
                                                 "Office",
                                                 "Science",
                                                 "Settings",
                                                 "System",
                                                 "Utility",
                                                 "Building",
                                                 "Debugger",
                                                 "IDE",
                                                 "GUIDesigner",
                                                 "Profiling",
                                                 "RevisionControl",
                                                 "Translation",
                                                 "Calendar",
                                                 "ContactManagement",
                                                 "Database",
                                                 "Dictionary",
                                                 "Chart",
                                                 "Email",
                                                 "Finance",
                                                 "FlowChart",
                                                 "PDA",
                                                 "ProjectManagement",
                                                 "Presentation",
                                                 "Spreadsheet",
                                                 "WordProcessor",
                                                 "2DGraphics",
                                                 "VectorGraphics",
                                                 "RasterGraphics",
                                                 "3DGraphics",
                                                 "Scanning",
                                                 "OCR",
                                                 "Photography",
                                                 "Publishing",
                                                 "Viewer",
                                                 "TextTools",
                                                 "DesktopSettings",
                                                 "HardwareSettings",
                                                 "Printing",
                                                 "PackageManager",
                                                 "Dialup",
                                                 "InstantMessaging",
                                                 "Chat",
                                                 "IRCClient",
                                                 "Feed",
                                                 "FileTransfer",
                                                 "HamRadio",
                                                 "News",
                                                 "P2P",
                                                 "RemoteAccess",
                                                 "Telephony",
                                                 "TelephonyTools",
                                                 "VideoConference",
                                                 "WebBrowser",
                                                 "WebDevelopment",
                                                 "Midi",
                                                 "Mixer",
                                                 "Sequencer",
                                                 "Tuner",
                                                 "TV",
                                                 "AudioVideoEditing",
                                                 "Player",
                                                 "Recorder",
                                                 "DiscBurning",
                                                 "ActionGame",
                                                 "AdventureGame",
                                                 "ArcadeGame",
                                                 "BoardGame",
                                                 "BlocksGame",
                                                 "CardGame",
                                                 "KidsGame",
                                                 "LogicGame",
                                                 "RolePlaying",
                                                 "Shooter",
                                                 "Simulation",
                                                 "SportsGame",
                                                 "StrategyGame",
                                                 "Art",
                                                 "Construction",
                                                 "Music",
                                                 "Languages",
                                                 "ArtificialIntelligence",
                                                 "Astronomy",
                                                 "Biology",
                                                 "Chemistry",
                                                 "ComputerScience",
                                                 "DataVisualization",
                                                 "Economy",
                                                 "Electricity",
                                                 "Geography",
                                                 "Geology",
                                                 "Geoscience",
                                                 "History",
                                                 "Humanities",
                                                 "ImageProcessing",
                                                 "Literature",
                                                 "Maps",
                                                 "Math",
                                                 "NumericalAnalysis",
                                                 "MedicalSoftware",
                                                 "Physics",
                                                 "Robotics",
                                                 "Spirituality",
                                                 "Sports",
                                                 "ParallelComputing",
                                                 "Amusement",
                                                 "Archiving",
                                                 "Compression",
                                                 "Electronics",
                                                 "Emulator",
                                                 "Engineering",
                                                 "FileTools",
                                                 "FileManager",
                                                 "TerminalEmulator",
                                                 "Filesystem",
                                                 "Monitor",
                                                 "Security",
                                                 "Accessibility",
                                                 "Calculator",
                                                 "Clock",
                                                 "TextEditor",
                                                 "Documentation",
                                                 "Adult",
                                                 "Core",
                                                 "KDE",
                                                 "GNOME",
                                                 "XFCE",
                                                 "GTK",
                                                 "Qt",
                                                 "ConsoleOnly"};
    for (std::string_view item : known) {
        if (item == category)
            return true;
    }
    return false;
}

/// @brief Parse, validate, and normalize a semicolon-delimited freedesktop.org
/// Categories string. Each token is trimmed of whitespace, checked against the
/// known-categories list, and rejoined with semicolons. Returns "" for empty input.
/// @param categories Semicolon-delimited category text.
/// @return Normalized categories ending in semicolons, or an empty string.
/// @throws std::runtime_error If a token or character is invalid.
inline std::string normalizeDesktopCategories(const std::string &categories) {
    if (categories.empty())
        return {};
    validateSingleLineField(categories, "desktop categories");
    std::string out;
    size_t pos = 0;
    while (pos <= categories.size()) {
        const size_t next = categories.find(';', pos);
        std::string token =
            trimAsciiWhitespace(next == std::string::npos ? categories.substr(pos)
                                                          : categories.substr(pos, next - pos));
        if (!token.empty()) {
            for (char c : token) {
                unsigned char uc = static_cast<unsigned char>(c);
                if (!std::isalnum(uc) && c != '-' && c != '_' && c != '.')
                    throw std::runtime_error("desktop categories contain an invalid character: '" +
                                             categories + "'");
            }
            if (!isKnownDesktopCategory(token))
                throw std::runtime_error("unknown freedesktop desktop category: '" + token + "'");
            out += token;
            out.push_back(';');
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return out;
}

/// @brief Validate desktop categories without returning the normalized form.
/// @param categories Semicolon-delimited category text.
/// @throws std::runtime_error If normalization would fail.
inline void validateDesktopCategories(const std::string &categories) {
    (void)normalizeDesktopCategories(categories);
}

/// @brief Validate an RPM version string.
/// RPM versions may contain alphanumerics plus '.', '_', '+', '~', '^'.
/// No epoch/revision parsing — RPM spec file handles those separately.
/// @param version Version text to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If empty or containing unsupported characters.
inline void validateRpmVersion(const std::string &version, const char *fieldName = "version") {
    if (version.empty())
        throw std::runtime_error(std::string(fieldName) + " must not be empty");
    for (char c : version) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '.' && c != '_' && c != '+' && c != '~' && c != '^') {
            throw std::runtime_error(std::string(fieldName) +
                                     " contains a character invalid for RPM versions: '" + version +
                                     "'");
        }
    }
}

/// @brief Validate a reverse-DNS package identifier (e.g. "com.example.MyApp").
/// Rules: ≤255 chars total; each dot-separated component ≤63 chars, starts and
/// ends with alphanumeric, contains only alphanumerics and '-'; must have ≥1 dot.
/// An empty identifier is accepted (optional field).
/// @param identifier Optional reverse-DNS identifier.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If a non-empty value violates component rules.
inline void validatePackageIdentifier(const std::string &identifier,
                                      const char *fieldName = "package identifier") {
    if (identifier.empty())
        return;
    rejectLineBreaks(identifier, fieldName);
    if (identifier.size() > 255)
        throw std::runtime_error(std::string(fieldName) + " is too long: '" + identifier + "'");
    std::size_t componentStart = 0;
    bool sawDot = false;
    for (std::size_t i = 0; i <= identifier.size(); ++i) {
        if (i < identifier.size() && identifier[i] != '.')
            continue;
        const std::size_t len = i - componentStart;
        if (len == 0)
            throw std::runtime_error(std::string(fieldName) + " has an empty component: '" +
                                     identifier + "'");
        if (len > 63)
            throw std::runtime_error(std::string(fieldName) +
                                     " has a component longer than 63 "
                                     "characters: '" +
                                     identifier + "'");
        const char first = identifier[componentStart];
        const char last = identifier[i - 1];
        if (!std::isalnum(static_cast<unsigned char>(first)) ||
            !std::isalnum(static_cast<unsigned char>(last))) {
            throw std::runtime_error(std::string(fieldName) +
                                     " components must start and end with a letter or digit: '" +
                                     identifier + "'");
        }
        for (std::size_t j = componentStart; j < i; ++j) {
            const char c = identifier[j];
            unsigned char uc = static_cast<unsigned char>(c);
            if (!std::isalnum(uc) && c != '-')
                throw std::runtime_error(
                    std::string(fieldName) +
                    " components may contain only letters, digits, and '-': '" + identifier + "'");
        }
        if (i < identifier.size()) {
            sawDot = true;
            componentStart = i + 1;
        }
    }
    if (!sawDot)
        throw std::runtime_error(std::string(fieldName) +
                                 " must contain at least one '.' reverse-DNS separator: '" +
                                 identifier + "'");
}

/// @brief Validate a macOS bundle identifier. Delegates to validatePackageIdentifier
/// with a macOS-specific field name for clearer error messages.
/// @param identifier Optional reverse-DNS bundle identifier.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If a non-empty value is invalid.
inline void validateMacOSBundleIdentifier(const std::string &identifier,
                                          const char *fieldName = "macOS bundle identifier") {
    validatePackageIdentifier(identifier, fieldName);
}

/// @brief Validate a Windows ProgID base string (e.g. "Company.AppName").
/// Must contain at least one '.' separating non-empty components composed of
/// alphanumerics, '_', or '-'. Empty identifier is accepted (optional field).
/// @param identifier Optional ProgID base.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If the value exceeds 39 characters or violates syntax.
inline void validateWindowsProgIdBase(const std::string &identifier,
                                      const char *fieldName = "Windows ProgID base") {
    if (identifier.empty())
        return;
    rejectLineBreaks(identifier, fieldName);
    if (identifier.size() > 39) {
        throw std::runtime_error(std::string(fieldName) +
                                 " must not exceed 39 characters for a Windows ProgID: '" +
                                 identifier + "'");
    }
    bool sawDot = false;
    bool lastWasDot = true;
    for (char c : identifier) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '_' || c == '-') {
            lastWasDot = false;
            continue;
        }
        if (c == '.' && !lastWasDot) {
            sawDot = true;
            lastWasDot = true;
            continue;
        }
        throw std::runtime_error(std::string(fieldName) + " is not a valid Windows ProgID base: '" +
                                 identifier + "'");
    }
    if (!sawDot || lastWasDot)
        throw std::runtime_error(std::string(fieldName) +
                                 " must contain non-empty dot-separated components: '" +
                                 identifier + "'");
}

/// @brief Normalize a package lifecycle hook script (preinst, postinst, etc.).
/// Converts Windows CRLF line endings to LF, and rejects NUL bytes and other
/// non-printable control characters. Returns "" for an empty input.
/// @param script Raw hook text.
/// @param fieldName Human-readable field name used in diagnostics.
/// @return LF-normalized script text.
/// @throws std::runtime_error If forbidden control bytes are present.
inline std::string normalizePackageHookScript(const std::string &script, const char *fieldName) {
    if (script.empty())
        return {};
    std::string out;
    out.reserve(script.size());
    for (std::size_t i = 0; i < script.size(); ++i) {
        const unsigned char uc = static_cast<unsigned char>(script[i]);
        if (uc == 0)
            throw std::runtime_error(std::string(fieldName) + " must not contain NUL bytes");
        if (script[i] == '\r') {
            if (i + 1 < script.size() && script[i + 1] == '\n')
                continue;
            out.push_back('\n');
            continue;
        }
        if ((uc < 0x20 && script[i] != '\n' && script[i] != '\t') || uc == 0x7F) {
            throw std::runtime_error(std::string(fieldName) +
                                     " must not contain control characters");
        }
        out.push_back(script[i]);
    }
    return out;
}

/// @brief Require an explicit manifest opt-in before lifecycle hook scripts are packaged.
/// @details Hook text is still supported, but install-time shell execution is surprising enough
/// that manifests must set `allow-install-hooks true` before post/pre scripts are emitted.
/// @param pkg Package configuration to validate.
/// @throws std::runtime_error If hook text exists without explicit opt-in.
inline void validatePackageHooksAllowed(const PackageConfig &pkg) {
    if (pkg.allowInstallHooks)
        return;
    if (!pkg.postInstallScript.empty() || !pkg.preUninstallScript.empty()) {
        throw std::runtime_error("package hook scripts require 'allow-install-hooks true' in "
                                 "zanna.project");
    }
}

/// @brief Validate a single file association entry.
/// Checks that the extension starts with '.' and contains valid characters,
/// that description and mimeType are single-line printable strings, and that
/// mimeType has exactly one '/' separating non-empty type and subtype.
/// @param extension Dotted filename extension.
/// @param description User-visible file type name.
/// @param mimeType MIME type/subtype value.
/// @throws std::runtime_error If any field is unsafe or syntactically invalid.
inline void validateFileAssociation(const std::string &extension,
                                    const std::string &description,
                                    const std::string &mimeType) {
    if (extension.empty())
        throw std::runtime_error("file association extension must not be empty");
    if (extension.front() != '.' || extension.size() < 2 || extension.back() == '.')
        throw std::runtime_error("file association extension must be a dotted extension such as "
                                 "'.zia': '" +
                                 extension + "'");
    validateSingleLineField(description, "file association description");
    validateSingleLineField(mimeType, "file association MIME type");
    bool sawExtensionChar = false;
    for (char c : extension) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '.' && c != '_' && c != '-' && c != '+')
            throw std::runtime_error("file association extension contains an invalid character: '" +
                                     extension + "'");
        if (std::isalnum(uc) || c == '_' || c == '-' || c == '+')
            sawExtensionChar = true;
    }
    if (!sawExtensionChar)
        throw std::runtime_error("file association extension must contain at least one extension "
                                 "character: '" +
                                 extension + "'");
    const std::size_t slash = mimeType.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= mimeType.size())
        throw std::runtime_error("file association MIME type must be type/subtype: '" + mimeType +
                                 "'");
    if (mimeType.find('/', slash + 1) != std::string::npos)
        throw std::runtime_error("file association MIME type must contain only one '/': '" +
                                 mimeType + "'");
    for (char c : mimeType) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '/' && c != '-' && c != '+' && c != '.' && c != '_' &&
            c != '!' && c != '#' && c != '$' && c != '&' && c != '^') {
            throw std::runtime_error("file association MIME type contains an invalid character: '" +
                                     mimeType + "'");
        }
    }
}

/// @brief Validate all file associations in the package config.
/// Calls validateFileAssociation for each entry and additionally checks for
/// duplicate extensions (case-insensitive comparison).
/// @param associations File association list to validate.
/// @throws std::runtime_error If an entry is invalid or extensions collide.
inline void validatePackageFileAssociations(const std::vector<FileAssoc> &associations) {
    std::set<std::string> seenExtensions;
    for (const auto &assoc : associations) {
        validateFileAssociation(assoc.extension, assoc.description, assoc.mimeType);
        validateSingleLineField(assoc.openCommandArguments,
                                "file association open command arguments");
        const std::string key = lowerAsciiCopy(assoc.extension);
        if (!seenExtensions.insert(key).second) {
            throw std::runtime_error("duplicate file association extension: '" + assoc.extension +
                                     "'");
        }
    }
}

/// @brief Normalize and validate a Windows certificate-store SHA-1 thumbprint.
/// Accepts optional spaces/tabs for paste-friendliness, rejects line breaks, and
/// returns the compact lowercase 40-hex-character form expected by signtool /sha1.
/// @param thumbprint User-provided SHA-1 thumbprint.
/// @param fieldName Human-readable field name used in diagnostics.
/// @return Empty string or compact 40-character lowercase hexadecimal text.
/// @throws std::runtime_error If the non-whitespace content is not a SHA-1 digest.
inline std::string normalizeWindowsCertificateThumbprint(const std::string &thumbprint,
                                                         const char *fieldName) {
    if (thumbprint.empty())
        return {};
    rejectLineBreaks(thumbprint, fieldName);
    std::string compact;
    compact.reserve(40);
    for (char c : thumbprint) {
        if (c == ' ' || c == '\t')
            continue;
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isxdigit(uc)) {
            throw std::runtime_error(std::string(fieldName) +
                                     " must contain only SHA-1 hexadecimal digits");
        }
        compact.push_back(static_cast<char>(std::tolower(uc)));
    }
    if (compact.size() != 40) {
        throw std::runtime_error(std::string(fieldName) +
                                 " must be a 40-character SHA-1 thumbprint");
    }
    return compact;
}

/// @brief Validate a Windows certificate-store SHA-1 thumbprint when present.
/// @param thumbprint Optional user-provided thumbprint.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If normalization fails.
inline void validateWindowsCertificateThumbprint(const std::string &thumbprint,
                                                 const char *fieldName) {
    (void)normalizeWindowsCertificateThumbprint(thumbprint, fieldName);
}

/// @brief Return true if mode is one of the four supported macOS signing modes.
/// Valid values: "" (unset/default), "none", "preserve", "adhoc", "developer-id".
/// @param mode Signing-mode text to inspect.
/// @return Whether the value is supported or unset.
inline bool isValidMacOSSignModeText(const std::string &mode) {
    return mode.empty() || mode == "none" || mode == "preserve" || mode == "adhoc" ||
           mode == "developer-id";
}

/// @brief Return the effective macOS sign mode for the current host.
/// If pkg.macosSignMode is set, returns it directly. Otherwise defaults to
/// "adhoc" on Apple hosts (where codesign is available) and "preserve" elsewhere.
/// @param pkg Package signing configuration.
/// @return Explicit mode or the host-specific default.
inline std::string resolveMacOSSignModeForHost(const PackageConfig &pkg) {
    if (!pkg.macosSignMode.empty())
        return pkg.macosSignMode;
#if ZANNA_HOST_MACOS
    return "adhoc";
#else
    return "preserve";
#endif
}

/// @brief Validate the full macOS signing/notarization/stapling configuration.
/// Enforces that developer-id mode is required for notarization and stapling,
/// that an identity string is present when developer-id is selected, and that
/// stapling requires a notary profile.
/// @param pkg Package signing configuration.
/// @throws std::runtime_error If mode, identity, notarization, or stapling settings conflict.
inline void validateMacOSSigningConfig(const PackageConfig &pkg) {
    if (!isValidMacOSSignModeText(pkg.macosSignMode)) {
        throw std::runtime_error("macOS sign mode must be none, preserve, adhoc, or "
                                 "developer-id: " +
                                 pkg.macosSignMode);
    }

    const std::string mode = resolveMacOSSignModeForHost(pkg);
    if (mode == "developer-id" && pkg.macosSignIdentity.empty()) {
        throw std::runtime_error("macOS Developer ID signing requires macos-sign-identity");
    }
    if (!pkg.macosNotaryProfile.empty() && mode != "developer-id") {
        throw std::runtime_error("macOS notarization requires macos-sign-mode developer-id");
    }
    if (pkg.macosStaple) {
        if (mode != "developer-id")
            throw std::runtime_error("macos-staple requires macos-sign-mode developer-id");
        if (pkg.macosNotaryProfile.empty())
            throw std::runtime_error(
                "macos-staple requires macos-notary-profile for this package build");
    }
}

/// @brief Validate a package metadata URL.
/// Checks for a valid URI scheme (letters only start), "://" separator, non-empty
/// authority (host + optional port), no userinfo, and that the port (if present)
/// is purely numeric. Accepts IPv6 bracket notation. Empty URL is accepted.
/// @param url Optional HTTP(S) URL.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If a non-empty URL violates scheme, host, IPv6, or port rules.
inline void validatePackageUrl(const std::string &url, const char *fieldName) {
    if (url.empty())
        return;
    validateSingleLineField(url, fieldName);
    for (char c : url) {
        if (std::isspace(static_cast<unsigned char>(c)))
            throw std::runtime_error(std::string(fieldName) + " must not contain whitespace: '" +
                                     url + "'");
    }
    const auto schemePos = url.find("://");
    if (schemePos == std::string::npos || schemePos == 0 || schemePos + 3 >= url.size())
        throw std::runtime_error(std::string(fieldName) +
                                 " must include a URL scheme and host/path: '" + url + "'");
    if (!std::isalpha(static_cast<unsigned char>(url[0])))
        throw std::runtime_error(std::string(fieldName) +
                                 " URL scheme must start with a letter: '" + url + "'");
    for (std::size_t i = 0; i < schemePos; ++i) {
        unsigned char uc = static_cast<unsigned char>(url[i]);
        if (!std::isalpha(uc) && !std::isdigit(uc) && url[i] != '+' && url[i] != '-' &&
            url[i] != '.')
            throw std::runtime_error(std::string(fieldName) + " contains an invalid URL scheme: '" +
                                     url + "'");
    }
    std::string scheme = url.substr(0, schemePos);
    /// @brief Fold one URL-scheme byte through ctype safely.
    /// @param c Unsigned scheme byte.
    /// @return Lowercase byte converted back to plain char.
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (scheme != "http" && scheme != "https") {
        throw std::runtime_error(std::string(fieldName) +
                                 " must use http:// or https:// URL scheme: '" + url + "'");
    }

    const std::size_t authorityStart = schemePos + 3;
    const std::size_t authorityEnd = url.find_first_of("/?#", authorityStart);
    const std::string authority = authorityEnd == std::string::npos
                                      ? url.substr(authorityStart)
                                      : url.substr(authorityStart, authorityEnd - authorityStart);
    if (authority.empty())
        throw std::runtime_error(std::string(fieldName) + " URL host must not be empty: '" + url +
                                 "'");
    if (authority.find('@') != std::string::npos)
        throw std::runtime_error(std::string(fieldName) + " URL userinfo is not supported: '" +
                                 url + "'");

    std::string host;
    std::string port;
    if (authority.front() == '[') {
        const std::size_t close = authority.find(']');
        if (close == std::string::npos || close == 1)
            throw std::runtime_error(std::string(fieldName) + " URL IPv6 host is malformed: '" +
                                     url + "'");
        host = authority.substr(1, close - 1);
        if (close + 1 < authority.size()) {
            if (authority[close + 1] != ':')
                throw std::runtime_error(std::string(fieldName) +
                                         " URL host has invalid suffix: '" + url + "'");
            port = authority.substr(close + 2);
        }
        for (char c : host) {
            if (c != ':' && !std::isxdigit(static_cast<unsigned char>(c))) {
                throw std::runtime_error(std::string(fieldName) +
                                         " URL IPv6 host contains an invalid character: '" + url +
                                         "'");
            }
        }
        const std::size_t doubleColon = host.find("::");
        if (doubleColon != std::string::npos &&
            host.find("::", doubleColon + 2) != std::string::npos)
            throw std::runtime_error(std::string(fieldName) +
                                     " URL IPv6 host has multiple '::' elisions: '" + url + "'");
        if (host.find(":::") != std::string::npos)
            throw std::runtime_error(std::string(fieldName) + " URL IPv6 host is malformed: '" +
                                     url + "'");

        int groups = 0;
        std::size_t groupStart = 0;
        while (groupStart <= host.size()) {
            const std::size_t colon = host.find(':', groupStart);
            const std::string_view group =
                colon == std::string::npos
                    ? std::string_view(host).substr(groupStart)
                    : std::string_view(host).substr(groupStart, colon - groupStart);
            if (!group.empty()) {
                if (group.size() > 4)
                    throw std::runtime_error(std::string(fieldName) +
                                             " URL IPv6 host group is too long: '" + url + "'");
                ++groups;
            }
            if (colon == std::string::npos)
                break;
            groupStart = colon + 1;
        }
        const bool hasDoubleColon = doubleColon != std::string::npos;
        if (groups > 8 || (!hasDoubleColon && groups != 8) || (hasDoubleColon && groups >= 8)) {
            throw std::runtime_error(std::string(fieldName) + " URL IPv6 host is malformed: '" +
                                     url + "'");
        }
    } else {
        const std::size_t colon = authority.rfind(':');
        if (colon != std::string::npos) {
            host = authority.substr(0, colon);
            port = authority.substr(colon + 1);
        } else {
            host = authority;
        }
        if (host.empty() || host.front() == '.' || host.back() == '.' || host.front() == '-' ||
            host.back() == '-')
            throw std::runtime_error(std::string(fieldName) + " URL host is malformed: '" + url +
                                     "'");
        if (host.size() > 253)
            throw std::runtime_error(std::string(fieldName) + " URL host is too long: '" + url +
                                     "'");
        bool sawHostChar = false;
        bool lastWasDot = false;
        std::size_t labelStart = 0;
        for (std::size_t i = 0; i < host.size(); ++i) {
            const char c = host[i];
            unsigned char uc = static_cast<unsigned char>(c);
            if (std::isalnum(uc)) {
                sawHostChar = true;
                lastWasDot = false;
                continue;
            }
            if (c == '-') {
                lastWasDot = false;
                continue;
            }
            if (c == '.') {
                if (lastWasDot)
                    throw std::runtime_error(std::string(fieldName) +
                                             " URL host has an empty label: '" + url + "'");
                const std::size_t labelLen = i - labelStart;
                if (labelLen > 63) {
                    throw std::runtime_error(std::string(fieldName) +
                                             " URL host has a label longer than 63 characters: '" +
                                             url + "'");
                }
                labelStart = i + 1;
                lastWasDot = true;
                continue;
            }
            throw std::runtime_error(std::string(fieldName) +
                                     " URL host contains an invalid character: '" + url + "'");
        }
        const std::size_t finalLabelLen = host.size() - labelStart;
        if (finalLabelLen > 63) {
            throw std::runtime_error(std::string(fieldName) +
                                     " URL host has a label longer than 63 characters: '" + url +
                                     "'");
        }
        if (!sawHostChar)
            throw std::runtime_error(std::string(fieldName) +
                                     " URL host must contain letters or digits: '" + url + "'");
    }
    if (!port.empty()) {
        uint32_t portValue = 0;
        for (char c : port) {
            if (!std::isdigit(static_cast<unsigned char>(c)))
                throw std::runtime_error(std::string(fieldName) + " URL port must be numeric: '" +
                                         url + "'");
            const uint32_t digit = static_cast<uint32_t>(c - '0');
            if (portValue > (65535u - digit) / 10u) {
                throw std::runtime_error(std::string(fieldName) +
                                         " URL port must be between 0 and 65535: '" + url + "'");
            }
            portValue = portValue * 10u + digit;
        }
    } else if (authority.back() == ':') {
        throw std::runtime_error(std::string(fieldName) + " URL port must not be empty: '" + url +
                                 "'");
    }
}

/// @brief Validate a package URL and require encrypted HTTPS transport.
/// @details Use for signing and timestamp endpoints where accepting cleartext HTTP would weaken
///          the release trust chain. Empty URLs remain accepted so callers can apply defaults.
/// @param url Optional URL to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If URL validation fails or the scheme is not HTTPS.
inline void validateHttpsPackageUrl(const std::string &url, const char *fieldName) {
    validatePackageUrl(url, fieldName);
    if (url.empty())
        return;
    if (url.size() < 8 ||
        /// @brief Compare one URL byte against the HTTPS prefix case-insensitively.
        /// @param lhs URL byte.
        /// @param rhs Expected prefix byte.
        /// @return `true` when the ASCII lowercase bytes are equal.
        !std::equal(url.begin(), url.begin() + 8, "https://", [](char lhs, char rhs) {
            return std::tolower(static_cast<unsigned char>(lhs)) ==
                   std::tolower(static_cast<unsigned char>(rhs));
        })) {
        throw std::runtime_error(std::string(fieldName) + " must use https://: '" + url + "'");
    }
}

/// @brief Validate a filename for use on Windows.
/// Rejects control characters, the characters <, >, :, ", /, \, |, ?, *,
/// trailing spaces/dots, and Windows reserved device names (CON, NUL, COM1, etc.).
/// @param name Filename leaf to validate.
/// @param fieldName Human-readable field name used in diagnostics.
/// @throws std::runtime_error If empty, reserved, or containing a forbidden form.
inline void validateWindowsFileName(const std::string &name, const char *fieldName) {
    if (name.empty())
        throw std::runtime_error(std::string(fieldName) + " must not be empty");
    if (name.back() == ' ' || name.back() == '.')
        throw std::runtime_error(std::string(fieldName) + " must not end in space or dot: '" +
                                 name + "'");
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 || uc == 0x7F || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') {
            throw std::runtime_error(std::string(fieldName) +
                                     " contains a character invalid on Windows: '" + name + "'");
        }
    }

    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const std::string stem = lower.substr(0, lower.find('.'));
    for (std::string_view reserved :
         {"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
          "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"}) {
        if (stem == reserved)
            throw std::runtime_error(std::string(fieldName) +
                                     " must not be a Windows reserved device name: '" + name + "'");
    }
}

/// @brief Return true if path starts with every component of root.
/// Used to guard against symlink escapes that resolve outside the project root.
/// Both paths should be canonical (absolute, no symlinks) before comparison.
/// @param root Canonical containment root.
/// @param path Canonical candidate path.
/// @return Whether every component of `root` prefixes `path`.
inline bool isPathWithin(const std::filesystem::path &root, const std::filesystem::path &path) {
    auto rootIt = root.begin();
    auto pathIt = path.begin();
    for (; rootIt != root.end(); ++rootIt, ++pathIt) {
        if (pathIt == path.end() || *rootIt != *pathIt)
            return false;
    }
    return true;
}

/// @brief Normalize a package-relative path and reject archive escapes.
///
/// Converts backslashes to forward slashes and rejects absolute paths, parent
/// traversal, empty segments, drive-qualified paths, and control characters.
/// Returns an empty string for "." or an empty input.
/// @param raw Untrusted package path text.
/// @param fieldName Human-readable field name used in diagnostics.
/// @return Normalized slash-delimited relative path.
/// @throws std::runtime_error If the input is absolute, escaping, special, or unsafe.
inline std::string sanitizePackageRelativePath(const std::string &raw,
                                               const char *fieldName = "package path") {
    std::string normalized = raw;
    for (char &c : normalized) {
        if (c == '\\')
            c = '/';
    }

    std::string collapsed;
    collapsed.reserve(normalized.size());
    bool lastWasSlash = false;
    for (char c : normalized) {
        if (c == '/') {
            if (lastWasSlash)
                continue;
            lastWasSlash = true;
        } else {
            lastWasSlash = false;
        }
        collapsed.push_back(c);
    }
    normalized.swap(collapsed);

    while (!normalized.empty() && normalized.back() == '/')
        normalized.pop_back();

    if (normalized.empty() || normalized == ".")
        return "";

    if (normalized.front() == '/')
        throw std::runtime_error(std::string(fieldName) + " must be relative: '" + raw + "'");

    if (normalized.size() >= 2 && std::isalpha(static_cast<unsigned char>(normalized[0])) &&
        normalized[1] == ':') {
        throw std::runtime_error(std::string(fieldName) + " must not use a drive prefix: '" + raw +
                                 "'");
    }

    std::string out;
    std::size_t pos = 0;
    while (pos < normalized.size()) {
        std::size_t next = normalized.find('/', pos);
        std::string_view segment = next == std::string::npos
                                       ? std::string_view(normalized).substr(pos)
                                       : std::string_view(normalized).substr(pos, next - pos);

        if (segment.empty())
            throw std::runtime_error(std::string(fieldName) + " contains an empty path segment: '" +
                                     raw + "'");
        if (segment == "." || segment == "..")
            throw std::runtime_error(std::string(fieldName) +
                                     " must not contain '.' or '..' segments: '" + raw + "'");

        for (char c : segment) {
            unsigned char uc = static_cast<unsigned char>(c);
            if (uc < 0x20 || uc == 0x7F || c == ':') {
                throw std::runtime_error(std::string(fieldName) +
                                         " contains an invalid character: '" + raw + "'");
            }
        }

        if (!out.empty())
            out.push_back('/');
        out.append(segment.data(), segment.size());

        if (next == std::string::npos)
            break;
        pos = next + 1;
    }

    return out;
}

/// @brief Join two package-relative paths and sanitize the result.
/// @param base First relative path.
/// @param leaf Second relative path.
/// @param fieldName Human-readable field name used in diagnostics.
/// @return Safely joined normalized path.
/// @throws std::runtime_error If either path is unsafe.
inline std::string joinPackageRelativePath(const std::string &base,
                                           const std::string &leaf,
                                           const char *fieldName = "package path") {
    const std::string cleanBase = sanitizePackageRelativePath(base, fieldName);
    const std::string cleanLeaf = sanitizePackageRelativePath(leaf, fieldName);
    if (cleanBase.empty())
        return cleanLeaf;
    if (cleanLeaf.empty())
        return cleanBase;
    return sanitizePackageRelativePath(cleanBase + "/" + cleanLeaf, fieldName);
}

/// @brief Resolve a package-relative source path to an absolute filesystem path.
/// Sanitizes raw via sanitizePackageRelativePath, then resolves against projectRoot.
/// Uses fs::canonical when the file exists; falls back to weakly_canonical otherwise
/// (so missing files can still be validated for escapes before creation).
/// Throws if the result escapes the project root.
/// @param projectRoot Existing trusted project directory.
/// @param raw Untrusted project-relative source path.
/// @param fieldName Human-readable field name used in diagnostics.
/// @return Canonical or weakly canonical contained path.
/// @throws std::runtime_error If resolution fails or escapes the project root.
inline std::filesystem::path resolvePackageSourcePath(const std::filesystem::path &projectRoot,
                                                      const std::string &raw,
                                                      const char *fieldName) {
    namespace fs = std::filesystem;
    const std::string clean = sanitizePackageRelativePath(raw, fieldName);
    if (clean.empty())
        throw std::runtime_error(std::string(fieldName) + " must not be empty");

    std::error_code ec;
    const fs::path canonicalRoot = fs::canonical(projectRoot, ec);
    if (ec)
        throw std::runtime_error("cannot resolve project root for packaging: " +
                                 zanna::filesystem::pathToUtf8(projectRoot));

    fs::path candidate =
        (canonicalRoot / zanna::filesystem::pathFromUtf8(clean)).lexically_normal();
    const fs::path weakCandidate = fs::weakly_canonical(candidate, ec);
    if (ec)
        throw std::runtime_error(std::string("cannot resolve ") + fieldName + ": " + raw);
    if (!isPathWithin(canonicalRoot, weakCandidate)) {
        throw std::runtime_error(std::string(fieldName) + " escapes the project root: '" + raw +
                                 "'");
    }
    ec.clear();
    const bool candidateExists = fs::exists(candidate, ec);
    if (ec)
        throw std::runtime_error(std::string("cannot stat ") + fieldName + ": " + raw);
    if (!candidateExists)
        return weakCandidate;

    const fs::path resolved = fs::canonical(candidate, ec);
    if (ec)
        throw std::runtime_error(std::string("cannot resolve ") + fieldName + ": " + raw);
    if (!isPathWithin(canonicalRoot, resolved)) {
        throw std::runtime_error(std::string(fieldName) + " escapes the project root: '" + raw +
                                 "'");
    }
    return resolved;
}

/// @brief Read a project-relative package metadata text file.
/// @details Resolves @p raw with resolvePackageSourcePath(), requires the result
///          to be a regular file, and returns its byte contents as a string
///          without transcoding or newline normalization. Intended for metadata
///          files such as package-license-file and package-readme that are copied
///          into installer UX surfaces or portable archives.
/// @param projectRoot Canonical package root used to contain source paths.
/// @param raw Project-relative path from the manifest.
/// @param fieldName Name used in diagnostics.
/// @return File contents as a byte-preserving string.
/// @throws std::runtime_error on unsafe paths, missing/non-file inputs, or I/O errors.
inline std::string readPackageTextFile(const std::filesystem::path &projectRoot,
                                       const std::string &raw,
                                       const char *fieldName) {
    namespace fs = std::filesystem;
    const fs::path path = resolvePackageSourcePath(projectRoot, raw, fieldName);
    if (!fs::is_regular_file(path))
        throw std::runtime_error(std::string(fieldName) + " is not a regular file: " + raw);
    const std::vector<uint8_t> data = readFile(path);
    return std::string(reinterpret_cast<const char *>(data.data()), data.size());
}

/// @brief Convert a UTF-8 string to a vector of UTF-16 code units (no BOM).
/// Handles the full Unicode range including supplementary characters via
/// surrogate pairs (U+10000–U+10FFFF). Throws on overlong sequences, truncated
/// multi-byte sequences, invalid leading bytes, or lone surrogates.
/// @param text UTF-8 input.
/// @return UTF-16 code units without a terminator.
/// @throws std::runtime_error If the UTF-8 sequence is malformed or denotes an invalid code point.
inline std::vector<uint16_t> utf8ToUtf16CodeUnits(const std::string &text) {
    std::vector<uint16_t> out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            extra = 1;
            if (cp == 0)
                throw std::runtime_error("invalid overlong UTF-8 sequence");
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            extra = 3;
        } else {
            throw std::runtime_error("invalid UTF-8 leading byte");
        }
        if (i + extra >= text.size())
            throw std::runtime_error("truncated UTF-8 sequence");
        for (size_t j = 1; j <= extra; ++j) {
            unsigned char cc = static_cast<unsigned char>(text[i + j]);
            if ((cc & 0xC0) != 0x80)
                throw std::runtime_error("invalid UTF-8 continuation byte");
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
            (extra == 3 && cp < 0x10000) || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            throw std::runtime_error("invalid UTF-8 code point");
        }
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<uint16_t>(cp));
        } else {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        }
        i += extra + 1;
    }
    return out;
}

/// @brief Return the number of UTF-16 code units that text would occupy.
/// Convenience wrapper around utf8ToUtf16CodeUnits for size-checking without
/// retaining the full code unit array.
/// @param text UTF-8 input.
/// @return Number of UTF-16 code units excluding a terminator.
/// @throws std::runtime_error If the UTF-8 input is invalid.
inline size_t utf16CodeUnitCountFromUtf8(const std::string &text) {
    return utf8ToUtf16CodeUnits(text).size();
}

/// @brief Convert a UTF-8 string to a little-endian UTF-16 byte sequence.
/// @param text UTF-8 input.
/// @param nulTerminate If true, appends a UTF-16 NUL (two zero bytes).
/// Used for Win32 registry string fields and .lnk StringData blocks.
/// @return Little-endian UTF-16 bytes without a BOM.
/// @throws std::runtime_error If UTF-8 is invalid or the encoded string exceeds 65535 units.
inline std::vector<uint8_t> utf8ToUtf16LEBytes(const std::string &text, bool nulTerminate = true) {
    const auto units = utf8ToUtf16CodeUnits(text);
    if (units.size() > static_cast<size_t>(std::numeric_limits<uint16_t>::max()))
        throw std::runtime_error("UTF-16 string is too long");
    std::vector<uint8_t> bytes;
    bytes.reserve((units.size() + (nulTerminate ? 1 : 0)) * 2);
    for (uint16_t unit : units) {
        bytes.push_back(static_cast<uint8_t>(unit & 0xFF));
        bytes.push_back(static_cast<uint8_t>((unit >> 8) & 0xFF));
    }
    if (nulTerminate) {
        bytes.push_back(0);
        bytes.push_back(0);
    }
    return bytes;
}

/// @brief A package-directory entry after resolving any symlink used to reach it.
///
/// logicalPath is the path that determines the archive/install-relative name.
/// resolvedPath is the filesystem path that was validated against projectRoot
/// and must be used for stat/read/copy operations to avoid post-validation
/// symlink swaps changing the packaged payload.
struct SafeDirectoryEntry {
    std::filesystem::path logicalPath;  ///< Archive/install-relative name source.
    std::filesystem::path resolvedPath; ///< Validated path to stat/read/copy from.
    bool directory{false};              ///< True when the resolved target is a directory.
    bool regularFile{false};            ///< True when the resolved target is a regular file.
    bool symlink{false};                ///< True when the entry was reached through a symlink.
};

/// @brief Safely iterate a directory tree, following only symlinks that remain
///        inside the project root and handling permission errors gracefully.
///
/// The callback receives both the logical archive path and the resolved path to
/// use for filesystem reads. Callers should read/stat resolvedPath, not
/// logicalPath, to preserve the validation decision.
/// @param root Existing directory to traverse.
/// @param projectRoot Trusted containment root for resolved entries.
/// @param callback Invoked once per safe directory, file, or followed symlink.
/// @throws std::runtime_error If traversal, canonicalization, containment, or stat checks fail.
inline void safeDirectoryIterateResolved(
    const std::filesystem::path &root,
    const std::filesystem::path &projectRoot,
    const std::function<void(const SafeDirectoryEntry &)> &callback) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path canonicalRoot = fs::canonical(projectRoot, ec);
    if (ec)
        throw std::runtime_error("cannot resolve project root for directory traversal: " +
                                 zanna::filesystem::pathToUtf8(projectRoot));
    ec.clear();

    fs::path canonicalIterRoot = fs::canonical(root, ec);
    if (ec)
        throw std::runtime_error("cannot resolve directory for traversal: " +
                                 zanna::filesystem::pathToUtf8(root));
    ec.clear();
    std::set<fs::path> visitedDirectories;
    visitedDirectories.insert(canonicalIterRoot);

    /// @brief Recursively traverse one verified physical directory at its logical package path.
    /// @param physicalDir Canonical or safely resolved directory to enumerate.
    /// @param logicalDir Corresponding path spelling exposed to the package callback.
    /// @throws std::runtime_error If enumeration, resolution, containment, or metadata checks fail.
    std::function<void(const fs::path &, const fs::path &)> walk = [&](const fs::path &physicalDir,
                                                                       const fs::path &logicalDir) {
        auto it = fs::directory_iterator(physicalDir, ec);
        if (ec) {
            throw std::runtime_error("cannot access package asset directory '" +
                                     zanna::filesystem::pathToUtf8(logicalDir) +
                                     "': " + ec.message());
        }
        const auto end = fs::directory_iterator();
        while (it != end) {
            const fs::path entryPath = logicalDir / it->path().filename();
            bool isDirectory = false;
            bool isRegularFile = false;
            bool hasResolvedSymlink = false;
            fs::path resolvedSymlink;

            std::error_code entryEc;
            const fs::path physicalEntryPath = it->path();
            if (fs::is_symlink(fs::symlink_status(physicalEntryPath, entryEc))) {
                fs::path resolved = fs::canonical(physicalEntryPath, entryEc);
                if (entryEc) {
                    throw std::runtime_error("cannot resolve package asset symlink '" +
                                             zanna::filesystem::pathToUtf8(entryPath) +
                                             "': " + entryEc.message());
                } else if (!isPathWithin(canonicalRoot, resolved)) {
                    throw std::runtime_error("package asset symlink escapes the project root: '" +
                                             zanna::filesystem::pathToUtf8(entryPath) + "'");
                } else {
                    resolvedSymlink = resolved;
                    hasResolvedSymlink = true;
                }
            }

            entryEc.clear();
            const fs::path resolvedPath = hasResolvedSymlink ? resolvedSymlink : physicalEntryPath;
            const fs::file_status status = fs::status(resolvedPath, entryEc);
            if (!entryEc) {
                isDirectory = fs::is_directory(status);
                isRegularFile = fs::is_regular_file(status);
            }
            if (entryEc) {
                throw std::runtime_error("cannot stat package asset entry '" +
                                         zanna::filesystem::pathToUtf8(entryPath) +
                                         "': " + entryEc.message());
            }

            callback(SafeDirectoryEntry{
                entryPath, resolvedPath, isDirectory, isRegularFile, hasResolvedSymlink});

            if (isDirectory) {
                fs::path resolvedDir = hasResolvedSymlink
                                           ? resolvedSymlink
                                           : fs::canonical(physicalEntryPath, entryEc);
                if (entryEc) {
                    throw std::runtime_error("cannot resolve package asset directory '" +
                                             zanna::filesystem::pathToUtf8(entryPath) +
                                             "': " + entryEc.message());
                } else if (visitedDirectories.insert(resolvedDir).second) {
                    walk(resolvedDir, entryPath);
                }
            }

            it.increment(ec);
            if (ec) {
                throw std::runtime_error("cannot advance package asset directory iterator from '" +
                                         zanna::filesystem::pathToUtf8(entryPath) +
                                         "': " + ec.message());
            }
        }
    };
    walk(canonicalIterRoot, root);
}

/// @brief Compatibility wrapper for callers that only need a directory_entry-shaped object.
/// @details Preserves the legacy logical traversal path exposed by the original
///          directory iterator API. Callers that need the validated filesystem
///          location for stat/read/copy operations should use
///          safeDirectoryIterateResolved(), which provides both the logical
///          archive path and the resolved path that passed containment checks.
/// @param root Existing directory to traverse.
/// @param projectRoot Trusted containment root.
/// @param callback Invoked with a logical-path directory entry for each safe item.
/// @throws std::runtime_error If the resolved traversal fails.
inline void safeDirectoryIterate(
    const std::filesystem::path &root,
    const std::filesystem::path &projectRoot,
    const std::function<void(const std::filesystem::directory_entry &)> &callback) {
    /// @brief Adapt a validated safe entry to the legacy logical directory-entry callback.
    /// @param entry Validated resolved traversal entry.
    safeDirectoryIterateResolved(root, projectRoot, [&](const SafeDirectoryEntry &entry) {
        callback(std::filesystem::directory_entry(entry.logicalPath));
    });
}

} // namespace zanna::pkg
