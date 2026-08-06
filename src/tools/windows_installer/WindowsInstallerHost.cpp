//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerHost.cpp
// Purpose: Discover and validate Windows installer packages, commands, and logs.
// Key invariants:
//   - Overlay structure, metadata, versions, PE images, inventories, and hashes fail closed.
//   - Options reject conflicts and logs contain sanitized single-line UTF-8 records.
// Ownership/Lifetime:
//   - Loaded packages own executable, payload, metadata, and cleanup bytes.
//   - Archive readers borrow buffers only within the package-loading operation.
// Links: src/tools/windows_installer/WindowsInstallerHost.hpp,
//        src/tools/common/packaging/WindowsInstallerMetadata.hpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerHost.hpp"

#include "PkgHash.hpp"
#include "WindowsPEValidation.hpp"
#include "ZipReader.hpp"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cwchar>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace zanna::installer {
namespace {

constexpr uint64_t kMaximumInstallerExecutableBytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;

/// @brief Decode an unsigned 16-bit little-endian integer.
/// @param p Pointer to at least two readable bytes.
/// @return Decoded value.
uint16_t readLe16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8U);
}

/// @brief Decode an unsigned 32-bit little-endian integer.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded value.
uint32_t readLe32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
           (static_cast<uint32_t>(p[2]) << 16U) | (static_cast<uint32_t>(p[3]) << 24U);
}

/// @brief Fold ASCII letters in a wide string to lowercase.
/// @param value Text to transform.
/// @return Folded copy; non-ASCII code units are unchanged.
std::wstring lowerWide(std::wstring value) {
    /// @brief Fold one wide ASCII uppercase code unit to lowercase.
    /// @param ch Code unit to normalize.
    /// @return Lowercase ASCII code unit or the unchanged input.
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) -> wchar_t {
        return ch >= L'A' && ch <= L'Z' ? static_cast<wchar_t>(ch + (L'a' - L'A')) : ch;
    });
    return value;
}

/// @brief Fold ASCII letters in a narrow string to lowercase.
/// @param value Text to transform.
/// @return Folded copy; non-ASCII bytes are unchanged.
std::string lowerAscii(std::string value) {
    /// @brief Fold one narrow ASCII uppercase byte to lowercase.
    /// @param ch Byte to normalize.
    /// @return Lowercase ASCII byte or the unchanged input.
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    return value;
}

/// @brief Test whether a byte is an ASCII letter or digit.
/// @param ch Candidate byte.
/// @return @c true for A-Z, a-z, or 0-9.
bool isAsciiAlnum(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
}

/// @brief Read a bounded installer executable without sharing writes.
/// @param path Executable path to open.
/// @return Complete stable file bytes.
/// @throws std::runtime_error On open, size, read, concurrent-change, or close failure.
std::vector<uint8_t> readWholeFile(const fs::path &path) {
    HANDLE file = CreateFileW(path.c_str(),
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot open installer executable");
    try {
        LARGE_INTEGER nativeSize{};
        if (!GetFileSizeEx(file, &nativeSize) || nativeSize.QuadPart < 0 ||
            static_cast<uint64_t>(nativeSize.QuadPart) > kMaximumInstallerExecutableBytes ||
            static_cast<uint64_t>(nativeSize.QuadPart) >
                static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
            throw std::runtime_error("installer executable is too large");
        }
        std::vector<uint8_t> bytes(static_cast<size_t>(nativeSize.QuadPart));
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD requested = static_cast<DWORD>(
                std::min<size_t>(bytes.size() - offset, static_cast<size_t>(MAXDWORD)));
            DWORD read = 0;
            if (!ReadFile(file, bytes.data() + offset, requested, &read, nullptr) || read == 0 ||
                read > requested) {
                throw std::runtime_error("cannot read installer executable");
            }
            offset += read;
        }
        uint8_t extra = 0;
        DWORD extraRead = 0;
        if (!ReadFile(file, &extra, 1, &extraRead, nullptr) || extraRead != 0)
            throw std::runtime_error("installer executable changed while it was being read");
        if (!CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            throw std::runtime_error("cannot close installer executable");
        }
        file = INVALID_HANDLE_VALUE;
        return bytes;
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
        throw;
    }
}

struct OverlayRange {
    size_t offset{0};
    size_t length{0};
};

/// @brief Locate a structurally valid ZIP overlay at the end of an executable.
/// @param bytes Complete executable bytes.
/// @return Offset and length of the ZIP archive.
/// @throws std::runtime_error When no bounded single-disk EOCD/central directory
///         relationship reaches the exact file end.
OverlayRange locateZipOverlay(const std::vector<uint8_t> &bytes) {
    constexpr uint32_t kEocdSignature = 0x06054B50U;
    constexpr uint32_t kCentralSignature = 0x02014B50U;
    if (bytes.size() < 22)
        throw std::runtime_error("installer has no ZIP overlay");

    const size_t minimum = bytes.size() > (0xFFFFU + 22U + 1024U * 1024U)
                               ? bytes.size() - (0xFFFFU + 22U + 1024U * 1024U)
                               : 0;
    for (size_t pos = bytes.size() - 22;; --pos) {
        if (readLe32(bytes.data() + pos) == kEocdSignature) {
            const uint16_t commentLength = readLe16(bytes.data() + pos + 20);
            const size_t eocdEnd = pos + 22U + commentLength;
            if (eocdEnd == bytes.size()) {
                const uint16_t diskNumber = readLe16(bytes.data() + pos + 4U);
                const uint16_t centralDisk = readLe16(bytes.data() + pos + 6U);
                const uint16_t entriesOnDisk = readLe16(bytes.data() + pos + 8U);
                const uint16_t totalEntries = readLe16(bytes.data() + pos + 10U);
                const uint32_t centralSize = readLe32(bytes.data() + pos + 12);
                const uint32_t centralOffset = readLe32(bytes.data() + pos + 16);
                const uint64_t prefixDistance = static_cast<uint64_t>(centralSize) + centralOffset;
                if (diskNumber == 0U && centralDisk == 0U && entriesOnDisk != 0U &&
                    entriesOnDisk == totalEntries && totalEntries != UINT16_MAX &&
                    centralSize != 0U && centralOffset != UINT32_MAX && centralSize != UINT32_MAX &&
                    prefixDistance <= pos) {
                    const size_t start = pos - static_cast<size_t>(prefixDistance);
                    const uint64_t centralAbsolute = static_cast<uint64_t>(start) + centralOffset;
                    if (centralAbsolute + centralSize == pos && centralAbsolute + 4U <= pos &&
                        readLe32(bytes.data() + static_cast<size_t>(centralAbsolute)) ==
                            kCentralSignature) {
                        return {start, eocdEnd - start};
                    }
                }
            }
        }
        if (pos == minimum)
            break;
    }
    throw std::runtime_error("installer ZIP end record is missing or invalid");
}

