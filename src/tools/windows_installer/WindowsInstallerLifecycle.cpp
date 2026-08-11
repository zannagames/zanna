//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerLifecycle.cpp
// Purpose: Implement transactional Windows install, maintenance, and removal.
// Key invariants:
//   - Verified payloads move through recoverable same-volume transaction states.
//   - Path, ownership, OS-version, integration, and concurrency checks fail closed.
// Ownership/Lifetime:
//   - RAII wrappers own every handle, registry key, mutex, and Restart Manager session.
//   - Transaction artifacts persist only when needed for deterministic recovery.
// Links: src/tools/windows_installer/WindowsInstallerHost.hpp,
//        src/tools/common/packaging/WindowsInstallerMetadata.hpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerHost.hpp"
#include "WindowsInstallerWizard.hpp"

#include "PkgHash.hpp"
#include "ZipReader.hpp"
#include "common/Filesystem.hpp"

#include <restartmanager.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace zanna::installer {
namespace {

constexpr wchar_t kUninstallBase[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\";
constexpr wchar_t kUserEnvironment[] = L"Environment";
constexpr wchar_t kMachineEnvironment[] =
    L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment";
constexpr wchar_t kClassesBase[] = L"Software\\Classes\\";
constexpr wchar_t kManifestHeader[] = L"ZANNA-INSTALL-MANIFEST\t2";
constexpr wchar_t kStateHeader[] = L"ZANNA-INSTALL-STATE\t2";

/// @brief Enforce cooperative cancellation at a safe lifecycle boundary.
/// @param logger Session logger holding the cancellation predicate.
/// @throws InstallerError With kExitUserCancelled when cancellation is requested.
void cancellationPoint(Logger &logger) {
    if (logger.cancellationRequested())
        throw InstallerError(kExitUserCancelled, "installation was cancelled by the user");
}

class UniqueHandle {
  public:
    /// @brief Construct an empty handle owner.
    UniqueHandle() = default;

    /// @brief Adopt a Win32 handle.
    /// @param handle Handle closed by this object unless released.
    explicit UniqueHandle(HANDLE handle) : handle_(handle) {}

    /// @brief Close the adopted handle.
    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle &) = delete;
    UniqueHandle &operator=(const UniqueHandle &) = delete;

    /// @brief Move-construct by releasing another owner.
    /// @param other Owner left empty.
    UniqueHandle(UniqueHandle &&other) noexcept : handle_(other.release()) {}

    /// @brief Move-assign and close any current handle.
    /// @param other Owner whose handle is transferred.
    /// @return Reference to this owner.
    UniqueHandle &operator=(UniqueHandle &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    /// @brief Return the borrowed native handle.
    /// @return Stored handle without transferring ownership.
    HANDLE get() const {
        return handle_;
    }

    /// @brief Test whether the owner contains a closable handle.
    /// @return @c true for nonnull, non-invalid handles.
    explicit operator bool() const {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    /// @brief Relinquish ownership without closing.
    /// @return Former handle; this object becomes empty.
    HANDLE release() {
        const HANDLE result = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return result;
    }

    /// @brief Close the current handle and optionally adopt a replacement.
    /// @param replacement New handle, defaulting to the invalid sentinel.
    void reset(HANDLE replacement = INVALID_HANDLE_VALUE) {
        if (*this)
            CloseHandle(handle_);
        handle_ = replacement;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

template <typename T> class ComPtr {
  public:
    /// @brief Construct an empty COM interface owner.
    ComPtr() = default;

    /// @brief Release the owned interface.
    ~ComPtr() {
        if (value_)
            value_->Release();
    }

    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    /// @brief Prepare an out-parameter after releasing any current interface.
    /// @return Address of the internal interface pointer.
    T **put() {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
        return &value_;
    }

    /// @brief Access the borrowed COM interface.
    /// @return Stored interface pointer.
    T *operator->() const {
        return value_;
    }

    /// @brief Test whether an interface is owned.
    /// @return @c true when nonnull.
    explicit operator bool() const {
        return value_ != nullptr;
    }

  private:
    T *value_{nullptr};
};

class RegKey {
  public:
    /// @brief Construct an empty registry-key owner.
    RegKey() = default;

    /// @brief Adopt an open registry key.
    /// @param key Key closed unless released.
    explicit RegKey(HKEY key) : key_(key) {}

    /// @brief Close the adopted key.
    ~RegKey() {
        reset();
    }

    RegKey(const RegKey &) = delete;
    RegKey &operator=(const RegKey &) = delete;

    /// @brief Move-construct by releasing another key owner.
    /// @param other Owner left empty.
    RegKey(RegKey &&other) noexcept : key_(other.release()) {}

    /// @brief Move-assign and close any current key.
    /// @param other Owner whose key is transferred.
    /// @return Reference to this owner.
    RegKey &operator=(RegKey &&other) noexcept {
        if (this != &other)
            reset(other.release());
        return *this;
    }

    /// @brief Return the borrowed registry handle.
    /// @return Stored key without ownership transfer.
    HKEY get() const {
        return key_;
    }

    /// @brief Test whether a registry key is owned.
    /// @return @c true when nonnull.
    explicit operator bool() const {
        return key_ != nullptr;
    }

    /// @brief Relinquish ownership without closing.
    /// @return Former key; this owner becomes empty.
    HKEY release() {
        const HKEY result = key_;
        key_ = nullptr;
        return result;
    }

    /// @brief Close the current key and optionally adopt another.
    /// @param replacement New key, defaulting to null.
    void reset(HKEY replacement = nullptr) {
        if (key_)
            RegCloseKey(key_);
        key_ = replacement;
    }

  private:
    HKEY key_{nullptr};
};

struct InstalledRecord {
    bool present{false};
    InstallScope scope{InstallScope::User};
    fs::path installRoot;
    fs::path cacheExecutable;
    std::string version;
    std::set<std::string> components;
    std::wstring pathEntry;
    std::vector<fs::path> shortcuts;
    bool settingsPresent{false};
    bool addToPath{false};
    bool registerAssociations{false};
    bool createShortcuts{false};
};

struct SelectedFile {
    std::string path;
    std::string sha256;
    uint64_t sizeBytes{0};
};

struct InstallationPlan {
    Operation operation{Operation::Install};
    InstallScope scope{InstallScope::User};
    fs::path installRoot;
    fs::path cacheExecutable;
    std::set<std::string> components;
    std::vector<SelectedFile> files;
    uint64_t selectedSizeBytes{0};
    bool addToPath{false};
    bool registerAssociations{false};
    bool createShortcuts{false};
    InstalledRecord existing;
};

/// @brief Compare UTF-16 strings with Windows ordinal case-insensitive semantics.
/// @param left First string.
/// @param right Second string.
/// @return @c true when equal under CompareStringOrdinal.
bool ordinalEqualsIgnoreCase(std::wstring_view left, std::wstring_view right) {
    if (left.size() != right.size() || left.size() > static_cast<size_t>(INT_MAX))
        return false;
    if (left.empty())
        return true;
    return CompareStringOrdinal(left.data(),
                                static_cast<int>(left.size()),
                                right.data(),
                                static_cast<int>(right.size()),
                                TRUE) == CSTR_EQUAL;
}

/// @brief Fold UTF-16 text to invariant Windows lowercase.
/// @param value Text to normalize.
/// @return Case-folded copy.
/// @throws std::runtime_error On API limits or mapping failure.
std::wstring foldWindowsCase(std::wstring_view value) {
    if (value.empty())
        return {};
    if (value.size() > static_cast<size_t>(INT_MAX))
        throw std::runtime_error("Windows path is too long to case-fold safely");
    const int length = static_cast<int>(value.size());
    const int required = LCMapStringEx(LOCALE_NAME_INVARIANT,
                                       LCMAP_LOWERCASE,
                                       value.data(),
                                       length,
                                       nullptr,
                                       0,
                                       nullptr,
                                       nullptr,
                                       0);
    if (required <= 0)
        throw std::runtime_error("cannot case-fold a Windows path");
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (LCMapStringEx(LOCALE_NAME_INVARIANT,
                      LCMAP_LOWERCASE,
                      value.data(),
                      length,
                      result.data(),
                      required,
                      nullptr,
                      nullptr,
                      0) != required) {
        throw std::runtime_error("cannot case-fold a Windows path");
    }
    return result;
}

/// @brief Lexically normalize a path and use preferred Windows separators.
/// @param path Input filesystem path.
/// @return Native normalized path text.
std::wstring normalizedWindowsPathText(const fs::path &path) {
    fs::path normalized = path.lexically_normal();
    normalized.make_preferred();
    return normalized.wstring();
}

/// @brief Compare two path spellings using Windows ordinal case rules.
/// @param left First path.
/// @param right Second path.
/// @return @c true when normalized spellings compare equal.
bool sameWindowsPath(const fs::path &left, const fs::path &right) {
    return ordinalEqualsIgnoreCase(normalizedWindowsPathText(left),
                                   normalizedWindowsPathText(right));
}

/// @brief Test whether a normalized path equals or lies beneath a root.
/// @param candidate Candidate path.
/// @param root Required ancestor root.
/// @return @c true on component-boundary containment under ordinal case folding.
bool windowsPathBeginsWith(const fs::path &candidate, const fs::path &root) {
    const std::wstring value = normalizedWindowsPathText(candidate);
    std::wstring prefix = normalizedWindowsPathText(root);
    while (prefix.size() > 3U && (prefix.back() == L'\\' || prefix.back() == L'/'))
        prefix.pop_back();
    if (prefix.empty() || value.size() < prefix.size())
        return false;
    const std::wstring_view leading(value.data(), prefix.size());
    if (!ordinalEqualsIgnoreCase(leading, prefix))
        return false;
    if (value.size() == prefix.size())
        return true;
    return value[prefix.size()] == L'\\' || value[prefix.size()] == L'/';
}

/// @brief View a fixed wide buffer only when it contains a terminator.
/// @tparam Size Compile-time buffer capacity.
/// @param buffer Fixed buffer to inspect.
/// @return View before the first NUL, or nullopt when unterminated.
template <size_t Size>
std::optional<std::wstring_view> terminatedWideView(const std::array<wchar_t, Size> &buffer) {
    const auto terminator = std::find(buffer.begin(), buffer.end(), L'\0');
    if (terminator == buffer.end())
        return std::nullopt;
    return std::wstring_view(buffer.data(), static_cast<size_t>(terminator - buffer.begin()));
}

/// @brief Fold ASCII letters in a byte string to lowercase.
/// @param value Text to transform.
/// @return Folded copy.
std::string lowerAscii(std::string value) {
    /// @brief Fold one ASCII uppercase byte to lowercase.
    /// @param ch Byte to normalize.
    /// @return Lowercase ASCII byte or the unchanged input.
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) -> char {
        return static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch + ('a' - 'A') : ch);
    });
    return value;
}

/// @brief Test whether a byte is an ASCII letter.
/// @param ch Candidate byte.
/// @return @c true for A-Z or a-z.
bool isAsciiAlpha(unsigned char ch) {
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

/// @brief Normalize path separators to forward slashes.
/// @param value UTF-8 path text.
/// @return Normalized copy.
std::string slashPath(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}

/// @brief Decode a UTF-8 path and normalize separators to backslashes.
/// @param value UTF-8 path text.
/// @return Native Windows path text.
std::wstring backslashPath(std::string_view value) {
    std::wstring result = utf8ToWide(value);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
}

/// @brief Build a case-insensitive manifest path key.
/// @param value UTF-8 path spelling.
/// @return Lowercase forward-slash spelling.
std::string normalizedPathKey(std::string value) {
    value = slashPath(std::move(value));
    return lowerAscii(std::move(value));
}

/// @brief Validate a stable install-relative package path.
/// @param path UTF-8 relative path.
/// @throws std::runtime_error For absolute/traversing paths, empty components,
///         forbidden characters, trailing ambiguity, or reserved devices.
void validateRelativePath(std::string_view path) {
    if (path.empty() || path.size() >= 32760 || path.front() == '/' || path.front() == '\\' ||
        (path.size() >= 2 && isAsciiAlpha(static_cast<unsigned char>(path[0])) && path[1] == ':')) {
        throw std::runtime_error("unsafe install-relative path");
    }
    size_t start = 0;
    while (start <= path.size()) {
        const size_t end = path.find_first_of("/\\", start);
        const std::string_view segment =
            path.substr(start, end == std::string_view::npos ? path.size() - start : end - start);
        if (segment.empty() || segment == "." || segment == "..")
            throw std::runtime_error("unsafe install-relative path");
        if (segment.back() == '.' || segment.back() == ' ')
            throw std::runtime_error("install-relative path has an unsafe trailing character");
        for (unsigned char ch : segment) {
            if (ch < 0x20U || ch == 0x7FU || ch == ':' || ch == '*' || ch == '?' || ch == '"' ||
                ch == '<' || ch == '>' || ch == '|') {
                throw std::runtime_error("unsafe character in install-relative path");
            }
        }
        const size_t dot = segment.find('.');
        const std::string base = lowerAscii(
            std::string(segment.substr(0, dot == std::string_view::npos ? segment.size() : dot)));
        const bool numberedDevice = base.size() == 4U &&
                                    (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
                                    base[3] >= '1' && base[3] <= '9';
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" || numberedDevice) {
            throw std::runtime_error("install-relative path uses a reserved Windows device name");
        }
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
}

/// @brief Append a validated UTF-8 relative path component by component.
/// @param root Trusted destination root.
/// @param relative Untrusted package-relative path.
/// @return Joined native path.
/// @throws std::runtime_error When validation or UTF conversion fails.
fs::path safeJoin(const fs::path &root, std::string_view relative) {
    validateRelativePath(relative);
    fs::path result = root;
    size_t start = 0;
    while (start <= relative.size()) {
        const size_t end = relative.find_first_of("/\\", start);
        const std::string_view segment = relative.substr(
            start, end == std::string_view::npos ? relative.size() - start : end - start);
        result /= utf8ToWide(segment);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return result;
}

/// @brief Resolve a required Windows known folder.
/// @param id Known-folder identifier.
/// @return Nonempty filesystem path.
/// @throws std::runtime_error When shell resolution fails.
std::wstring knownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(result) || !raw || !*raw) {
        if (raw)
            CoTaskMemFree(raw);
        throw std::runtime_error("cannot resolve a required Windows known folder");
    }
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

/// @brief Select the registry hive for an installation scope.
/// @param scope User or machine scope.
/// @return HKEY_CURRENT_USER or HKEY_LOCAL_MACHINE.
HKEY rootKey(InstallScope scope) {
    return scope == InstallScope::User ? HKEY_CURRENT_USER : HKEY_LOCAL_MACHINE;
}

/// @brief Build the ARP uninstall subkey for a package identifier.
/// @param identifier UTF-8 package identifier.
/// @return Full relative registry subkey.
std::wstring uninstallSubkey(std::string_view identifier) {
    return std::wstring(kUninstallBase) + utf8ToWide(identifier);
}

/// @brief Open or create a registry key with requested access.
/// @param root Registry hive/root key.
/// @param subkey Relative key path.
/// @param access Desired access mask.
/// @param create Whether to create a missing key.
/// @return Owned key, or empty when opening a missing key without creation.
/// @throws std::runtime_error On other registry failures.
RegKey openKey(HKEY root, std::wstring_view subkey, REGSAM access, bool create) {
    HKEY key = nullptr;
    LONG result = ERROR_SUCCESS;
    if (create) {
        result = RegCreateKeyExW(root,
                                 std::wstring(subkey).c_str(),
                                 0,
                                 nullptr,
                                 REG_OPTION_NON_VOLATILE,
                                 access,
                                 nullptr,
                                 &key,
                                 nullptr);
    } else {
        result = RegOpenKeyExW(root, std::wstring(subkey).c_str(), 0, access, &key);
    }
    if (result != ERROR_SUCCESS) {
        if (key)
            RegCloseKey(key);
        if (result == ERROR_FILE_NOT_FOUND && !create)
            return {};
        throw std::runtime_error("Windows registry operation failed: " +
                                 wideToUtf8(formatWindowsError(static_cast<DWORD>(result))));
    }
    if (!key)
        throw std::runtime_error("Windows registry operation returned no key");
    return RegKey(key);
}

/// @brief Read a bounded REG_SZ or REG_EXPAND_SZ value robustly.
/// @param key Open registry key.
/// @param name Value name, empty for the default value.
/// @return Terminated string without its trailing NUL, or nullopt when absent.
/// @throws std::runtime_error On malformed, changing, oversized, or unreadable data.
std::optional<std::wstring> queryRegistryString(HKEY key, std::wstring_view name) {
    const std::wstring valueName(name);
    const wchar_t *nativeName = name.empty() ? nullptr : valueName.c_str();
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        DWORD type = 0;
        DWORD size = 0;
        LONG result = RegQueryValueExW(key, nativeName, nullptr, &type, nullptr, &size);
        if (result == ERROR_FILE_NOT_FOUND)
            return std::nullopt;
        if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
            size > 32U * 1024U * 1024U || size % sizeof(wchar_t) != 0U)
            throw std::runtime_error("Windows registry string is unreadable or malformed");
        std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1U, L'\0');
        DWORD readSize = size;
        DWORD readType = 0;
        result = RegQueryValueExW(key,
                                  nativeName,
                                  nullptr,
                                  &readType,
                                  reinterpret_cast<BYTE *>(buffer.data()),
                                  &readSize);
        if (result == ERROR_MORE_DATA)
            continue;
        if (result != ERROR_SUCCESS || (readType != REG_SZ && readType != REG_EXPAND_SZ) ||
            readSize > size || readSize % sizeof(wchar_t) != 0U)
            throw std::runtime_error("Windows registry string changed or became malformed");
        const size_t readCharacters = static_cast<size_t>(readSize) / sizeof(wchar_t);
        const size_t length =
            std::find(buffer.begin(), buffer.begin() + readCharacters, L'\0') - buffer.begin();
        return std::wstring(buffer.data(), length);
    }
    throw std::runtime_error("Windows registry string changed repeatedly while being read");
}

/// @brief Write a validated terminated registry string.
/// @param key Open writable key.
/// @param name Value name, empty for the default value.
/// @param value NUL-free string payload.
/// @param type REG_SZ or REG_EXPAND_SZ.
/// @throws std::runtime_error On invalid input or registry failure.
void setRegistryString(HKEY key,
                       std::wstring_view name,
                       std::wstring_view value,
                       DWORD type = REG_SZ) {
    if (!key || (type != REG_SZ && type != REG_EXPAND_SZ) ||
        value.find(L'\0') != std::wstring_view::npos ||
        value.size() > static_cast<size_t>(MAXDWORD) / sizeof(wchar_t) - 1U) {
        throw std::runtime_error("refusing to write an invalid Windows registry string");
    }
    const DWORD bytes = static_cast<DWORD>((value.size() + 1U) * sizeof(wchar_t));
    const LONG result = RegSetValueExW(key,
                                       name.empty() ? nullptr : std::wstring(name).c_str(),
                                       0,
                                       type,
                                       reinterpret_cast<const BYTE *>(value.data()),
                                       bytes);
    if (result != ERROR_SUCCESS)
        throw std::runtime_error("cannot write Windows registry string: " +
                                 wideToUtf8(formatWindowsError(static_cast<DWORD>(result))));
}

/// @brief Write one REG_DWORD value.
/// @param key Open writable key.
/// @param name Value name.
/// @param value Numeric payload.
/// @throws std::runtime_error On registry failure.
void setRegistryDword(HKEY key, std::wstring_view name, DWORD value) {
    const LONG result = RegSetValueExW(key,
                                       std::wstring(name).c_str(),
                                       0,
                                       REG_DWORD,
                                       reinterpret_cast<const BYTE *>(&value),
                                       sizeof(value));
    if (result != ERROR_SUCCESS)
        throw std::runtime_error("cannot write Windows registry value: " +
                                 wideToUtf8(formatWindowsError(static_cast<DWORD>(result))));
}

/// @brief Read one exact REG_DWORD value.
/// @param key Open readable key.
/// @param name Value name.
/// @return Stored value, or nullopt when absent.
/// @throws std::runtime_error On registry failure or wrong type/size.
std::optional<DWORD> queryRegistryDword(HKEY key, std::wstring_view name) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD size = sizeof(value);
    const LONG result = RegQueryValueExW(
        key, std::wstring(name).c_str(), nullptr, &type, reinterpret_cast<BYTE *>(&value), &size);
    if (result == ERROR_FILE_NOT_FOUND)
        return std::nullopt;
    if (result != ERROR_SUCCESS) {
        throw std::runtime_error("cannot read Windows registry value: " +
                                 wideToUtf8(formatWindowsError(static_cast<DWORD>(result))));
    }
    if (type != REG_DWORD || size != sizeof(value))
        throw std::runtime_error("Windows installer registry value has an invalid type or size");
    return value;
}

/// @brief Split nonempty CRLF/LF-delimited registry text.
/// @param value Multiline text.
/// @return Nonempty lines with trailing carriage returns removed.
std::vector<std::wstring> splitLines(std::wstring_view value) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(L'\n', start);
        std::wstring line(value.substr(
            start, end == std::wstring_view::npos ? value.size() - start : end - start));
        if (!line.empty() && line.back() == L'\r')
            line.pop_back();
        if (!line.empty())
            lines.push_back(std::move(line));
        if (end == std::wstring_view::npos)
            break;
        start = end + 1;
    }
    return lines;
}

/// @brief Parse a stored comma-separated component list.
/// @param value Registry component text.
/// @return Unique lowercase UTF-8 component IDs.
std::set<std::string> parseComponentList(std::wstring_view value) {
    std::set<std::string> result;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(L',', start);
        std::wstring item(value.substr(
            start, comma == std::wstring_view::npos ? value.size() - start : comma - start));
        if (!item.empty())
            result.insert(lowerAscii(wideToUtf8(item)));
        if (comma == std::wstring_view::npos)
            break;
        start = comma + 1;
    }
    return result;
}

