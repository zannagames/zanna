//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerUpdate.cpp
// Purpose: Implement bounded HTTPS update discovery with pinned RSA signatures.
// Key invariants:
//   - Redirects, noncanonical text, cross-origin links, and unsigned data fail closed.
//   - Only a revalidated authenticated HTTPS URL can reach ShellExecuteW.
// Ownership/Lifetime:
//   - Local RAII wrappers close every WinHTTP and CNG handle on all exit paths.
//   - Returned update records own their text and retain no system handles.
// Links: src/tools/windows_installer/WindowsInstallerUpdate.hpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerUpdate.hpp"
#include "WindowsInstallerResources.h"

#include "PkgHash.hpp"

#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zanna::installer {
namespace {

constexpr std::size_t kMaximumManifestBytes = 64U * 1024U;
constexpr std::size_t kMinimumRsaModulusBytes = 256U;
constexpr std::size_t kMaximumRsaModulusBytes = 512U;
constexpr std::size_t kMaximumRsaExponentBytes = sizeof(uint32_t);
constexpr int kOpenUpdate = 2401;

/// @brief Move-only owner for a WinHTTP session, connection, or request handle.
class InternetHandle {
  public:
    /// @brief Construct an empty handle owner.
    InternetHandle() = default;

    /// @brief Adopt a WinHTTP handle.
    /// @param value Handle to close at destruction.
    explicit InternetHandle(HINTERNET value) : value_(value) {}

    /// @brief Close the adopted WinHTTP handle.
    ~InternetHandle() {
        if (value_)
            WinHttpCloseHandle(value_);
    }

    /// @brief WinHTTP handles have unique ownership and cannot be copied.
    InternetHandle(const InternetHandle &) = delete;

    /// @brief WinHTTP handles have unique ownership and cannot be copy-assigned.
    InternetHandle &operator=(const InternetHandle &) = delete;

    /// @brief Transfer ownership from another wrapper.
    /// @param other Wrapper to empty.
    InternetHandle(InternetHandle &&other) noexcept : value_(other.value_) {
        other.value_ = nullptr;
    }

    /// @brief Close the current handle and transfer ownership from another wrapper.
    /// @param other Wrapper to empty.
    /// @return This wrapper after the transfer.
    InternetHandle &operator=(InternetHandle &&other) noexcept {
        if (this != &other) {
            if (value_)
                WinHttpCloseHandle(value_);
            value_ = other.value_;
            other.value_ = nullptr;
        }
        return *this;
    }

    /// @brief Access the borrowed WinHTTP handle.
    /// @return Stored handle, possibly null.
    HINTERNET get() const {
        return value_;
    }

    /// @brief Test whether this wrapper owns a handle.
    /// @return @c true when the stored handle is nonnull.
    explicit operator bool() const {
        return value_ != nullptr;
    }

  private:
    HINTERNET value_{nullptr};
};

/// @brief RAII owner for a CNG algorithm-provider handle.
class AlgorithmHandle {
  public:
    /// @brief Close the provider if one was opened.
    ~AlgorithmHandle() {
        if (value_)
            BCryptCloseAlgorithmProvider(value_, 0);
    }

    /// @brief Clear the current provider and expose storage to an opening API.
    /// @return Pointer to the null handle slot.
    BCRYPT_ALG_HANDLE *put() {
        if (value_) {
            BCryptCloseAlgorithmProvider(value_, 0);
            value_ = nullptr;
        }
        return &value_;
    }

    /// @brief Access the borrowed provider handle.
    /// @return Stored provider handle, possibly null.
    BCRYPT_ALG_HANDLE get() const {
        return value_;
    }

  private:
    BCRYPT_ALG_HANDLE value_{nullptr};
};

/// @brief RAII owner for an imported CNG key handle.
class KeyHandle {
  public:
    /// @brief Destroy the key if one was imported.
    ~KeyHandle() {
        if (value_)
            BCryptDestroyKey(value_);
    }

    /// @brief Clear the current key and expose storage to an import API.
    /// @return Pointer to the null handle slot.
    BCRYPT_KEY_HANDLE *put() {
        if (value_) {
            BCryptDestroyKey(value_);
            value_ = nullptr;
        }
        return &value_;
    }

