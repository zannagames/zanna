//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/lsp-common/DocumentStore.cpp
// Purpose: Implementation of in-memory document storage for LSP.
// Key invariants:
//   - URI keys are stored verbatim (no normalization beyond uriToPath)
//   - uriToPath handles file:// prefix and %XX URL decoding
// Ownership/Lifetime:
//   - All data fully owned
// Links: tools/lsp-common/DocumentStore.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements open-document state and safe file-URI decoding.

#include "tools/lsp-common/DocumentStore.hpp"

#include <cctype>
#include <stdexcept>
#include <string_view>

namespace zanna::server {
namespace {

/// @brief Return true when @p text begins with a Windows drive-letter path prefix.
/// @param text URI-or-path text.
/// @return true for `C:`, `C:/...`, or `C:\...` syntax.
bool startsWithWindowsDrivePath(std::string_view text) {
    return text.size() >= 2 && std::isalpha(static_cast<unsigned char>(text[0])) &&
           text[1] == ':' && (text.size() == 2 || text[2] == '/' || text[2] == '\\');
}

/// @brief Return the URI scheme prefix length, or npos when no scheme is present.
/// @details Plain paths are allowed by the server; this helper only reports a scheme when the
/// leading token satisfies RFC 3986 scheme syntax and is not a Windows drive path.
/// @param text URI-or-path text.
/// @return Number of characters before `:`, or `npos` when no valid scheme exists.
std::size_t uriSchemeLength(std::string_view text) {
    if (startsWithWindowsDrivePath(text) || text.empty() ||
        !std::isalpha(static_cast<unsigned char>(text.front()))) {
        return std::string_view::npos;
    }
    for (std::size_t i = 1; i < text.size(); ++i) {
        const unsigned char uc = static_cast<unsigned char>(text[i]);
        if (text[i] == ':')
            return i;
        if (text[i] == '/' || text[i] == '\\' || text[i] == '?' || text[i] == '#')
            return std::string_view::npos;
        if (!std::isalnum(uc) && text[i] != '+' && text[i] != '-' && text[i] != '.')
            return std::string_view::npos;
    }
    return std::string_view::npos;
}

/// @brief Compare two ASCII tokens case-insensitively.
/// @details URI schemes are ASCII and case-insensitive. This helper is scoped to
///          protocol tokens and intentionally does not attempt locale-aware or
///          Unicode case folding.
/// @param lhs First ASCII token.
/// @param rhs Second ASCII token.
/// @return true when equal after ASCII case folding.
bool asciiEqualsIgnoreCase(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size())
        return false;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        const auto lc = static_cast<unsigned char>(lhs[i]);
        const auto rc = static_cast<unsigned char>(rhs[i]);
        if (std::tolower(lc) != std::tolower(rc))
            return false;
    }
    return true;
}

/// @brief Return true when @p text starts with a case-insensitive `file://` prefix.
/// @details `file`, like all URI schemes, is case-insensitive even though most
///          clients send it in lowercase.
/// @param text Candidate URI text.
/// @return true when the first seven characters equal `file://`.
bool startsWithFileUriPrefix(std::string_view text) {
    return text.size() >= 7 && asciiEqualsIgnoreCase(text.substr(0, 7), "file://");
}

/// @brief Return true when @p c is forbidden in a decoded filesystem path from the client.
/// @param c Decoded path byte.
/// @return true for C0 controls or DEL.
bool isForbiddenUriPathChar(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return uc < 0x20 || uc == 0x7F;
}

} // namespace

/// @brief Insert or replace one open document.
/// @param uri Verbatim document key.
/// @param documentVersion Client-provided version.
/// @param content Full text, moved into owned storage.
void DocumentStore::open(const std::string &uri, int documentVersion, std::string content) {
    docs_[uri] = {documentVersion, std::move(content)};
}

/// @brief Replace one document's version and content, opening it if absent.
/// @param uri Verbatim document key.
/// @param documentVersion Client-provided version.
/// @param content Full text, moved into owned storage.
void DocumentStore::update(const std::string &uri, int documentVersion, std::string content) {
    docs_[uri] = {documentVersion, std::move(content)};
}

/// @brief Stop tracking a document.
/// @param uri Verbatim document key; absence is a no-op.
void DocumentStore::close(const std::string &uri) {
    docs_.erase(uri);
}

/// @brief Retrieve immutable content for an open document.
/// @param uri Verbatim document key.
/// @return Pointer stable until the entry is mutated/removed or the map rehashes; null if absent.
const std::string *DocumentStore::getContent(const std::string &uri) const {
    auto it = docs_.find(uri);
    if (it == docs_.end())
        return nullptr;
    return &it->second.content;
}

/// @brief Test whether a URI is currently tracked.
/// @param uri Verbatim document key.
/// @return true when an entry exists.
bool DocumentStore::isOpen(const std::string &uri) const {
    return docs_.find(uri) != docs_.end();
}

/// @brief Retrieve the last client-provided version.
/// @param uri Verbatim document key.
/// @return Stored version, or `std::nullopt` if closed/unknown.
std::optional<int> DocumentStore::version(const std::string &uri) const {
    const auto it = docs_.find(uri);
    if (it == docs_.end())
        return std::nullopt;
    return it->second.version;
}