/// @brief Join normalized component IDs for registry or command-line storage.
/// @param components Ordered unique UTF-8 IDs.
/// @return Comma-separated UTF-16 text.
std::wstring joinComponents(const std::set<std::string> &components) {
    std::wstring result;
    for (const std::string &component : components) {
        if (!result.empty())
            result.push_back(L',');
        result += utf8ToWide(component);
    }
    return result;
}

/// @brief Read and validate the package's installed ARP record.
/// @param identifier Expected package identifier.
/// @param scope Registry scope to inspect.
/// @return Populated record when marker/location/version are valid, otherwise absent.
InstalledRecord readInstalledRecord(std::string_view identifier, InstallScope scope) {
    InstalledRecord record;
    record.scope = scope;
    RegKey key = openKey(rootKey(scope), uninstallSubkey(identifier), KEY_READ, false);
    if (!key)
        return record;
    const auto marker = queryRegistryString(key.get(), L"ZannaPackageIdentifier");
    if (!marker || wideToUtf8(*marker) != identifier)
        return record;
    const auto location = queryRegistryString(key.get(), L"InstallLocation");
    const auto version = queryRegistryString(key.get(), L"DisplayVersion");
    if (!location || location->empty() || !version)
        return record;
    record.present = true;
    record.installRoot = *location;
    record.version = wideToUtf8(*version);
    if (const auto cache = queryRegistryString(key.get(), L"ZannaMaintenanceCache"))
        record.cacheExecutable = *cache;
    if (const auto components = queryRegistryString(key.get(), L"ZannaComponents"))
        record.components = parseComponentList(*components);
    if (const auto path = queryRegistryString(key.get(), L"ZannaPathEntry"))
        record.pathEntry = *path;
    if (const auto shortcuts = queryRegistryString(key.get(), L"ZannaShortcutPaths")) {
        for (const std::wstring &line : splitLines(*shortcuts))
            record.shortcuts.emplace_back(line);
    }
    if (queryRegistryDword(key.get(), L"ZannaSettingsVersion").value_or(0) == 1U) {
        record.settingsPresent = true;
        record.addToPath = queryRegistryDword(key.get(), L"ZannaAddToPath").value_or(0) != 0;
        record.registerAssociations =
            queryRegistryDword(key.get(), L"ZannaAssociations").value_or(0) != 0;
        record.createShortcuts =
            queryRegistryDword(key.get(), L"ZannaCreateShortcuts").value_or(0) != 0;
    }
    return record;
}

/// @brief Compute a deterministic case-folded FNV-1a hash.
/// @param value UTF-16 identity text.
/// @return 64-bit hash used for cache/mutex names.
uint64_t fnv1a64(std::wstring_view value) {
    uint64_t hash = 1469598103934665603ULL;
    const std::wstring folded = foldWindowsCase(value);
    for (wchar_t ch : folded) {
        hash ^= static_cast<uint16_t>(ch);
        hash *= 1099511628211ULL;
    }
    return hash;
}

/// @brief Format a 64-bit value as fixed-width lowercase hexadecimal.
/// @param value Numeric hash.
/// @return Sixteen UTF-16 hex digits.
std::wstring hashHex(uint64_t value) {
    std::wostringstream out;
    out << std::hex << std::setw(16) << std::setfill(L'0') << value;
    return out.str();
}

/// @brief Derive the per-package maintenance executable cache path.
/// @param scope User or machine installation scope.
/// @param identifier Package identifier hashed for path isolation.
/// @return Path under LocalAppData or ProgramData.
fs::path cacheExecutablePath(InstallScope scope, std::string_view identifier) {
    const fs::path base = scope == InstallScope::User ? fs::path(knownFolder(FOLDERID_LocalAppData))
                                                      : fs::path(knownFolder(FOLDERID_ProgramData));
    return base / L"Zanna" / L"InstallerCache" / hashHex(fnv1a64(utf8ToWide(identifier))) /
           L"maintenance.exe";
}

/// @brief Query whether the current process token is elevated.
/// @return @c true when TokenIsElevated is set.
/// @throws std::runtime_error On token API failures or malformed output.
bool isProcessElevated() {
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        throw std::runtime_error("cannot query the installer process token: " +
                                 wideToUtf8(formatWindowsError(GetLastError())));
    UniqueHandle token(rawToken);
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size)) {
        throw std::runtime_error("cannot query installer elevation state: " +
                                 wideToUtf8(formatWindowsError(GetLastError())));
    }
    if (size != sizeof(elevation))
        throw std::runtime_error("Windows returned an invalid installer elevation record");
    return elevation.TokenIsElevated != 0;
}

/// @brief Map a lifecycle operation to its elevation-relaunch switch.
/// @param operation Planned operation.
/// @return Native command-line switch; auto/default map to install.
std::wstring operationSwitch(Operation operation) {
    switch (operation) {
        case Operation::Modify:
            return L"/modify";
        case Operation::Repair:
            return L"/repair";
        case Operation::Uninstall:
            return L"/uninstall";
        case Operation::Install:
        case Operation::Auto:
        default:
            return L"/install";
    }
}

/// @brief Relaunch a machine-scope plan through UAC and wait for completion.
/// @param package Verified package whose executable is relaunched.
/// @param options Original normalized options to forward.
/// @param plan Resolved scope, destination, components, and operation.
/// @param logger Session logger whose path is forwarded.
/// @return Elevated child process exit code.
/// @throws std::runtime_error On launch, wait, or exit-code failures.
int relaunchElevated(const HostPackage &package,
                     const HostOptions &options,
                     const InstallationPlan &plan,
                     Logger &logger) {
    std::vector<std::wstring> arguments = {operationSwitch(plan.operation),
                                           L"/scope",
                                           L"machine",
                                           L"/installDir",
                                           plan.installRoot.wstring(),
                                           L"/elevated-worker",
                                           L"/log",
                                           logger.path().wstring()};
    if (options.uiLevel == UiLevel::Quiet)
        arguments.push_back(L"/quiet");
    else if (options.uiLevel == UiLevel::Passive)
        arguments.push_back(L"/passive");
    if (options.allowDowngrade)
        arguments.push_back(L"/allowdowngrade");
    if (options.noRestart)
        arguments.push_back(L"/norestart");
    if (options.closeApplications)
        arguments.push_back(L"/closeapplications");
    if (options.addToPath)
        arguments.push_back(*options.addToPath ? L"/addToPath" : L"/noPath");
    if (options.registerAssociations) {
        arguments.push_back(*options.registerAssociations ? L"/associations" : L"/noAssociations");
    }
    if (options.createShortcuts)
        arguments.push_back(*options.createShortcuts ? L"/shortcuts" : L"/noShortcuts");
    if (!plan.components.empty()) {
        arguments.push_back(L"/components");
        arguments.push_back(joinComponents(plan.components));
    }
    std::wstring parameters;
    for (const std::wstring &argument : arguments) {
        if (!parameters.empty())
            parameters.push_back(L' ');
        parameters += quoteCommandLineArgument(argument);
    }
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = package.executablePath.c_str();
    execute.lpParameters = parameters.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&execute)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED)
            return kExitUserCancelled;
        throw std::runtime_error("cannot start elevated installer: " +
                                 wideToUtf8(formatWindowsError(error)));
    }
    if (!execute.hProcess)
        throw std::runtime_error("elevated installer returned no process handle");
    UniqueHandle process(execute.hProcess);
    const DWORD wait = WaitForSingleObject(process.get(), INFINITE);
    if (wait != WAIT_OBJECT_0)
        throw std::runtime_error("cannot wait for the elevated installer");
    DWORD exitCode = kExitFatalError;
    if (!GetExitCodeProcess(process.get(), &exitCode))
        throw std::runtime_error("cannot read elevated installer exit code");
    return static_cast<int>(exitCode);
}

/// @brief Parse a canonical one-to-three-part Windows OS version.
/// @param version Dotted decimal major, minor, and optional build number.
/// @return Three unsigned components with omitted trailing parts zero-filled.
/// @throws std::runtime_error On empty, zero-padded, overflowing, or extra components.
std::array<uint32_t, 3> parseWindowsVersion(std::string_view version) {
    if (version.empty() || version.size() > 64U)
        throw std::runtime_error("invalid Windows version");
    std::array<uint32_t, 3> parts{};
    size_t field = 0;
    size_t start = 0;
    while (start <= version.size()) {
        if (field == parts.size())
            throw std::runtime_error("invalid Windows version");
        const size_t dot = version.find('.', start);
        const std::string_view part = version.substr(
            start, dot == std::string_view::npos ? version.size() - start : dot - start);
        uint32_t value = 0;
        const auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || (part.size() > 1U && part.front() == '0') || parsed.ec != std::errc{} ||
            parsed.ptr != part.data() + part.size()) {
            throw std::runtime_error("invalid Windows version");
        }
        parts[field++] = value;
        if (dot == std::string_view::npos)
            break;
        start = dot + 1U;
    }
    return parts;
}

/// @brief Require the host Windows version to satisfy package metadata.
/// @param package Verified package containing the minimum supported version.
/// @param logger Session logger receiving the detected version.
/// @throws InstallerError When Windows is too old.
/// @throws std::runtime_error On version metadata or OS-query failure.
void preflightWindowsVersion(const HostPackage &package, Logger &logger) {
    std::array<uint32_t, 3> installed{};
    bool testOverride = false;
#if defined(ZANNA_INSTALLER_ENABLE_TEST_HOOKS)
    if (const wchar_t *value = _wgetenv(L"ZANNA_INSTALLER_TEST_WINDOWS_VERSION")) {
        installed = parseWindowsVersion(wideToUtf8(value));
        testOverride = true;
    }
#endif
    if (!testOverride) {
        using RtlGetVersionFunction = LONG(WINAPI *)(OSVERSIONINFOW *);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto rtlGetVersion =
            ntdll ? reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"))
                  : nullptr;
        if (!rtlGetVersion)
            throw std::runtime_error("cannot determine the installed Windows version");
        OSVERSIONINFOW current{};
        current.dwOSVersionInfoSize = sizeof(current);
        if (rtlGetVersion(&current) != 0)
            throw std::runtime_error("cannot query the installed Windows version");
        installed = {current.dwMajorVersion, current.dwMinorVersion, current.dwBuildNumber};
    }

    const std::array<uint32_t, 3> minimum =
        parseWindowsVersion(package.metadata.minimumWindowsVersion);
    logger.info(L"Windows version: " + std::to_wstring(installed[0]) + L"." +
                std::to_wstring(installed[1]) + L"." + std::to_wstring(installed[2]) +
                (testOverride ? L" (test override)" : L""));
    if (std::lexicographical_compare(
            installed.begin(), installed.end(), minimum.begin(), minimum.end())) {
        throw InstallerError(kExitFatalError,
                             "this package requires Windows " +
                                 package.metadata.minimumWindowsVersion + " or newer");
    }
}

/// @brief Normalize and validate a fixed-volume installation destination.
/// @param requested User- or metadata-selected path.
/// @return Absolute lexical path below a fixed local volume.
/// @throws std::runtime_error For roots, UNC/device paths, unsafe components,
///         protected folders, Windows ancestry, or resolution failures.
fs::path canonicalDestination(const fs::path &requested) {
    if (requested.empty())
        throw std::runtime_error("installation destination is empty");
    std::error_code error;
    fs::path absolute = fs::absolute(requested, error).lexically_normal();
    if (error || !absolute.has_root_name() || absolute == absolute.root_path())
        throw std::runtime_error(
            "installation destination must be an absolute directory below a fixed volume");
    const std::wstring text = absolute.wstring();
    /// @brief Identify a control code unit forbidden in an installation path.
    /// @param ch Wide character to inspect.
    /// @return `true` for code units below U+0020.
    if (std::any_of(text.begin(), text.end(), [](wchar_t ch) { return ch < 0x20; }))
        throw std::runtime_error("installation destination contains a control character");
    if (text.size() >= 32760 || PathIsUNCW(text.c_str()) || text.rfind(L"\\\\.\\", 0) == 0 ||
        text.rfind(L"\\\\?\\GLOBALROOT", 0) == 0) {
        throw std::runtime_error("installation destination is not a supported fixed-volume path");
    }
    for (const fs::path &componentPath : absolute.relative_path()) {
        const std::wstring component = componentPath.wstring();
        if (component.empty() || component.back() == L'.' || component.back() == L' ' ||
            component.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) {
            throw std::runtime_error("installation destination contains an invalid Windows name");
        }
        std::wstring base = component.substr(0, component.find(L'.'));
        base = foldWindowsCase(base);
        const bool numberedDevice = base.size() == 4U &&
                                    (base.rfind(L"com", 0) == 0 || base.rfind(L"lpt", 0) == 0) &&
                                    base[3] >= L'1' && base[3] <= L'9';
        if (base == L"con" || base == L"prn" || base == L"aux" || base == L"nul" ||
            numberedDevice) {
            throw std::runtime_error("installation destination uses a reserved Windows name");
        }
    }
    wchar_t volumePath[32768]{};
    if (!GetVolumePathNameW(text.c_str(), volumePath, static_cast<DWORD>(std::size(volumePath))) ||
        GetDriveTypeW(volumePath) != DRIVE_FIXED) {
        throw std::runtime_error("installation destination must be on a fixed local volume");
    }
    /// @brief Resolve and normalize the protected Windows installation directory.
    /// @return Lexically normalized Windows directory path.
    /// @throws std::runtime_error If the operating-system directory cannot be queried safely.
    const fs::path windowsDir = [] {
        std::wstring value(32768, L'\0');
        const UINT length = GetWindowsDirectoryW(value.data(), static_cast<UINT>(value.size()));
        if (length == 0 || length >= value.size())
            throw std::runtime_error("cannot resolve the protected Windows directory");
        value.resize(length);
        return fs::path(value).lexically_normal();
    }();
    if (windowsPathBeginsWith(absolute, windowsDir) ||
        windowsPathBeginsWith(windowsDir, absolute)) {
        throw std::runtime_error("installation in or above the Windows directory is prohibited");
    }
    const std::array<KNOWNFOLDERID, 9> protectedFolders = {FOLDERID_ProgramFiles,
                                                           FOLDERID_ProgramData,
                                                           FOLDERID_UserProfiles,
                                                           FOLDERID_Profile,
                                                           FOLDERID_LocalAppData,
                                                           FOLDERID_RoamingAppData,
                                                           FOLDERID_Desktop,
                                                           FOLDERID_Documents,
                                                           FOLDERID_Programs};
    for (REFKNOWNFOLDERID id : protectedFolders) {
        PWSTR raw = nullptr;
        const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
        if (FAILED(result) || !raw || !*raw) {
            if (raw)
                CoTaskMemFree(raw);
            throw std::runtime_error("cannot resolve a protected Windows known folder");
        }
        const fs::path protectedPath(raw);
        CoTaskMemFree(raw);
        if (windowsPathBeginsWith(protectedPath, absolute)) {
            throw std::runtime_error(
                "installation destination is a protected Windows or user-profile folder");
        }
    }
    return absolute;
}

/// @brief Reject any existing reparse point along a destination ancestry.
/// @param path Candidate destination.
/// @throws std::runtime_error On a reparse ancestor or unexpected attribute error.
void rejectReparseAncestors(const fs::path &path) {
    fs::path current = path;
    while (!current.empty() && current != current.root_path()) {
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES) {
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                throw std::runtime_error("installation path traverses a reparse point: " +
                                         wideToUtf8(current.wstring()));
        } else {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                throw std::runtime_error("cannot verify an installation path ancestor: " +
                                         wideToUtf8(formatWindowsError(error)));
            }
        }
        current = current.parent_path();
    }
}