    /// @brief Access the borrowed imported key handle.
    /// @return Stored key handle, possibly null.
    BCRYPT_KEY_HANDLE get() const {
        return value_;
    }

  private:
    BCRYPT_KEY_HANDLE value_{nullptr};
};

/// @brief Security-relevant components of a validated HTTPS URL.
struct ParsedUrl {
    /// @brief Validated WinHTTP scheme.
    INTERNET_SCHEME scheme{static_cast<INTERNET_SCHEME>(0)};
    /// @brief Authority host name.
    std::wstring host;
    /// @brief Effective network port.
    INTERNET_PORT port{0};
    /// @brief Path and query sent as the HTTP request target.
    std::wstring resource;
};

/// @brief Escape UTF-8 bytes and control characters for a JSON string literal.
/// @param value Unquoted text to encode.
/// @return Escaped bytes without surrounding quotation marks.
std::string jsonEscape(std::string_view value) {
    std::ostringstream out;
    for (unsigned char ch : value) {
        switch (ch) {
            case '"':
                out << "\\\"";
                break;
            case '\\':
                out << "\\\\";
                break;
            case '\b':
                out << "\\b";
                break;
            case '\f':
                out << "\\f";
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
                    static constexpr char kHex[] = "0123456789abcdef";
                    out << "\\u00" << kHex[ch >> 4U] << kHex[ch & 0x0fU];
                } else {
                    out << static_cast<char>(ch);
                }
                break;
        }
    }
    return out.str();
}

/// @brief Decode strict lowercase hexadecimal with field-specific diagnostics.
/// @param value Nonempty, even-length encoded bytes.
/// @param field Field name included in validation errors.
/// @return Decoded byte vector.
/// @throws std::runtime_error If length or any digit is invalid.
std::vector<uint8_t> decodeHex(std::string_view value, std::string_view field) {
    if (value.empty() || value.size() % 2U != 0U)
        throw std::runtime_error("invalid " + std::string(field));
    /// @brief Decode one lowercase hexadecimal digit.
    /// @param ch Character to decode.
    /// @return Numeric nibble value in the range zero through fifteen.
    /// @throws std::runtime_error If `ch` is not lowercase hexadecimal.
    auto nibble = [field](char ch) -> uint8_t {
        if (ch >= '0' && ch <= '9')
            return static_cast<uint8_t>(ch - '0');
        if (ch >= 'a' && ch <= 'f')
            return static_cast<uint8_t>(ch - 'a' + 10);
        throw std::runtime_error("invalid " + std::string(field));
    };
    std::vector<uint8_t> result(value.size() / 2U);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] =
            static_cast<uint8_t>((nibble(value[i * 2U]) << 4U) | nibble(value[i * 2U + 1U]));
    return result;
}

/// @brief Test whether every byte is a lowercase hexadecimal digit.
/// @param value Text to inspect.
/// @return @c true for an empty or entirely lowercase-hexadecimal sequence.
bool isLowerHex(std::string_view value) {
    /// @brief Test one byte for lowercase hexadecimal membership.
    /// @param ch Byte to inspect.
    /// @return `true` for `0` through `9` or `a` through `f`.
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    });
}

/// @brief Test whether a lowercase hexadecimal digit represents an odd nibble.
/// @param ch Character to inspect.
/// @return @c true for @c 1, 3, 5, 7, 9, b, d, or f.
bool isOddLowerHexDigit(char ch) {
    return ch == '1' || ch == '3' || ch == '5' || ch == '7' || ch == '9' || ch == 'b' ||
           ch == 'd' || ch == 'f';
}