/// @brief Copy an arbitrary byte vector into a narrow string.
/// @param bytes Source bytes.
/// @return String preserving every byte, including embedded NULs.
std::string bytesToString(const std::vector<uint8_t> &bytes) {
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

/// @brief Validate a bounded executable PE32+ image and its target architecture.
/// @param bytes Complete candidate image.
/// @param architecture Metadata architecture name, @c x64 or @c arm64.
/// @param label Human-readable image label used in errors.
/// @throws std::runtime_error On malformed headers, sections, image properties,
///         unsupported metadata, or architecture mismatch.
void requirePeArchitecture(const std::vector<uint8_t> &bytes,
                           std::string_view architecture,
                           std::string_view label) {
    uint16_t expected = 0;
    if (architecture == "arm64")
        expected = 0xAA64U;
    else if (architecture == "x64")
        expected = 0x8664U;
    else
        throw std::runtime_error(std::string(label) + " metadata architecture is unsupported");
    std::string validationError;
    zanna::pkg::WindowsPEImageInfo image;
    if (!zanna::pkg::validateWindowsPEImage(bytes.empty() ? nullptr : bytes.data(),
                                            bytes.size(),
                                            expected,
                                            &image,
                                            validationError)) {
        throw std::runtime_error(std::string(label) + " is not a bounded " +
                                 std::string(architecture) +
                                 " PE32+ executable: " + validationError);
    }
}

/// @brief Require the outer archive to match its owned metadata inventory exactly.
/// @param outer Verified outer ZIP reader.
/// @param metadata Parsed installer metadata describing required entries.
/// @throws std::runtime_error For missing, extra, or unowned files/directories.
void requireOuterInventory(const zanna::pkg::ZipReader &outer,
                           const zanna::pkg::WindowsInstallerMetadata &metadata) {
    std::set<std::string> required = {
        "meta/installer-v2.txt", metadata.payloadEntry, metadata.cleanupEntry};
    std::set<std::string> allowed = required;
    if (!metadata.licenseEntry.empty())
        allowed.insert(metadata.licenseEntry);
    if (!metadata.readmeEntry.empty())
        allowed.insert(metadata.readmeEntry);
    for (const auto &file : metadata.outerFiles) {
        required.insert(file.overlayPath);
        allowed.insert(file.overlayPath);
    }
    std::set<std::string> allowedDirectories = {"app/"};
    for (const std::string &path : allowed) {
        size_t separator = path.find('/');
        while (separator != std::string::npos) {
            allowedDirectories.insert(path.substr(0, separator + 1U));
            separator = path.find('/', separator + 1U);
        }
    }
    std::set<std::string> found;
    for (const zanna::pkg::ZipEntry &entry : outer.entries()) {
        if (!entry.name.empty() && entry.name.back() == '/') {
            if (allowedDirectories.find(entry.name) == allowedDirectories.end())
                throw std::runtime_error("installer contains an unowned outer directory");
            continue;
        }
        if (allowed.find(entry.name) == allowed.end())
            throw std::runtime_error("installer contains an unowned outer entry");
        found.insert(entry.name);
    }
    for (const std::string &path : required) {
        if (found.find(path) == found.end())
            throw std::runtime_error("installer outer inventory is incomplete");
    }
}

/// @brief Escape bytes for inclusion in a JSON string literal body.
/// @param value Unquoted UTF-8 or metadata text.
/// @return Escaped text without surrounding quotes.
std::string jsonEscape(std::string_view value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\':
                out << "\\\\";
                break;
            case '"':
                out << "\\\"";
                break;
            case '\n':
                out << "\\n";
                break;
            case '\r':
                out << "\\r";
                break;
            case '\t':
                out << "\\t";
                break;
            default:
                if (ch < 0x20U) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

/// @brief Test an already-normalized wide option prefix.
/// @param value Candidate text.
/// @param prefix Prefix to match exactly.
/// @return @c true when @p prefix begins at offset zero.
bool startsWith(std::wstring_view value, std::wstring_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

/// @brief Extract an inline option value following @c = or @c :.
/// @param original Original-cased option text supplying the returned value.
/// @param lower Lowercase copy used for matching.
/// @param name Lowercase option name without separator.
/// @return Owned value when either inline spelling matches, otherwise nullopt.
std::optional<std::wstring> optionValue(std::wstring_view original,
                                        std::wstring_view lower,
                                        std::wstring_view name) {
    const std::wstring equals = std::wstring(name) + L"=";
    const std::wstring colon = std::wstring(name) + L":";
    if (startsWith(lower, equals) || startsWith(lower, colon))
        return std::wstring(original.substr(name.size() + 1));
    return std::nullopt;
}

/// @brief Parse a nonzero decimal maintenance-handoff process ID.
/// @param text Decimal UTF-16 digits.
/// @return DWORD process identifier.
/// @throws std::runtime_error On empty, nondigit, zero, or overflow input.
DWORD parseHandoffProcessId(std::wstring_view text) {
    if (text.empty())
        throw std::runtime_error("invalid internal handoff process identifier");
    uint64_t value = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9')
            throw std::runtime_error("invalid internal handoff process identifier");
        value = value * 10U + static_cast<unsigned>(ch - L'0');
        if (value > MAXDWORD)
            throw std::runtime_error("invalid internal handoff process identifier");
    }
    if (value == 0)
        throw std::runtime_error("invalid internal handoff process identifier");
    return static_cast<DWORD>(value);
}

/// @brief Parse and normalize a comma-separated component selection.
/// @param text User-supplied component IDs.
/// @return Unique lowercase UTF-8 identifiers.
/// @throws std::runtime_error On empty, duplicate, or invalid identifiers.
std::set<std::string> parseComponents(std::wstring_view text) {
    std::set<std::string> result;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(L',', start);
        std::wstring item(text.substr(
            start, comma == std::wstring_view::npos ? text.size() - start : comma - start));
        item = lowerWide(item);
        if (item.empty())
            throw std::runtime_error("/components contains an empty component id");
        const std::string utf8 = wideToUtf8(item);
        for (unsigned char ch : utf8) {
            if (!(isAsciiAlnum(ch) || ch == '-' || ch == '_' || ch == '.'))
                throw std::runtime_error("/components contains an invalid component id");
        }
        if (!result.insert(utf8).second)
            throw std::runtime_error("/components contains a duplicate component id");
        if (comma == std::wstring_view::npos)
            break;
        start = comma + 1;
    }
    return result;
}

/// @brief Parse a component-preset name and accepted aliases.
/// @param value User-supplied preset spelling.
/// @return Normalized preset enumeration.
/// @throws std::runtime_error When the preset is unknown.
ComponentPreset parseComponentPreset(std::wstring value) {
    value = lowerWide(std::move(value));
    if (value == L"minimal")
        return ComponentPreset::Minimal;
    if (value == L"typical" || value == L"recommended")
        return ComponentPreset::Typical;
    if (value == L"sdk" || value == L"developer")
        return ComponentPreset::SDK;
    if (value == L"complete" || value == L"full")
        return ComponentPreset::Complete;
    throw std::runtime_error("/type must be minimal, typical, sdk, or complete");
}

/// @brief Sanitize and bound one user-visible message for a single log record.
/// @param message Raw UTF-16 message.
/// @return Bounded text with controls/bidi markers replaced, invalid surrogates
///         repaired, and truncation explicitly marked.
std::wstring sanitizeLogLine(std::wstring_view message) {
    std::wstring result;
    constexpr size_t kMaximumLine = 8192;
    constexpr std::wstring_view kTruncated = L" [truncated]";
    result.reserve(std::min(message.size(), kMaximumLine));
    for (size_t index = 0; index < message.size();) {
        const wchar_t ch = message[index++];
        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (index < message.size() && message[index] >= 0xDC00 && message[index] <= 0xDFFF) {
                if (result.size() + 2U > kMaximumLine - kTruncated.size())
                    break;
                result.push_back(ch);
                result.push_back(message[index++]);
                continue;
            }
            result.push_back(static_cast<wchar_t>(0xFFFD));
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            result.push_back(static_cast<wchar_t>(0xFFFD));
        } else if (ch < 0x20 || (ch >= 0x7F && ch <= 0x9F) || ch == 0x2028 || ch == 0x2029 ||
                   (ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2066 && ch <= 0x2069) ||
                   ch == 0xFEFF) {
            result.push_back(L' ');
        } else {
            result.push_back(ch);
        }
        if (result.size() > kMaximumLine - kTruncated.size())
            break;
    }
    if (result.size() > kMaximumLine - kTruncated.size() || message.size() > result.size()) {
        result.resize(std::min(result.size(), kMaximumLine - kTruncated.size()));
        if (!result.empty() && result.back() >= 0xD800 && result.back() <= 0xDBFF)
            result.pop_back();
        result += kTruncated;
    }
    return result;
}