/// @brief Probe write access in the nearest existing parent directory.
/// @param path Candidate destination.
/// @throws std::runtime_error When no parent exists or bounded unique probe creation fails.
void ensureParentWritable(const fs::path &path) {
    fs::path existing = path.parent_path();
    while (!existing.empty() && !fs::exists(existing))
        existing = existing.parent_path();
    if (existing.empty())
        throw std::runtime_error("installation destination has no existing parent directory");
    DWORD lastError = ERROR_FILE_EXISTS;
    for (unsigned attempt = 0; attempt < 64U; ++attempt) {
        LARGE_INTEGER counter{};
        QueryPerformanceCounter(&counter);
        const uint64_t nonce = static_cast<uint64_t>(counter.QuadPart) ^ (GetTickCount64() << 1U) ^
                               (static_cast<uint64_t>(GetCurrentThreadId()) << 32U) ^ attempt;
        const fs::path probe =
            existing / (L".zanna-write-probe-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                        hashHex(nonce));
        UniqueHandle file(CreateFileW(probe.c_str(),
                                      GENERIC_WRITE,
                                      0,
                                      nullptr,
                                      CREATE_NEW,
                                      FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                                      nullptr));
        if (file)
            return;
        lastError = GetLastError();
        if (lastError != ERROR_FILE_EXISTS && lastError != ERROR_ALREADY_EXISTS)
            break;
    }
    throw std::runtime_error("installation destination is not writable: " +
                             wideToUtf8(formatWindowsError(lastError)));
}

/// @brief Resolve required, preset, explicit, or retained component selection.
/// @param package Verified package and component catalog.
/// @param options Parsed component options.
/// @param existing Existing installation settings, if present.
/// @return Normalized selected IDs including every required component.
/// @throws std::runtime_error When an explicit component is unknown.
std::set<std::string> selectComponents(const HostPackage &package,
                                       const HostOptions &options,
                                       const InstalledRecord &existing) {
    std::set<std::string> known;
    std::set<std::string> result;
    for (const auto &component : package.metadata.components) {
        known.insert(lowerAscii(component.id));
        if (component.required ||
            (options.componentPreset != ComponentPreset::Minimal &&
             options.componentPreset != ComponentPreset::SDK && component.defaultSelected) ||
            (options.componentPreset == ComponentPreset::SDK && lowerAscii(component.id) == "sdk"))
            result.insert(lowerAscii(component.id));
    }
    if (options.componentPreset == ComponentPreset::Complete) {
        result = known;
    } else if (existing.present && !existing.components.empty() &&
               options.componentPreset == ComponentPreset::Unspecified) {
        result.clear();
        for (const std::string &component : existing.components) {
            const std::string normalized = lowerAscii(component);
            if (known.find(normalized) != known.end())
                result.insert(normalized);
        }
    }
    if (options.componentsSpecified) {
        result.clear();
        for (const std::string &component : options.selectedComponents)
            result.insert(lowerAscii(component));
    }
    for (const std::string &component : result) {
        if (known.find(component) == known.end())
            throw std::runtime_error("unknown installer component: " + component);
    }
    for (const auto &component : package.metadata.components) {
        if (component.required)
            result.insert(lowerAscii(component.id));
    }
    return result;
}

/// @brief Test whether a payload component is selected.
/// @param component Component ID; empty means unconditional.
/// @param selected Normalized selected IDs.
/// @return @c true for unconditional or selected entries.
bool componentEnabled(std::string_view component, const std::set<std::string> &selected) {
    return component.empty() || selected.find(lowerAscii(std::string(component))) != selected.end();
}

/// @brief Resolve installed state and options into one validated lifecycle plan.
/// @param package Verified incoming package.
/// @param options Parsed host request.
/// @param recoveryRecord Optional installed record reconstructed during recovery.
/// @return Scope, operation, destination, cache, component, file, and integration plan.
/// @throws std::runtime_error On scope conflicts, missing maintenance state,
///         destination mismatch, unsafe paths, or invalid component/integration data.
InstallationPlan makePlan(const HostPackage &package,
                          const HostOptions &options,
                          const InstalledRecord *recoveryRecord = nullptr) {
    InstallationPlan plan;
    if (recoveryRecord) {
        plan.existing = *recoveryRecord;
        plan.scope = recoveryRecord->scope;
    } else {
        const InstalledRecord user =
            readInstalledRecord(package.metadata.identifier, InstallScope::User);
        const InstalledRecord machine =
            readInstalledRecord(package.metadata.identifier, InstallScope::Machine);
        if (options.scope) {
            plan.scope = *options.scope;
            plan.existing = plan.scope == InstallScope::User ? user : machine;
            const InstalledRecord &opposite = plan.scope == InstallScope::User ? machine : user;
            if (!plan.existing.present && opposite.present) {
                throw std::runtime_error(
                    "Zanna is already installed in the other scope; use Modify to keep that "
                    "scope or uninstall it before changing scope");
            }
        } else if (user.present != machine.present) {
            plan.scope = user.present ? InstallScope::User : InstallScope::Machine;
            plan.existing = user.present ? user : machine;
        } else if (user.present && machine.present) {
            if (options.uiLevel != UiLevel::Full)
                throw std::runtime_error(
                    "Zanna is registered for both scopes; specify /scope user or /scope machine");
            plan.scope = package.metadata.defaultScope == "machine" ? InstallScope::Machine
                                                                    : InstallScope::User;
            plan.existing = plan.scope == InstallScope::User ? user : machine;
        } else {
            plan.scope = package.metadata.defaultScope == "machine" ? InstallScope::Machine
                                                                    : InstallScope::User;
            plan.existing = {};
        }
    }
    plan.operation = options.operation;
    if (plan.operation == Operation::Auto) {
        if (package.metadata.packageMode == "maintenance") {
            const fs::path installedUninstaller =
                plan.existing.present
                    ? safeJoin(plan.existing.installRoot, package.metadata.uninstallerRelativePath)
                    : fs::path{};
            if (!installedUninstaller.empty() &&
                sameWindowsPath(currentExecutablePath(), installedUninstaller)) {
                plan.operation = Operation::Uninstall;
            } else if (options.uiLevel != UiLevel::Full) {
                throw std::runtime_error(
                    "maintenance mode requires /modify, /repair, or /uninstall");
            } else {
                plan.operation = Operation::Repair;
            }
        } else {
            plan.operation = Operation::Install;
        }
    }
    if ((plan.operation == Operation::Modify || plan.operation == Operation::Repair ||
         plan.operation == Operation::Uninstall) &&
        !plan.existing.present) {
        throw std::runtime_error("no matching Zanna installation is registered for this scope");
    }
    if (!options.destination.empty())
        plan.installRoot = canonicalDestination(options.destination);
    else if (plan.existing.present)
        plan.installRoot = canonicalDestination(plan.existing.installRoot);
    else {
        const fs::path base = plan.scope == InstallScope::User
                                  ? fs::path(knownFolder(FOLDERID_LocalAppData)) / L"Programs"
                                  : fs::path(knownFolder(FOLDERID_ProgramFiles));
        plan.installRoot =
            canonicalDestination(base / utf8ToWide(package.metadata.defaultInstallDir));
    }
    if (plan.existing.present && !sameWindowsPath(plan.installRoot, plan.existing.installRoot)) {
        throw std::runtime_error(
            "maintenance destination does not match the registered installation");
    }
    rejectReparseAncestors(plan.installRoot);
    plan.cacheExecutable = cacheExecutablePath(plan.scope, package.metadata.identifier);
    plan.components = selectComponents(package, options, plan.existing);
    const bool existingSettings = plan.existing.present && plan.existing.settingsPresent;
    plan.addToPath = options.addToPath.value_or(existingSettings ? plan.existing.addToPath
                                                                 : package.metadata.addToPath);
    plan.registerAssociations = options.registerAssociations.value_or(
        existingSettings ? plan.existing.registerAssociations
                         : package.metadata.registerFileAssociations);
    plan.createShortcuts = options.createShortcuts.value_or(
        existingSettings ? plan.existing.createShortcuts : package.metadata.createShortcuts);
    plan.addToPath = plan.addToPath && !package.metadata.pathRelativePath.empty();
    plan.registerAssociations = plan.registerAssociations &&
                                !package.metadata.associations.empty() &&
                                !package.metadata.associationExecutable.empty();
    plan.createShortcuts = plan.createShortcuts && !package.metadata.shortcuts.empty();
    if (plan.registerAssociations && !package.metadata.associationExecutable.empty()) {
        const std::string associationPath =
            normalizedPathKey(package.metadata.associationExecutable);
        /// @brief Match a payload entry to the configured association executable.
        /// @param file Payload metadata to inspect.
        /// @return `true` when its normalized path equals `associationPath`.
        const auto associationPayload =
            std::find_if(package.metadata.payloadFiles.begin(),
                         package.metadata.payloadFiles.end(),
                         [&](const zanna::pkg::WindowsInstallerPayloadMetadata &file) {
                             return normalizedPathKey(file.path) == associationPath;
                         });
        if (associationPayload != package.metadata.payloadFiles.end() &&
            !componentEnabled(associationPayload->componentId, plan.components)) {
            plan.registerAssociations = false;
        }
    }
    for (const auto &file : package.metadata.payloadFiles) {
        if (!componentEnabled(file.componentId, plan.components))
            continue;
        plan.files.push_back({file.path, file.sha256, file.sizeBytes});
        if (file.sizeBytes > std::numeric_limits<uint64_t>::max() - plan.selectedSizeBytes)
            throw std::runtime_error("selected installer payload size overflow");
        plan.selectedSizeBytes += file.sizeBytes;
    }
    for (const auto &file : package.metadata.outerFiles) {
        if (!componentEnabled(file.componentId, plan.components))
            continue;
        plan.files.push_back({file.path, file.sha256, file.sizeBytes});
        if (file.sizeBytes > std::numeric_limits<uint64_t>::max() - plan.selectedSizeBytes)
            throw std::runtime_error("selected installer payload size overflow");
        plan.selectedSizeBytes += file.sizeBytes;
    }
    if (package.metadata.packageMode == "maintenance" && plan.operation != Operation::Uninstall) {
        const std::string selfHash =
            zanna::pkg::sha256Hex(package.executableBytes.data(), package.executableBytes.size());
        if (package.executableBytes.size() >
            std::numeric_limits<uint64_t>::max() - plan.selectedSizeBytes) {
            throw std::runtime_error("selected installer payload size overflow");
        }
        plan.files.push_back(
            {package.metadata.uninstallerRelativePath, selfHash, package.executableBytes.size()});
        plan.selectedSizeBytes += package.executableBytes.size();
    }
    return plan;
}

/// @brief Reject an unintended downgrade before modifying an installed product.
/// @param package Verified incoming package whose version will be installed.
/// @param options Parsed request, including the explicit downgrade override.
/// @param plan Resolved operation and currently installed product state.
/// @throws InstallerError When the incoming version is older and downgrades were not allowed.
void preflightVersion(const HostPackage &package,
                      const HostOptions &options,
                      const InstallationPlan &plan) {
    if (!plan.existing.present || plan.operation == Operation::Uninstall)
        return;
    const int comparison =
        compareInstallerVersions(package.metadata.version, plan.existing.version);
    if (comparison < 0 && !options.allowDowngrade)
        throw InstallerError(
            kExitNewerVersionInstalled,
            "a newer Zanna version is already installed; use /allowDowngrade to proceed");
}

/// @brief Load normalized installer-owned paths from an installed manifest.
/// @param installRoot Root against which the manifest and its entries are resolved.
/// @param manifestRelative Package-relative manifest path.
/// @return Normalized relative paths owned by the installer, or an empty set if absent.
std::set<std::string> loadOwnershipManifest(const fs::path &installRoot,
                                            std::string_view manifestRelative);

/// @brief Sum files that must be preserved from an existing installation tree.
/// @param root Existing installation root to inspect.
/// @param ownedPaths Normalized relative paths that may be replaced rather than preserved.
/// @return Total byte size of regular files not listed as installer-owned.
/// @throws std::runtime_error On traversal, attribute, relative-path, or size errors.
uint64_t preservedDirectoryBytes(const fs::path &root, const std::set<std::string> &ownedPaths) {
    if (!fs::exists(root))
        return 0;
    uint64_t total = 0;
    std::error_code error;
    for (fs::recursive_directory_iterator
             it(root, fs::directory_options::skip_permission_denied, error),
         end;
         it != end;
         it.increment(error)) {
        if (error)
            throw std::runtime_error("cannot enumerate existing installation for disk preflight");
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            throw std::runtime_error("cannot inspect an existing installation entry: " +
                                     wideToUtf8(formatWindowsError(GetLastError())));
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::runtime_error("existing installation contains a reparse point");
        }
        if (it->is_regular_file(error)) {
            const fs::path relativePath = fs::relative(it->path(), root, error);
            if (error)
                throw std::runtime_error(
                    "cannot resolve an existing installation file for disk preflight");
            if (!ownedPaths.empty() &&
                ownedPaths.find(normalizedPathKey(
                    zanna::filesystem::genericPathToUtf8(relativePath))) != ownedPaths.end()) {
                continue;
            }
            const uint64_t size = it->file_size(error);
            if (error || size > std::numeric_limits<uint64_t>::max() - total)
                throw std::runtime_error("existing installation size overflow");
            total += size;
        }
    }
    return total;
}

/// @brief Apply the optional test-only free-space ceiling to a measured value.
/// @param available Actual number of available bytes reported by Windows.
/// @return Effective byte count used by disk preflight.
uint64_t testLimitedFreeBytes(uint64_t available);

/// @brief Verify that installation and maintenance-cache volumes have sufficient space.
/// @param package Verified package providing payload sizes and manifest location.
/// @param plan Resolved operation, destination, cache path, and selected payload size.
/// @throws std::runtime_error On arithmetic overflow, volume-query failure, or insufficient space.
void preflightDisk(const HostPackage &package, const InstallationPlan &plan) {
    const std::set<std::string> owned =
        loadOwnershipManifest(plan.installRoot, package.metadata.installedManifestRelativePath);
    const uint64_t preserved = preservedDirectoryBytes(plan.installRoot, owned);
    const uint64_t selected = plan.operation == Operation::Uninstall ? 0 : plan.selectedSizeBytes;
    const uint64_t safety = plan.operation == Operation::Uninstall ? 16ULL * 1024ULL * 1024ULL
                                                                   : 64ULL * 1024ULL * 1024ULL;
    if (selected > std::numeric_limits<uint64_t>::max() - preserved)
        throw std::runtime_error("installer disk requirement overflow");
    const uint64_t transactionBytes = selected + preserved;
    const uint64_t transactionSafety = transactionBytes / 5U;
    if (transactionBytes > std::numeric_limits<uint64_t>::max() - safety ||
        transactionBytes + safety > std::numeric_limits<uint64_t>::max() - transactionSafety) {
        throw std::runtime_error("installer disk requirement overflow");
    }
    uint64_t required = transactionBytes + safety + transactionSafety;
    uint64_t cacheRequired = 0;
    if (plan.operation != Operation::Uninstall) {
        cacheRequired = package.metadata.outerFiles.empty()
                            ? static_cast<uint64_t>(package.executableBytes.size())
                            : package.metadata.outerFiles.front().sizeBytes;
        constexpr uint64_t kCacheSafety = 16ULL * 1024ULL * 1024ULL;
        if (cacheRequired > std::numeric_limits<uint64_t>::max() - kCacheSafety)
            throw std::runtime_error("installer cache disk requirement overflow");
        cacheRequired += kCacheSafety;
    }
    fs::path probe = plan.installRoot.parent_path();
    while (!probe.empty() && !fs::exists(probe))
        probe = probe.parent_path();
    wchar_t installVolume[32768]{};
    wchar_t cacheVolume[32768]{};
    if (probe.empty() || !GetVolumePathNameW(probe.c_str(),
                                             installVolume,
                                             static_cast<DWORD>(std::size(installVolume))))
        throw std::runtime_error("cannot determine the installation destination volume");
    fs::path cacheProbe = plan.cacheExecutable.parent_path();
    while (!cacheProbe.empty() && !fs::exists(cacheProbe))
        cacheProbe = cacheProbe.parent_path();
    if (cacheRequired != 0 &&
        (cacheProbe.empty() || !GetVolumePathNameW(cacheProbe.c_str(),
                                                   cacheVolume,
                                                   static_cast<DWORD>(std::size(cacheVolume))))) {
        throw std::runtime_error("cannot determine the maintenance cache volume");
    }
    if (cacheRequired != 0 && ordinalEqualsIgnoreCase(installVolume, cacheVolume)) {
        if (cacheRequired > std::numeric_limits<uint64_t>::max() - required)
            throw std::runtime_error("installer disk requirement overflow");
        required += cacheRequired;
        cacheRequired = 0;
    }
    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(probe.c_str(), &freeBytes, nullptr, nullptr))
        throw std::runtime_error("cannot determine free space for the installation destination");
    const uint64_t availableInstallBytes = testLimitedFreeBytes(freeBytes.QuadPart);
    if (availableInstallBytes < required) {
        std::ostringstream message;
        message << "insufficient disk space: " << required << " bytes required, "
                << availableInstallBytes << " bytes available";
        throw std::runtime_error(message.str());
    }
    if (cacheRequired != 0) {
        ULARGE_INTEGER cacheFree{};
        if (!GetDiskFreeSpaceExW(cacheProbe.c_str(), &cacheFree, nullptr, nullptr))
            throw std::runtime_error("cannot determine free space for the maintenance cache");
        const uint64_t availableCacheBytes = testLimitedFreeBytes(cacheFree.QuadPart);
        if (availableCacheBytes < cacheRequired) {
            std::ostringstream message;
            message << "insufficient maintenance-cache disk space: " << cacheRequired
                    << " bytes required, " << availableCacheBytes << " bytes available";
            throw std::runtime_error(message.str());
        }
    }
}

/// @brief Process-wide guard preventing concurrent lifecycle work on one installation.
class LifecycleMutex {
  public:
    /// @brief Create and attempt to acquire the scope-and-destination-specific mutex.
    /// @param plan Resolved installation scope and root used to derive the mutex identity.
    /// @param identifier Stable package identifier included in the mutex identity.
    /// @throws std::runtime_error If the mutex cannot be created or queried.
    LifecycleMutex(const InstallationPlan &plan, std::string_view identifier) {
        const std::wstring seed = utf8ToWide(identifier) + L"|" +
                                  (plan.scope == InstallScope::User ? L"user|" : L"machine|") +
                                  foldWindowsCase(normalizedWindowsPathText(plan.installRoot));
        const std::wstring name = (plan.scope == InstallScope::Machine ? L"Global\\" : L"Local\\") +
                                  std::wstring(L"ZannaInstaller-") + hashHex(fnv1a64(seed));
        handle_.reset(CreateMutexW(nullptr, FALSE, name.c_str()));
        if (!handle_)
            throw std::runtime_error("cannot create installer lifecycle mutex");
        const DWORD result = WaitForSingleObject(handle_.get(), 0);
        if (result == WAIT_TIMEOUT)
            active_ = false;
        else if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED)
            throw std::runtime_error("cannot acquire installer lifecycle mutex");
    }

    /// @brief Release the mutex when this instance successfully acquired it.
    ~LifecycleMutex() {
        if (active_)
            ReleaseMutex(handle_.get());
    }

    /// @brief Report whether this process acquired the lifecycle mutex.
    /// @return @c true when lifecycle work may proceed.
    bool acquired() const {
        return active_;
    }

  private:
    UniqueHandle handle_;
    bool active_{true};
};

/// @brief RAII wrapper for a Windows Restart Manager session used around owned files.
class RestartManagerSession {
  public:
    /// @brief End the Restart Manager session if one was started.
    ~RestartManagerSession() {
        if (started_)
            RmEndSession(session_);
    }

    /// @brief Find applications currently using any supplied installation file.
    /// @param paths Existing owned files to register as Restart Manager resources.
    /// @return Process records for applications holding the files, possibly empty.
    /// @throws std::runtime_error If the session, registration, or process query fails.
    std::vector<RM_PROCESS_INFO> inspect(const std::vector<fs::path> &paths) {
        if (paths.empty())
            return {};
        wchar_t key[CCH_RM_SESSION_KEY + 1]{};
        if (RmStartSession(&session_, 0, key) != ERROR_SUCCESS)
            throw std::runtime_error("Restart Manager could not start a session");
        started_ = true;
        std::vector<LPCWSTR> resources;
        resources.reserve(paths.size());
        for (const fs::path &path : paths)
            resources.push_back(path.c_str());
        const DWORD registerResult = RmRegisterResources(session_,
                                                         static_cast<UINT>(resources.size()),
                                                         resources.data(),
                                                         0,
                                                         nullptr,
                                                         0,
                                                         nullptr);
        if (registerResult != ERROR_SUCCESS)
            throw std::runtime_error("Restart Manager could not register installation files");
        UINT needed = 0;
        UINT count = 0;
        DWORD reasons = 0;
        DWORD result = RmGetList(session_, &needed, &count, nullptr, &reasons);
        if (result == ERROR_SUCCESS)
            return {};
        if (result != ERROR_MORE_DATA)
            throw std::runtime_error("Restart Manager could not inspect files in use");
        for (unsigned attempt = 0; attempt < 4U; ++attempt) {
            std::vector<RM_PROCESS_INFO> processes(needed);
            count = needed;
            result = RmGetList(session_, &needed, &count, processes.data(), &reasons);
            if (result == ERROR_SUCCESS) {
                processes.resize(count);
                return processes;
            }
            if (result != ERROR_MORE_DATA)
                break;
        }
        throw std::runtime_error("Restart Manager could not enumerate files in use");
    }

    /// @brief Ask Restart Manager to shut down every blocking application safely.
    /// @throws std::runtime_error If Restart Manager cannot complete the shutdown.
    void closeApplications() {
        const DWORD result = RmShutdown(session_, 0, nullptr);
        if (result != ERROR_SUCCESS)
            throw std::runtime_error("Restart Manager could not close all applications safely");
        applicationsClosed_ = true;
    }

    /// @brief Restart applications previously closed by this session when requested.
    /// @param enabled Whether application restart was requested by the host options.
    void restartApplications(bool enabled) {
        if (started_ && applicationsClosed_ && enabled)
            RmRestart(session_, 0, nullptr);
    }

  private:
    DWORD session_{0};
    bool started_{false};
    bool applicationsClosed_{false};
};

/// @brief Resolve installer-owned manifest entries that currently exist as regular files.
/// @param package Verified package providing the installed-manifest location.
/// @param plan Resolved installation root.
/// @return Existing owned file paths suitable for Restart Manager registration.
std::vector<fs::path> ownedExistingPaths(const HostPackage &package, const InstallationPlan &plan) {
    std::vector<fs::path> paths;
    const std::set<std::string> owned =
        loadOwnershipManifest(plan.installRoot, package.metadata.installedManifestRelativePath);
    for (const std::string &relative : owned) {
        const fs::path candidate = safeJoin(plan.installRoot, relative);
        if (fs::is_regular_file(candidate))
            paths.push_back(candidate);
    }
    return paths;
}

/// @brief Detect blocking applications and optionally close them through Restart Manager.
/// @param restart Active Restart Manager wrapper that owns the eventual restart state.
/// @param package Verified package providing ownership metadata.
/// @param plan Resolved installation state.
/// @param options Parsed UI and application-close policy.
/// @param logger Installer logger used to report blocking process names.
/// @throws std::runtime_error If files remain in use or applications cannot be closed.
void handleFilesInUse(RestartManagerSession &restart,
                      const HostPackage &package,
                      const InstallationPlan &plan,
                      const HostOptions &options,
                      Logger &logger) {
    const auto processes = restart.inspect(ownedExistingPaths(package, plan));
    if (processes.empty())
        return;
    std::wstring names;
    for (const auto &process : processes) {
        if (!names.empty())
            names += L", ";
        names += process.strAppName[0] ? process.strAppName : L"Unknown application";
    }
    logger.warning(L"Files are in use by: " + names);
    bool close = options.closeApplications;
    if (!close && options.uiLevel == UiLevel::Full) {
        const std::wstring message = L"Zanna files are in use by:\r\n\r\n" + names +
                                     L"\r\n\r\nClose these applications and continue?";
        close = MessageBoxW(nullptr,
                            message.c_str(),
                            L"Zanna Tools Installer - Files in Use",
                            MB_YESNO | MB_ICONWARNING | MB_SETFOREGROUND) == IDYES;
    }
    if (!close)
        throw std::runtime_error("Zanna files are in use; close applications and retry");
    restart.closeApplications();
}

/// @brief Read a bounded UTF-8 metadata file and convert it to UTF-16.
/// @param path Metadata file to read; a missing file is treated as empty.
/// @return Decoded text, or an empty string when the file does not exist.
/// @throws std::runtime_error If the path is unsafe, too large, unreadable, or changes while read.
std::wstring readTextFileWide(const fs::path &path) {
    constexpr uintmax_t kMaximumTextFileBytes = 32ULL * 1024ULL * 1024ULL;
    std::error_code error;
    const bool exists = fs::exists(path, error);
    if (error)
        throw std::runtime_error("cannot inspect installer metadata text file");
    if (!exists)
        return {};
    if (!fs::is_regular_file(path, error) || error)
        throw std::runtime_error("installer metadata text path is not a readable regular file");
    const uintmax_t size = fs::file_size(path, error);
    if (error || size > kMaximumTextFileBytes)
        throw std::runtime_error("installer metadata text file is unreadable or too large");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot open installer metadata text file");
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!bytes.empty()) {
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
            throw std::runtime_error("installer metadata text file changed while being read");
    }
    if (input.peek() != std::char_traits<char>::eof())
        throw std::runtime_error("installer metadata text file grew while being read");
    if (input.bad())
        throw std::runtime_error("cannot finish reading installer metadata text file");
    return utf8ToWide(bytes);
}

/// @brief Durably replace a file with the supplied byte sequence.
/// @param path Destination file whose parent directories are created as needed.
/// @param bytes Complete contents to write.
/// @throws std::runtime_error If staging, flushing, or atomic replacement fails.
void writeBytesAtomic(const fs::path &path, const std::vector<uint8_t> &bytes) {
    fs::create_directories(path.parent_path());
    const fs::path temporary = path.wstring() + L".tmp-" + std::to_wstring(GetCurrentProcessId()) +
                               L"-" + hashHex(GetTickCount64());
    bool created = false;
    try {
        UniqueHandle output(CreateFileW(temporary.c_str(),
                                        GENERIC_WRITE,
                                        0,
                                        nullptr,
                                        CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr));
        if (!output)
            throw std::runtime_error("cannot create a staged installer file: " +
                                     wideToUtf8(formatWindowsError(GetLastError())));
        created = true;
        size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD chunk = static_cast<DWORD>(
                std::min<size_t>(bytes.size() - offset, static_cast<size_t>(MAXDWORD)));
            DWORD written = 0;
            if (!WriteFile(output.get(), bytes.data() + offset, chunk, &written, nullptr) ||
                written == 0U || written > chunk) {
                throw std::runtime_error("cannot write a staged installer file: " +
                                         wideToUtf8(formatWindowsError(GetLastError())));
            }
            offset += written;
        }
        if (!FlushFileBuffers(output.get()))
            throw std::runtime_error("cannot durably flush a staged installer file: " +
                                     wideToUtf8(formatWindowsError(GetLastError())));
        output.reset();
    } catch (...) {
        if (created && !DeleteFileW(temporary.c_str())) {
            const DWORD cleanupError = GetLastError();
            if (cleanupError != ERROR_FILE_NOT_FOUND && cleanupError != ERROR_PATH_NOT_FOUND) {
                throw std::runtime_error("cannot remove a failed staged installer file: " +
                                         wideToUtf8(formatWindowsError(cleanupError)));
            }
        }
        throw;
    }
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        if (!DeleteFileW(temporary.c_str())) {
            const DWORD cleanupError = GetLastError();
            if (cleanupError != ERROR_FILE_NOT_FOUND && cleanupError != ERROR_PATH_NOT_FOUND) {
                throw std::runtime_error(
                    "cannot commit or remove a staged installer file: " +
                    wideToUtf8(formatWindowsError(cleanupError)) +
                    "; commit error: " + wideToUtf8(formatWindowsError(error)));
            }
        }
        throw std::runtime_error("cannot commit staged installer file: " +
                                 wideToUtf8(formatWindowsError(error)));
    }
}