/// @brief Validate the structural and arithmetic constraints of a pinned RSA public key.
/// @param metadata Package metadata containing lowercase modulus and exponent fields.
/// @throws std::runtime_error If modulus size, top bit, parity, or exponent is invalid.
void validatePinnedKey(const zanna::pkg::WindowsInstallerMetadata &metadata) {
    if (metadata.updateRsaModulus.size() < kMinimumRsaModulusBytes * 2U ||
        metadata.updateRsaModulus.size() > kMaximumRsaModulusBytes * 2U ||
        metadata.updateRsaModulus.size() % 2U != 0U || !isLowerHex(metadata.updateRsaModulus) ||
        metadata.updateRsaModulus.front() < '8' ||
        !isOddLowerHexDigit(metadata.updateRsaModulus.back())) {
        throw std::runtime_error("invalid pinned update RSA modulus");
    }
    if (metadata.updateRsaExponent.size() < 2U ||
        metadata.updateRsaExponent.size() > kMaximumRsaExponentBytes * 2U ||
        metadata.updateRsaExponent.size() % 2U != 0U || !isLowerHex(metadata.updateRsaExponent) ||
        metadata.updateRsaExponent.rfind("00", 0) == 0) {
        throw std::runtime_error("invalid pinned update RSA exponent");
    }
    uint32_t exponent = 0;
    for (std::size_t offset = 0; offset < metadata.updateRsaExponent.size(); offset += 2U) {
        const std::vector<uint8_t> byte =
            decodeHex(metadata.updateRsaExponent.substr(offset, 2U), "update RSA exponent");
        exponent = (exponent << 8U) | byte.front();
    }
    if (exponent < 3U || exponent % 2U == 0U)
        throw std::runtime_error("invalid pinned update RSA exponent");
}

/// @brief Validate a bounded printable UTF-8 manifest field.
/// @param value Field value to inspect.
/// @param field Field name included in diagnostics.
/// @param allowEmpty Whether an empty value is permitted.
/// @throws std::runtime_error If presence, size, control bytes, or UTF-8 validity fails.
void validateManifestValue(std::string_view value,
                           std::string_view field,
                           bool allowEmpty = false) {
    /// @brief Identify a control byte forbidden in update-manifest fields.
    /// @param ch Byte to inspect.
    /// @return `true` for C0 controls or DEL.
    if ((!allowEmpty && value.empty()) || value.size() > 8192U ||
        std::any_of(value.begin(), value.end(), [](unsigned char ch) {
            return ch < 0x20U || ch == 0x7fU;
        })) {
        throw std::runtime_error("invalid update manifest " + std::string(field));
    }
    (void)utf8ToWide(value);
}

/// @brief Parse a manifest field as an unambiguous HTTPS URL without credentials.
/// @param utf8 URL text to validate and crack.
/// @param field Field name included in diagnostics.
/// @return Scheme, host, effective port, and request resource.
/// @throws std::runtime_error If characters, encoding, scheme, authority, or credentials are
/// unsafe.
ParsedUrl parseHttpsUrl(std::string_view utf8, std::string_view field) {
    validateManifestValue(utf8, field);
    if (utf8.find('#') != std::string_view::npos || utf8.find('\\') != std::string_view::npos ||
        utf8.find(' ') != std::string_view::npos) {
        throw std::runtime_error("update " + std::string(field) +
                                 " contains an ambiguous URL character");
    }
    const std::wstring url = utf8ToWide(utf8);
    URL_COMPONENTSW components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components) ||
        components.nScheme != INTERNET_SCHEME_HTTPS || components.dwHostNameLength == 0U ||
        components.dwUserNameLength != 0U || components.dwPasswordLength != 0U) {
        throw std::runtime_error("update " + std::string(field) + " must be an HTTPS URL");
    }
    ParsedUrl result;
    result.scheme = components.nScheme;
    result.host.assign(components.lpszHostName, components.dwHostNameLength);
    result.port = components.nPort;
    if (components.dwUrlPathLength != 0U)
        result.resource.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0U)
        result.resource.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    if (result.resource.empty())
        result.resource = L"/";
    return result;
}

/// @brief Compare two parsed URL origins using Windows ordinal host folding.
/// @param left First validated URL.
/// @param right Second validated URL.
/// @return @c true when scheme, port, and case-insensitive host all match.
bool sameOrigin(const ParsedUrl &left, const ParsedUrl &right) {
    if (left.scheme != right.scheme || left.port != right.port || left.host.size() > INT_MAX ||
        right.host.size() > INT_MAX) {
        return false;
    }
    return CompareStringOrdinal(left.host.data(),
                                static_cast<int>(left.host.size()),
                                right.host.data(),
                                static_cast<int>(right.host.size()),
                                TRUE) == CSTR_EQUAL;
}