/// @brief Decode a plain path or `file://` URI without throwing.
/// @param uri Client-provided URI or plain path.
/// @param outPath Receives decoded path on success and is cleared initially.
/// @param err Optional destination for a human-readable failure reason.
/// @return true when syntax, scheme, authority, escapes, and path bytes are accepted.
bool DocumentStore::tryUriToPath(const std::string &uri, std::string &outPath, std::string *err) {
    outPath.clear();
    if (uri.empty()) {
        if (err)
            *err = "document URI must not be empty";
        return false;
    }

    std::string path;
    std::string_view sv = uri;
    const std::size_t schemeLen = uriSchemeLength(sv);
    const bool isUri = schemeLen != std::string_view::npos;
    if (schemeLen != std::string_view::npos) {
        if (!asciiEqualsIgnoreCase(sv.substr(0, schemeLen), "file")) {
            if (err)
                *err = "unsupported document URI scheme: " + uri.substr(0, schemeLen);
            return false;
        }
        if (!startsWithFileUriPrefix(sv)) {
            if (err)
                *err = "file URI must use file:// form: " + uri;
            return false;
        }
    }

    // Strip file:// prefix and handle the URI authority component.
    if (startsWithFileUriPrefix(sv)) {
        sv.remove_prefix(7);
        const std::size_t uriSuffix = sv.find_first_of("?#");
        if (uriSuffix != std::string_view::npos) {
            if (err)
                *err = "file URI query and fragment components are not supported: " + uri;
            return false;
        }
        std::string_view authority;
        if (!sv.empty() && sv.front() != '/') {
            const std::size_t slash = sv.find('/');
            authority = slash == std::string_view::npos ? sv : sv.substr(0, slash);
            sv = slash == std::string_view::npos ? std::string_view{} : sv.substr(slash);
        }

        if (!authority.empty() && !asciiEqualsIgnoreCase(authority, "localhost")) {
            path.append("//");
            path.append(authority);
            if (!sv.empty() && sv.front() != '/')
                path.push_back('/');
        } else if (sv.size() >= 3 && sv[0] == '/' && sv[2] == ':') {
            // Windows drive letter: /C:/path -> C:/path
            sv.remove_prefix(1);
        }
    }

    if (isUri) {
        const std::size_t suffix = sv.find_first_of("?#");
        if (suffix != std::string_view::npos) {
            if (err)
                *err = "file URI query and fragment components are not supported: " + uri;
            return false;
        }
    }

    // URL-decode %XX sequences. Encoded separators are rejected so percent
    // decoding cannot create extra path components after URI parsing.
    path.reserve(path.size() + sv.size());
    for (size_t i = 0; i < sv.size(); ++i) {
        if (sv[i] == '%') {
            if (i + 2 >= sv.size()) {
                if (err)
                    *err = "malformed percent escape in URI: " + uri;
                return false;
            }
            char hi = sv[i + 1];
            char lo = sv[i + 2];
            /// @brief Decode one hexadecimal URI-escape digit.
            /// @param c Character to decode.
            /// @return Nibble value, or `-1` when `c` is not hexadecimal.
            auto hexVal = [](char c) -> int {
                if (c >= '0' && c <= '9')
                    return c - '0';
                if (c >= 'a' && c <= 'f')
                    return c - 'a' + 10;
                if (c >= 'A' && c <= 'F')
                    return c - 'A' + 10;
                return -1;
            };
            int h = hexVal(hi);
            int l = hexVal(lo);
            if (h < 0 || l < 0) {
                if (err)
                    *err = "malformed percent escape in URI: " + uri;
                return false;
            }
            const char decoded = static_cast<char>((h << 4) | l);
            if (decoded == '/' || decoded == '\\') {
                if (err)
                    *err = "URI must not percent-encode path separators: " + uri;
                return false;
            }
            if (isForbiddenUriPathChar(decoded)) {
                if (err)
                    *err = "URI path must not contain decoded control characters: " + uri;
                return false;
            }
            path += decoded;
            i += 2;
            continue;
        }
        if (isUri && sv[i] == '\\') {
            if (err)
                *err = "file URI path must use forward slashes: " + uri;
            return false;
        }
        if (isForbiddenUriPathChar(sv[i])) {
            if (err)
                *err = "URI path must not contain control characters: " + uri;
            return false;
        }
        path += sv[i];
    }

    if (path.empty()) {
        if (err)
            *err = "document URI path must not be empty: " + uri;
        return false;
    }

    outPath = std::move(path);
    return true;
}

/// @brief Decode only strict `file://` document URIs without throwing.
/// @param uri Client-provided LSP document URI.
/// @param outPath Receives decoded path on success.
/// @param err Optional destination for a failure reason.
/// @return true when the required prefix and broader URI validation succeed.
bool DocumentStore::tryFileUriToPath(const std::string &uri,
                                     std::string &outPath,
                                     std::string *err) {
    if (!startsWithFileUriPrefix(uri)) {
        outPath.clear();
        if (err)
            *err = "LSP document URI must use file:// form: " + uri;
        return false;
    }
    return tryUriToPath(uri, outPath, err);
}

/// @brief Decode a plain path or file URI and throw on rejection.
/// @param uri Client-provided URI or plain path.
/// @return Decoded filesystem path.
/// @throws std::runtime_error With the same diagnostic produced by tryUriToPath().
std::string DocumentStore::uriToPath(const std::string &uri) {
    std::string path;
    std::string err;
    if (!tryUriToPath(uri, path, &err))
        throw std::runtime_error(err);
    return path;
}

} // namespace zanna::server