/// @brief Encode UTF-16 text as UTF-8 and atomically replace a file.
/// @param path Destination metadata file.
/// @param text Text to encode and persist.
/// @throws std::runtime_error If encoding or the atomic byte write fails.
void writeTextAtomic(const fs::path &path, std::wstring_view text) {
    const std::string utf8 = wideToUtf8(text);
    writeBytesAtomic(path, std::vector<uint8_t>(utf8.begin(), utf8.end()));
}

/// @brief Derive the transaction recovery-marker path beside the cached executable.
/// @param cacheExecutable Maintenance executable stored in the package cache.
/// @return Path of the versioned recovery marker.
fs::path recoveryMarkerPath(const fs::path &cacheExecutable) {
    return cacheExecutable.parent_path() / L"recovery-v2.txt";
}

/// @brief Persist the identity needed to recover an interrupted transaction.
/// @param plan Resolved scope, installation root, and cache path.
/// @param identifier Stable package identifier recorded in the marker.
/// @throws std::runtime_error If the marker cannot be encoded or written atomically.
void writeRecoveryMarker(const InstallationPlan &plan, std::string_view identifier) {
    std::wostringstream text;
    text << L"ZANNA-RECOVERY\t2\r\n"
         << L"identifier\t" << utf8ToWide(identifier) << L"\r\n"
         << L"scope\t" << (plan.scope == InstallScope::User ? L"user" : L"machine") << L"\r\n"
         << L"root\t" << plan.installRoot.wstring() << L"\r\n";
    writeTextAtomic(recoveryMarkerPath(plan.cacheExecutable), text.str());
}

/// @brief Best-effort remove the recovery marker after a completed transaction.
/// @param plan Resolved cache path identifying the marker.
void removeRecoveryMarker(const InstallationPlan &plan) {
    std::error_code error;
    fs::remove(recoveryMarkerPath(plan.cacheExecutable), error);
}

/// @brief Locate and validate an interrupted transaction recovery marker.
/// @param package Verified package whose identifier and default scope constrain the marker.
/// @param options Parsed explicit scope, if any.
/// @param logger Installer logger used when stale markers are discarded.
/// @return Reconstructed installed record when a live transaction requires recovery.
/// @throws std::runtime_error If a marker exists but has invalid identity or schema.
std::optional<InstalledRecord> readRecoveryRecord(const HostPackage &package,
                                                  const HostOptions &options,
                                                  Logger &logger) {
    std::vector<InstallScope> scopes;
    if (options.scope) {
        scopes.push_back(*options.scope);
    } else {
        const InstallScope preferred =
            package.metadata.defaultScope == "machine" ? InstallScope::Machine : InstallScope::User;
        scopes.push_back(preferred);
        scopes.push_back(preferred == InstallScope::User ? InstallScope::Machine
                                                         : InstallScope::User);
    }
    for (const InstallScope scope : scopes) {
        const fs::path cache = cacheExecutablePath(scope, package.metadata.identifier);
        const fs::path markerPath = recoveryMarkerPath(cache);
        if (!fs::is_regular_file(markerPath))
            continue;
        const std::wstring text = readTextFileWide(markerPath);
        if (text.rfind(L"ZANNA-RECOVERY\t2\r\n", 0) != 0)
            throw std::runtime_error("installer recovery marker has an invalid schema");
        std::wstring identifier;
        std::wstring scopeText;
        std::wstring rootText;
        for (const std::wstring &line : splitLines(text)) {
            const size_t tab = line.find(L'\t');
            if (tab == std::wstring::npos)
                continue;
            const std::wstring key = line.substr(0, tab);
            const std::wstring value = line.substr(tab + 1U);
            if (key == L"identifier")
                identifier = value;
            else if (key == L"scope")
                scopeText = value;
            else if (key == L"root")
                rootText = value;
        }
        const std::wstring expectedScope = scope == InstallScope::User ? L"user" : L"machine";
        if (wideToUtf8(identifier) != package.metadata.identifier || scopeText != expectedScope ||
            rootText.empty()) {
            throw std::runtime_error("installer recovery marker identity is invalid");
        }
        const fs::path root = canonicalDestination(rootText);
        const fs::path transaction =
            root.parent_path() / (L"." + root.filename().wstring() + L".zanna-transaction-" +
                                  hashHex(fnv1a64(utf8ToWide(package.metadata.identifier))));
        if (!fs::is_directory(transaction)) {
            logger.warning(L"Removed a stale installer recovery marker");
            std::error_code error;
            fs::remove(markerPath, error);
            continue;
        }
        InstalledRecord record;
        record.present = true;
        record.scope = scope;
        record.installRoot = root;
        record.cacheExecutable = cache;
        record.version = package.metadata.version;
        logger.warning(L"Found an interrupted installer transaction requiring recovery");
        return record;
    }
    return std::nullopt;
}

/// @brief Parse and validate the installed ownership manifest.
/// @param installRoot Root against which the manifest path is resolved safely.
/// @param manifestRelative Relative path of either the current tabular or legacy line format.
/// @return Deduplicated normalized relative paths owned by the installer.
/// @throws std::runtime_error If the manifest is malformed, duplicated, or contains unsafe paths.
std::set<std::string> loadOwnershipManifest(const fs::path &installRoot,
                                            std::string_view manifestRelative) {
    const fs::path path = safeJoin(installRoot, manifestRelative);
    const std::wstring text = readTextFileWide(path);
    std::set<std::string> owned;
    if (text.empty())
        return owned;
    const auto lines = splitLines(text);
    size_t start = 0;
    if (!lines.empty() && lines.front() == kManifestHeader)
        start = 1;
    for (size_t i = start; i < lines.size(); ++i) {
        std::wstring relative;
        if (start == 1) {
            const size_t first = lines[i].find(L'\t');
            const size_t second =
                first == std::wstring::npos ? std::wstring::npos : lines[i].find(L'\t', first + 1);
            if (second == std::wstring::npos)
                throw std::runtime_error("installed ownership manifest is malformed");
            relative = lines[i].substr(second + 1);
        } else {
            relative = lines[i];
        }
        const std::string utf8 = wideToUtf8(relative);
        validateRelativePath(utf8);
        if (!owned.insert(normalizedPathKey(utf8)).second)
            throw std::runtime_error("installed ownership manifest contains a duplicate path");
    }
    return owned;
}

/// @brief Read an entire maintenance-cache file into memory.
/// @param path File to read.
/// @return Complete contents, or an empty vector if the file cannot be opened.
/// @throws std::runtime_error If its reported size is unsupported or the read fails.
std::vector<uint8_t> readFileBytes(const fs::path &path);

/// @brief Build the normalized ownership set implied by package metadata.
/// @param package Package whose payload, metadata, and maintenance paths are included.
/// @return Normalized installer-owned relative paths.
std::set<std::string> packageOwnedPaths(const HostPackage &package) {
    std::set<std::string> owned;
    for (const auto &file : package.metadata.payloadFiles)
        owned.insert(normalizedPathKey(file.path));
    for (const auto &file : package.metadata.outerFiles)
        owned.insert(normalizedPathKey(file.path));
    owned.insert(normalizedPathKey(package.metadata.uninstallerRelativePath));
    owned.insert(normalizedPathKey(package.metadata.stateRelativePath));
    owned.insert(normalizedPathKey(package.metadata.installedManifestRelativePath));
    return owned;
}

/// @brief Read one localized string from an executable version resource.
/// @param path Executable whose version resource is queried.
/// @param field String-table field name, such as @c OriginalFilename.
/// @return Field value without its terminator, or @c std::nullopt when unavailable or invalid.
std::optional<std::wstring> versionResourceString(const fs::path &path, std::wstring_view field) {
    DWORD ignored = 0;
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (bytes == 0U || bytes > 16U * 1024U * 1024U)
        return std::nullopt;
    std::vector<uint8_t> resource(bytes);
    if (!GetFileVersionInfoW(path.c_str(), 0, bytes, resource.data()))
        return std::nullopt;

    struct Translation {
        WORD language;
        WORD codePage;
    };

    Translation *translations = nullptr;
    UINT translationBytes = 0;
    if (!VerQueryValueW(resource.data(),
                        L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void **>(&translations),
                        &translationBytes) ||
        !translations || translationBytes < sizeof(Translation)) {
        return std::nullopt;
    }
    wchar_t query[256]{};
    if (swprintf_s(query,
                   L"\\StringFileInfo\\%04x%04x\\%.*s",
                   translations[0].language,
                   translations[0].codePage,
                   static_cast<int>(field.size()),
                   field.data()) < 0) {
        return std::nullopt;
    }
    wchar_t *value = nullptr;
    UINT valueChars = 0;
    if (!VerQueryValueW(resource.data(), query, reinterpret_cast<void **>(&value), &valueChars) ||
        !value || valueChars <= 1U) {
        return std::nullopt;
    }
    return std::wstring(value, valueChars - 1U);
}

/// @brief Search raw bytes for the native UTF-16 representation of text.
/// @param bytes Binary image to scan.
/// @param text Nonempty wide string to locate.
/// @return @c true when the complete wide-string byte sequence occurs.
bool containsWideBytes(const std::vector<uint8_t> &bytes, std::wstring_view text) {
    if (text.empty() || text.size() > std::numeric_limits<size_t>::max() / sizeof(wchar_t))
        return false;
    const auto *begin = reinterpret_cast<const uint8_t *>(text.data());
    const size_t length = text.size() * sizeof(wchar_t);
    return std::search(bytes.begin(), bytes.end(), begin, begin + length) != bytes.end();
}

/// @brief Recognize a generated legacy uninstaller without trusting it as a package.
/// @param path Candidate legacy executable.
/// @param incomingPackage Incoming package supplying identity strings expected in the binary.
/// @return @c true when attributes, version metadata, size, and embedded identity all match.
bool isRecognizedLegacyUninstaller(const fs::path &path, const HostPackage &incomingPackage) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        return false;
    }
    const auto originalName = versionResourceString(path, L"OriginalFilename");
    const auto description = versionResourceString(path, L"FileDescription");
    if (!originalName || !ordinalEqualsIgnoreCase(*originalName, L"uninstall.exe") ||
        !description || description->size() < 12U ||
        !ordinalEqualsIgnoreCase(description->substr(description->size() - 12U), L" uninstaller")) {
        return false;
    }
    std::error_code sizeError;
    const uintmax_t size = fs::file_size(path, sizeError);
    if (sizeError || size == 0U || size > 512ULL * 1024ULL * 1024ULL)
        return false;
    const std::vector<uint8_t> bytes = readFileBytes(path);
    return containsWideBytes(bytes, utf8ToWide(incomingPackage.metadata.identifier)) &&
           containsWideBytes(bytes,
                             utf8ToWide(incomingPackage.metadata.installedManifestRelativePath));
}

/// @brief Establish ownership for upgrades, including verified and recognized legacy installs.
/// @param incomingPackage Verified package defining current manifest and identity metadata.
/// @param installRoot Existing installation root to inspect.
/// @param logger Installer logger used to report migration decisions.
/// @return Normalized paths considered owned by the existing installer.
std::set<std::string> loadUpgradeOwnership(const HostPackage &incomingPackage,
                                           const fs::path &installRoot,
                                           Logger &logger) {
    std::set<std::string> owned =
        loadOwnershipManifest(installRoot, incomingPackage.metadata.installedManifestRelativePath);
    if (!owned.empty() || !fs::is_directory(installRoot))
        return owned;
    const fs::path uninstaller =
        safeJoin(installRoot, incomingPackage.metadata.uninstallerRelativePath);
    if (!fs::is_regular_file(uninstaller))
        return owned;
    try {
        const HostPackage previous = loadHostPackage(uninstaller);
        if (previous.metadata.identifier != incomingPackage.metadata.identifier)
            throw std::runtime_error("existing maintenance package identifier does not match");
        logger.warning(L"Migrating a verified Zanna installation with a missing ownership "
                       L"manifest");
        return packageOwnedPaths(previous);
    } catch (const std::exception &error) {
        if (!isRecognizedLegacyUninstaller(uninstaller, incomingPackage)) {
            logger.warning(L"Could not establish legacy installer ownership: " +
                           utf8ToWide(error.what()));
            return owned;
        }
    }
    logger.warning(L"Migrating generated legacy installation at " + installRoot.wstring());
    return packageOwnedPaths(incomingPackage);
}

/// @brief Preserve user-owned regular files while constructing a replacement tree.
/// @param oldRoot Existing installation tree.
/// @param newRoot Staged replacement tree.
/// @param owned Normalized paths that belong to the installer and must not be copied.
/// @param logger Installer logger and cancellation source.
/// @throws std::runtime_error On unsafe entries, traversal errors, conflicts, or copy failure.
void copyUnownedFiles(const fs::path &oldRoot,
                      const fs::path &newRoot,
                      const std::set<std::string> &owned,
                      Logger &logger) {
    if (!fs::exists(oldRoot))
        return;
    std::error_code error;
    for (fs::recursive_directory_iterator
             it(oldRoot, fs::directory_options::skip_permission_denied, error),
         end;
         it != end;
         it.increment(error)) {
        cancellationPoint(logger);
        if (error)
            throw std::runtime_error("cannot enumerate existing installation content");
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::runtime_error("existing installation contains a reparse point");
        }
        const fs::path relativePath = fs::relative(it->path(), oldRoot, error);
        if (error)
            throw std::runtime_error("cannot resolve existing installation content");
        const std::string relative = slashPath(wideToUtf8(relativePath.generic_wstring()));
        validateRelativePath(relative);
        const fs::path destination = safeJoin(newRoot, relative);
        if (it->is_directory(error)) {
            if (error)
                throw std::runtime_error("cannot inspect existing installation directory");
            continue;
        }
        if (!it->is_regular_file(error) || error)
            throw std::runtime_error("existing installation contains an unsupported file type");
        if (owned.find(normalizedPathKey(relative)) != owned.end())
            continue;
        if (fs::exists(destination)) {
            throw std::runtime_error("unowned file conflicts with the new Zanna payload: " +
                                     relative);
        }
        fs::create_directories(destination.parent_path());
        fs::copy_file(it->path(), destination, fs::copy_options::none, error);
        if (error)
            throw std::runtime_error("cannot preserve unowned installation file: " + relative);
        logger.info(L"Preserved unowned file: " + utf8ToWide(relative));
    }
}

/// @brief Serialize installed package identity, selection, and integration settings.
/// @param package Verified package providing identifier and version.
/// @param plan Resolved scope, components, and integration choices.
/// @return Versioned UTF-16 state-file contents.
std::wstring stateText(const HostPackage &package, const InstallationPlan &plan) {
    std::wostringstream out;
    out << kStateHeader << L"\r\n"
        << L"identifier\t" << utf8ToWide(package.metadata.identifier) << L"\r\n"
        << L"version\t" << utf8ToWide(package.metadata.version) << L"\r\n"
        << L"scope\t" << (plan.scope == InstallScope::User ? L"user" : L"machine") << L"\r\n"
        << L"components\t" << joinComponents(plan.components) << L"\r\n"
        << L"add-to-path\t" << (plan.addToPath ? L'1' : L'0') << L"\r\n"
        << L"associations\t" << (plan.registerAssociations ? L'1' : L'0') << L"\r\n"
        << L"shortcuts\t" << (plan.createShortcuts ? L'1' : L'0') << L"\r\n";
    return out.str();
}

/// @brief Serialize the installed ownership manifest for staged files and state.
/// @param package Verified package providing metadata file locations.
/// @param installedFiles Payload and maintenance files written into the staged tree.
/// @param stateHash SHA-256 digest of the encoded state file.
/// @param stateSize Encoded state-file size in bytes.
/// @return Sorted, versioned UTF-16 ownership-manifest contents.
std::wstring manifestText(const HostPackage &package,
                          const std::vector<SelectedFile> &installedFiles,
                          std::string_view stateHash,
                          uint64_t stateSize) {
    std::vector<SelectedFile> files = installedFiles;
    files.push_back({package.metadata.stateRelativePath, std::string(stateHash), stateSize});
    /// @brief Order installed files by normalized package path.
    /// @param left Left-hand file record.
    /// @param right Right-hand file record.
    /// @return `true` when `left` precedes `right`.
    std::sort(files.begin(), files.end(), [](const SelectedFile &left, const SelectedFile &right) {
        return normalizedPathKey(left.path) < normalizedPathKey(right.path);
    });
    std::wostringstream out;
    out << kManifestHeader << L"\r\n";
    for (const SelectedFile &file : files)
        out << utf8ToWide(file.sha256) << L'\t' << file.sizeBytes << L'\t'
            << utf8ToWide(slashPath(file.path)) << L"\r\n";
    out << L'-' << L'\t' << 0 << L'\t'
        << utf8ToWide(slashPath(package.metadata.installedManifestRelativePath)) << L"\r\n";
    return out.str();
}

/// @brief Select the maintenance executable embedded by the current package mode.
/// @param package Verified setup or maintenance package.
/// @return Executable bytes to install as the product uninstaller.
/// @throws std::runtime_error If setup metadata references absent outer-file bytes.
std::vector<uint8_t> maintenanceBytes(const HostPackage &package) {
    if (!package.metadata.outerFiles.empty()) {
        const auto &record = package.metadata.outerFiles.front();
        const auto found = package.outerFileBytes.find(record.overlayPath);
        if (found == package.outerFileBytes.end())
            throw std::runtime_error("setup package lacks its maintenance executable");
        return found->second;
    }
    return package.executableBytes;
}

/// @brief Materialize and verify the selected installation tree in a staging root.
/// @param package Verified package and embedded archive bytes.
/// @param plan Resolved component selection and installed-state settings.
/// @param newRoot Empty transaction directory that receives the staged tree.
/// @param logger Installer logger and cancellation source.
/// @return Records for installed payload and maintenance files.
/// @throws std::runtime_error On missing entries, digest mismatch, cancellation, or write failure.
std::vector<SelectedFile> stageSelectedTree(const HostPackage &package,
                                            const InstallationPlan &plan,
                                            const fs::path &newRoot,
                                            Logger &logger) {
    zanna::pkg::ZipReader payload(package.payloadZip.data(), package.payloadZip.size());
    std::vector<SelectedFile> installed;
    for (const auto &record : package.metadata.payloadFiles) {
        cancellationPoint(logger);
        if (!componentEnabled(record.componentId, plan.components))
            continue;
        const zanna::pkg::ZipEntry *entry = payload.find(record.path);
        if (!entry)
            throw std::runtime_error("selected payload entry is missing");
        std::vector<uint8_t> bytes = payload.extract(*entry);
        if (bytes.size() != record.sizeBytes ||
            zanna::pkg::sha256Hex(bytes.data(), bytes.size()) != record.sha256) {
            throw std::runtime_error("selected payload entry failed SHA-256 verification");
        }
        const fs::path destination = safeJoin(newRoot, record.path);
        writeBytesAtomic(destination, bytes);
        installed.push_back({record.path, record.sha256, record.sizeBytes});
    }

    const std::vector<uint8_t> uninstaller = maintenanceBytes(package);
    cancellationPoint(logger);
    const std::string uninstallerHash =
        zanna::pkg::sha256Hex(uninstaller.data(), uninstaller.size());
    writeBytesAtomic(safeJoin(newRoot, package.metadata.uninstallerRelativePath), uninstaller);
    installed.push_back(
        {package.metadata.uninstallerRelativePath, uninstallerHash, uninstaller.size()});

    const std::wstring state = stateText(package, plan);
    const std::string stateUtf8 = wideToUtf8(state);
    const std::string stateHash = zanna::pkg::sha256Hex(
        reinterpret_cast<const uint8_t *>(stateUtf8.data()), stateUtf8.size());
    writeTextAtomic(safeJoin(newRoot, package.metadata.stateRelativePath), state);
    const std::wstring manifest = manifestText(package, installed, stateHash, stateUtf8.size());
    writeTextAtomic(safeJoin(newRoot, package.metadata.installedManifestRelativePath), manifest);
    logger.info(L"Selected payload staged and verified");
    return installed;
}

/// @brief Durable phase recorded for an installation-directory transaction.
enum class JournalState {
    None,
    Prepared,
    OldMoved,
    NewActive,
    MetadataCommitted,
    RollbackFilesRestored,
    Committed
};

/// @brief Convert a transaction phase to its stable journal token.
/// @param state Phase to encode.
/// @return Lowercase token, or @c none for the absence of a transaction.
std::wstring journalName(JournalState state) {
    switch (state) {
        case JournalState::Prepared:
            return L"prepared";
        case JournalState::OldMoved:
            return L"old-moved";
        case JournalState::NewActive:
            return L"new-active";
        case JournalState::MetadataCommitted:
            return L"metadata-committed";
        case JournalState::RollbackFilesRestored:
            return L"rollback-files-restored";
        case JournalState::Committed:
            return L"committed";
        case JournalState::None:
        default:
            return L"none";
    }
}

/// @brief Parse and strictly validate a transaction journal document.
/// @param value Complete journal text, or empty text when no journal exists.
/// @return Recorded transaction phase, including @c None for empty input.
/// @throws std::runtime_error If nonempty input does not match the current schema exactly.
JournalState parseJournal(std::wstring_view value) {
    if (value.empty())
        return JournalState::None;
    constexpr std::array states = {JournalState::Prepared,
                                   JournalState::OldMoved,
                                   JournalState::NewActive,
                                   JournalState::MetadataCommitted,
                                   JournalState::RollbackFilesRestored,
                                   JournalState::Committed};
    for (const JournalState state : states) {
        if (value == L"schema=2\r\nstate=" + journalName(state) + L"\r\n")
            return state;
    }
    throw std::runtime_error("installer transaction journal is malformed");
}