/// @brief Download a bounded manifest over TLS 1.2 or 1.3 without redirects.
/// @param manifestUrl Configured HTTPS manifest URL.
/// @param version Current package version included in the user agent.
/// @return Nonempty response body no larger than 64 KiB.
/// @throws std::runtime_error On URL, TLS, connection, HTTP, timeout, or size failure.
std::string downloadManifest(std::string_view manifestUrl, std::string_view version) {
    const ParsedUrl url = parseHttpsUrl(manifestUrl, "manifest URL");
    const std::wstring agent = L"Zanna-Installer/" + utf8ToWide(version);
    InternetHandle session(
        WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0));
    if (!session) {
        session = InternetHandle(
            WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0));
    }
    if (!session)
        throw std::runtime_error("cannot initialize secure update networking");
    DWORD secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    secureProtocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    if (!WinHttpSetOption(session.get(),
                          WINHTTP_OPTION_SECURE_PROTOCOLS,
                          &secureProtocols,
                          sizeof(secureProtocols))) {
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        // Older WinHTTP implementations reject the newer flag. Retain TLS 1.2
        // as the floor without enabling any legacy protocol.
        secureProtocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        if (!WinHttpSetOption(session.get(),
                              WINHTTP_OPTION_SECURE_PROTOCOLS,
                              &secureProtocols,
                              sizeof(secureProtocols)))
#endif
            throw std::runtime_error("cannot require TLS 1.2 for update networking");
    }
    if (!WinHttpSetTimeouts(session.get(), 5000, 5000, 10000, 10000))
        throw std::runtime_error("cannot configure update network timeouts");
    InternetHandle connection(WinHttpConnect(session.get(), url.host.c_str(), url.port, 0));
    if (!connection)
        throw std::runtime_error("cannot connect to the update service");
    LPCWSTR accept[] = {L"text/plain", nullptr};
    InternetHandle request(WinHttpOpenRequest(connection.get(),
                                              L"GET",
                                              url.resource.c_str(),
                                              nullptr,
                                              WINHTTP_NO_REFERER,
                                              accept,
                                              WINHTTP_FLAG_SECURE));
    if (!request)
        throw std::runtime_error("cannot create the secure update request");
    DWORD secureFeatures = WINHTTP_ENABLE_SSL_REVOCATION;
    if (!WinHttpSetOption(request.get(),
                          WINHTTP_OPTION_ENABLE_FEATURE,
                          &secureFeatures,
                          sizeof(secureFeatures))) {
        throw std::runtime_error("cannot enable update certificate revocation checks");
    }
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(request.get(),
                          WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirectPolicy,
                          sizeof(redirectPolicy))) {
        throw std::runtime_error("cannot disable update-service redirects");
    }
    if (!WinHttpSendRequest(request.get(),
                            L"Accept: text/plain\r\nCache-Control: no-cache\r\n",
                            static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA,
                            0,
                            0,
                            0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        throw std::runtime_error("the update service request failed");
    }
    DWORD status = 0;
    DWORD statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(),
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &status,
                             &statusBytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        throw std::runtime_error("cannot read the update service HTTP status");
    }
    if (status != 200U) {
        throw std::runtime_error("the update service returned HTTP status " +
                                 std::to_string(status));
    }
    DWORD contentLength = 0;
    DWORD lengthBytes = sizeof(contentLength);
    if (WinHttpQueryHeaders(request.get(),
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &contentLength,
                            &lengthBytes,
                            WINHTTP_NO_HEADER_INDEX) &&
        contentLength > kMaximumManifestBytes) {
        throw std::runtime_error("the update manifest exceeds the 64 KiB safety limit");
    }
    std::string body;
    if (contentLength != 0U)
        body.reserve(contentLength);
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(
                request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &read))
            throw std::runtime_error("cannot read the update manifest");
        if (read == 0U)
            break;
        if (read > buffer.size() || read > kMaximumManifestBytes - body.size())
            throw std::runtime_error("the update manifest exceeds the 64 KiB safety limit");
        body.append(buffer.data(), read);
    }
    if (body.empty())
        throw std::runtime_error("the update service returned an empty manifest");
    return body;
}