/// @brief Append and durably flush one UTF-8 log record.
/// @param handle Open writable file handle.
/// @param record Complete record bytes.
/// @return @c true when every byte was written and the file buffer flushed.
bool appendLogRecord(HANDLE handle, std::string_view record) {
    size_t offset = 0;
    while (offset < record.size()) {
        const DWORD requested =
            static_cast<DWORD>(std::min<size_t>(record.size() - offset, MAXDWORD));
        DWORD written = 0;
        if (!WriteFile(handle, record.data() + offset, requested, &written, nullptr)) {
            return false;
        }
        if (written == 0 || written > requested) {
            SetLastError(ERROR_WRITE_FAULT);
            return false;
        }
        offset += written;
    }
    return FlushFileBuffers(handle) != FALSE;
}

} // namespace

/// @brief Strictly decode UTF-8 package text to UTF-16.
/// @param text UTF-8 bytes.
/// @return Decoded wide string.
/// @throws std::runtime_error On invalid encoding, Windows conversion failure,
///         or input beyond API limits.
std::wstring utf8ToWide(std::string_view text) {
    if (text.empty())
        return {};
    if (text.size() > static_cast<size_t>(INT_MAX))
        throw std::runtime_error("package metadata is too large for Windows text conversion");
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
        throw std::runtime_error("package metadata is not valid UTF-8");
    std::wstring out(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8,
                            MB_ERR_INVALID_CHARS,
                            text.data(),
                            static_cast<int>(text.size()),
                            out.data(),
                            required) != required) {
        throw std::runtime_error("cannot decode package metadata as UTF-8");
    }
    return out;
}