/// @brief Fixed paths comprising one installation transaction workspace.
struct TransactionPaths {
    fs::path directory;
    fs::path newRoot;
    fs::path oldRoot;
    fs::path journal;
    fs::path pathBackup;
    fs::path appliedShortcuts;
};

/// @brief Derive the deterministic transaction workspace beside an installation root.
/// @param plan Resolved installation root.
/// @param identifier Stable package identifier used to disambiguate the workspace.
/// @return Directory, staged roots, journal, and rollback-metadata paths.
TransactionPaths transactionPaths(const InstallationPlan &plan, std::string_view identifier) {
    const fs::path directory = plan.installRoot.parent_path() /
                               (L"." + plan.installRoot.filename().wstring() +
                                L".zanna-transaction-" + hashHex(fnv1a64(utf8ToWide(identifier))));
    return {directory,
            directory / L"new",
            directory / L"old",
            directory / L"journal.txt",
            directory / L"path-backup.txt",
            directory / L"applied-shortcuts.txt"};
}

/// @brief Atomically record the current transaction phase.
/// @param paths Transaction workspace containing the journal location.
/// @param state Durable phase to record.
/// @throws std::runtime_error If the journal cannot be written atomically.
void writeJournal(const TransactionPaths &paths, JournalState state) {
    writeTextAtomic(paths.journal, L"schema=2\r\nstate=" + journalName(state) + L"\r\n");
}

/// @brief Remove a transaction tree only after proving it contains no reparse points.
/// @param path Tree to validate and recursively remove; a missing path is accepted.
/// @throws std::runtime_error On unsafe attributes, enumeration failure, or incomplete removal.
void removeTreeChecked(const fs::path &path) {
    if (!fs::exists(path))
        return;
    const DWORD rootAttributes = GetFileAttributesW(path.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES ||
        (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        throw std::runtime_error("refusing to remove a reparse-point transaction tree");
    }
    std::error_code error;
    for (fs::recursive_directory_iterator
             it(path, fs::directory_options::skip_permission_denied, error),
         end;
         it != end;
         it.increment(error)) {
        if (error)
            throw std::runtime_error("cannot inspect an installer tree before removal");
        const DWORD attributes = GetFileAttributesW(it->path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            throw std::runtime_error("refusing to remove a tree containing a reparse point");
        }
    }
    fs::remove_all(path, error);
    if (error || fs::exists(path))
        throw std::runtime_error("cannot remove installer transaction path: " +
                                 wideToUtf8(path.wstring()));
}

/// @brief Atomically rename a directory with bounded retries for transient sharing failures.
/// @param source Existing directory to move.
/// @param destination Nonconflicting destination path.
/// @throws std::runtime_error If the move does not succeed within the retry window.
void moveDirectory(const fs::path &source, const fs::path &destination) {
    constexpr ULONGLONG kRetryWindowMilliseconds = 30000U;
    DWORD delayMilliseconds = 25U;
    DWORD error = ERROR_SUCCESS;
    const ULONGLONG deadline = GetTickCount64() + kRetryWindowMilliseconds;
    do {
        if (MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
            return;
        error = GetLastError();
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
            error != ERROR_LOCK_VIOLATION && error != ERROR_BUSY) {
            break;
        }
        if (GetTickCount64() >= deadline)
            break;
        Sleep(delayMilliseconds);
        delayMilliseconds = std::min<DWORD>(delayMilliseconds * 2U, 500U);
    } while (true);
    throw std::runtime_error("cannot atomically move installation directory after retrying: " +
                             wideToUtf8(formatWindowsError(error)));
}

/// @brief Recover or complete a transaction according to its durable journal phase.
/// @param package Verified package supplying identity and installed metadata paths.
/// @param plan Resolved installation state.
/// @param paths Transaction workspace paths.
/// @param logger Installer logger used for recovery diagnostics.
/// @throws std::runtime_error If the transaction is malformed or cannot be recovered safely.
void recoverTransaction(const HostPackage &package,
                        const InstallationPlan &plan,
                        const TransactionPaths &paths,
                        Logger &logger);

/// @brief Apply an optional installer test hook at a named transaction boundary.
/// @param stage Stable stage token compared with enabled hook environment variables.
/// @throws InstallerError When cancellation injection selects this stage.
/// @throws std::runtime_error When failure injection selects this stage.
void maybeInjectFailure(std::string_view stage) {
#if defined(ZANNA_INSTALLER_ENABLE_TEST_HOOKS)
    const wchar_t *value = _wgetenv(L"ZANNA_INSTALLER_TEST_CANCEL_AT");
    if (value && wideToUtf8(value) == stage)
        throw InstallerError(kExitUserCancelled,
                             "injected installer cancellation at " + std::string(stage));
    value = _wgetenv(L"ZANNA_INSTALLER_TEST_FAIL_AT");
    if (value && wideToUtf8(value) == stage)
        throw std::runtime_error("injected installer failure at " + std::string(stage));
    value = _wgetenv(L"ZANNA_INSTALLER_TEST_CRASH_AT");
    if (value && wideToUtf8(value) == stage)
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(kExitFatalError));
    value = _wgetenv(L"ZANNA_INSTALLER_TEST_PAUSE_AT");
    if (value && wideToUtf8(value) == stage) {
        DWORD milliseconds = 3000U;
        if (const wchar_t *duration = _wgetenv(L"ZANNA_INSTALLER_TEST_PAUSE_MS")) {
            wchar_t *end = nullptr;
            errno = 0;
            const unsigned long parsed = std::wcstoul(duration, &end, 10);
            if (errno == 0 && end && *end == L'\0' && parsed >= 1U && parsed <= 30000U)
                milliseconds = static_cast<DWORD>(parsed);
        }
        Sleep(milliseconds);
    }
#else
    static_cast<void>(stage);
#endif
}

/// @brief Apply the enabled test-hook ceiling to a measured free-space value.
/// @param available Actual available bytes reported by Windows.
/// @return The lesser of the actual value and a valid configured ceiling.
uint64_t testLimitedFreeBytes(uint64_t available) {
#if defined(ZANNA_INSTALLER_ENABLE_TEST_HOOKS)
    const wchar_t *value = _wgetenv(L"ZANNA_INSTALLER_TEST_FREE_BYTES");
    if (value && *value) {
        wchar_t *end = nullptr;
        errno = 0;
        const unsigned long long parsed = std::wcstoull(value, &end, 10);
        if (errno == 0 && end && *end == L'\0')
            return std::min<uint64_t>(available, parsed);
    }
#endif
    return available;
}

/// @brief Split a registry PATH value while preserving empty and untrimmed entries.
/// @param value Semicolon-delimited PATH text.
/// @return Entries in their original order and spelling.
std::vector<std::wstring> splitPathValue(std::wstring_view value) {
    std::vector<std::wstring> entries;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t end = value.find(L';', start);
        entries.emplace_back(value.substr(
            start, end == std::wstring_view::npos ? value.size() - start : end - start));
        if (end == std::wstring_view::npos)
            break;
        start = end + 1;
    }
    return entries;
}

/// @brief Normalize a PATH entry for case-insensitive comparisons.
/// @param value Entry to trim, unquote, and strip of non-root trailing separators.
/// @return Comparison form of the entry.
std::wstring trimPathEntry(std::wstring value) {
    while (!value.empty() && std::iswspace(value.front()))
        value.erase(value.begin());
    while (!value.empty() && std::iswspace(value.back()))
        value.pop_back();
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/'))
        value.pop_back();
    return value;
}

/// @brief Exact registry PATH state captured for transactional rollback.
struct PathBackup {
    bool present{false};
    DWORD type{REG_EXPAND_SZ};
    std::wstring value;
};

/// @brief Snapshot a bounded string-valued PATH registry entry.
/// @param environment Open environment key with query access.
/// @return Presence, registry type, and value; an absent entry produces the default snapshot.
/// @throws std::runtime_error On invalid type, excessive size, or repeated concurrent changes.
PathBackup readPathValue(HKEY environment) {
    if (!environment)
        throw std::runtime_error("cannot read PATH through a null registry key");
    for (unsigned attempt = 0; attempt < 8U; ++attempt) {
        DWORD type = 0;
        DWORD bytes = 0;
        LONG result = RegQueryValueExW(environment, L"Path", nullptr, &type, nullptr, &bytes);
        if (result == ERROR_FILE_NOT_FOUND)
            return {};
        if (result != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) ||
            bytes > 32U * 1024U * 1024U || bytes % sizeof(wchar_t) != 0U) {
            throw std::runtime_error("cannot snapshot the environment PATH value");
        }
        std::vector<wchar_t> value(static_cast<size_t>(bytes) / sizeof(wchar_t) + 1U, L'\0');
        DWORD readBytes = bytes;
        DWORD readType = 0;
        result = RegQueryValueExW(environment,
                                  L"Path",
                                  nullptr,
                                  &readType,
                                  reinterpret_cast<BYTE *>(value.data()),
                                  &readBytes);
        if (result == ERROR_MORE_DATA)
            continue;
        if (result != ERROR_SUCCESS || (readType != REG_SZ && readType != REG_EXPAND_SZ) ||
            readBytes > bytes || readBytes % sizeof(wchar_t) != 0U) {
            throw std::runtime_error("cannot read the environment PATH value for rollback");
        }
        const size_t readCharacters = static_cast<size_t>(readBytes) / sizeof(wchar_t);
        const size_t length =
            std::find(value.begin(), value.begin() + readCharacters, L'\0') - value.begin();
        return {true, readType, std::wstring(value.data(), length)};
    }
    throw std::runtime_error("the environment PATH changed repeatedly while being read");
}

/// @brief Read the user or machine environment PATH.
/// @param scope Installation scope selecting the registry hive and environment key.
/// @return Exact PATH snapshot suitable for rollback.
PathBackup readCurrentPath(InstallScope scope) {
    RegKey environment =
        openKey(rootKey(scope),
                scope == InstallScope::User ? kUserEnvironment : kMachineEnvironment,
                KEY_QUERY_VALUE,
                true);
    return readPathValue(environment.get());
}

/// @brief Notify desktop applications that environment variables changed.
void broadcastEnvironment() {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST,
                        WM_SETTINGCHANGE,
                        0,
                        reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG,
                        5000,
                        &result);
}

/// @brief Transactionally remove and optionally append an installation PATH entry.
/// @param scope Registry scope whose environment PATH is updated.
/// @param removeEntry Entry to remove case-insensitively after comparison normalization.
/// @param addEntry Entry to append after duplicates are removed; empty means removal only.
/// @return Original PATH text for diagnostics or compatibility.
/// @throws std::runtime_error If the registry value cannot be read or written.
std::wstring updatePath(InstallScope scope,
                        std::wstring_view removeEntry,
                        std::wstring_view addEntry) {
    RegKey environment =
        openKey(rootKey(scope),
                scope == InstallScope::User ? kUserEnvironment : kMachineEnvironment,
                KEY_QUERY_VALUE | KEY_SET_VALUE,
                true);
    const PathBackup original = readPathValue(environment.get());
    std::vector<std::wstring> entries = splitPathValue(original.value);
    const std::wstring removeKey = trimPathEntry(std::wstring(removeEntry));
    const std::wstring addKey = trimPathEntry(std::wstring(addEntry));
    /// @brief Identify an empty, removed, or duplicate PATH entry.
    /// @param entry Existing PATH entry to normalize and compare.
    /// @return `true` when the entry should be erased before the optional append.
    entries.erase(
        std::remove_if(entries.begin(),
                       entries.end(),
                       [&](const std::wstring &entry) {
                           const std::wstring key = trimPathEntry(entry);
                           return key.empty() ||
                                  (!removeKey.empty() && ordinalEqualsIgnoreCase(key, removeKey)) ||
                                  (!addKey.empty() && ordinalEqualsIgnoreCase(key, addKey));
                       }),
        entries.end());
    if (!addEntry.empty())
        entries.emplace_back(addEntry);
    std::wstring updated;
    for (const std::wstring &entry : entries) {
        if (!updated.empty())
            updated.push_back(L';');
        updated += entry;
    }
    setRegistryString(
        environment.get(), L"Path", updated, original.present ? original.type : REG_EXPAND_SZ);
    broadcastEnvironment();
    return original.value;
}

/// @brief Encode arbitrary bytes as lowercase wide-character hexadecimal.
/// @param bytes Byte sequence to encode.
/// @return Two hexadecimal characters per input byte.
std::wstring bytesToHex(std::string_view bytes) {
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(bytes.size() * 2U);
    for (const unsigned char byte : bytes) {
        result.push_back(kHex[byte >> 4U]);
        result.push_back(kHex[byte & 0x0FU]);
    }
    return result;
}

/// @brief Decode one supported lowercase hexadecimal digit.
/// @param ch Digit in the ranges @c 0-9 or @c a-f.
/// @return Numeric nibble value.
/// @throws std::runtime_error If @p ch is not valid transaction hexadecimal.
unsigned hexNibble(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9')
        return static_cast<unsigned>(ch - L'0');
    if (ch >= L'a' && ch <= L'f')
        return 10U + static_cast<unsigned>(ch - L'a');
    throw std::runtime_error("invalid installer transaction hexadecimal data");
}

/// @brief Decode even-length lowercase wide-character hexadecimal.
/// @param text Encoded transaction data.
/// @return Original byte string.
/// @throws std::runtime_error On odd length or an unsupported digit.
std::string hexToBytes(std::wstring_view text) {
    if ((text.size() & 1U) != 0)
        throw std::runtime_error("invalid installer transaction hexadecimal length");
    std::string result;
    result.reserve(text.size() / 2U);
    for (size_t i = 0; i < text.size(); i += 2U) {
        result.push_back(static_cast<char>((hexNibble(text[i]) << 4U) | hexNibble(text[i + 1U])));
    }
    return result;
}

/// @brief Persist the exact current PATH state before metadata changes.
/// @param paths Transaction workspace containing the rollback backup.
/// @param scope Registry scope whose PATH is captured.
/// @throws std::runtime_error If the PATH cannot be read or the backup cannot be written.
void writePathBackup(const TransactionPaths &paths, InstallScope scope) {
    const PathBackup backup = readCurrentPath(scope);
    std::wostringstream text;
    text << L"ZANNA-PATH-BACKUP\t1\r\n"
         << L"present\t" << (backup.present ? L'1' : L'0') << L"\r\n"
         << L"type\t" << backup.type << L"\r\n"
         << L"value\t" << bytesToHex(wideToUtf8(backup.value)) << L"\r\n";
    writeTextAtomic(paths.pathBackup, text.str());
}

/// @brief Parse and validate the PATH rollback backup in a transaction workspace.
/// @param paths Transaction workspace containing the backup.
/// @return Exact registry presence, type, and value captured before mutation.
/// @throws std::runtime_error If the backup is missing, malformed, duplicated, or invalid.
PathBackup readPathBackup(const TransactionPaths &paths) {
    const std::wstring text = readTextFileWide(paths.pathBackup);
    if (text.rfind(L"ZANNA-PATH-BACKUP\t1\r\n", 0) != 0)
        throw std::runtime_error("installer transaction PATH backup is missing or invalid");
    PathBackup result;
    bool sawPresent = false;
    bool sawType = false;
    bool sawValue = false;
    for (const std::wstring &line : splitLines(text)) {
        const size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos)
            continue;
        const std::wstring key = line.substr(0, tab);
        const std::wstring value = line.substr(tab + 1U);
        if (key == L"present") {
            if (sawPresent || (value != L"0" && value != L"1"))
                throw std::runtime_error("invalid installer transaction PATH presence");
            result.present = value == L"1";
            sawPresent = true;
        } else if (key == L"type") {
            if (sawType ||
                (value != std::to_wstring(REG_SZ) && value != std::to_wstring(REG_EXPAND_SZ))) {
                throw std::runtime_error("invalid installer transaction PATH type");
            }
            result.type = value == std::to_wstring(REG_SZ) ? REG_SZ : REG_EXPAND_SZ;
            sawType = true;
        } else if (key == L"value") {
            if (sawValue)
                throw std::runtime_error("duplicate installer transaction PATH value");
            result.value = utf8ToWide(hexToBytes(value));
            sawValue = true;
        }
    }
    if (!sawPresent || !sawType || !sawValue)
        throw std::runtime_error("incomplete installer transaction PATH backup");
    return result;
}

/// @brief Restore the exact registry PATH state captured before a transaction.
/// @param paths Transaction workspace containing the PATH backup.
/// @param scope Registry scope to restore.
/// @throws std::runtime_error If the backup or registry mutation is invalid.
void restorePathBackup(const TransactionPaths &paths, InstallScope scope) {
    const PathBackup backup = readPathBackup(paths);
    RegKey environment =
        openKey(rootKey(scope),
                scope == InstallScope::User ? kUserEnvironment : kMachineEnvironment,
                KEY_SET_VALUE,
                true);
    if (backup.present) {
        setRegistryString(environment.get(), L"Path", backup.value, backup.type);
    } else {
        const LONG result = RegDeleteValueW(environment.get(), L"Path");
        if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
            throw std::runtime_error("cannot restore the absent environment PATH value");
    }
    broadcastEnvironment();
}

/// @brief Create an empty durable journal for shortcuts applied by a transaction.
/// @param paths Transaction workspace containing the shortcut journal.
void initializeAppliedShortcuts(const TransactionPaths &paths) {
    writeTextAtomic(paths.appliedShortcuts, L"ZANNA-APPLIED-SHORTCUTS\t1\r\n");
}

/// @brief Append and durably flush one shortcut path to the rollback journal.
/// @param paths Transaction workspace containing the shortcut journal.
/// @param path Absolute shortcut path created by the transaction.
/// @throws std::runtime_error If the entry is too large or cannot be persisted.
void recordAppliedShortcut(const TransactionPaths &paths, const fs::path &path) {
    UniqueHandle file(CreateFileW(paths.appliedShortcuts.c_str(),
                                  FILE_APPEND_DATA,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr));
    if (!file)
        throw std::runtime_error("cannot open the shortcut rollback journal");
    const std::wstring line = bytesToHex(wideToUtf8(path.wstring())) + L"\r\n";
    const std::string bytes = wideToUtf8(line);
    if (bytes.size() > static_cast<size_t>(MAXDWORD))
        throw std::runtime_error("shortcut rollback journal entry is too large");
    const DWORD byteCount = static_cast<DWORD>(bytes.size());
    DWORD written = 0;
    if (!WriteFile(file.get(), bytes.data(), byteCount, &written, nullptr) ||
        written != byteCount || !FlushFileBuffers(file.get())) {
        throw std::runtime_error("cannot update the shortcut rollback journal");
    }
}

/// @brief Read shortcut paths recorded for transactional rollback.
/// @param paths Transaction workspace containing the shortcut journal.
/// @return Decoded shortcut paths in application order.
/// @throws std::runtime_error If the journal schema or an encoded entry is invalid.
std::vector<fs::path> readAppliedShortcuts(const TransactionPaths &paths) {
    const std::wstring text = readTextFileWide(paths.appliedShortcuts);
    if (text.rfind(L"ZANNA-APPLIED-SHORTCUTS\t1\r\n", 0) != 0)
        throw std::runtime_error("shortcut rollback journal is missing or invalid");
    std::vector<fs::path> result;
    const std::vector<std::wstring> lines = splitLines(text);
    for (size_t i = 1; i < lines.size(); ++i) {
        if (!lines[i].empty())
            result.emplace_back(utf8ToWide(hexToBytes(lines[i])));
    }
    return result;
}