/// @brief Extract one strictly named, single-tab manifest field.
/// @param line Complete manifest line.
/// @param name Expected field name.
/// @param allowEmpty Whether the field value may be empty.
/// @return Validated UTF-8 value after the field delimiter.
/// @throws std::runtime_error If name, delimiters, or value constraints are invalid.
std::string fieldValue(const std::string &line, std::string_view name, bool allowEmpty = false) {
    const std::string prefix = std::string(name) + "\t";
    if (line.rfind(prefix, 0) != 0 || line.find('\t', prefix.size()) != std::string::npos)
        throw std::runtime_error("malformed update manifest field " + std::string(name));
    std::string value = line.substr(prefix.size());
    validateManifestValue(value, name, allowEmpty);
    return value;
}

/// @brief Verify a PKCS#1 v1.5 SHA-256 signature with the package-pinned RSA key.
/// @param metadata Package metadata containing the pinned modulus and exponent.
/// @param canonical Exact canonical manifest bytes covered by the signature.
/// @param signatureHex Lowercase signature bytes with modulus-matched length.
/// @throws std::runtime_error If key, signature shape, CNG import, or verification fails.
void verifySignature(const zanna::pkg::WindowsInstallerMetadata &metadata,
                     std::string_view canonical,
                     std::string_view signatureHex) {
    validatePinnedKey(metadata);
    if (signatureHex.size() != metadata.updateRsaModulus.size() || !isLowerHex(signatureHex))
        throw std::runtime_error("update manifest signature size does not match its pinned key");
    const std::vector<uint8_t> modulus = decodeHex(metadata.updateRsaModulus, "update RSA modulus");
    const std::vector<uint8_t> exponent =
        decodeHex(metadata.updateRsaExponent, "update RSA exponent");
    const std::vector<uint8_t> signature = decodeHex(signatureHex, "update manifest signature");
    if (signature.size() != modulus.size())
        throw std::runtime_error("update manifest signature size does not match its pinned key");
    const std::string hashHex = zanna::pkg::sha256Hex(
        reinterpret_cast<const uint8_t *>(canonical.data()), canonical.size());
    const std::vector<uint8_t> hash = decodeHex(hashHex, "update manifest digest");

    BCRYPT_RSAKEY_BLOB header{};
    header.Magic = BCRYPT_RSAPUBLIC_MAGIC;
    header.BitLength = static_cast<ULONG>(modulus.size() * 8U);
    header.cbPublicExp = static_cast<ULONG>(exponent.size());
    header.cbModulus = static_cast<ULONG>(modulus.size());
    std::vector<uint8_t> blob(sizeof(header) + exponent.size() + modulus.size());
    std::memcpy(blob.data(), &header, sizeof(header));
    std::memcpy(blob.data() + sizeof(header), exponent.data(), exponent.size());
    std::memcpy(blob.data() + sizeof(header) + exponent.size(), modulus.data(), modulus.size());

    AlgorithmHandle algorithm;
    if (BCryptOpenAlgorithmProvider(algorithm.put(), BCRYPT_RSA_ALGORITHM, nullptr, 0) != 0 ||
        !algorithm.get()) {
        throw std::runtime_error("cannot initialize update signature verification");
    }
    KeyHandle key;
    if (BCryptImportKeyPair(algorithm.get(),
                            nullptr,
                            BCRYPT_RSAPUBLIC_BLOB,
                            key.put(),
                            blob.data(),
                            static_cast<ULONG>(blob.size()),
                            0) != 0 ||
        !key.get()) {
        throw std::runtime_error("cannot import the pinned update public key");
    }
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    if (BCryptVerifySignature(key.get(),
                              &padding,
                              const_cast<PUCHAR>(hash.data()),
                              static_cast<ULONG>(hash.size()),
                              const_cast<PUCHAR>(signature.data()),
                              static_cast<ULONG>(signature.size()),
                              BCRYPT_PAD_PKCS1) != 0) {
        throw std::runtime_error("update manifest RSA-SHA256 signature verification failed");
    }
}