/// @brief Strictly encode UTF-16 Windows text as UTF-8.
/// @param text UTF-16 code units.
/// @return Encoded UTF-8 bytes.
/// @throws std::runtime_error On invalid surrogates, API failure, or oversized input.
std::string wideToUtf8(std::wstring_view text) {
    if (text.empty())
        return {};
    if (text.size() > static_cast<size_t>(INT_MAX))
        throw std::runtime_error("Windows text is too large for UTF-8 conversion");
    const int required = WideCharToMultiByte(CP_UTF8,
                                             WC_ERR_INVALID_CHARS,
                                             text.data(),
                                             static_cast<int>(text.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if (required <= 0)
        throw std::runtime_error("Windows text contains invalid UTF-16");
    std::string out(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            text.data(),
                            static_cast<int>(text.size()),
                            out.data(),
                            required,
                            nullptr,
                            nullptr) != required) {
        throw std::runtime_error("cannot encode Windows text as UTF-8");
    }
    return out;
}

/// @brief Format a Windows error code as compact user-facing text.
/// @param error Win32 error code.
/// @return System message without trailing punctuation/whitespace, or a numeric fallback.
std::wstring formatWindowsError(DWORD error) {
    wchar_t *buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<wchar_t *>(&buffer),
        0,
        nullptr);
    std::wstring result = length && buffer ? std::wstring(buffer, length)
                                           : L"Windows error " + std::to_wstring(error);
    if (buffer)
        LocalFree(buffer);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' ||
                               result.back() == L' ' || result.back() == L'.')) {
        result.pop_back();
    }
    return result;
}

/// @brief Quote one argument according to CommandLineToArgvW escaping rules.
/// @param argument Unquoted argument text.
/// @return Safe command-line representation, left unquoted when possible.
/// @throws std::runtime_error If slash-doubling arithmetic would overflow.
std::wstring quoteCommandLineArgument(std::wstring_view argument) {
    if (argument.empty())
        return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            if (slashes > (std::numeric_limits<size_t>::max() - 1U) / 2U)
                throw std::runtime_error("Windows command-line argument is too large");
            out.append(slashes * 2U + 1U, L'\\');
            out.push_back(L'\"');
            slashes = 0;
        } else {
            out.append(slashes, L'\\');
            slashes = 0;
            out.push_back(ch);
        }
    }
    if (slashes > std::numeric_limits<size_t>::max() / 2U)
        throw std::runtime_error("Windows command-line argument is too large");
    out.append(slashes * 2U, L'\\');
    out.push_back(L'\"');
    return out;
}

/// @brief Resolve the running installer executable path.
/// @return Absolute module path.
/// @throws std::runtime_error On API failure or paths beyond the Windows limit.
fs::path currentExecutablePath() {
    std::wstring buffer(512, L'\0');
    while (buffer.size() <= 32768) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
            throw std::runtime_error("cannot determine installer executable path");
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer);
        }
        buffer.resize(buffer.size() * 2U);
    }
    throw std::runtime_error("installer executable path exceeds the Windows limit");
}

/// @brief Build a unique default installer log path in the temporary directory.
/// @param identifier Package identifier embedded in the filename.
/// @return Path containing UTC timestamp, process ID, and monotonic tick count.
/// @throws std::runtime_error When the temporary directory or identifier conversion fails.
fs::path defaultLogPath(std::string_view identifier) {
    wchar_t tempPath[32768]{};
    const DWORD count = GetTempPathW(static_cast<DWORD>(std::size(tempPath)), tempPath);
    if (count == 0 || count >= std::size(tempPath))
        throw std::runtime_error("cannot locate the temporary directory for the installer log");
    SYSTEMTIME now{};
    GetSystemTime(&now);
    const DWORD pid = GetCurrentProcessId();
    const ULONGLONG tick = GetTickCount64();
    std::wostringstream leaf;
    leaf << L"ZannaInstaller-" << utf8ToWide(identifier) << L'-' << std::setfill(L'0')
         << std::setw(4) << now.wYear << std::setw(2) << now.wMonth << std::setw(2) << now.wDay
         << L'T' << std::setw(2) << now.wHour << std::setw(2) << now.wMinute << std::setw(2)
         << now.wSecond << L'Z' << L'-' << pid << L'-' << tick << L".log";
    return fs::path(tempPath) / leaf.str();
}