/// @brief Remove only file associations owned by this package identity.
/// @param package Package providing association metadata and ownership identifier.
/// @param scope Registry scope from which associations are removed.
/// @throws std::runtime_error If an owned association cannot be removed safely.
void unregisterAssociations(const HostPackage &package, InstallScope scope) {
    for (const auto &association : package.metadata.associations) {
        const std::wstring progIdKey = std::wstring(kClassesBase) + utf8ToWide(association.progId);
        if (RegKey key = openKey(rootKey(scope), progIdKey, KEY_READ, false)) {
            const auto owner = queryRegistryString(key.get(), L"ZannaOwner");
            if (owner && wideToUtf8(*owner) == package.metadata.identifier) {
                const std::wstring extensionKey = std::wstring(kClassesBase) +
                                                  utf8ToWide(association.extension) +
                                                  L"\\OpenWithProgids";
                bool openWithEmpty = false;
                {
                    if (RegKey openWith = openKey(
                            rootKey(scope), extensionKey, KEY_QUERY_VALUE | KEY_SET_VALUE, false)) {
                        const LONG removed =
                            RegDeleteValueW(openWith.get(), utf8ToWide(association.progId).c_str());
                        if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND)
                            throw std::runtime_error("cannot remove a Zanna Open-With entry");
                        DWORD subkeys = 0;
                        DWORD values = 0;
                        if (RegQueryInfoKeyW(openWith.get(),
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             &subkeys,
                                             nullptr,
                                             nullptr,
                                             &values,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr) == ERROR_SUCCESS) {
                            openWithEmpty = subkeys == 0 && values == 0;
                        }
                    }
                }
                if (openWithEmpty) {
                    RegDeleteKeyW(rootKey(scope), extensionKey.c_str());
                    const std::wstring extensionBase =
                        std::wstring(kClassesBase) + utf8ToWide(association.extension);
                    bool extensionEmpty = false;
                    if (RegKey extension =
                            openKey(rootKey(scope), extensionBase, KEY_READ, false)) {
                        DWORD subkeys = 0;
                        DWORD values = 0;
                        if (RegQueryInfoKeyW(extension.get(),
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             &subkeys,
                                             nullptr,
                                             nullptr,
                                             &values,
                                             nullptr,
                                             nullptr,
                                             nullptr,
                                             nullptr) == ERROR_SUCCESS) {
                            extensionEmpty = subkeys == 0 && values == 0;
                        }
                    }
                    if (extensionEmpty)
                        RegDeleteKeyW(rootKey(scope), extensionBase.c_str());
                }
                const LONG removed = RegDeleteTreeW(rootKey(scope), progIdKey.c_str());
                if (removed != ERROR_SUCCESS && removed != ERROR_FILE_NOT_FOUND)
                    throw std::runtime_error("cannot remove a Zanna file-association ProgID");
            }
        }
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

/// @brief Replace package-owned file associations with the resolved installation settings.
/// @param package Verified package providing association definitions and executable metadata.
/// @param plan Resolved scope, installation root, and association selection.
/// @param logger Installer logger and cancellation source.
/// @throws std::runtime_error If owned registry entries cannot be created or updated.
void registerAssociations(const HostPackage &package,
                          const InstallationPlan &plan,
                          Logger &logger) {
    unregisterAssociations(package, plan.scope);
    if (!plan.registerAssociations)
        return;
    const fs::path executable = safeJoin(plan.installRoot, package.metadata.associationExecutable);
    if (!fs::is_regular_file(executable)) {
        logger.warning(L"File associations were skipped because Zanna Studio is not selected");
        return;
    }
    for (const auto &association : package.metadata.associations) {
        cancellationPoint(logger);
        const std::wstring extensionBase =
            std::wstring(kClassesBase) + utf8ToWide(association.extension);
        const std::wstring progId = utf8ToWide(association.progId);
        const std::wstring progIdBase = std::wstring(kClassesBase) + progId;
        if (RegKey existing = openKey(rootKey(plan.scope), progIdBase, KEY_READ, false)) {
            const auto owner = queryRegistryString(existing.get(), L"ZannaOwner");
            if (!owner || wideToUtf8(*owner) != package.metadata.identifier) {
                logger.warning(L"Skipped unowned file-association ProgID collision: " + progId);
                continue;
            }
        }
        RegKey prog = openKey(rootKey(plan.scope), progIdBase, KEY_SET_VALUE, true);
        setRegistryString(prog.get(), L"ZannaOwner", utf8ToWide(package.metadata.identifier));
        setRegistryString(prog.get(), {}, utf8ToWide(association.description));
        RegKey openWith =
            openKey(rootKey(plan.scope), extensionBase + L"\\OpenWithProgids", KEY_SET_VALUE, true);
        const LONG noneResult =
            RegSetValueExW(openWith.get(), progId.c_str(), 0, REG_NONE, nullptr, 0);
        if (noneResult != ERROR_SUCCESS)
            throw std::runtime_error("cannot register Zanna Open-With association");
        RegKey icon =
            openKey(rootKey(plan.scope), progIdBase + L"\\DefaultIcon", KEY_SET_VALUE, true);
        const fs::path iconPath =
            package.metadata.displayIconRelativePath.empty()
                ? executable
                : safeJoin(plan.installRoot, package.metadata.displayIconRelativePath);
        setRegistryString(icon.get(), {}, iconPath.wstring());
        RegKey command = openKey(
            rootKey(plan.scope), progIdBase + L"\\shell\\open\\command", KEY_SET_VALUE, true);
        std::wstring commandText = quoteCommandLineArgument(executable.wstring());
        if (!association.arguments.empty())
            commandText += L" " + utf8ToWide(association.arguments);
        commandText += L" \"%1\"";
        setRegistryString(command.get(), {}, commandText);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
}

/// @brief Resolve the Windows installation directory.
/// @return Absolute Windows directory path.
/// @throws std::runtime_error If Windows does not provide a bounded directory value.
fs::path windowsDirectory() {
    std::wstring value(32768, L'\0');
    const UINT length = GetWindowsDirectoryW(value.data(), static_cast<UINT>(value.size()));
    if (length == 0 || length >= value.size())
        throw std::runtime_error("cannot resolve the Windows directory for a shortcut");
    value.resize(length);
    return fs::path(value);
}

/// @brief Resolve a metadata-defined shortcut path against an approved root.
/// @param plan Resolved installation root.
/// @param root Root token: @c install, @c windows, or @c profile.
/// @param relative Optional safe relative path below the selected root.
/// @return Absolute resolved path.
/// @throws std::runtime_error If the root token or relative path is unsafe.
fs::path resolveShortcutPath(const InstallationPlan &plan,
                             std::string_view root,
                             std::string_view relative) {
    fs::path base;
    if (root == "install")
        base = plan.installRoot;
    else if (root == "windows")
        base = windowsDirectory();
    else if (root == "profile")
        base = knownFolder(FOLDERID_Profile);
    else
        throw std::runtime_error("unsupported shortcut path root");
    return relative.empty() ? base : safeJoin(base, relative);
}

/// @brief Construct the command-line arguments for a metadata-defined shortcut.
/// @param metadata Shortcut definition containing an optional argument path and prefix.
/// @param plan Resolved installation root used to resolve the argument path.
/// @return Empty text when no argument path exists, otherwise a safely quoted argument string.
std::wstring shortcutArguments(const zanna::pkg::WindowsInstallerShortcutMetadata &metadata,
                               const InstallationPlan &plan) {
    if (metadata.argumentPath.empty())
        return {};
    const fs::path argument = safeJoin(plan.installRoot, metadata.argumentPath);
    return utf8ToWide(metadata.argumentPrefix) + L" " +
           quoteCommandLineArgument(argument.wstring());
}

/// @brief Determine whether an existing Shell Link exactly matches package metadata.
/// @param path Shortcut file to load.
/// @param metadata Expected shortcut target, working directory, arguments, and icon.
/// @param plan Resolved installation root used for destination-aware path resolution.
/// @return @c true when every configured property matches; COM or load failures return @c false.
bool shellLinkMatches(const fs::path &path,
                      const zanna::pkg::WindowsInstallerShortcutMetadata &metadata,
                      const InstallationPlan &plan) {
    const HRESULT apartment =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE)
        return false;
    const bool uninitialize = SUCCEEDED(apartment);

    /// @brief Balance successful COM initialization on every return path.
    struct ApartmentGuard {
        bool active;

        /// @brief Uninitialize the current COM apartment when this guard owns it.
        ~ApartmentGuard() {
            if (active)
                CoUninitialize();
        }
    } guard{uninitialize};

    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(
            CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()))) ||
        !link) {
        return false;
    }
    ComPtr<IPersistFile> persist;
    if (FAILED(link->QueryInterface(IID_PPV_ARGS(persist.put()))) || !persist ||
        FAILED(persist->Load(path.c_str(), STGM_READ))) {
        return false;
    }
    std::array<wchar_t, 32768> target{};
    std::array<wchar_t, 32768> working{};
    std::array<wchar_t, 32768> arguments{};
    std::array<wchar_t, 32768> icon{};
    WIN32_FIND_DATAW findData{};
    int iconIndex = 0;
    if (FAILED(link->GetPath(
            target.data(), static_cast<int>(target.size()), &findData, SLGP_RAWPATH)) ||
        FAILED(link->GetWorkingDirectory(working.data(), static_cast<int>(working.size()))) ||
        FAILED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))) ||
        FAILED(link->GetIconLocation(icon.data(), static_cast<int>(icon.size()), &iconIndex))) {
        return false;
    }
    const auto targetText = terminatedWideView(target);
    const auto workingText = terminatedWideView(working);
    const auto argumentText = terminatedWideView(arguments);
    const auto iconText = terminatedWideView(icon);
    if (!targetText || !workingText || !argumentText || !iconText)
        return false;
    if (!sameWindowsPath(fs::path(*targetText),
                         resolveShortcutPath(plan, metadata.targetRoot, metadata.targetPath)) ||
        !sameWindowsPath(fs::path(*workingText),
                         resolveShortcutPath(plan, metadata.workingRoot, metadata.workingPath)) ||
        *argumentText != shortcutArguments(metadata, plan)) {
        return false;
    }
    if (!metadata.iconPath.empty() &&
        (!sameWindowsPath(fs::path(*iconText),
                          resolveShortcutPath(plan, metadata.iconRoot, metadata.iconPath)) ||
         iconIndex != metadata.iconIndex)) {
        return false;
    }
    return true;
}

/// @brief Create and atomically install a Windows Shell Link from package metadata.
/// @param metadata Shortcut target, working directory, arguments, description, and icon.
/// @param plan Resolved installation root used for destination-aware resolution.
/// @param destination Final shortcut path.
/// @throws std::runtime_error If COM, property configuration, persistence, or commit fails.
void createShellLink(const zanna::pkg::WindowsInstallerShortcutMetadata &metadata,
                     const InstallationPlan &plan,
                     const fs::path &destination) {
    const HRESULT apartment =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE)
        throw std::runtime_error("cannot initialize COM for a Start menu shortcut");
    const bool uninitialize = SUCCEEDED(apartment);

    /// @brief Balance successful COM initialization during shortcut creation.
    struct ApartmentGuard {
        bool active;

        /// @brief Uninitialize the current COM apartment when this guard owns it.
        ~ApartmentGuard() {
            if (active)
                CoUninitialize();
        }
    } guard{uninitialize};

    ComPtr<IShellLinkW> link;
    HRESULT result =
        CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(link.put()));
    if (FAILED(result) || !link)
        throw std::runtime_error("cannot create a Windows Shell Link object");
    const fs::path target = resolveShortcutPath(plan, metadata.targetRoot, metadata.targetPath);
    const fs::path working = resolveShortcutPath(plan, metadata.workingRoot, metadata.workingPath);
    result = link->SetPath(target.c_str());
    if (SUCCEEDED(result))
        result = link->SetWorkingDirectory(working.c_str());
    if (SUCCEEDED(result))
        result = link->SetDescription(utf8ToWide(metadata.description).c_str());
    if (SUCCEEDED(result) && !metadata.argumentPath.empty()) {
        const std::wstring arguments = shortcutArguments(metadata, plan);
        result = link->SetArguments(arguments.c_str());
    }
    if (SUCCEEDED(result) && !metadata.iconPath.empty()) {
        const fs::path icon = resolveShortcutPath(plan, metadata.iconRoot, metadata.iconPath);
        result = link->SetIconLocation(icon.c_str(), metadata.iconIndex);
    }
    if (FAILED(result))
        throw std::runtime_error("cannot configure a destination-aware Windows shortcut");

    ComPtr<IPersistFile> persist;
    result = link->QueryInterface(IID_PPV_ARGS(persist.put()));
    if (FAILED(result) || !persist)
        throw std::runtime_error("cannot persist a Windows shortcut");
    fs::create_directories(destination.parent_path());
    const fs::path temporary = destination.wstring() + L".tmp-" +
                               std::to_wstring(GetCurrentProcessId()) + L"-" +
                               hashHex(GetTickCount64()) + L".lnk";
    result = persist->Save(temporary.c_str(), TRUE);
    if (FAILED(result)) {
        if (!DeleteFileW(temporary.c_str())) {
            const DWORD cleanupError = GetLastError();
            if (cleanupError != ERROR_FILE_NOT_FOUND && cleanupError != ERROR_PATH_NOT_FOUND) {
                throw std::runtime_error("cannot save or remove a staged Windows shortcut: " +
                                         wideToUtf8(formatWindowsError(cleanupError)));
            }
        }
        throw std::runtime_error("cannot save a Windows shortcut");
    }
    if (!MoveFileExW(temporary.c_str(),
                     destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        if (!DeleteFileW(temporary.c_str())) {
            const DWORD cleanupError = GetLastError();
            if (cleanupError != ERROR_FILE_NOT_FOUND && cleanupError != ERROR_PATH_NOT_FOUND) {
                throw std::runtime_error(
                    "cannot commit or remove a staged Windows shortcut: " +
                    wideToUtf8(formatWindowsError(cleanupError)) +
                    "; commit error: " + wideToUtf8(formatWindowsError(error)));
            }
        }
        throw std::runtime_error("cannot commit a Windows shortcut: " +
                                 wideToUtf8(formatWindowsError(error)));
    }
}

/// @brief Remove recorded installer-owned shortcuts and empty product directories.
/// @param record Installed record containing absolute shortcut paths.
/// @throws std::runtime_error If a path is unsafe or cannot be removed.
void removeShortcuts(const InstalledRecord &record);

/// @brief Reconcile and install the selected set of package shortcuts.
/// @param package Verified package providing shortcut metadata.
/// @param plan Resolved scope, destination, components, and shortcut setting.
/// @param existing Existing installed record used to protect unowned collisions.
/// @param logger Installer logger and cancellation source.
/// @param transaction Optional workspace whose rollback journal records created shortcuts.
/// @return Absolute paths of shortcuts installed by this invocation.
/// @throws std::runtime_error If owned cleanup, path resolution, or shortcut creation fails.
std::vector<fs::path> installShortcuts(const HostPackage &package,
                                       const InstallationPlan &plan,
                                       const InstalledRecord &existing,
                                       Logger &logger,
                                       const TransactionPaths *transaction = nullptr) {
    InstalledRecord recognized = existing;
    for (const auto &shortcut : package.metadata.shortcuts) {
        fs::path root;
        if (shortcut.root == "desktop") {
            root = knownFolder(plan.scope == InstallScope::User ? FOLDERID_Desktop
                                                                : FOLDERID_PublicDesktop);
        } else {
            root = knownFolder(plan.scope == InstallScope::User ? FOLDERID_Programs
                                                                : FOLDERID_CommonPrograms);
            root /= utf8ToWide(package.metadata.defaultInstallDir);
        }
        const fs::path destination = safeJoin(root, shortcut.relativePath);
        /// @brief Test whether the destination is already an owned shortcut.
        /// @param old Previously recorded shortcut path.
        /// @return `true` when `old` and `destination` denote the same Windows path.
        const bool recorded =
            std::any_of(recognized.shortcuts.begin(),
                        recognized.shortcuts.end(),
                        [&](const fs::path &old) { return sameWindowsPath(old, destination); });
        if (!recorded && fs::is_regular_file(destination) &&
            shellLinkMatches(destination, shortcut, plan)) {
            recognized.shortcuts.push_back(destination);
            logger.warning(L"Recovered ownership of a matching Zanna shortcut: " +
                           destination.wstring());
        }
    }
    // Remove the complete owned set through the same checked cleanup path used
    // by uninstall.  Deleting the files inline used to leave an empty Start
    // Menu product directory whenever Modify disabled shortcuts.
    removeShortcuts(recognized);
    if (!plan.createShortcuts)
        return {};
    std::vector<fs::path> installed;
    for (const auto &shortcut : package.metadata.shortcuts) {
        cancellationPoint(logger);
        if (!componentEnabled(shortcut.componentId, plan.components))
            continue;
        fs::path root;
        if (shortcut.root == "desktop") {
            root = knownFolder(plan.scope == InstallScope::User ? FOLDERID_Desktop
                                                                : FOLDERID_PublicDesktop);
        } else {
            root = knownFolder(plan.scope == InstallScope::User ? FOLDERID_Programs
                                                                : FOLDERID_CommonPrograms);
            root /= utf8ToWide(package.metadata.defaultInstallDir);
        }
        const fs::path destination = safeJoin(root, shortcut.relativePath);
        /// @brief Test whether an existing owned shortcut matches the destination path.
        /// @param old Previously recorded shortcut path.
        /// @return `true` when `old` and `destination` denote the same Windows path.
        if (fs::exists(destination) && std::find_if(existing.shortcuts.begin(),
                                                    existing.shortcuts.end(),
                                                    [&](const fs::path &old) {
                                                        return sameWindowsPath(old, destination);
                                                    }) == existing.shortcuts.end()) {
            logger.warning(L"Skipped unowned shortcut collision: " + destination.wstring());
            continue;
        }
        if (transaction)
            recordAppliedShortcut(*transaction, destination);
        createShellLink(shortcut, plan, destination);
        installed.push_back(destination);
    }
    return installed;
}

/// @brief Test whether a directory is one of the system-managed shortcut roots.
/// @param path Directory considered for post-removal cleanup.
/// @return @c true for a known root or when root resolution fails conservatively.
bool isProtectedShortcutRoot(const fs::path &path) {
    const std::array<KNOWNFOLDERID, 4> roots = {
        FOLDERID_Desktop, FOLDERID_PublicDesktop, FOLDERID_Programs, FOLDERID_CommonPrograms};
    for (REFKNOWNFOLDERID id : roots) {
        PWSTR raw = nullptr;
        const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
        if (FAILED(result) || !raw || !*raw) {
            if (raw)
                CoTaskMemFree(raw);
            return true;
        }
        const bool matches = sameWindowsPath(path, fs::path(raw));
        CoTaskMemFree(raw);
        if (matches)
            return true;
    }
    return false;
}

/// @brief Remove recorded installer-owned shortcut files and empty child directories.
/// @param record Installed record containing absolute owned shortcut paths.
/// @throws std::runtime_error On unsafe file types, deletion failure, or directory cleanup error.
void removeShortcuts(const InstalledRecord &record) {
    for (const fs::path &path : record.shortcuts) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                continue;
            throw std::runtime_error("cannot inspect an installed Zanna shortcut: " +
                                     wideToUtf8(formatWindowsError(error)));
        }
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U)
            throw std::runtime_error("refusing to remove a non-file Zanna shortcut path");
        if (!DeleteFileW(path.c_str()))
            throw std::runtime_error("cannot remove an installed Zanna shortcut: " +
                                     wideToUtf8(formatWindowsError(GetLastError())));
        fs::path parent = path.parent_path();
        if (!parent.empty() && !isProtectedShortcutRoot(parent)) {
            std::error_code error;
            fs::remove(parent, error);
            if (error == std::errc::directory_not_empty) {
                error.clear();
            }
            if (error)
                throw std::runtime_error("cannot clean the Zanna shortcut directory");
        }
    }
}

/// @brief Build a Windows command string with a safely quoted executable.
/// @param path Executable path to quote.
/// @param argument Optional preformatted argument text to append.
/// @return Command string suitable for an Add/Remove Programs registry value.
std::wstring quotedExecutableCommand(const fs::path &path, std::wstring_view argument) {
    std::wstring result = quoteCommandLineArgument(path.wstring());
    if (!argument.empty())
        result += L" " + std::wstring(argument);
    return result;
}

/// @brief Write the complete Add/Remove Programs registration for an installation.
/// @param package Verified package supplying display and provenance metadata.
/// @param plan Resolved scope, paths, selections, settings, and estimated size.
/// @param shortcuts Installer-owned shortcut paths to persist for later cleanup.
/// @param pathEntry Effective PATH entry to persist for later removal.
/// @param logger Installer logger whose path is recorded for diagnostics.
/// @throws std::runtime_error If the registration key or any value cannot be written.
void registerArp(const HostPackage &package,
                 const InstallationPlan &plan,
                 const std::vector<fs::path> &shortcuts,
                 std::wstring_view pathEntry,
                 const Logger &logger) {
    RegKey key = openKey(rootKey(plan.scope),
                         uninstallSubkey(package.metadata.identifier),
                         KEY_QUERY_VALUE | KEY_SET_VALUE,
                         true);
    const fs::path icon =
        package.metadata.displayIconRelativePath.empty()
            ? plan.installRoot / utf8ToWide(package.metadata.executableName)
            : safeJoin(plan.installRoot, package.metadata.displayIconRelativePath);
    setRegistryString(key.get(), L"DisplayName", utf8ToWide(package.metadata.displayName));
    setRegistryString(key.get(), L"DisplayVersion", utf8ToWide(package.metadata.version));
    setRegistryString(key.get(), L"Publisher", utf8ToWide(package.metadata.publisher));
    setRegistryString(key.get(), L"InstallLocation", plan.installRoot.wstring());
    setRegistryString(key.get(), L"DisplayIcon", icon.wstring());
    setRegistryString(key.get(),
                      L"UninstallString",
                      quotedExecutableCommand(plan.cacheExecutable, L"/uninstall"));
    setRegistryString(key.get(),
                      L"QuietUninstallString",
                      quotedExecutableCommand(plan.cacheExecutable, L"/uninstall /quiet"));
    setRegistryString(
        key.get(), L"ModifyPath", quotedExecutableCommand(plan.cacheExecutable, L"/modify"));
    setRegistryString(
        key.get(), L"RepairPath", quotedExecutableCommand(plan.cacheExecutable, L"/repair"));
    setRegistryDword(key.get(), L"NoModify", 0);
    setRegistryDword(key.get(), L"NoRepair", 0);
    setRegistryDword(key.get(), L"WindowsInstaller", 0);
    setRegistryDword(key.get(),
                     L"EstimatedSize",
                     static_cast<DWORD>(std::min<uint64_t>((plan.selectedSizeBytes + 1023U) / 1024U,
                                                           std::numeric_limits<DWORD>::max())));
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wostringstream date;
    date << std::setfill(L'0') << std::setw(4) << now.wYear << std::setw(2) << now.wMonth
         << std::setw(2) << now.wDay;
    setRegistryString(key.get(), L"InstallDate", date.str());
    if (!package.metadata.homepage.empty()) {
        const std::wstring homepage = utf8ToWide(package.metadata.homepage);
        setRegistryString(key.get(), L"URLInfoAbout", homepage);
        setRegistryString(key.get(),
                          L"HelpLink",
                          package.metadata.documentationUrl.empty()
                              ? homepage
                              : utf8ToWide(package.metadata.documentationUrl));
    }
    if (!package.metadata.updateManifestUrl.empty())
        setRegistryString(
            key.get(), L"URLUpdateInfo", utf8ToWide(package.metadata.updateManifestUrl));
    setRegistryString(key.get(), L"Comments", utf8ToWide(package.metadata.description));
    setRegistryString(key.get(), L"Contact", utf8ToWide(package.metadata.contact));
    setRegistryString(
        key.get(), L"ZannaPackageIdentifier", utf8ToWide(package.metadata.identifier));
    setRegistryString(key.get(), L"ZannaArchitecture", utf8ToWide(package.metadata.architecture));
    setRegistryString(key.get(), L"ZannaChannel", utf8ToWide(package.metadata.channel));
    setRegistryString(key.get(), L"ZannaCommit", utf8ToWide(package.metadata.commit));
    setRegistryString(key.get(), L"ZannaPackageSha256", utf8ToWide(package.executableSha256));
    setRegistryString(key.get(), L"ZannaLastInstallerLog", logger.path().wstring());
    setRegistryString(key.get(), L"ZannaComponents", joinComponents(plan.components));
    setRegistryString(key.get(), L"ZannaMaintenanceCache", plan.cacheExecutable.wstring());
    setRegistryString(key.get(), L"ZannaPathEntry", pathEntry);
    std::wstring shortcutText;
    for (const fs::path &shortcut : shortcuts)
        shortcutText += shortcut.wstring() + L"\r\n";
    setRegistryString(key.get(), L"ZannaShortcutPaths", shortcutText);
    setRegistryDword(key.get(), L"ZannaInstallSchema", 2);
    setRegistryDword(key.get(), L"ZannaSettingsVersion", 1);
    setRegistryDword(key.get(), L"ZannaAddToPath", plan.addToPath ? 1U : 0U);
    setRegistryDword(key.get(), L"ZannaAssociations", plan.registerAssociations ? 1U : 0U);
    setRegistryDword(key.get(), L"ZannaCreateShortcuts", plan.createShortcuts ? 1U : 0U);
}