/// @brief Revalidate an update record before presenting or opening its URL.
/// @param package Verified package identity and pinned update origin.
/// @param result Candidate result, normally returned by verifyUpdateManifest().
/// @throws std::runtime_error If status, identity, precedence, digest, or URLs are inconsistent.
void validateUpdateResultForPresentation(const HostPackage &package,
                                         const UpdateCheckResult &result) {
    switch (result.status) {
        case UpdateStatus::Unconfigured:
            if (result.currentVersion != package.metadata.version ||
                result.channel != package.metadata.channel ||
                result.architecture != package.metadata.architecture ||
                !package.metadata.updateManifestUrl.empty() ||
                !package.metadata.updateRsaModulus.empty() ||
                !package.metadata.updateRsaExponent.empty() || !result.availableVersion.empty() ||
                !result.downloadUrl.empty() || !result.downloadSha256.empty() ||
                !result.releaseNotesUrl.empty()) {
                throw std::runtime_error("inconsistent unconfigured update result");
            }
            return;
        case UpdateStatus::Current:
        case UpdateStatus::Available:
            break;
        default:
            throw std::runtime_error("invalid update result status");
    }
    if (result.currentVersion != package.metadata.version ||
        result.channel != package.metadata.channel ||
        result.architecture != package.metadata.architecture) {
        throw std::runtime_error("update result identity does not match this installer");
    }
    const int precedence = compareInstallerVersions(result.availableVersion, result.currentVersion);
    if ((result.status == UpdateStatus::Available && precedence <= 0) ||
        (result.status == UpdateStatus::Current && precedence > 0)) {
        throw std::runtime_error("update result status does not match version precedence");
    }
    if (result.downloadSha256.size() != 64U || !isLowerHex(result.downloadSha256))
        throw std::runtime_error("update result has an invalid download SHA-256");

    const ParsedUrl manifestUrl = parseHttpsUrl(package.metadata.updateManifestUrl, "manifest URL");
    const ParsedUrl downloadUrl = parseHttpsUrl(result.downloadUrl, "download URL");
    if (!sameOrigin(manifestUrl, downloadUrl))
        throw std::runtime_error("update result download URL does not match the pinned origin");
    if (!result.releaseNotesUrl.empty()) {
        const ParsedUrl releaseNotesUrl =
            parseHttpsUrl(result.releaseNotesUrl, "release notes URL");
        if (!sameOrigin(manifestUrl, releaseNotesUrl)) {
            throw std::runtime_error(
                "update result release-notes URL does not match the pinned origin");
        }
    }
}

} // namespace