/// @brief Close the owned log handle, if open.
Logger::~Logger() {
    if (handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(handle_);
}

/// @brief Move a logger and transfer its handle and callbacks.
/// @param other Logger left with an invalid handle.
Logger::Logger(Logger &&other) noexcept
    : handle_(other.handle_), path_(std::move(other.path_)),
      progressCallback_(std::move(other.progressCallback_)),
      cancellationCallback_(std::move(other.cancellationCallback_)) {
    other.handle_ = INVALID_HANDLE_VALUE;
}

/// @brief Replace this logger by moving another logger's resources.
/// @param other Logger whose handle, path, and callbacks are transferred.
/// @return Reference to this logger.
Logger &Logger::operator=(Logger &&other) noexcept {
    if (this == &other)
        return *this;
    if (handle_ != INVALID_HANDLE_VALUE)
        CloseHandle(handle_);
    handle_ = other.handle_;
    path_ = std::move(other.path_);
    progressCallback_ = std::move(other.progressCallback_);
    cancellationCallback_ = std::move(other.cancellationCallback_);
    other.handle_ = INVALID_HANDLE_VALUE;
    return *this;
}

/// @brief Open or create an append-only UTF-8 installer log.
/// @param path Destination file, whose parent directories are created.
/// @param requireNew Require exclusive creation instead of appending an existing file.
/// @details Closes any prior handle and writes a UTF-8 BOM only to an empty file.
/// @throws std::runtime_error On directory, close, open, size, BOM, or flush failure.
void Logger::open(const fs::path &path, bool requireNew) {
    if (handle_ != INVALID_HANDLE_VALUE) {
        const HANDLE previous = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        if (!CloseHandle(previous))
            throw std::runtime_error("cannot close the previous installer log");
    }
    path_ = path;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    handle_ = CreateFileW(path.c_str(),
                          FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr,
                          requireNew ? CREATE_NEW : OPEN_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL,
                          nullptr);
    if (handle_ == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot create the installer log");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle_, &size) || size.QuadPart < 0) {
        const DWORD error = GetLastError();
        CloseHandle(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        throw std::runtime_error("cannot inspect the installer log: " +
                                 wideToUtf8(formatWindowsError(error)));
    }
    if (size.QuadPart == 0) {
        static constexpr uint8_t kUtf8Bom[] = {0xEF, 0xBB, 0xBF};
        DWORD written = 0;
        const BOOL writeOk = WriteFile(handle_, kUtf8Bom, sizeof(kUtf8Bom), &written, nullptr);
        const BOOL flushOk =
            writeOk && written == sizeof(kUtf8Bom) ? FlushFileBuffers(handle_) : FALSE;
        if (!writeOk || written != sizeof(kUtf8Bom) || !flushOk) {
            DWORD error =
                writeOk && written != sizeof(kUtf8Bom) ? ERROR_WRITE_FAULT : GetLastError();
            if (error == ERROR_SUCCESS)
                error = ERROR_WRITE_FAULT;
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("cannot initialize the installer log: " +
                                     wideToUtf8(formatWindowsError(error)));
        }
    }
}

/// @brief Append a timestamped sanitized log line and publish progress text.
/// @param level Short severity label.
/// @param message Raw user-visible message.
/// @throws std::runtime_error On encoding or durable-write failure.
void Logger::write(std::wstring_view level, std::wstring_view message) {
    if (handle_ != INVALID_HANDLE_VALUE) {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        std::wostringstream line;
        line << std::setfill(L'0') << std::setw(4) << now.wYear << L'-' << std::setw(2)
             << now.wMonth << L'-' << std::setw(2) << now.wDay << L'T' << std::setw(2) << now.wHour
             << L':' << std::setw(2) << now.wMinute << L':' << std::setw(2) << now.wSecond << L'.'
             << std::setw(3) << now.wMilliseconds << L"Z [" << level << L"] "
             << sanitizeLogLine(message) << L"\r\n";
        const std::string utf8 = wideToUtf8(line.str());
        if (!appendLogRecord(handle_, utf8)) {
            const DWORD error = GetLastError();
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error(
                "cannot append the installer log: " +
                wideToUtf8(formatWindowsError(error == ERROR_SUCCESS ? ERROR_WRITE_FAULT : error)));
        }
    }
    if (progressCallback_ && level != L"ERROR") {
        try {
            progressCallback_(message);
        } catch (...) {
            // Logging and lifecycle progress must survive a failed presentation callback.
        }
    }
}

/// @brief Write an informational record.
/// @param message Message to sanitize and append.
void Logger::info(std::wstring_view message) {
    write(L"INFO", message);
}

/// @brief Write a warning record.
/// @param message Message to sanitize and append.
void Logger::warning(std::wstring_view message) {
    write(L"WARN", message);
}

/// @brief Write an error record.
/// @param message Message to sanitize and append.
void Logger::error(std::wstring_view message) {
    write(L"ERROR", message);
}

/// @brief Replace the optional presentation progress callback.
/// @param callback Callable invoked after each record, or empty to clear it.
void Logger::setProgressCallback(std::function<void(std::wstring_view)> callback) {
    progressCallback_ = std::move(callback);
}

/// @brief Replace the optional cooperative cancellation callback.
/// @param callback Predicate queried by cancellationRequested(), or empty to clear.
void Logger::setCancellationCallback(std::function<bool()> callback) {
    cancellationCallback_ = std::move(callback);
}

/// @brief Query the current cooperative cancellation predicate.
/// @return Callback result, @c false when absent, or @c true if the callback throws.
bool Logger::cancellationRequested() const {
    if (!cancellationCallback_)
        return false;
    try {
        return cancellationCallback_();
    } catch (...) {
        /* A broken presentation callback must stop mutation, not disable the
         * installer's cooperative cancellation boundary. */
        return true;
    }
}

/// @brief Parse and cross-validate the installer's Unicode command line.
/// @param argc Number of argument pointers.
/// @param argv CommandLineToArgvW-compatible arguments.
/// @return Normalized lifecycle, UI, scope, component, integration, and worker options.
/// @throws std::runtime_error On unknown, repeated, conflicting, missing, or
///         contextually invalid options.
HostOptions parseCommandLine(int argc, wchar_t **argv) {
    HostOptions result;
    std::optional<Operation> explicitOperation;
    bool uiSpecified = false;
    bool destinationSpecified = false;
    bool logSpecified = false;
    bool outputSpecified = false;
    bool scopeSpecified = false;
    bool presetSpecified = false;
    bool elevatedWorkerSpecified = false;
    bool uninstallWorkerSpecified = false;
    bool handoffParentSpecified = false;
    bool helpRequested = false;
    /// @brief Record one mutually exclusive lifecycle operation.
    /// @param operation Requested operation.
    /// @throws std::runtime_error If any operation was already specified.
    auto setOperation = [&](Operation operation) {
        if (explicitOperation)
            throw std::runtime_error(
                *explicitOperation == operation
                    ? "installer lifecycle operation was specified more than once"
                    : "installer lifecycle operations are mutually exclusive");
        explicitOperation = operation;
        result.operation = operation;
    };
    /// @brief Record one mutually exclusive installer UI level.
    /// @param level Requested UI level.
    /// @throws std::runtime_error If a UI level was already specified.
    auto setUiLevel = [&](UiLevel level) {
        if (uiSpecified)
            throw std::runtime_error(result.uiLevel == level
                                         ? "installer UI level was specified more than once"
                                         : "/quiet and /passive cannot be combined");
        uiSpecified = true;
        result.uiLevel = level;
    };
    /// @brief Record an explicit enabled or disabled integration option.
    /// @param[in,out] field Optional state to assign.
    /// @param value Requested Boolean state.
    /// @param name Option name used in diagnostics.
    /// @throws std::runtime_error If the integration option was already specified.
    auto setIntegration = [](std::optional<bool> &field, bool value, std::string_view name) {
        if (field)
            throw std::runtime_error(
                *field == value
                    ? "installer integration option was specified more than once for " +
                          std::string(name)
                    : "conflicting installer integration options for " + std::string(name));
        field = value;
    };
    /// @brief Record the requested installation scope.
    /// @param scope User or machine scope selected by the command line.
    /// @throws std::runtime_error If a scope was already specified.
    auto setScope = [&](InstallScope scope) {
        if (scopeSpecified)
            throw std::runtime_error(result.scope == scope ? "/scope was specified more than once"
                                                           : "conflicting /scope values");
        scopeSpecified = true;
        result.scope = scope;
    };
    /// @brief Record the requested component-selection preset.
    /// @param preset Preset selected by `/type` or `/preset`.
    /// @throws std::runtime_error If a preset was already specified.
    auto setPreset = [&](ComponentPreset preset) {
        if (presetSpecified)
            throw std::runtime_error(result.componentPreset == preset
                                         ? "/type or /preset was specified more than once"
                                         : "conflicting /type or /preset values");
        presetSpecified = true;
        result.componentPreset = preset;
    };
    /// @brief Set one non-repeatable Boolean command-line flag.
    /// @param[in,out] field Flag state to set.
    /// @param name Option name used in diagnostics.
    /// @throws std::runtime_error If the flag is already set.
    auto setFlag = [](bool &field, std::string_view name) {
        if (field)
            throw std::runtime_error(std::string(name) + " was specified more than once");
        field = true;
    };
    for (int i = 1; i < argc; ++i) {
        const std::wstring original(argv[i]);
        const std::wstring arg = lowerWide(original);
        if (arg == L"/?" || arg == L"/help" || arg == L"--help" || arg == L"-h") {
            setFlag(helpRequested, "installer help option");
        } else if (arg == L"/quiet" || arg == L"/silent") {
            setUiLevel(UiLevel::Quiet);
        } else if (arg == L"/passive") {
            setUiLevel(UiLevel::Passive);
        } else if (arg == L"/install") {
            setOperation(Operation::Install);
        } else if (arg == L"/modify") {
            setOperation(Operation::Modify);
        } else if (arg == L"/repair") {
            setOperation(Operation::Repair);
        } else if (arg == L"/uninstall") {
            setOperation(Operation::Uninstall);
        } else if (arg == L"/inspect") {
            setOperation(Operation::Inspect);
        } else if (arg == L"/checkforupdates" || arg == L"/check-updates") {
            setOperation(Operation::CheckUpdates);
        } else if (arg == L"/selftest" || arg == L"/self-test") {
            setOperation(Operation::SelfTest);
        } else if (arg == L"/allowdowngrade") {
            setFlag(result.allowDowngrade, "/allowDowngrade");
        } else if (arg == L"/norestart") {
            setFlag(result.noRestart, "/noRestart");
        } else if (arg == L"/closeapplications" || arg == L"/closeapps") {
            setFlag(result.closeApplications, "/closeApplications");
        } else if (arg == L"/addtopath" || arg == L"/path") {
            setIntegration(result.addToPath, true, "PATH");
        } else if (arg == L"/nopath" || arg == L"/no-path") {
            setIntegration(result.addToPath, false, "PATH");
        } else if (arg == L"/associations") {
            setIntegration(result.registerAssociations, true, "file associations");
        } else if (arg == L"/noassociations" || arg == L"/no-associations") {
            setIntegration(result.registerAssociations, false, "file associations");
        } else if (arg == L"/shortcuts") {
            setIntegration(result.createShortcuts, true, "shortcuts");
        } else if (arg == L"/noshortcuts" || arg == L"/no-shortcuts") {
            setIntegration(result.createShortcuts, false, "shortcuts");
        } else if (arg == L"/elevated-worker") {
            if (elevatedWorkerSpecified)
                throw std::runtime_error("/elevated-worker was specified more than once");
            elevatedWorkerSpecified = true;
            result.elevatedWorker = true;
        } else if (arg == L"/uninstall-worker") {
            if (uninstallWorkerSpecified)
                throw std::runtime_error("/uninstall-worker was specified more than once");
            uninstallWorkerSpecified = true;
            result.uninstallWorker = true;
        } else if (arg == L"/launch-ide") {
            setFlag(result.launchIDE, "/launch-ide");
        } else if (arg == L"/launch-prompt") {
            setFlag(result.launchPrompt, "/launch-prompt");
        } else if (arg == L"/open-quickstart") {
            setFlag(result.openQuickstart, "/open-quickstart");
        } else if (arg == L"/open-samples") {
            setFlag(result.openSamples, "/open-samples");
        } else if (const auto scopeValue = optionValue(original, arg, L"/scope"); scopeValue) {
            if (scopeValue->empty())
                throw std::runtime_error("/scope requires a non-empty value");
            const std::wstring lower = lowerWide(*scopeValue);
            if (lower == L"user" || lower == L"currentuser")
                setScope(InstallScope::User);
            else if (lower == L"machine" || lower == L"allusers")
                setScope(InstallScope::Machine);
            else
                throw std::runtime_error("/scope must be user or machine");
        } else if (const auto installDirValue = optionValue(original, arg, L"/installdir");
                   installDirValue) {
            if (installDirValue->empty())
                throw std::runtime_error("/installDir requires a non-empty path");
            if (destinationSpecified)
                throw std::runtime_error("/installDir was specified more than once");
            destinationSpecified = true;
            result.destination = *installDirValue;
        } else if (const auto logValue = optionValue(original, arg, L"/log"); logValue) {
            if (logValue->empty())
                throw std::runtime_error("/log requires a non-empty path");
            if (logSpecified)
                throw std::runtime_error("/log was specified more than once");
            logSpecified = true;
            result.logPath = *logValue;
        } else if (const auto outputValue = optionValue(original, arg, L"/output"); outputValue) {
            if (outputValue->empty())
                throw std::runtime_error("/output requires a non-empty path");
            if (outputSpecified)
                throw std::runtime_error("/output was specified more than once");
            outputSpecified = true;
            result.outputPath = *outputValue;
        } else if (const auto componentsValue = optionValue(original, arg, L"/components");
                   componentsValue) {
            if (result.componentsSpecified)
                throw std::runtime_error("/components was specified more than once");
            result.selectedComponents = parseComponents(*componentsValue);
            result.componentsSpecified = true;
        } else if (const auto typeValue = optionValue(original, arg, L"/type"); typeValue) {
            setPreset(parseComponentPreset(*typeValue));
        } else if (const auto presetValue = optionValue(original, arg, L"/preset"); presetValue) {
            setPreset(parseComponentPreset(*presetValue));
        } else if (const auto handoffValue = optionValue(original, arg, L"/handoff-parent");
                   handoffValue) {
            if (handoffParentSpecified)
                throw std::runtime_error("/handoff-parent was specified more than once");
            result.handoffParentId = parseHandoffProcessId(*handoffValue);
            handoffParentSpecified = true;
        } else if (arg == L"/scope" || arg == L"/installdir" || arg == L"/log" ||
                   arg == L"/output" || arg == L"/components" || arg == L"/type" ||
                   arg == L"/preset" || arg == L"/handoff-parent") {
            if (++i >= argc)
                throw std::runtime_error("installer option is missing its value");
            const std::wstring separatedValue(argv[i]);
            if (separatedValue.empty())
                throw std::runtime_error("installer option requires a non-empty value");
            if (arg == L"/scope") {
                const std::wstring lower = lowerWide(separatedValue);
                if (lower == L"user" || lower == L"currentuser")
                    setScope(InstallScope::User);
                else if (lower == L"machine" || lower == L"allusers")
                    setScope(InstallScope::Machine);
                else
                    throw std::runtime_error("/scope must be user or machine");
            } else if (arg == L"/installdir") {
                if (destinationSpecified)
                    throw std::runtime_error("/installDir was specified more than once");
                destinationSpecified = true;
                result.destination = separatedValue;
            } else if (arg == L"/log") {
                if (logSpecified)
                    throw std::runtime_error("/log was specified more than once");
                logSpecified = true;
                result.logPath = separatedValue;
            } else if (arg == L"/output") {
                if (outputSpecified)
                    throw std::runtime_error("/output was specified more than once");
                outputSpecified = true;
                result.outputPath = separatedValue;
            } else if (arg == L"/handoff-parent") {
                if (handoffParentSpecified)
                    throw std::runtime_error("/handoff-parent was specified more than once");
                handoffParentSpecified = true;
                result.handoffParentId = parseHandoffProcessId(separatedValue);
            } else if (arg == L"/type" || arg == L"/preset") {
                setPreset(parseComponentPreset(separatedValue));
            } else {
                if (result.componentsSpecified)
                    throw std::runtime_error("/components was specified more than once");
                result.selectedComponents = parseComponents(separatedValue);
                result.componentsSpecified = true;
            }
        } else {
            throw std::runtime_error("unknown installer option: " + wideToUtf8(original));
        }
    }
    if (result.componentsSpecified && result.componentPreset != ComponentPreset::Unspecified)
        throw std::runtime_error("/components cannot be combined with /type or /preset");
    if (result.uninstallWorker != (result.handoffParentId != 0))
        throw std::runtime_error("/uninstall-worker and /handoff-parent must be supplied together");
    if (result.elevatedWorker && result.uninstallWorker)
        throw std::runtime_error("elevated and maintenance-handoff worker modes are exclusive");
    if (helpRequested && explicitOperation)
        throw std::runtime_error("installer help cannot be combined with a lifecycle operation");
    if (helpRequested)
        result.operation = Operation::Help;
    if (outputSpecified && result.outputPath.empty())
        throw std::runtime_error("/output requires a non-empty path");
    if (outputSpecified && result.operation != Operation::Inspect &&
        result.operation != Operation::CheckUpdates) {
        throw std::runtime_error("/output is valid only with /inspect or /checkForUpdates");
    }
    if (result.uiLevel == UiLevel::Quiet && result.operation == Operation::Help)
        result.uiLevel = UiLevel::Full;
    return result;
}

/// @brief Return the complete native installer command-line reference.
/// @return Static help text describing operations, options, and process exit codes.
std::wstring commandLineHelp() {
    return LR"HELP(Zanna Tools Installer

Usage:
  setup.exe [/install|/modify|/repair|/uninstall|/inspect|/checkForUpdates] [options]

Options:
  /quiet, /silent          No user interface
  /passive                 Progress interface without prompts
  /scope user|machine      Install for the current user or all users
  /installDir <path>       Validated custom installation directory
  /components <ids>        Comma-separated component identifiers
  /type <preset>           minimal, typical (recommended), sdk, or complete
  /addToPath, /noPath      Enable or disable the owned PATH entry
  /associations            Register safe Open With entries
  /noAssociations          Do not register Open With entries
  /shortcuts, /noShortcuts Enable or disable packaged shortcuts
  /allowDowngrade          Explicitly permit installing an older version
  /closeApplications       Ask Restart Manager to close eligible applications
  /log <path>              Write the redacted UTF-8 session log to this path
  /output <path>           Atomically write inspection or update JSON to this path
  /norestart               Never restart applications or the computer
  /inspect                 Print verified package metadata as JSON
  /checkForUpdates         Check the pinned, signed update manifest (when configured)
  /?                       Show this help

Exit codes:
  0 success; 87 invalid arguments; 1602 cancelled; 1603 fatal error;
  1618 another lifecycle operation is active; 1638 newer version installed;
  3010 success with restart required.
)HELP";
}

/// @brief Load and verify an installer executable and every owned overlay artifact.
/// @param executablePath Installer image to read.
/// @return Fully owned executable, metadata, payload, cleanup helper, text, and
///         outer-file buffers.
/// @throws std::runtime_error On executable/ZIP structure, PE architecture,
///         inventory, extraction, size, or SHA-256 verification failure.
HostPackage loadHostPackage(const fs::path &executablePath) {
    HostPackage package;
    package.executablePath = executablePath;
    package.executableBytes = readWholeFile(executablePath);
    package.executableSha256 =
        zanna::pkg::sha256Hex(package.executableBytes.data(), package.executableBytes.size());
    const OverlayRange overlay = locateZipOverlay(package.executableBytes);
    package.overlayOffset = overlay.offset;
    package.overlayLength = overlay.length;
    zanna::pkg::ZipReader outer(package.executableBytes.data() + overlay.offset, overlay.length);
    const zanna::pkg::ZipEntry *metadataEntry = outer.find("meta/installer-v2.txt");
    if (!metadataEntry)
        throw std::runtime_error("installer metadata entry is missing");
    const std::vector<uint8_t> metadataBytes = outer.extract(*metadataEntry);
    package.metadata = zanna::pkg::parseWindowsInstallerMetadata(bytesToString(metadataBytes));
    (void)compareInstallerVersions(package.metadata.version, package.metadata.version);
    requirePeArchitecture(
        package.executableBytes, package.metadata.architecture, "installer executable");
    requireOuterInventory(outer, package.metadata);
    const zanna::pkg::ZipEntry *payloadEntry = outer.find(package.metadata.payloadEntry);
    if (!payloadEntry)
        throw std::runtime_error("installer payload entry is missing");
    package.payloadZip = outer.extract(*payloadEntry);
    const zanna::pkg::ZipEntry *cleanupEntry = outer.find(package.metadata.cleanupEntry);
    if (!cleanupEntry)
        throw std::runtime_error("installer detached cleanup helper is missing");
    package.cleanupBytes = outer.extract(*cleanupEntry);
    if (zanna::pkg::sha256Hex(package.cleanupBytes.data(), package.cleanupBytes.size()) !=
        package.metadata.cleanupSha256) {
        throw std::runtime_error("installer detached cleanup helper SHA-256 verification failed");
    }
    requirePeArchitecture(
        package.cleanupBytes, package.metadata.architecture, "installer cleanup helper");

    zanna::pkg::ZipReader payload(package.payloadZip.data(), package.payloadZip.size());
    if (payload.entries().size() != package.metadata.payloadFiles.size())
        throw std::runtime_error("installer payload entry count does not match metadata");
    for (const auto &file : package.metadata.payloadFiles) {
        const zanna::pkg::ZipEntry *entry = payload.find(file.path);
        if (!entry || entry->uncompressedSize != file.sizeBytes)
            throw std::runtime_error("installer payload inventory does not match metadata");
        const std::vector<uint8_t> bytes = payload.extract(*entry);
        if (zanna::pkg::sha256Hex(bytes.data(), bytes.size()) != file.sha256)
            throw std::runtime_error("installer payload SHA-256 verification failed");
    }

    if (const zanna::pkg::ZipEntry *license = outer.find(package.metadata.licenseEntry))
        package.licenseText = bytesToString(outer.extract(*license));
    if (const zanna::pkg::ZipEntry *readme = outer.find(package.metadata.readmeEntry))
        package.readmeText = bytesToString(outer.extract(*readme));
    for (const auto &file : package.metadata.outerFiles) {
        const zanna::pkg::ZipEntry *entry = outer.find(file.overlayPath);
        if (!entry || entry->uncompressedSize != file.sizeBytes)
            throw std::runtime_error(
                "installer outer-file payload is missing or has the wrong size");
        std::vector<uint8_t> bytes = outer.extract(*entry);
        if (zanna::pkg::sha256Hex(bytes.data(), bytes.size()) != file.sha256)
            throw std::runtime_error("installer outer-file SHA-256 verification failed");
        if (file.path == package.metadata.uninstallerRelativePath)
            requirePeArchitecture(bytes, package.metadata.architecture, "maintenance executable");
        package.outerFileBytes.emplace(file.overlayPath, std::move(bytes));
    }
    return package;
}

/// @brief Render deterministic inspection JSON from a verified host package.
/// @param package Fully verified package returned by loadHostPackage().
/// @return UTF-16 JSON document containing identity, update, payload, and component metadata.
std::wstring inspectPackageJson(const HostPackage &package) {
    const auto &m = package.metadata;
    std::ostringstream out;
    out << "{\n"
        << "  \"schema_version\": " << m.schemaVersion << ",\n"
        << "  \"mode\": \"" << jsonEscape(m.packageMode) << "\",\n"
        << "  \"kind\": \"" << jsonEscape(m.productKind) << "\",\n"
        << "  \"identifier\": \"" << jsonEscape(m.identifier) << "\",\n"
        << "  \"display_name\": \"" << jsonEscape(m.displayName) << "\",\n"
        << "  \"version\": \"" << jsonEscape(m.version) << "\",\n"
        << "  \"architecture\": \"" << jsonEscape(m.architecture) << "\",\n"
        << "  \"channel\": \"" << jsonEscape(m.channel) << "\",\n"
        << "  \"default_scope\": \"" << jsonEscape(m.defaultScope) << "\",\n"
        << "  \"default_install_dir\": \"" << jsonEscape(m.defaultInstallDir) << "\",\n"
        << "  \"minimum_windows_version\": \"" << jsonEscape(m.minimumWindowsVersion) << "\",\n"
        << "  \"source_commit\": \"" << jsonEscape(m.commit) << "\",\n"
        << "  \"package_sha256\": \"" << jsonEscape(package.executableSha256) << "\",\n"
        << "  \"signed_update_discovery\": " << (!m.updateManifestUrl.empty() ? "true" : "false")
        << ",\n"
        << "  \"payload_files\": " << m.payloadFiles.size() << ",\n"
        << "  \"installed_size\": " << m.installedSizeBytes << ",\n"
        << "  \"components\": [";
    for (size_t i = 0; i < m.components.size(); ++i) {
        if (i)
            out << ", ";
        out << "\"" << jsonEscape(m.components[i].id) << "\"";
    }
    out << "]\n}\n";
    return utf8ToWide(out.str());
}

} // namespace zanna::installer