/// @brief Remove this package's Add/Remove Programs registration tree.
/// @param package Package supplying the stable uninstall-key identifier.
/// @param scope Registry scope containing the registration.
/// @throws std::runtime_error If a present registration cannot be removed.
void removeArp(const HostPackage &package, InstallScope scope) {
    const LONG result =
        RegDeleteTreeW(rootKey(scope), uninstallSubkey(package.metadata.identifier).c_str());
    if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
        throw std::runtime_error("cannot remove Add/Remove Programs registration");
}

/// @brief Read an entire maintenance-cache file into memory.
/// @param path File to read.
/// @return Complete contents, or an empty vector if the file cannot be opened.
/// @throws std::runtime_error If its reported size is unsupported or the read fails.
std::vector<uint8_t> readFileBytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return {};
    const std::streamoff size = input.tellg();
    if (size < 0 ||
        static_cast<uint64_t>(size) > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error("maintenance cache file is too large");
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char *>(bytes.data()),
                                      static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("cannot read the maintenance cache file");
    }
    return bytes;
}

/// @brief Ensure the maintenance cache contains the exact expected executable bytes.
/// @param path Cache destination.
/// @param maintenance Expected maintenance executable.
/// @throws std::runtime_error If an outdated or absent cache cannot be replaced atomically.
void ensureMaintenanceCache(const fs::path &path, const std::vector<uint8_t> &maintenance) {
    if (fs::is_regular_file(path)) {
        const std::vector<uint8_t> old = readFileBytes(path);
        if (zanna::pkg::sha256Hex(old.data(), old.size()) ==
            zanna::pkg::sha256Hex(maintenance.data(), maintenance.size())) {
            return;
        }
    }
    writeBytesAtomic(path, maintenance);
}

/// @brief Apply all transactional Windows integration metadata for an installation.
/// @param package Verified package supplying maintenance and integration metadata.
/// @param plan Resolved scope, paths, components, and integration choices.
/// @param paths Transaction workspace used for rollback journals.
/// @param logger Installer logger and cancellation source.
/// @throws std::runtime_error If cache, PATH, association, shortcut, or ARP updates fail.
void applyMetadata(const HostPackage &package,
                   const InstallationPlan &plan,
                   const TransactionPaths &paths,
                   Logger &logger) {
    const std::vector<uint8_t> maintenance = maintenanceBytes(package);
    ensureMaintenanceCache(plan.cacheExecutable, maintenance);

    std::wstring pathEntry;
    if (plan.addToPath && !package.metadata.pathRelativePath.empty())
        pathEntry = safeJoin(plan.installRoot, package.metadata.pathRelativePath).wstring();
    updatePath(plan.scope, plan.existing.pathEntry, pathEntry);
    registerAssociations(package, plan, logger);
    const std::vector<fs::path> shortcuts =
        installShortcuts(package, plan, plan.existing, logger, &paths);
    registerArp(package, plan, shortcuts, pathEntry, logger);
    logger.info(L"Windows integration metadata committed");
}

/// @brief Remove all recorded Windows integration metadata for an installation.
/// @param package Package supplying association and ARP identity metadata.
/// @param plan Resolved scope and existing installed record.
/// @param logger Installer logger used to report completion.
/// @throws std::runtime_error If any owned integration metadata cannot be removed.
void removeMetadata(const HostPackage &package, const InstallationPlan &plan, Logger &logger) {
    removeShortcuts(plan.existing);
    unregisterAssociations(package, plan.scope);
    updatePath(plan.scope, plan.existing.pathEntry, {});
    removeArp(package, plan.scope);
    logger.info(L"Windows integration metadata removed");
}

/// @brief Resolve the current Windows temporary directory with dynamic sizing.
/// @return Absolute temporary-directory path.
/// @throws std::runtime_error If Windows cannot provide the path.
fs::path temporaryDirectory() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
        if (length == 0)
            throw std::runtime_error("cannot locate the Windows temporary directory");
        if (length < buffer.size())
            return fs::path(std::wstring(buffer.data(), length));
        buffer.resize(static_cast<size_t>(length) + 1U);
    }
}

/// @brief Write and durably flush a byte sequence through an existing Windows handle.
/// @param handle Writable file handle.
/// @param bytes Complete contents to write.
/// @throws std::runtime_error If any chunk or the final flush fails.
void writeHandleBytes(HANDLE handle, const std::vector<uint8_t> &bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD chunk =
            static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<size_t>(MAXDWORD)));
        DWORD written = 0;
        if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) ||
            written != chunk) {
            throw std::runtime_error("cannot write the detached cleanup helper");
        }
        offset += written;
    }
    if (!FlushFileBuffers(handle))
        throw std::runtime_error("cannot flush the detached cleanup helper");
}

/// @brief Compare an open, write-locked file with the exact expected byte sequence.
/// @param handle Readable file handle whose share mode denies mutation and replacement.
/// @param expected Trusted package bytes.
/// @return @c true only when size, positioning, reads, and every byte match.
bool handleBytesMatch(HANDLE handle, const std::vector<uint8_t> &expected) {
    LARGE_INTEGER size{};
    LARGE_INTEGER beginning{};
    std::array<uint8_t, 64U * 1024U> buffer{};
    size_t offset = 0;

    if (!handle || handle == INVALID_HANDLE_VALUE || !GetFileSizeEx(handle, &size) ||
        size.QuadPart < 0 ||
        static_cast<ULONGLONG>(size.QuadPart) != static_cast<ULONGLONG>(expected.size()) ||
        !SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN)) {
        return false;
    }
    while (offset < expected.size()) {
        const DWORD requested =
            static_cast<DWORD>((std::min)(buffer.size(), expected.size() - offset));
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), requested, &read, nullptr) || read != requested ||
            std::memcmp(buffer.data(), expected.data() + offset, requested) != 0) {
            return false;
        }
        offset += read;
    }
    return true;
}

/// @brief Materialize and launch the detached helper that removes maintenance-cache artifacts.
/// @param package Verified package containing cleanup-helper bytes.
/// @param plan Resolved maintenance cache location.
/// @param logger Installer logger used to report launch success or deferred cleanup.
/// @return @c true when the cleanup helper was successfully launched.
/// @throws std::runtime_error If the helper cannot be created, populated, or started safely.
bool launchDetachedCleanup(const HostPackage &package,
                           const InstallationPlan &plan,
                           Logger &logger) {
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
        throw std::runtime_error("cannot allocate a unique cleanup helper name");
    wchar_t guidText[40]{};
    if (StringFromGUID2(guid, guidText, static_cast<int>(std::size(guidText))) == 0)
        throw std::runtime_error("cannot format the cleanup helper name");
    std::wstring directoryName = L"ZannaCleanup-" + std::wstring(guidText);
    /// @brief Identify GUID brace characters that are omitted from the helper directory.
    /// @param ch Wide character to inspect.
    /// @return `true` for an opening or closing brace.
    directoryName.erase(std::remove_if(directoryName.begin(),
                                       directoryName.end(),
                                       [](wchar_t ch) { return ch == L'{' || ch == L'}'; }),
                        directoryName.end());
    const fs::path helperDirectory = temporaryDirectory() / directoryName;
    const fs::path helperPath = helperDirectory / L"cleanup.exe";
    if (!CreateDirectoryW(helperDirectory.c_str(), nullptr))
        throw std::runtime_error("cannot create the detached cleanup directory");

    UniqueHandle helper(CreateFileW(helperPath.c_str(),
                                    GENERIC_READ | GENERIC_WRITE | DELETE | SYNCHRONIZE,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY,
                                    nullptr));
    if (!helper) {
        const DWORD createError = GetLastError();
        if (!RemoveDirectoryW(helperDirectory.c_str())) {
            const DWORD cleanupError = GetLastError();
            throw std::runtime_error(
                "cannot create the detached cleanup helper and cannot remove its directory: " +
                wideToUtf8(formatWindowsError(cleanupError)) +
                "; create error: " + wideToUtf8(formatWindowsError(createError)));
        }
        throw std::runtime_error("cannot create the detached cleanup helper: " +
                                 wideToUtf8(formatWindowsError(createError)));
    }

    bool processMayBeRunning = false;
    try {
        writeHandleBytes(helper.get(), package.cleanupBytes);
        helper.reset();
        helper.reset(CreateFileW(helperPath.c_str(),
                                 GENERIC_READ | SYNCHRONIZE,
                                 FILE_SHARE_READ,
                                 nullptr,
                                 OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
                                 nullptr));
        if (!helper)
            throw std::runtime_error("cannot reopen the detached cleanup helper");
        FILE_ATTRIBUTE_TAG_INFO helperAttributes{};
        if (!GetFileInformationByHandleEx(
                helper.get(), FileAttributeTagInfo, &helperAttributes, sizeof(helperAttributes)) ||
            (helperAttributes.FileAttributes &
             (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
            throw std::runtime_error("detached cleanup helper became an unsafe filesystem entry");
        }
        if (!handleBytesMatch(helper.get(), package.cleanupBytes))
            throw std::runtime_error("detached cleanup helper changed before launch");
        std::wstring command =
            quoteCommandLineArgument(helperPath.wstring()) + L" /parent " +
            std::to_wstring(GetCurrentProcessId()) + L" /delete " +
            quoteCommandLineArgument(plan.cacheExecutable.wstring()) + L" /rmdir " +
            quoteCommandLineArgument(plan.cacheExecutable.parent_path().wstring()) +
            L" /rmdir-if-empty " +
            quoteCommandLineArgument(plan.cacheExecutable.parent_path().parent_path().wstring()) +
            L" /rmdir-if-empty " +
            quoteCommandLineArgument(
                plan.cacheExecutable.parent_path().parent_path().parent_path().wstring()) +
            L" /rmdir " + quoteCommandLineArgument(helperDirectory.wstring());
        std::vector<wchar_t> mutableCommand(command.begin(), command.end());
        mutableCommand.push_back(L'\0');
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(helperPath.c_str(),
                            mutableCommand.data(),
                            nullptr,
                            nullptr,
                            FALSE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW,
                            nullptr,
                            helperDirectory.parent_path().c_str(),
                            &startup,
                            &process)) {
            const DWORD error = GetLastError();
            throw std::runtime_error("cannot start the detached cleanup helper: " +
                                     wideToUtf8(formatWindowsError(error)));
        }
        processMayBeRunning = true;
        UniqueHandle processHandle(process.hProcess);
        UniqueHandle threadHandle(process.hThread);
        helper.reset();
        if (ResumeThread(threadHandle.get()) == static_cast<DWORD>(-1)) {
            const DWORD resumeError = GetLastError();
            if (!TerminateProcess(processHandle.get(), ERROR_INVALID_FUNCTION))
                throw std::runtime_error("cannot resume or terminate the detached cleanup helper");
            if (WaitForSingleObject(processHandle.get(), 5000) != WAIT_OBJECT_0)
                throw std::runtime_error("the unresumed detached cleanup helper did not terminate");
            processMayBeRunning = false;
            throw std::runtime_error("cannot resume the detached cleanup helper: " +
                                     wideToUtf8(formatWindowsError(resumeError)));
        }
        bool unlinked = false;
        bool processExited = false;
        DWORD helperExit = STILL_ACTIVE;
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            if (GetFileAttributesW(helperPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
                    unlinked = true;
                    break;
                }
            }
            const DWORD wait = WaitForSingleObject(processHandle.get(), 0);
            if (wait == WAIT_FAILED) {
                helperExit = GetLastError();
                break;
            }
            if (wait == WAIT_OBJECT_0) {
                processExited = true;
                processMayBeRunning = false;
                if (!GetExitCodeProcess(processHandle.get(), &helperExit))
                    helperExit = GetLastError();
                break;
            }
            Sleep(20);
        }
        if (!unlinked) {
            if (!processExited) {
                if (!TerminateProcess(processHandle.get(), ERROR_ACCESS_DENIED))
                    throw std::runtime_error("cannot terminate the detached cleanup helper");
                if (WaitForSingleObject(processHandle.get(), 5000) != WAIT_OBJECT_0)
                    throw std::runtime_error("the detached cleanup helper did not terminate");
                processMayBeRunning = false;
            }
            throw std::runtime_error("detached cleanup helper could not self-delete (exit " +
                                     std::to_string(helperExit) + ")");
        }
        logger.info(L"Detached maintenance-cache cleanup was started");
        return true;
    } catch (...) {
        helper.reset();
        if (!processMayBeRunning) {
            if (!DeleteFileW(helperPath.c_str())) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                    logger.error(L"Cannot remove failed detached cleanup helper: " +
                                 formatWindowsError(error));
            }
            if (!RemoveDirectoryW(helperDirectory.c_str())) {
                const DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
                    logger.error(L"Cannot remove failed detached cleanup directory: " +
                                 formatWindowsError(error));
            }
        }
        throw;
    }
}

/// @brief Start cache cleanup after uninstall, falling back to deletion at reboot.
/// @param package Verified package containing the detached cleanup helper.
/// @param plan Resolved maintenance-cache path.
/// @param logger Installer logger used for success and fallback diagnostics.
/// @return @c true when detached cleanup started; @c false when reboot cleanup was requested or
///         the cache must remain for a later repair.
bool cleanupCacheAfterUninstall(const HostPackage &package,
                                const InstallationPlan &plan,
                                Logger &logger) {
    try {
        return launchDetachedCleanup(package, plan, logger);
    } catch (const std::exception &error) {
        if (!MoveFileExW(plan.cacheExecutable.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
            const DWORD scheduleError = GetLastError();
            logger.error(L"Detached cleanup failed and maintenance cache cleanup could not be "
                         L"scheduled for reboot: " +
                         formatWindowsError(scheduleError) + L"; launch error: " +
                         utf8ToWide(error.what()));
            return false;
        }
        logger.warning(L"Detached cleanup failed; maintenance cache cleanup was scheduled "
                       L"for reboot: " +
                       utf8ToWide(error.what()));
        return false;
    }
}

/// @brief Test whether a candidate path is equal to or nested beneath a Windows root.
/// @param root Trusted containing path.
/// @param candidate Path to test.
/// @return @c true when the normalized Windows path begins with @p root on a component boundary.
bool pathIsWithin(const fs::path &root, const fs::path &candidate) {
    return windowsPathBeginsWith(candidate, root);
}

/// @brief Hand a maintenance operation from the installed executable to its verified cache.
/// @param package Currently running verified maintenance package.
/// @param options Parsed options to reproduce in the worker process.
/// @param plan Resolved operation, scope, paths, components, and integration settings.
/// @param logger Installer logger whose path is passed to the worker.
/// @return Success after the verified cache process starts.
/// @throws std::runtime_error If cache verification or process creation fails.
int launchMaintenanceHandoff(const HostPackage &package,
                             const HostOptions &options,
                             const InstallationPlan &plan,
                             Logger &logger) {
    if (!fs::is_regular_file(plan.cacheExecutable))
        throw std::runtime_error("the verified maintenance cache is missing");
    const HostPackage cached = loadHostPackage(plan.cacheExecutable);
    if (cached.metadata.identifier != package.metadata.identifier ||
        cached.metadata.version != package.metadata.version ||
        zanna::pkg::sha256Hex(cached.executableBytes.data(), cached.executableBytes.size()) !=
            zanna::pkg::sha256Hex(package.executableBytes.data(), package.executableBytes.size())) {
        throw std::runtime_error("the maintenance cache does not match the installed package");
    }

    std::vector<std::wstring> arguments = {operationSwitch(plan.operation),
                                           L"/scope",
                                           plan.scope == InstallScope::User ? L"user" : L"machine",
                                           L"/installDir",
                                           plan.installRoot.wstring(),
                                           L"/uninstall-worker",
                                           L"/handoff-parent",
                                           std::to_wstring(GetCurrentProcessId()),
                                           L"/log",
                                           logger.path().wstring()};
    if (options.uiLevel == UiLevel::Quiet)
        arguments.push_back(L"/quiet");
    else if (options.uiLevel == UiLevel::Passive)
        arguments.push_back(L"/passive");
    if (options.allowDowngrade)
        arguments.push_back(L"/allowDowngrade");
    if (options.noRestart)
        arguments.push_back(L"/norestart");
    if (options.closeApplications)
        arguments.push_back(L"/closeApplications");
    if (options.addToPath)
        arguments.push_back(*options.addToPath ? L"/addToPath" : L"/noPath");
    if (options.registerAssociations) {
        arguments.push_back(*options.registerAssociations ? L"/associations" : L"/noAssociations");
    }
    if (options.createShortcuts)
        arguments.push_back(*options.createShortcuts ? L"/shortcuts" : L"/noShortcuts");
    if (!plan.components.empty()) {
        arguments.push_back(L"/components");
        arguments.push_back(joinComponents(plan.components));
    }
    if (options.launchIDE)
        arguments.push_back(L"/launch-ide");
    if (options.launchPrompt)
        arguments.push_back(L"/launch-prompt");
    if (options.openQuickstart)
        arguments.push_back(L"/open-quickstart");
    if (options.openSamples)
        arguments.push_back(L"/open-samples");

    std::wstring command = quoteCommandLineArgument(plan.cacheExecutable.wstring());
    for (const std::wstring &argument : arguments)
        command += L" " + quoteCommandLineArgument(argument);
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(plan.cacheExecutable.c_str(),
                        mutableCommand.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        plan.cacheExecutable.parent_path().c_str(),
                        &startup,
                        &process)) {
        const DWORD error = GetLastError();
        throw std::runtime_error("cannot hand maintenance off to the verified cache: " +
                                 wideToUtf8(formatWindowsError(error)));
    }
    const BOOL threadClosed = CloseHandle(process.hThread);
    const DWORD threadCloseError = threadClosed ? ERROR_SUCCESS : GetLastError();
    const BOOL processClosed = CloseHandle(process.hProcess);
    const DWORD processCloseError = processClosed ? ERROR_SUCCESS : GetLastError();
    if (!threadClosed || !processClosed) {
        logger.error(L"Maintenance handoff started, but a launcher handle could not be closed: " +
                     formatWindowsError(!threadClosed ? threadCloseError : processCloseError));
    }
    logger.info(L"Maintenance operation handed off to the verified cache");
    return kExitSuccess;
}

/// @brief Wait for the originating maintenance process to release its executable.
/// @param processId Parent process identifier, or zero when no handoff synchronization is needed.
/// @throws std::runtime_error If the process cannot be opened or does not exit within one minute.
void waitForHandoffParent(DWORD processId) {
    if (processId == 0)
        return;
    UniqueHandle parent(OpenProcess(SYNCHRONIZE, FALSE, processId));
    if (!parent) {
        if (GetLastError() == ERROR_INVALID_PARAMETER)
            return;
        throw std::runtime_error("cannot synchronize the maintenance handoff");
    }
    const DWORD wait = WaitForSingleObject(parent.get(), 60U * 1000U);
    if (wait != WAIT_OBJECT_0)
        throw std::runtime_error("the originating maintenance process did not exit");
}

/// @brief Load a verified maintenance package from a transaction installation tree.
/// @param root Old or new installation root to inspect.
/// @param fallbackPackage Package supplying current and legacy uninstaller locations and identity.
/// @param logger Installer logger used for rejected-candidate diagnostics.
/// @return First verified package with the expected identifier, or @c std::nullopt.
std::optional<HostPackage> loadInstalledPackage(const fs::path &root,
                                                const HostPackage &fallbackPackage,
                                                Logger &logger) {
    std::vector<fs::path> candidates = {
        safeJoin(root, fallbackPackage.metadata.uninstallerRelativePath), root / L"uninstall.exe"};
    for (const fs::path &candidate : candidates) {
        if (!fs::is_regular_file(candidate))
            continue;
        try {
            HostPackage package = loadHostPackage(candidate);
            if (package.metadata.identifier == fallbackPackage.metadata.identifier)
                return package;
            logger.warning(L"Ignored a transaction uninstaller with a mismatched package id");
        } catch (const std::exception &error) {
            logger.warning(L"Could not read a transaction uninstaller: " +
                           utf8ToWide(error.what()));
        }
    }
    return std::nullopt;
}

/// @brief Recover selected components from installed state with metadata defaults as fallback.
/// @param package Package defining the state path and component defaults.
/// @param root Installation root containing the state file.
/// @return Normalized selected component IDs.
std::set<std::string> installedComponents(const HostPackage &package, const fs::path &root) {
    const std::wstring state = readTextFileWide(safeJoin(root, package.metadata.stateRelativePath));
    for (const std::wstring &line : splitLines(state)) {
        constexpr std::wstring_view kPrefix = L"components\t";
        if (line.rfind(kPrefix, 0) == 0)
            return parseComponentList(std::wstring_view(line).substr(kPrefix.size()));
    }
    std::set<std::string> selected;
    for (const auto &component : package.metadata.components) {
        if (component.required || component.defaultSelected)
            selected.insert(lowerAscii(component.id));
    }
    return selected;
}