/// @brief Parse, constrain, and authenticate a canonical update manifest.
/// @param package Verified package providing pinned update identity and public key.
/// @param manifestText Complete downloaded manifest bytes.
/// @return Matching release data with current, available, or unconfigured status.
/// @throws std::runtime_error If configuration, schema, fields, origins, digest, or signature
/// fails.
UpdateCheckResult verifyUpdateManifest(const HostPackage &package, std::string_view manifestText) {
    const bool hasUpdateUrl = !package.metadata.updateManifestUrl.empty();
    const bool hasUpdateModulus = !package.metadata.updateRsaModulus.empty();
    const bool hasUpdateExponent = !package.metadata.updateRsaExponent.empty();
    if (!hasUpdateUrl && !hasUpdateModulus && !hasUpdateExponent) {
        return {UpdateStatus::Unconfigured,
                package.metadata.version,
                {},
                package.metadata.channel,
                package.metadata.architecture,
                {},
                {},
                {}};
    }
    if (!hasUpdateUrl || !hasUpdateModulus || !hasUpdateExponent)
        throw std::runtime_error("incomplete pinned update configuration");
    validatePinnedKey(package.metadata);
    if (manifestText.empty() || manifestText.size() > kMaximumManifestBytes ||
        manifestText.find('\0') != std::string_view::npos || manifestText.back() != '\n' ||
        manifestText.find('\r') != std::string_view::npos) {
        throw std::runtime_error("invalid update manifest size or encoding");
    }
    std::istringstream input{std::string(manifestText)};
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    if (lines.size() != 8U || lines[0] != "ZANNA-WINDOWS-UPDATE\t1")
        throw std::runtime_error("unsupported or malformed Zanna update manifest");

    UpdateCheckResult result;
    result.currentVersion = package.metadata.version;
    result.channel = fieldValue(lines[1], "channel");
    result.architecture = fieldValue(lines[2], "architecture");
    result.availableVersion = fieldValue(lines[3], "version");
    result.downloadUrl = fieldValue(lines[4], "download-url");
    result.downloadSha256 = fieldValue(lines[5], "sha256");
    result.releaseNotesUrl = fieldValue(lines[6], "release-notes-url", true);
    const std::string signature = fieldValue(lines[7], "signature");
    if (result.channel != package.metadata.channel)
        throw std::runtime_error("update manifest channel does not match this installer");
    if (result.architecture != package.metadata.architecture)
        throw std::runtime_error("update manifest architecture does not match this installer");
    if (result.downloadSha256.size() != 64U || !isLowerHex(result.downloadSha256))
        throw std::runtime_error("update manifest download SHA-256 must contain 32 bytes");
    const std::vector<uint8_t> digest = decodeHex(result.downloadSha256, "download SHA-256");
    if (digest.size() != 32U)
        throw std::runtime_error("update manifest download SHA-256 must contain 32 bytes");

    const ParsedUrl manifestUrl = parseHttpsUrl(package.metadata.updateManifestUrl, "manifest URL");
    const ParsedUrl downloadUrl = parseHttpsUrl(result.downloadUrl, "download URL");
    if (!sameOrigin(manifestUrl, downloadUrl))
        throw std::runtime_error("update download URL must use the pinned manifest origin");
    if (!result.releaseNotesUrl.empty()) {
        const ParsedUrl notesUrl = parseHttpsUrl(result.releaseNotesUrl, "release notes URL");
        if (!sameOrigin(manifestUrl, notesUrl))
            throw std::runtime_error(
                "update release-notes URL must use the pinned manifest origin");
    }

    std::string canonical;
    for (std::size_t index = 0; index < 7U; ++index) {
        canonical += lines[index];
        canonical.push_back('\n');
    }
    verifySignature(package.metadata, canonical, signature);
    result.status = compareInstallerVersions(result.availableVersion, result.currentVersion) > 0
                        ? UpdateStatus::Available
                        : UpdateStatus::Current;
    return result;
}

/// @brief Discover an update through the package's pinned secure service.
/// @param package Verified package providing the current version and update configuration.
/// @return Unconfigured status or an authenticated manifest result.
/// @throws std::runtime_error If configuration, networking, or authentication fails.
UpdateCheckResult checkForUpdates(const HostPackage &package) {
    const bool hasUrl = !package.metadata.updateManifestUrl.empty();
    const bool hasModulus = !package.metadata.updateRsaModulus.empty();
    const bool hasExponent = !package.metadata.updateRsaExponent.empty();
    if (!hasUrl && !hasModulus && !hasExponent)
        return verifyUpdateManifest(package, {});
    if (!hasUrl || !hasModulus || !hasExponent)
        throw std::runtime_error("incomplete pinned update configuration");
    validatePinnedKey(package.metadata);
    return verifyUpdateManifest(
        package, downloadManifest(package.metadata.updateManifestUrl, package.metadata.version));
}