/// @brief Reconstruct metadata settings needed to restore a rolled-back package.
/// @param package Verified prior package now restored on disk.
/// @param currentPlan Current transaction's scope, root, and cache identity.
/// @return Repair-style plan derived from prior state and package defaults.
InstallationPlan restorationPlan(const HostPackage &package, const InstallationPlan &currentPlan) {
    InstallationPlan restored;
    restored.operation = Operation::Repair;
    restored.scope = currentPlan.scope;
    restored.installRoot = currentPlan.installRoot;
    restored.cacheExecutable = currentPlan.cacheExecutable;
    restored.components = installedComponents(package, restored.installRoot);
    restored.addToPath = package.metadata.addToPath;
    restored.registerAssociations = package.metadata.registerFileAssociations;
    restored.createShortcuts = package.metadata.createShortcuts;
    const std::wstring state =
        readTextFileWide(safeJoin(restored.installRoot, package.metadata.stateRelativePath));
    for (const std::wstring &line : splitLines(state)) {
        const size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos)
            continue;
        const std::wstring key = line.substr(0, tab);
        const std::wstring value = line.substr(tab + 1U);
        if (value != L"0" && value != L"1")
            continue;
        if (key == L"add-to-path")
            restored.addToPath = value == L"1";
        else if (key == L"associations")
            restored.registerAssociations = value == L"1";
        else if (key == L"shortcuts")
            restored.createShortcuts = value == L"1";
    }
    for (const auto &file : package.metadata.payloadFiles) {
        if (componentEnabled(file.componentId, restored.components))
            restored.selectedSizeBytes += file.sizeBytes;
    }
    for (const auto &file : package.metadata.outerFiles) {
        if (componentEnabled(file.componentId, restored.components))
            restored.selectedSizeBytes += file.sizeBytes;
    }
    if (package.metadata.packageMode == "maintenance")
        restored.selectedSizeBytes += package.executableBytes.size();
    return restored;
}

/// @brief Best-effort remove a cached maintenance executable and empty cache ancestors.
/// @param cacheExecutable Cache file whose package directories are pruned.
void removeCacheFile(const fs::path &cacheExecutable) {
    std::error_code error;
    fs::remove(cacheExecutable, error);
    fs::remove(cacheExecutable.parent_path(), error);
    fs::remove(cacheExecutable.parent_path().parent_path(), error);
    fs::remove(cacheExecutable.parent_path().parent_path().parent_path(), error);
}

/// @brief Remove partially applied metadata and restore the prior package's integration state.
/// @param newPackage Package whose partially installed metadata must be removed.
/// @param oldPackage Verified prior package, or no value for a rolled-back first install.
/// @param plan Current transaction scope, root, cache, and installed state.
/// @param paths Transaction workspace containing PATH and shortcut rollback journals.
/// @param logger Installer logger and cancellation source for restored shortcuts.
/// @throws std::runtime_error If cleanup or restoration cannot be completed safely.
void restoreMetadataAfterRollback(const HostPackage &newPackage,
                                  const std::optional<HostPackage> &oldPackage,
                                  const InstallationPlan &plan,
                                  const TransactionPaths &paths,
                                  Logger &logger) {
    for (const fs::path &shortcut : readAppliedShortcuts(paths)) {
        std::error_code error;
        fs::remove(shortcut, error);
    }
    const InstalledRecord current = readInstalledRecord(newPackage.metadata.identifier, plan.scope);
    removeShortcuts(current);
    unregisterAssociations(newPackage, plan.scope);
    removeArp(newPackage, plan.scope);
    restorePathBackup(paths, plan.scope);

    if (!oldPackage) {
        removeCacheFile(plan.cacheExecutable);
        logger.warning(
            L"Rolled back Windows integration metadata for an interrupted first install");
        return;
    }

    InstallationPlan restored = restorationPlan(*oldPackage, plan);
    ensureMaintenanceCache(restored.cacheExecutable, maintenanceBytes(*oldPackage));
    registerAssociations(*oldPackage, restored, logger);
    InstalledRecord noExisting;
    const std::vector<fs::path> shortcuts =
        installShortcuts(*oldPackage, restored, noExisting, logger);
    std::wstring pathEntry;
    if (restored.addToPath && !oldPackage->metadata.pathRelativePath.empty()) {
        pathEntry = safeJoin(restored.installRoot, oldPackage->metadata.pathRelativePath).wstring();
    }
    registerArp(*oldPackage, restored, shortcuts, pathEntry, logger);
    logger.warning(L"Restored the previous package's Windows integration metadata");
}

/// @brief Recover, roll back, or clean a previously interrupted directory transaction.
/// @param package Verified current package used to validate transaction executables.
/// @param plan Resolved installation state and recovery-marker path.
/// @param paths Deterministic transaction workspace.
/// @param logger Installer logger used to report recovery decisions.
/// @throws std::runtime_error If journal state or prior package metadata prevents safe recovery.
void recoverTransaction(const HostPackage &package,
                        const InstallationPlan &plan,
                        const TransactionPaths &paths,
                        Logger &logger) {
    if (!fs::exists(paths.directory))
        return;
    const JournalState state = parseJournal(readTextFileWide(paths.journal));
    logger.warning(L"Recovering interrupted installer transaction in state " + journalName(state));
    if (state == JournalState::Committed) {
        if (fs::exists(paths.oldRoot))
            removeTreeChecked(paths.oldRoot);
        if (fs::exists(paths.directory))
            removeTreeChecked(paths.directory);
        removeRecoveryMarker(plan);
        return;
    }
    if (state == JournalState::None) {
        if (fs::exists(paths.oldRoot)) {
            throw std::runtime_error("installer transaction journal is missing after the old tree "
                                     "moved; transaction retained");
        }
        removeTreeChecked(paths.directory);
        removeRecoveryMarker(plan);
        return;
    }
    if (state == JournalState::Prepared) {
        removeTreeChecked(paths.directory);
        removeRecoveryMarker(plan);
        return;
    }
    if (state == JournalState::RollbackFilesRestored) {
        std::optional<HostPackage> restoredPackage;
        if (fs::exists(plan.installRoot))
            restoredPackage = loadInstalledPackage(plan.installRoot, package, logger);
        restoreMetadataAfterRollback(package, restoredPackage, plan, paths, logger);
        removeTreeChecked(paths.directory);
        removeRecoveryMarker(plan);
        return;
    }

    std::optional<HostPackage> oldPackage;
    const bool hadOldRoot = fs::exists(paths.oldRoot);
    if (hadOldRoot)
        oldPackage = loadInstalledPackage(paths.oldRoot, package, logger);
    if (hadOldRoot && !oldPackage)
        throw std::runtime_error(
            "cannot recover the prior installation metadata; transaction retained");
    if (state == JournalState::OldMoved) {
        if (!fs::exists(plan.installRoot) && fs::exists(paths.oldRoot))
            moveDirectory(paths.oldRoot, plan.installRoot);
        removeTreeChecked(paths.directory);
        removeRecoveryMarker(plan);
        return;
    }

    std::optional<HostPackage> newPackage;
    if (fs::exists(plan.installRoot))
        newPackage = loadInstalledPackage(plan.installRoot, package, logger);
    if (fs::exists(plan.installRoot))
        removeTreeChecked(plan.installRoot);
    if (fs::exists(paths.oldRoot))
        moveDirectory(paths.oldRoot, plan.installRoot);
    writeJournal(paths, JournalState::RollbackFilesRestored);
    restoreMetadataAfterRollback(
        newPackage ? *newPackage : package, oldPackage, plan, paths, logger);
    removeTreeChecked(paths.directory);
    removeRecoveryMarker(plan);
}

/// @brief Execute install, modify, or repair as a recoverable directory transaction.
/// @param package Verified package and embedded payload.
/// @param options Parsed application-close, restart, and injected lifecycle settings.
/// @param plan Resolved destination, selection, metadata, and existing installation state.
/// @param logger Installer logger and cancellation source.
/// @return Installer exit code for the committed operation.
/// @throws std::runtime_error If staging, ownership preservation, commit, or cleanup fails.
int performInstallLike(const HostPackage &package,
                       const HostOptions &options,
                       const InstallationPlan &plan,
                       Logger &logger) {
    ensureParentWritable(plan.installRoot);
    const TransactionPaths paths = transactionPaths(plan, package.metadata.identifier);
    recoverTransaction(package, plan, paths, logger);
    RestartManagerSession restart;
    bool committed = false;
    try {
        writeRecoveryMarker(plan, package.metadata.identifier);
        fs::create_directories(paths.newRoot);
        writePathBackup(paths, plan.scope);
        initializeAppliedShortcuts(paths);
        writeJournal(paths, JournalState::Prepared);
        stageSelectedTree(package, plan, paths.newRoot, logger);
        cancellationPoint(logger);
        const std::set<std::string> oldOwned =
            loadUpgradeOwnership(package, plan.installRoot, logger);
        copyUnownedFiles(plan.installRoot, paths.newRoot, oldOwned, logger);
        cancellationPoint(logger);
        maybeInjectFailure("after-stage");

        handleFilesInUse(restart, package, plan, options, logger);
        cancellationPoint(logger);
        const bool hadOldRoot = fs::exists(plan.installRoot);
        if (hadOldRoot)
            moveDirectory(plan.installRoot, paths.oldRoot);
        writeJournal(paths, JournalState::OldMoved);
        maybeInjectFailure("after-old-move");
        moveDirectory(paths.newRoot, plan.installRoot);
        writeJournal(paths, JournalState::NewActive);
        maybeInjectFailure("after-new-move");
        cancellationPoint(logger);
        applyMetadata(package, plan, paths, logger);
        writeJournal(paths, JournalState::MetadataCommitted);
        maybeInjectFailure("after-registry");
        writeJournal(paths, JournalState::Committed);
        committed = true;
        if (fs::exists(paths.oldRoot))
            removeTreeChecked(paths.oldRoot);
        removeTreeChecked(paths.directory);
        removeRecoveryMarker(plan);
        restart.restartApplications(!options.noRestart);
        logger.info(L"Transactional installation committed");
        return kExitSuccess;
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        try {
            recoverTransaction(package, plan, paths, logger);
            restart.restartApplications(!options.noRestart);
            if (committed) {
                logger.warning(L"Recovered cleanup after the installation commit point");
                return kExitSuccess;
            }
        } catch (const std::exception &recoveryError) {
            logger.error(L"Transaction rollback failed; the recovery journal was retained: " +
                         utf8ToWide(recoveryError.what()));
        }
        std::rethrow_exception(failure);
    }
}

/// @brief Execute uninstall as a recoverable transaction that preserves unowned files.
/// @param package Verified installed package and cleanup-helper payload.
/// @param options Parsed application-close and restart settings.
/// @param plan Resolved installation, metadata, ownership, and cache state.
/// @param logger Installer logger and cancellation source.
/// @return Success or reboot-required after the uninstall commit point.
/// @throws std::runtime_error If ownership, preservation, metadata removal, or rollback fails.
int performUninstall(const HostPackage &package,
                     const HostOptions &options,
                     const InstallationPlan &plan,
                     Logger &logger) {
    const TransactionPaths paths = transactionPaths(plan, package.metadata.identifier);
    recoverTransaction(package, plan, paths, logger);
    RestartManagerSession restart;
    bool committed = false;
    bool hasUnowned = false;
    try {
        writeRecoveryMarker(plan, package.metadata.identifier);
        fs::create_directories(paths.newRoot);
        writePathBackup(paths, plan.scope);
        initializeAppliedShortcuts(paths);
        writeJournal(paths, JournalState::Prepared);
        const std::set<std::string> owned =
            loadOwnershipManifest(plan.installRoot, package.metadata.installedManifestRelativePath);
        if (owned.empty())
            throw std::runtime_error("ownership manifest is missing; refusing an unsafe uninstall");
        copyUnownedFiles(plan.installRoot, paths.newRoot, owned, logger);
        cancellationPoint(logger);

        handleFilesInUse(restart, package, plan, options, logger);
        cancellationPoint(logger);
        moveDirectory(plan.installRoot, paths.oldRoot);
        writeJournal(paths, JournalState::OldMoved);
        maybeInjectFailure("after-old-move");
        hasUnowned =
            fs::recursive_directory_iterator(paths.newRoot) != fs::recursive_directory_iterator{};
        if (hasUnowned)
            moveDirectory(paths.newRoot, plan.installRoot);
        writeJournal(paths, JournalState::NewActive);
        maybeInjectFailure("after-new-move");
        cancellationPoint(logger);
        removeMetadata(package, plan, logger);
        writeJournal(paths, JournalState::MetadataCommitted);
        maybeInjectFailure("after-registry");
        writeJournal(paths, JournalState::Committed);
        committed = true;
        removeTreeChecked(paths.oldRoot);
        removeTreeChecked(paths.directory);
        removeRecoveryMarker(plan);
        const bool cleanupComplete = cleanupCacheAfterUninstall(package, plan, logger);
        restart.restartApplications(!options.noRestart);
        if (!hasUnowned && fs::exists(plan.installRoot))
            removeTreeChecked(plan.installRoot);
        logger.info(L"Transactional uninstall committed without owned residue");
        return cleanupComplete ? kExitSuccess : kExitRebootRequired;
    } catch (...) {
        const std::exception_ptr failure = std::current_exception();
        try {
            recoverTransaction(package, plan, paths, logger);
            restart.restartApplications(!options.noRestart);
            if (committed) {
                const bool cleanupComplete = cleanupCacheAfterUninstall(package, plan, logger);
                logger.warning(L"Recovered cleanup after the uninstall commit point");
                return cleanupComplete ? kExitSuccess : kExitRebootRequired;
            }
        } catch (const std::exception &recoveryError) {
            logger.error(L"Uninstall rollback failed; the recovery journal was retained: " +
                         utf8ToWide(recoveryError.what()));
        }
        std::rethrow_exception(failure);
    }
}

/// @brief Open user-requested tools, documentation, or samples after successful installation.
/// @param package Verified package providing product and executable metadata.
/// @param options Parsed post-install launch selections.
/// @param plan Resolved installation root.
/// @param logger Installer logger used for unavailable-item and launch warnings.
void launchPostInstallActions(const HostPackage &package,
                              const HostOptions &options,
                              const InstallationPlan &plan,
                              Logger &logger) {
    /// @brief Open one post-install target through the Windows shell.
    /// @param path File or directory to open.
    /// @param parameters Optional argument string supplied to the target.
    auto open = [&](const fs::path &path, const wchar_t *parameters = nullptr) {
        if (path.empty() || !fs::exists(path)) {
            logger.warning(L"Requested post-install item is unavailable: " + path.wstring());
            return;
        }
        const HINSTANCE result = ShellExecuteW(
            nullptr, L"open", path.c_str(), parameters, plan.installRoot.c_str(), SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32)
            logger.warning(L"Windows could not open the requested post-install item");
    };

    if (options.launchIDE) {
        const std::string relative = package.metadata.productKind == "toolchain" &&
                                             !package.metadata.associationExecutable.empty()
                                         ? package.metadata.associationExecutable
                                         : package.metadata.executableName;
        open(safeJoin(plan.installRoot, relative));
    }
    if (options.launchPrompt) {
        wchar_t systemDirectory[32768]{};
        const UINT length =
            GetSystemDirectoryW(systemDirectory, static_cast<UINT>(std::size(systemDirectory)));
        if (length > 0 && length < std::size(systemDirectory)) {
            const fs::path script = plan.installRoot / L"bin" / L"zanna-dev.cmd";
            const std::wstring parameters = L"/k " + quoteCommandLineArgument(script.wstring());
            open(fs::path(systemDirectory) / L"cmd.exe", parameters.c_str());
        }
    }
    if (options.openQuickstart) {
        const std::array<fs::path, 3> candidates = {
            plan.installRoot / L"share" / L"zanna" / L"README.windows-prerequisites.txt",
            plan.installRoot / L"share" / L"doc" / L"zanna" / L"README.md",
            plan.installRoot / L"README.md"};
        /// @brief Test whether one quick-start candidate is an existing regular file.
        /// @param path Candidate documentation path.
        /// @return `true` when `path` names a regular file.
        const auto found =
            std::find_if(candidates.begin(), candidates.end(), [](const fs::path &path) {
                return fs::is_regular_file(path);
            });
        open(found == candidates.end() ? fs::path{} : *found);
    }
    if (options.openSamples)
        open(plan.installRoot / L"share" / L"zanna" / L"samples");
}

} // namespace

/// @brief Orchestrate installer recovery, UI, elevation, handoff, preflight, and execution.
/// @param instance Current module instance used by wizard and progress interfaces.
/// @param package Verified host package and embedded installer data.
/// @param requestedOptions Parsed command-line request before UI-derived changes.
/// @param logger Installer logger and cancellation source.
/// @return Stable installer exit code for cancellation, concurrency, success, or reboot required.
/// @throws std::runtime_error If lifecycle validation or an operation fails before translation.
int runLifecycle(HINSTANCE instance,
                 const HostPackage &package,
                 const HostOptions &requestedOptions,
                 Logger &logger) {
    HostOptions options = requestedOptions;
    preflightWindowsVersion(package, logger);
    InstallationPlan plan = makePlan(package, options);
    if (options.uninstallWorker &&
        (package.metadata.packageMode != "maintenance" ||
         !sameWindowsPath(currentExecutablePath(), plan.cacheExecutable))) {
        throw std::runtime_error(
            "maintenance-handoff worker mode requires the verified cache executable");
    }
    if (options.elevatedWorker && (plan.scope != InstallScope::Machine || !isProcessElevated())) {
        throw std::runtime_error("elevated worker mode requires an elevated machine-scope process");
    }
    waitForHandoffParent(options.handoffParentId);
    const std::optional<InstalledRecord> recoveryRecord =
        readRecoveryRecord(package, options, logger);
    plan = makePlan(package, options, recoveryRecord ? &*recoveryRecord : nullptr);
    if (recoveryRecord) {
        if (plan.scope == InstallScope::Machine && !isProcessElevated()) {
            const int elevated = relaunchElevated(package, options, plan, logger);
            if ((elevated == kExitSuccess || elevated == kExitRebootRequired) &&
                options.uiLevel == UiLevel::Full && plan.operation != Operation::Uninstall) {
                showInstallerFinish(instance, package, plan.installRoot, plan.components, options);
                launchPostInstallActions(package, options, plan, logger);
            }
            return elevated;
        }
        LifecycleMutex recoveryMutex(plan, package.metadata.identifier);
        if (!recoveryMutex.acquired())
            return kExitAnotherInstallRunning;
        recoverTransaction(
            package, plan, transactionPaths(plan, package.metadata.identifier), logger);
        const InstalledRecord recovered =
            readInstalledRecord(package.metadata.identifier, plan.scope);
        if (!recovered.present && plan.operation == Operation::Uninstall) {
            logger.info(L"Interrupted uninstall recovery completed the requested removal");
            return kExitSuccess;
        }
        plan = makePlan(package, options);
        logger.info(L"Interrupted installer transaction recovery completed");
    }

    if (options.uiLevel == UiLevel::Full && !options.elevatedWorker && !options.uninstallWorker) {
        if (!options.addToPath)
            options.addToPath = plan.addToPath;
        if (!options.registerAssociations)
            options.registerAssociations = plan.registerAssociations;
        if (!options.createShortcuts)
            options.createShortcuts = plan.createShortcuts;
        if (!configureInstallerWizard(instance,
                                      package,
                                      plan.installRoot,
                                      plan.scope,
                                      plan.components,
                                      plan.existing.present,
                                      options)) {
            return kExitUserCancelled;
        }
        plan = makePlan(package, options);
    }

    logger.info(L"Operation: " + operationSwitch(plan.operation));
    logger.info(L"Scope: " +
                std::wstring(plan.scope == InstallScope::User ? L"current user" : L"all users"));
    logger.info(L"Destination: " + plan.installRoot.wstring());
    const fs::path runningExecutable = currentExecutablePath();
    if (!options.uninstallWorker && package.metadata.packageMode == "maintenance" &&
        !sameWindowsPath(runningExecutable, plan.cacheExecutable) &&
        pathIsWithin(plan.installRoot, runningExecutable)) {
        return launchMaintenanceHandoff(package, options, plan, logger);
    }
    if (plan.scope == InstallScope::Machine && !isProcessElevated()) {
        const int elevated = relaunchElevated(package, options, plan, logger);
        if ((elevated == kExitSuccess || elevated == kExitRebootRequired) &&
            options.uiLevel == UiLevel::Full && plan.operation != Operation::Uninstall) {
            showInstallerFinish(instance, package, plan.installRoot, plan.components, options);
            launchPostInstallActions(package, options, plan, logger);
        }
        return elevated;
    }
    LifecycleMutex mutex(plan, package.metadata.identifier);
    if (!mutex.acquired()) {
        logger.error(L"Another Zanna lifecycle operation is already active");
        return kExitAnotherInstallRunning;
    }
    preflightVersion(package, options, plan);
    preflightDisk(package, plan);
    /// @brief Execute the selected lifecycle operation under the progress interface.
    /// @return Stable installer exit code returned by install-like or uninstall processing.
    const int result =
        runInstallerProgress(instance, package, plan.operation, options.uiLevel, logger, [&] {
            if (plan.operation == Operation::Uninstall)
                return performUninstall(package, options, plan, logger);
            return performInstallLike(package, options, plan, logger);
        });
    if ((result == kExitSuccess || result == kExitRebootRequired) &&
        plan.operation != Operation::Uninstall) {
        if (options.uiLevel == UiLevel::Full && !options.elevatedWorker && !options.uninstallWorker)
            showInstallerFinish(instance, package, plan.installRoot, plan.components, options);
        if (!options.elevatedWorker)
            launchPostInstallActions(package, options, plan, logger);
    }
    return result;
}

} // namespace zanna::installer