/// @brief Serialize an update result as deterministic UTF-16 JSON.
/// @param result Authenticated result to encode.
/// @return Pretty-printed JSON with stable field order and a trailing newline.
std::wstring updateResultJson(const UpdateCheckResult &result) {
    const char *status = nullptr;
    switch (result.status) {
        case UpdateStatus::Unconfigured:
            status = "unconfigured";
            break;
        case UpdateStatus::Current:
            status = "current";
            break;
        case UpdateStatus::Available:
            status = "available";
            break;
        default:
            throw std::runtime_error("invalid update result status");
    }
    std::ostringstream out;
    out << "{\n"
        << "  \"status\": \"" << status << "\",\n"
        << "  \"current_version\": \"" << jsonEscape(result.currentVersion) << "\",\n"
        << "  \"available_version\": \"" << jsonEscape(result.availableVersion) << "\",\n"
        << "  \"channel\": \"" << jsonEscape(result.channel) << "\",\n"
        << "  \"architecture\": \"" << jsonEscape(result.architecture) << "\",\n"
        << "  \"download_url\": \"" << jsonEscape(result.downloadUrl) << "\",\n"
        << "  \"sha256\": \"" << jsonEscape(result.downloadSha256) << "\",\n"
        << "  \"release_notes_url\": \"" << jsonEscape(result.releaseNotesUrl) << "\"\n"
        << "}\n";
    return utf8ToWide(out.str());
}

/// @brief Present update status and optionally open its authenticated release URL.
/// @param instance Module instance supplying the dialog icon.
/// @param package Verified package supplying the product display name.
/// @param result Authenticated update metadata to display.
/// @throws std::runtime_error If the dialog or selected URL cannot be opened.
void showUpdateResult(HINSTANCE instance,
                      const HostPackage &package,
                      const UpdateCheckResult &result) {
    validateUpdateResultForPresentation(package, result);
    std::wstring instruction;
    std::wstring content;
    std::array<TASKDIALOG_BUTTON, 2> buttons{};
    UINT buttonCount = 0;
    switch (result.status) {
        case UpdateStatus::Unconfigured:
            instruction = L"Update discovery is not configured";
            content = L"This development package has no pinned signed update service.";
            break;
        case UpdateStatus::Current:
            instruction = L"Zanna is up to date";
            content = L"Installed/package version: " + utf8ToWide(result.currentVersion) +
                      L"\r\nChannel: " + utf8ToWide(result.channel) + L"  |  " +
                      utf8ToWide(result.architecture);
            break;
        case UpdateStatus::Available:
            instruction = L"A newer Zanna version is available";
            content =
                L"Current: " + utf8ToWide(result.currentVersion) + L"\r\nAvailable: " +
                utf8ToWide(result.availableVersion) + L"\r\nChannel: " +
                utf8ToWide(result.channel) + L"  |  " + utf8ToWide(result.architecture) +
                L"\r\n\r\nThe release link was authenticated by the public key pinned in this "
                L"installer.";
            buttons[buttonCount++] = {kOpenUpdate, L"Open authenticated release page"};
            break;
        default:
            throw std::runtime_error("invalid update result status");
    }
    buttons[buttonCount++] = {IDCLOSE, L"Close"};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hInstance = instance;
    const std::wstring title = utf8ToWide(package.metadata.displayName) + L" Update";
    config.pszWindowTitle = title.c_str();
    config.pszMainInstruction = instruction.c_str();
    config.pszContent = content.c_str();
    config.dwFlags = TDF_SIZE_TO_CONTENT | TDF_USE_COMMAND_LINKS | TDF_USE_HICON_MAIN;
    config.hMainIcon = static_cast<HICON>(LoadImageW(instance,
                                                     MAKEINTRESOURCEW(IDI_ZANNA_INSTALLER),
                                                     IMAGE_ICON,
                                                     0,
                                                     0,
                                                     LR_DEFAULTSIZE | LR_SHARED));
    config.cButtons = buttonCount;
    config.pButtons = buttons.data();
    config.nDefaultButton = result.status == UpdateStatus::Available ? kOpenUpdate : IDCLOSE;
    int selected = IDCLOSE;
    if (FAILED(TaskDialogIndirect(&config, &selected, nullptr, nullptr)))
        throw std::runtime_error("cannot display the Zanna update result");
    if (selected == kOpenUpdate) {
        const std::wstring url = utf8ToWide(
            result.releaseNotesUrl.empty() ? result.downloadUrl : result.releaseNotesUrl);
        const INT_PTR launched = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (launched <= 32)
            throw std::runtime_error("cannot open the authenticated Zanna release URL");
    }
}

} // namespace zanna::installer
