//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements lifecycle dispatch, document synchronization, editor
///        features, and diagnostics for the shared LSP server.
///
/// The handler advertises full-document synchronization, converts between LSP
/// UTF-16 positions and compiler byte columns, and delegates language-specific
/// operations through ICompilerBridge. Returned JSON strings own their data;
/// diagnostics are published after document opens and accepted changes.
///
/// @see LspHandler.hpp
/// @see ICompilerBridge.hpp
//
//===----------------------------------------------------------------------===//

#include "tools/lsp-common/LspHandler.hpp"

#include "common/PlatformCapabilities.hpp"
#include "tools/lsp-common/Transport.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <system_error>

namespace zanna::server {
namespace {

/// @brief Return @p obj's member @p name only if it is itself an Object, else null.
/// @param obj Candidate containing JSON value.
/// @param name Null-terminated member name to locate.
/// @return Pointer to the object-valued member, or @c nullptr on absence or a
///         type mismatch.
const JsonValue *objectMember(const JsonValue &obj, const char *name) {
    if (obj.type() != JsonType::Object)
        return nullptr;
    const JsonValue *value = obj.get(name);
    return value && value->type() == JsonType::Object ? value : nullptr;
}

/// @brief Return @p obj's member @p name only if it is a String, else null.
/// @param obj Candidate containing JSON value.
/// @param name Null-terminated member name to locate.
/// @return Pointer to the string-valued member, or @c nullptr on absence or a
///         type mismatch.
const JsonValue *stringMember(const JsonValue &obj, const char *name) {
    if (obj.type() != JsonType::Object)
        return nullptr;
    const JsonValue *value = obj.get(name);
    return value && value->type() == JsonType::String ? value : nullptr;
}

/// @brief Return @p obj's member @p name only if it is a Bool, else null.
/// @param obj Candidate containing JSON value.
/// @param name Null-terminated member name to locate.
/// @return Pointer to the Boolean-valued member, or @c nullptr on absence or a
///         type mismatch.
const JsonValue *boolMember(const JsonValue &obj, const char *name) {
    if (obj.type() != JsonType::Object)
        return nullptr;
    const JsonValue *value = obj.get(name);
    return value && value->type() == JsonType::Bool ? value : nullptr;
}

/// @brief Return @p obj's member @p name only if it is an Int, else null.
/// @param obj Candidate containing JSON value.
/// @param name Null-terminated member name to locate.
/// @return Pointer to the integer-valued member, or @c nullptr on absence or a
///         type mismatch.
const JsonValue *intMember(const JsonValue &obj, const char *name) {
    if (obj.type() != JsonType::Object)
        return nullptr;
    const JsonValue *value = obj.get(name);
    return value && value->type() == JsonType::Int ? value : nullptr;
}

/// @brief Convert a JSON integer into an int while enforcing caller-provided bounds.
/// @details JSON-RPC integers are represented as int64_t internally, while the
///          compiler bridges accept int values. This helper prevents oversized
///          protocol values from wrapping during a narrowing cast.
/// @param value JSON value that must be an Int.
/// @param minValue Inclusive lower bound.
/// @param maxValue Inclusive upper bound.
/// @param out Receives the narrowed integer on success.
/// @return True when @p value is an integer inside the requested range.
bool checkedJsonIntToInt(const JsonValue *value, int minValue, int maxValue, int &out) {
    if (!value || value->type() != JsonType::Int)
        return false;
    const int64_t raw = value->asInt();
    if (raw < static_cast<int64_t>(minValue) || raw > static_cast<int64_t>(maxValue))
        return false;
    out = static_cast<int>(raw);
    return true;
}

/// @brief Extract `params.textDocument.uri` into @p uri.
/// @param params Request parameters expected to contain @c textDocument.uri.
/// @param uri Receives the nonempty URI on success.
/// @return true when a non-empty URI string is present.
bool extractTextDocumentUri(const JsonValue &params, std::string &uri) {
    const JsonValue *textDocument = objectMember(params, "textDocument");
    if (!textDocument)
        return false;
    const JsonValue *uriValue = stringMember(*textDocument, "uri");
    if (!uriValue)
        return false;
    uri = uriValue->asString();
    return !uri.empty();
}

/// @brief Extract optional `params.textDocument.version` into @p version.
/// @details Some LSP clients send null or omit versions for virtual or
///          best-effort documents. This helper treats that as an absent version
///          while still rejecting non-integer, non-null values.
/// @param params Request parameters that may contain @c textDocument.version.
/// @param version Receives the narrowed version, or is reset when it is absent.
/// @return true when the version is valid or absent; false when malformed.
bool extractOptionalTextDocumentVersion(const JsonValue &params, std::optional<int> &version) {
    version.reset();
    const JsonValue *textDocument = objectMember(params, "textDocument");
    const JsonValue *versionValue = textDocument ? textDocument->get("version") : nullptr;
    if (!versionValue || versionValue->isNull())
        return true;
    int parsed = 0;
    if (!checkedJsonIntToInt(
            versionValue, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), parsed))
        return false;
    version = parsed;
    return true;
}

/// @brief Extract `params.position` into 1-based @p line / @p col.
/// @details LSP positions are 0-based; this adds 1 to each so the values match
///          the frontend's 1-based line/column convention.
/// @param params Request parameters expected to contain an LSP position object.
/// @param line Receives the one-based line value on success.
/// @param col Receives the one-based UTF-16 character value on success.
/// @return true when both line and character are present and positive.
bool extractPosition(const JsonValue &params, int &line, int &col) {
    const JsonValue *position = objectMember(params, "position");
    const JsonValue *lineValue = position ? intMember(*position, "line") : nullptr;
    const JsonValue *colValue = position ? intMember(*position, "character") : nullptr;
    int zeroBasedLine = 0;
    int zeroBasedCol = 0;
    if (!checkedJsonIntToInt(lineValue, 0, std::numeric_limits<int>::max() - 1, zeroBasedLine) ||
        !checkedJsonIntToInt(colValue, 0, std::numeric_limits<int>::max() - 1, zeroBasedCol)) {
        return false;
    }
    line = zeroBasedLine + 1;
    col = zeroBasedCol + 1;
    return true;
}

/// @brief Build an LSP `Range` JSON object from 0-based start/end line/character.
/// @param startLine Zero-based first line.
/// @param startCharacter Zero-based UTF-16 offset on the first line.
/// @param endLine Zero-based exclusive last line.
/// @param endCharacter Zero-based exclusive UTF-16 offset on the last line.
/// @return JSON object containing LSP @c start and @c end positions.
JsonValue makeRange(int startLine, int startCharacter, int endLine, int endCharacter) {
    return JsonValue::object({
        {"start",
         JsonValue::object(
             {{"line", JsonValue(startLine)}, {"character", JsonValue(startCharacter)}})},
        {"end",
         JsonValue::object({{"line", JsonValue(endLine)}, {"character", JsonValue(endCharacter)}})},
    });
}

/// @brief Test whether a byte may be emitted literally in a file-URI path.
/// @param c Candidate path byte.
/// @return @c true for unreserved URI characters, slash, or colon.
static bool isUriPathChar(unsigned char c) {
    return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':';
}

/// @brief Percent-encode path bytes that are not safe in a file URI.
/// @param path Path text using URI-style separators.
/// @return Encoded path retaining unreserved characters, slashes, and colons.
static std::string encodeUriPath(std::string_view path) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (unsigned char c : path) {
        if (isUriPathChar(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4u]);
            out.push_back(kHex[c & 0x0Fu]);
        }
    }
    return out;
}

/// @brief Normalize a source path for filesystem identity comparisons.
/// @param path Native or generic filesystem path.
/// @return Absolute, lexically normalized generic path when possible; Windows
///         results are additionally folded to lowercase.
std::string comparableSourcePath(const std::string &path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path p(path);
    const fs::path absolute = fs::absolute(p, ec);
    if (!ec)
        p = absolute;
    std::string normalized = p.lexically_normal().generic_string();
#if ZANNA_HOST_WINDOWS
    /// @brief Fold one path byte to lowercase for Windows identity comparison.
    /// @param c Byte to normalize.
    /// @return Lowercase representation converted back to `char`.
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
#endif
    return normalized;
}

/// @brief Compare two source paths using direct and normalized spellings.
/// @param lhs First path.
/// @param rhs Second path.
/// @return @c true when the strings match or both normalize to the same path.
bool sameSourceFile(const std::string &lhs, const std::string &rhs) {
    return lhs == rhs ||
           (!lhs.empty() && !rhs.empty() && comparableSourcePath(lhs) == comparableSourcePath(rhs));
}

/// @brief Best-effort conversion from a filesystem path to a file URI.
/// @param path Existing file URI or native filesystem path to encode.
/// @return @p path unchanged when already a file URI; otherwise a percent-encoded
///         URI with UNC, drive-letter, absolute, or relative prefix handling.
std::string pathToFileUri(const std::string &path) {
    if (path.rfind("file://", 0) == 0)
        return path;
    if (path.empty())
        return "file:///";

    std::string uriPath = path;
    std::replace(uriPath.begin(), uriPath.end(), '\\', '/');
    const std::string encoded = encodeUriPath(uriPath);
    if (uriPath.size() >= 2 && uriPath[0] == '/' && uriPath[1] == '/')
        return "file:" + encoded;
    if (uriPath.size() >= 2 && std::isalpha(static_cast<unsigned char>(uriPath[0])) &&
        uriPath[1] == ':') {
        return "file:///" + encoded;
    }
    if (!uriPath.empty() && uriPath.front() == '/')
        return "file://" + encoded;
    return "file:///" + encoded;
}

/// @brief Decode the UTF-8 scalar at the front of @p bytes for LSP unit counting.
/// @details Invalid sequences are treated as one replacement character so ranges
///          remain monotonic even when a source buffer contains malformed UTF-8.
/// @param bytes Byte span beginning at the candidate UTF-8 lead byte.
/// @param consumed Receives the number of bytes consumed from @p bytes.
/// @return Number of UTF-16 code units represented by the decoded scalar.
int utf16UnitsForUtf8Lead(std::string_view bytes, size_t &consumed) {
    consumed = 1;
    if (bytes.empty())
        return 0;

    const auto b0 = static_cast<unsigned char>(bytes[0]);
    if (b0 < 0x80)
        return 1;

    /// @brief Test whether one byte is a UTF-8 continuation byte.
    /// @param c Byte to inspect.
    /// @return `true` when its two high bits are `10`.
    auto isContinuation = [](unsigned char c) { return (c & 0xC0u) == 0x80u; };

    uint32_t codePoint = 0;
    size_t expected = 0;
    if ((b0 & 0xE0u) == 0xC0u) {
        codePoint = b0 & 0x1Fu;
        expected = 2;
    } else if ((b0 & 0xF0u) == 0xE0u) {
        codePoint = b0 & 0x0Fu;
        expected = 3;
    } else if ((b0 & 0xF8u) == 0xF0u) {
        codePoint = b0 & 0x07u;
        expected = 4;
    } else {
        return 1;
    }

    if (bytes.size() < expected)
        return 1;
    for (size_t i = 1; i < expected; ++i) {
        const auto b = static_cast<unsigned char>(bytes[i]);
        if (!isContinuation(b))
            return 1;
        codePoint = (codePoint << 6u) | (b & 0x3Fu);
    }

    const bool overlong = (expected == 2 && codePoint < 0x80u) ||
                          (expected == 3 && codePoint < 0x800u) ||
                          (expected == 4 && codePoint < 0x10000u);
    const bool surrogate = codePoint >= 0xD800u && codePoint <= 0xDFFFu;
    if (overlong || surrogate || codePoint > 0x10FFFFu)
        return 1;

    consumed = expected;
    return codePoint > 0xFFFFu ? 2 : 1;
}

/// @brief Count LSP UTF-16 code units in the first @p byteCount bytes of @p text.
/// @details LSP character positions use UTF-16 code units, but compiler source
///          locations and string searches are byte based. The count is clamped
///          to the range of int for direct use in JSON range construction.
/// @param text UTF-8 source line or prefix.
/// @param byteCount Number of bytes from @p text to measure.
/// @return UTF-16 code unit count for the requested prefix.
int utf16CodeUnitsBeforeByte(std::string_view text, size_t byteCount) {
    const size_t limit = std::min(byteCount, text.size());
    int units = 0;
    for (size_t i = 0; i < limit;) {
        size_t consumed = 1;
        const int next = utf16UnitsForUtf8Lead(text.substr(i, limit - i), consumed);
        if (units > std::numeric_limits<int>::max() - next)
            return std::numeric_limits<int>::max();
        units += next;
        i += consumed;
    }
    return units;
}

/// @brief Convert a source byte offset to a zero-based LSP line/character pair.
/// @param content Full UTF-8 source buffer.
/// @param byteOffset Byte offset into @p content; values past EOF are clamped.
/// @param line Receives the zero-based line number.
/// @param character Receives the zero-based UTF-16 character offset.
void byteOffsetToLspPosition(const std::string &content,
                             size_t byteOffset,
                             int &line,
                             int &character) {
    const size_t limit = std::min(byteOffset, content.size());
    size_t lineStart = 0;
    line = 0;
    for (size_t i = 0; i < limit; ++i) {
        if (content[i] == '\n') {
            if (line < std::numeric_limits<int>::max())
                ++line;
            lineStart = i + 1;
        }
    }

    character = utf16CodeUnitsBeforeByte(
        std::string_view(content).substr(lineStart, limit - lineStart), limit - lineStart);
}

/// @brief Return true when @p c is an ASCII identifier byte used by the frontends.
/// @param c Candidate byte from an LSP rename target.
/// @return True when @p c is alphanumeric ASCII or underscore.
bool isIdentifierByte(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) || c == '_';
}

/// @brief Return true when @p c can start an ASCII identifier.
/// @param c Candidate first byte from an LSP rename target.
/// @return True when @p c is alphabetic ASCII or underscore.
bool isIdentifierStartByte(char c) {
    const auto uc = static_cast<unsigned char>(c);
    return std::isalpha(uc) || c == '_';
}

/// @brief Validate a rename target before asking the compiler bridge to edit.
/// @details LSP rename requests carry arbitrary strings. The current frontends
///          accept ASCII identifier names, so rejecting whitespace, operators,
///          empty names, and leading digits here gives clients deterministic
///          `InvalidParams` errors instead of backend-specific failures.
/// @param name Candidate replacement symbol name from the client request.
/// @return True when @p name is a non-empty ASCII identifier.
bool isValidRenameIdentifier(std::string_view name) {
    if (name.empty() || !isIdentifierStartByte(name.front()))
        return false;
    return std::all_of(name.begin(), name.end(), isIdentifierByte);
}

/// @brief Return true when @p pos can begin or end an identifier occurrence.
/// @details This treats the source as a byte string because compiler columns are
///          currently byte-based. Non-ASCII bytes therefore act as boundaries, which
///          is consistent with the current ASCII identifier scanner.
/// @param content Full source text.
/// @param pos Candidate byte offset.
/// @return @c true at buffer edges or when the preceding byte is not an
///         identifier byte.
bool isIdentifierBoundary(const std::string &content, size_t pos) {
    return pos == 0 || pos >= content.size() || !isIdentifierByte(content[pos - 1]);
}

/// @brief Find @p name within [begin,end) as a whole identifier token.
/// @details Plain substring search can select occurrences inside comments, strings, or longer
///          identifiers. This helper at least enforces ASCII identifier boundaries so fallback
///          document-symbol ranges do not point into unrelated words.
/// @param content Full source text to search.
/// @param name Identifier spelling to locate.
/// @param begin Inclusive first byte offset.
/// @param end Exclusive search boundary.
/// @return Byte offset of the first whole-token match, or @c std::nullopt.
std::optional<size_t> findIdentifierInRange(const std::string &content,
                                            const std::string &name,
                                            size_t begin,
                                            size_t end) {
    if (name.empty() || begin > end || begin >= content.size())
        return std::nullopt;
    end = std::min(end, content.size());
    size_t pos = content.find(name, begin);
    while (pos != std::string::npos && pos + name.size() <= end) {
        if (isIdentifierBoundary(content, pos) && (pos + name.size() == content.size() ||
                                                   !isIdentifierByte(content[pos + name.size()]))) {
            return pos;
        }
        pos = content.find(name, pos + 1);
    }
    return std::nullopt;
}

/// @brief Build an LSP `Range` from byte offsets in @p content.
/// @details Converts byte positions into zero-based LSP UTF-16 line/character positions.
/// @param content Full UTF-8 source buffer.
/// @param start Inclusive starting byte offset, clamped to the buffer.
/// @param end Exclusive ending byte offset, clamped after @p start.
/// @return LSP range spanning at least one byte when the buffer is nonempty.
JsonValue rangeForByteSpan(const std::string &content, size_t start, size_t end) {
    if (content.empty())
        return makeRange(0, 0, 0, 1);
    start = std::min(start, content.size());
    end = std::min(std::max(end, start + 1), content.size());
    int startLine = 0;
    int startCharacter = 0;
    int endLine = 0;
    int endCharacter = 0;
    byteOffsetToLspPosition(content, start, startLine, startCharacter);
    byteOffsetToLspPosition(content, end, endLine, endCharacter);
    return makeRange(startLine, startCharacter, endLine, endCharacter);
}

/// @brief Build an LSP `Range` covering a likely declaration occurrence of @p name.
/// @details Used only when semantic source coordinates are unavailable. It prefers
///          whole-token matches and otherwise falls back to a small range at the
///          top of the document so the result is deterministic instead of selecting
///          an arbitrary substring.
/// @param content Full source text to search.
/// @param name Expected declaration identifier.
/// @return Range for the first whole-token match, or a deterministic one-unit
///         range at the document start.
JsonValue rangeForFallbackName(const std::string &content, const std::string &name) {
    const auto pos = findIdentifierInRange(content, name, 0, content.size());
    if (!pos)
        return makeRange(0, 0, 0, 1);
    return rangeForByteSpan(content, *pos, *pos + name.size());
}

/// @brief Build an LSP range from a 1-based compiler source location.
/// @details The compiler location points at the declaration token. The helper clamps malformed
/// coordinates to the source line, verifies the expected symbol spelling when possible, and falls
/// back to a same-line whole-token search before using rangeForFallbackName().
/// @param content Full source text.
/// @param name Expected symbol spelling.
/// @param line One-based compiler line.
/// @param column One-based compiler byte column.
/// @return UTF-16-based LSP range covering the most likely symbol occurrence.
JsonValue rangeForSourceLocation(const std::string &content,
                                 const std::string &name,
                                 uint32_t line,
                                 uint32_t column) {
    if (line == 0 || column == 0)
        return rangeForFallbackName(content, name);

    std::size_t lineStart = 0;
    std::size_t currentLine = 1;
    while (currentLine < line && lineStart < content.size()) {
        const std::size_t next = content.find('\n', lineStart);
        if (next == std::string::npos)
            return rangeForFallbackName(content, name);
        lineStart = next + 1;
        ++currentLine;
    }

    std::size_t lineEnd = content.find('\n', lineStart);
    if (lineEnd == std::string::npos)
        lineEnd = content.size();

    std::size_t start = lineStart + static_cast<std::size_t>(column - 1);
    if (start > lineEnd)
        start = lineEnd;

    if (!name.empty() &&
        (start + name.size() > content.size() || content.compare(start, name.size(), name) != 0)) {
        if (const auto sameLine = findIdentifierInRange(content, name, lineStart, lineEnd))
            start = *sameLine;
    }

    const std::size_t end = std::min(content.size(), start + std::max<std::size_t>(name.size(), 1));
    return rangeForByteSpan(content, start, end);
}

/// @brief Find the byte span for an existing zero-based source line.
/// @param content Full source buffer.
/// @param zeroBasedLine Requested source line.
/// @param lineStart Receives the byte offset of the first byte in the line.
/// @param lineEnd Receives the byte offset one past the last byte before '\n'.
/// @return True when @p zeroBasedLine exists in @p content.
bool sourceLineByteSpan(const std::string &content,
                        int zeroBasedLine,
                        size_t &lineStart,
                        size_t &lineEnd) {
    if (zeroBasedLine < 0)
        return false;
    lineStart = 0;
    int line = 0;
    for (size_t i = 0; i < content.size() && line < zeroBasedLine; ++i) {
        if (content[i] == '\n') {
            ++line;
            lineStart = i + 1;
        }
    }
    if (line != zeroBasedLine)
        return false;
    lineEnd = lineStart;
    while (lineEnd < content.size() && content[lineEnd] != '\n')
        ++lineEnd;
    return true;
}

/// @brief Convert a one-based LSP position to one-based compiler byte coordinates.
/// @details LSP `character` values are UTF-16 code units, while the frontends currently consume
///          byte columns. The conversion walks the target source line as UTF-8, clamps past-line
///          positions to the line end, and returns byte columns suitable for hover/completion.
/// @param content Full document text.
/// @param lspOneBasedLine LSP line converted to one-based form.
/// @param lspOneBasedCharacter LSP UTF-16 character converted to one-based form.
/// @param compilerLine Receives the one-based source line.
/// @param compilerColumn Receives the one-based byte column.
/// @return True when the requested line exists in @p content.
bool lspPositionToCompilerPosition(const std::string &content,
                                   int lspOneBasedLine,
                                   int lspOneBasedCharacter,
                                   int &compilerLine,
                                   int &compilerColumn) {
    if (lspOneBasedLine <= 0 || lspOneBasedCharacter <= 0)
        return false;
    size_t lineStart = 0;
    size_t lineEnd = 0;
    const int zeroBasedLine = lspOneBasedLine - 1;
    if (!sourceLineByteSpan(content, zeroBasedLine, lineStart, lineEnd))
        return false;

    const int targetUnits = lspOneBasedCharacter - 1;
    int consumedUnits = 0;
    size_t byteOffset = lineStart;
    while (byteOffset < lineEnd && consumedUnits < targetUnits) {
        size_t consumedBytes = 1;
        const int units = utf16UnitsForUtf8Lead(
            std::string_view(content).substr(byteOffset, lineEnd - byteOffset), consumedBytes);
        if (consumedUnits + units > targetUnits)
            break;
        consumedUnits += units;
        byteOffset += std::min(consumedBytes, lineEnd - byteOffset);
    }

    compilerLine = lspOneBasedLine;
    compilerColumn = static_cast<int>(byteOffset - lineStart) + 1;
    return true;
}

/// @brief Convert a one-based compiler diagnostic location into an LSP range.
/// @details Diagnostic columns are interpreted as byte columns. The generated
///          range is converted to UTF-16 units and clamped to the target line. Identifier tokens
///          are expanded so diagnostics highlight the relevant word instead of a single code unit.
/// @param content Full document text.
/// @param oneBasedLine Compiler diagnostic line number.
/// @param oneBasedColumn Compiler diagnostic column number.
/// @return One-character LSP range for the diagnostic position.
JsonValue diagnosticRangeForLocation(const std::string &content,
                                     uint32_t oneBasedLine,
                                     uint32_t oneBasedColumn) {
    const int line =
        oneBasedLine > 0 && oneBasedLine <= static_cast<uint32_t>(std::numeric_limits<int>::max())
            ? static_cast<int>(oneBasedLine) - 1
            : 0;
    size_t lineStart = 0;
    size_t lineEnd = 0;
    if (!sourceLineByteSpan(content, line, lineStart, lineEnd))
        return makeRange(0, 0, 0, 1);

    const size_t byteColumn = oneBasedColumn > 0 ? static_cast<size_t>(oneBasedColumn - 1u) : 0u;
    const size_t byteOffset = std::min(lineStart + byteColumn, lineEnd);
    int lspLine = 0;
    int lspCol = 0;
    size_t tokenStart = byteOffset;
    size_t tokenEnd = byteOffset;
    if (tokenStart < lineEnd && isIdentifierByte(content[tokenStart])) {
        while (tokenStart > lineStart && isIdentifierByte(content[tokenStart - 1]))
            --tokenStart;
        while (tokenEnd < lineEnd && isIdentifierByte(content[tokenEnd]))
            ++tokenEnd;
    } else {
        tokenEnd = std::min(lineEnd, byteOffset + 1);
    }
    byteOffsetToLspPosition(content, tokenStart, lspLine, lspCol);
    int endLine = 0;
    int endCol = 0;
    byteOffsetToLspPosition(content, tokenEnd, endLine, endCol);
    return makeRange(lspLine, lspCol, endLine, endCol);
}

/// @brief Convert a 1-based compiler diagnostic span into an LSP range.
/// @details Used when the frontend supplied a concrete end position. Both
///          endpoints are interpreted as byte columns and converted to UTF-16
///          code units. Falls back to diagnosticRangeForLocation when the end
///          position is missing, malformed, or precedes the start.
/// @param content Full UTF-8 document text.
/// @param beginLine One-based starting compiler line.
/// @param beginColumn One-based starting compiler byte column.
/// @param endLine One-based exclusive ending compiler line.
/// @param endColumn One-based exclusive ending compiler byte column.
/// @return Converted LSP range, or a location-derived fallback range.
JsonValue diagnosticRangeForSpan(const std::string &content,
                                 uint32_t beginLine,
                                 uint32_t beginColumn,
                                 uint32_t endLine,
                                 uint32_t endColumn) {
    if (endLine == 0 || endColumn == 0 || endLine < beginLine ||
        (endLine == beginLine && endColumn <= beginColumn))
        return diagnosticRangeForLocation(content, beginLine, beginColumn);

    /// @brief Convert one compiler line/column pair to a bounded document byte offset.
    /// @param oneBasedLine One-based source line.
    /// @param oneBasedColumn One-based byte column.
    /// @param[out] offset Receives the clamped byte offset.
    /// @return `true` when the selected line exists.
    auto byteOffsetFor =
        [&content](uint32_t oneBasedLine, uint32_t oneBasedColumn, size_t &offset) -> bool {
        const int line = oneBasedLine > 0 && oneBasedLine <= static_cast<uint32_t>(
                                                                 std::numeric_limits<int>::max())
                             ? static_cast<int>(oneBasedLine) - 1
                             : 0;
        size_t lineStart = 0;
        size_t lineEnd = 0;
        if (!sourceLineByteSpan(content, line, lineStart, lineEnd))
            return false;
        const size_t byteColumn =
            oneBasedColumn > 0 ? static_cast<size_t>(oneBasedColumn - 1u) : 0u;
        offset = std::min(lineStart + byteColumn, lineEnd);
        return true;
    };

    size_t beginOffset = 0;
    size_t endOffset = 0;
    if (!byteOffsetFor(beginLine, beginColumn, beginOffset) ||
        !byteOffsetFor(endLine, endColumn, endOffset) || endOffset < beginOffset)
        return diagnosticRangeForLocation(content, beginLine, beginColumn);

    int startLspLine = 0;
    int startLspCol = 0;
    int endLspLine = 0;
    int endLspCol = 0;
    byteOffsetToLspPosition(content, beginOffset, startLspLine, startLspCol);
    byteOffsetToLspPosition(content, endOffset, endLspLine, endLspCol);
    return makeRange(startLspLine, startLspCol, endLspLine, endLspCol);
}

/// @brief Convert a validated compiler source span to LSP UTF-16 coordinates.
/// @param content Full UTF-8 document text.
/// @param beginLine One-based starting compiler line.
/// @param beginColumn One-based starting compiler byte column.
/// @param endLine One-based exclusive ending compiler line.
/// @param endColumn One-based exclusive ending compiler byte column.
/// @param startLspLine Receives the zero-based starting line.
/// @param startLspCol Receives the zero-based starting UTF-16 offset.
/// @param endLspLine Receives the zero-based ending line.
/// @param endLspCol Receives the zero-based ending UTF-16 offset.
/// @return @c true when both endpoints are ordered and refer to existing lines.
bool sourceRangeToLspSpan(const std::string &content,
                          uint32_t beginLine,
                          uint32_t beginColumn,
                          uint32_t endLine,
                          uint32_t endColumn,
                          int &startLspLine,
                          int &startLspCol,
                          int &endLspLine,
                          int &endLspCol) {
    if (beginLine == 0 || beginColumn == 0 || endLine == 0 || endColumn == 0 ||
        endLine < beginLine || (endLine == beginLine && endColumn <= beginColumn)) {
        return false;
    }

    /// @brief Convert one validated compiler line/column pair to a document byte offset.
    /// @param oneBasedLine One-based source line.
    /// @param oneBasedColumn One-based byte column.
    /// @param[out] offset Receives the clamped byte offset.
    /// @return `true` when the line number is representable and exists.
    auto byteOffsetFor =
        [&content](uint32_t oneBasedLine, uint32_t oneBasedColumn, size_t &offset) -> bool {
        if (oneBasedLine == 0 ||
            oneBasedLine > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
            return false;
        }
        size_t lineStart = 0;
        size_t lineEnd = 0;
        if (!sourceLineByteSpan(content, static_cast<int>(oneBasedLine) - 1, lineStart, lineEnd))
            return false;
        const size_t byteColumn =
            oneBasedColumn > 0 ? static_cast<size_t>(oneBasedColumn - 1u) : 0u;
        offset = std::min(lineStart + byteColumn, lineEnd);
        return true;
    };

    size_t beginOffset = 0;
    size_t endOffset = 0;
    if (!byteOffsetFor(beginLine, beginColumn, beginOffset) ||
        !byteOffsetFor(endLine, endColumn, endOffset) || endOffset < beginOffset) {
        return false;
    }

    byteOffsetToLspPosition(content, beginOffset, startLspLine, startLspCol);
    byteOffsetToLspPosition(content, endOffset, endLspLine, endLspCol);
    return true;
}

/// @brief Convert bridge source-range metadata to an LSP range object.
/// @param range One-based compiler range and source-file identity.
/// @param content Optional current-document contents for exact UTF-16 conversion.
/// @param contentPath Filesystem path associated with @p content.
/// @return Exact current-document range when possible, otherwise a coordinate-only
///         range produced from the supplied one-based values.
JsonValue rangeForSourceRange(const SourceRangeInfo &range,
                              const std::string *content,
                              const std::string &contentPath) {
    if (content && sameSourceFile(range.file, contentPath))
        return diagnosticRangeForSpan(
            *content, range.line, range.column, range.endLine, range.endColumn);

    const int startLine =
        range.line > 0 && range.line <= static_cast<uint32_t>(std::numeric_limits<int>::max())
            ? static_cast<int>(range.line) - 1
            : 0;
    const int startCol =
        range.column > 0 && range.column <= static_cast<uint32_t>(std::numeric_limits<int>::max())
            ? static_cast<int>(range.column) - 1
            : 0;
    const int endLine =
        range.endLine > 0 && range.endLine <= static_cast<uint32_t>(std::numeric_limits<int>::max())
            ? static_cast<int>(range.endLine) - 1
            : startLine;
    const int endCol =
        range.endColumn > 0 &&
                range.endColumn <= static_cast<uint32_t>(std::numeric_limits<int>::max())
            ? static_cast<int>(range.endColumn) - 1
            : startCol + 1;
    return makeRange(startLine, startCol, endLine, std::max(endCol, startCol + 1));
}

/// @brief Select the client URI corresponding to a bridge-reported source file.
/// @param file Source path reported by the compiler bridge.
/// @param contentPath Path of the currently open document.
/// @param contentUri Original client URI for the current document.
/// @return @p contentUri for the same source file, otherwise a generated file URI.
std::string uriForSourceFile(const std::string &file,
                             const std::string &contentPath,
                             const std::string &contentUri) {
    if (!contentUri.empty() && sameSourceFile(file, contentPath))
        return contentUri;
    return pathToFileUri(file);
}

/// @brief Convert a compiler-bridge location to an LSP Location object.
/// @param location Source file and range to encode.
/// @param content Optional current-document contents used for exact conversion.
/// @param contentPath Filesystem path associated with @p content.
/// @param contentUri Original URI associated with @p content.
/// @return JSON object containing the selected @c uri and converted @c range.
JsonValue locationToJson(const LocationInfo &location,
                         const std::string *content,
                         const std::string &contentPath,
                         const std::string &contentUri) {
    return JsonValue::object({
        {"uri", JsonValue(uriForSourceFile(location.range.file, contentPath, contentUri))},
        {"range", rangeForSourceRange(location.range, content, contentPath)},
    });
}

/// @brief Wrap signature documentation as LSP Markdown markup.
/// @param documentation Markdown text supplied by the compiler bridge.
/// @return MarkupContent object, or JSON null when the text is empty.
JsonValue signatureDocumentationJson(const std::string &documentation) {
    if (documentation.empty())
        return JsonValue();
    return JsonValue::object({
        {"kind", JsonValue("markdown")},
        {"value", JsonValue(documentation)},
    });
}

/// @brief Convert bridge signature-help data to the LSP wire representation.
/// @param help Available signatures, parameter descriptions, and active indexes.
/// @return JSON SignatureHelp object preserving signature and parameter order.
JsonValue signatureHelpToJson(const SignatureHelpInfo &help) {
    JsonValue::ArrayType signatures;
    signatures.reserve(help.signatures.size());
    for (const auto &signature : help.signatures) {
        JsonValue::ArrayType params;
        params.reserve(signature.parameters.size());
        for (const auto &param : signature.parameters) {
            JsonValue::ObjectType paramObj;
            paramObj.push_back({"label", JsonValue(param.label)});
            if (!param.documentation.empty())
                paramObj.push_back(
                    {"documentation", signatureDocumentationJson(param.documentation)});
            params.push_back(JsonValue(std::move(paramObj)));
        }

        JsonValue::ObjectType signatureObj;
        signatureObj.push_back({"label", JsonValue(signature.label)});
        if (!signature.documentation.empty()) {
            signatureObj.push_back(
                {"documentation", signatureDocumentationJson(signature.documentation)});
        }
        signatureObj.push_back({"parameters", JsonValue(std::move(params))});
        signatures.push_back(JsonValue(std::move(signatureObj)));
    }

    return JsonValue::object({
        {"signatures", JsonValue(std::move(signatures))},
        {"activeSignature", JsonValue(help.activeSignature)},
        {"activeParameter", JsonValue(help.activeParameter)},
    });
}

/// @brief Construct the semantic-token type legend advertised during initialize.
/// @return Ordered token-type strings whose indices match SemanticTokenType.
JsonValue semanticTokenTypesLegend() {
    return JsonValue::array({
        JsonValue("namespace"),
        JsonValue("type"),
        JsonValue("class"),
        JsonValue("enum"),
        JsonValue("interface"),
        JsonValue("function"),
        JsonValue("method"),
        JsonValue("variable"),
        JsonValue("parameter"),
        JsonValue("property"),
        JsonValue("keyword"),
        JsonValue("number"),
        JsonValue("string"),
        JsonValue("operator"),
    });
}

/// @brief Return true for methods that are request/response-shaped in LSP.
/// @details Notifications must not receive responses. Dispatch uses this helper
///          to suppress accidental responses for missing-id calls to request
///          methods such as initialize, shutdown, completion, hover, and symbols.
/// @param method Incoming LSP method name.
/// @return @c true when the method has request/response semantics.
bool isRequestMethod(const std::string &method) {
    return method == "initialize" || method == "shutdown" || method == "textDocument/completion" ||
           method == "textDocument/hover" || method == "textDocument/documentSymbol" ||
           method == "textDocument/definition" || method == "textDocument/references" ||
           method == "textDocument/rename" || method == "textDocument/signatureHelp" ||
           method == "workspace/symbol" || method == "textDocument/semanticTokens/full";
}

} // namespace

/// @brief Construct a shared LSP protocol handler.
/// @param bridge Language-specific compiler adapter borrowed for the handler lifetime.
/// @param transport Message transport borrowed for the handler lifetime.
/// @param config Language-specific server strings copied into owned storage.
LspHandler::LspHandler(ICompilerBridge &bridge, Transport &transport, const ServerConfig &config)
    : bridge_(bridge), transport_(transport), config_(config) {}

// --- Request dispatch ---

/// @brief Dispatch one validated JSON-RPC message through the LSP state machine.
/// @param req Parsed request or notification.
/// @return Serialized response for requests, or an empty string for notifications
///         and the process-level @c exit message.
/// @details Enforces initialize/shutdown ordering, ignores request-shaped methods
///          sent without IDs, and reports unknown requests as method-not-found.
std::string LspHandler::handleRequest(const JsonRpcRequest &req) {
    if (req.method == "exit")
        return {}; // Main loop handles process exit.

    if (shutdownRequested_) {
        if (req.isNotification())
            return {};
        return buildError(req.id, kInvalidRequest, "Server is shutting down");
    }

    if (req.isNotification() && isRequestMethod(req.method))
        return {};

    if (req.method == "initialize")
        return handleInitialize(req);

    if (req.method == "initialized") {
        if (!initializeResponded_) {
            logMessage(2, "Ignoring initialized notification before initialize");
            return {};
        }
        clientInitialized_ = true;
        return {}; // Notification
    }

    if (req.method == "shutdown")
        return handleShutdown(req);

    if (!clientInitialized_) {
        if (req.isNotification()) {
            logMessage(2,
                       "Ignoring notification before LSP initialization completed: " + req.method);
            return {};
        }
        return buildError(req.id, kInvalidRequest, "Server has not been initialized");
    }

    // Document sync notifications
    if (req.method == "textDocument/didOpen") {
        handleDidOpen(req);
        return {};
    }
    if (req.method == "textDocument/didChange") {
        handleDidChange(req);
        return {};
    }
    if (req.method == "textDocument/didClose") {
        handleDidClose(req);
        return {};
    }

    // Requests
    if (req.method == "textDocument/completion")
        return handleCompletion(req);

    if (req.method == "textDocument/hover")
        return handleHover(req);

    if (req.method == "textDocument/documentSymbol")
        return handleDocumentSymbol(req);

    if (req.method == "textDocument/definition")
        return handleDefinition(req);

    if (req.method == "textDocument/references")
        return handleReferences(req);

    if (req.method == "textDocument/rename")
        return handleRename(req);

    if (req.method == "textDocument/signatureHelp")
        return handleSignatureHelp(req);

    if (req.method == "workspace/symbol")
        return handleWorkspaceSymbol(req);

    if (req.method == "textDocument/semanticTokens/full")
        return handleSemanticTokensFull(req);

    if (req.isNotification())
        return {}; // Unknown notification — silently ignore

    return buildError(req.id, kMethodNotFound, "Method not found: " + req.method);
}

// --- Lifecycle ---

/// @brief Handle the LSP @c initialize request and advertise bridge capabilities.
/// @param req Initialize request whose identifier is echoed in the response.
/// @return Success response containing server information and capability flags,
///         or invalid-request when initialization already occurred.
std::string LspHandler::handleInitialize(const JsonRpcRequest &req) {
    if (initializeResponded_)
        return buildError(req.id, kInvalidRequest, "Server has already been initialized");

    JsonValue::ObjectType capabilityMembers{
        {"textDocumentSync", JsonValue(1)}, // Full sync
        {"completionProvider",
         JsonValue::object({
             {"triggerCharacters", JsonValue::array({JsonValue(".")})},
         })},
        {"hoverProvider", JsonValue(true)},
        {"documentSymbolProvider", JsonValue(true)},
    };

    if (bridge_.supportsDefinition())
        capabilityMembers.push_back({"definitionProvider", JsonValue(true)});
    if (bridge_.supportsReferences())
        capabilityMembers.push_back({"referencesProvider", JsonValue(true)});
    if (bridge_.supportsRename())
        capabilityMembers.push_back({"renameProvider", JsonValue(true)});
    if (bridge_.supportsSignatureHelp()) {
        capabilityMembers.push_back(
            {"signatureHelpProvider",
             JsonValue::object({
                 {"triggerCharacters", JsonValue::array({JsonValue("("), JsonValue(",")})},
             })});
    }
    if (bridge_.supportsWorkspaceSymbols())
        capabilityMembers.push_back({"workspaceSymbolProvider", JsonValue(true)});
    if (bridge_.supportsSemanticTokens()) {
        capabilityMembers.push_back({"semanticTokensProvider",
                                     JsonValue::object({
                                         {"legend",
                                          JsonValue::object({
                                              {"tokenTypes", semanticTokenTypesLegend()},
                                              {"tokenModifiers", JsonValue::array({})},
                                          })},
                                         {"full", JsonValue(true)},
                                         {"range", JsonValue(false)},
                                     })});
    }

    auto capabilities = JsonValue(std::move(capabilityMembers));

    auto result = JsonValue::object({
        {"capabilities", std::move(capabilities)},
        {"serverInfo",
         JsonValue::object(
             {{"name", JsonValue(config_.serverName)}, {"version", JsonValue(config_.version)}})},
    });

    initializeResponded_ = true;
    return buildResponse(req.id, result);
}

/// @brief Handle the LSP @c shutdown request.
/// @param req Shutdown request whose identifier is echoed.
/// @return Null success response after recording the shutdown state.
std::string LspHandler::handleShutdown(const JsonRpcRequest &req) {
    shutdownRequested_ = true;
    return buildResponse(req.id, JsonValue());
}

/// @brief Emit an LSP log-message notification without affecting request flow.
/// @details Notification handlers cannot return JSON-RPC errors, so malformed client notifications
///          are surfaced through the editor log. Transport write failures are handled by the
///          transport layer in the same way as diagnostic notifications.
/// @param type LSP MessageType integer: error, warning, info, or log.
/// @param message Human-readable client log text.
void LspHandler::logMessage(int type, const std::string &message) {
    auto params = JsonValue::object({
        {"type", JsonValue(static_cast<int64_t>(type))},
        {"message", JsonValue(message)},
    });
    try {
        transport_.writeMessage(buildNotification("window/logMessage", params));
    } catch (const std::exception &) {
        // Logging must not fail the request/notification that triggered it.
    }
}

// --- Document sync ---

/// @brief Accept a full-text @c textDocument/didOpen notification.
/// @param req Notification containing URI, text, and optional integer version.
/// @details Valid documents are registered with both the bridge and store before
///          diagnostics are published; malformed notifications are logged.
void LspHandler::handleDidOpen(const JsonRpcRequest &req) {
    const auto *textDoc = objectMember(req.params, "textDocument");
    const auto *uriValue = textDoc ? stringMember(*textDoc, "uri") : nullptr;
    const auto *textValue = textDoc ? stringMember(*textDoc, "text") : nullptr;
    std::optional<int> clientVersion;
    if (!uriValue || !textValue || !extractOptionalTextDocumentVersion(req.params, clientVersion)) {
        logMessage(2, "Ignoring malformed textDocument/didOpen notification");
        return;
    }
    std::string uri = uriValue->asString();
    int version = clientVersion.value_or(0);
    std::string text = textValue->asString();

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr)) {
        logMessage(2, pathErr);
        return;
    }
    (void)path;

    bridge_.updateDocument(path, text);
    store_.open(uri, version, std::move(text));
    publishDiagnostics(uri);
}

/// @brief Apply a full-text @c textDocument/didChange notification.
/// @param req Notification containing one complete content change and optional version.
/// @details Rejects unopened documents, stale versions, incremental ranges, and
///          multiple change entries, then republishes diagnostics after an update.
void LspHandler::handleDidChange(const JsonRpcRequest &req) {
    std::string uri;
    std::optional<int> clientVersion;
    if (!extractTextDocumentUri(req.params, uri) ||
        !extractOptionalTextDocumentVersion(req.params, clientVersion)) {
        logMessage(2, "Ignoring malformed textDocument/didChange notification");
        return;
    }
    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr)) {
        logMessage(2, pathErr);
        return;
    }
    (void)path;
    const auto currentVersion = store_.version(uri);
    if (!currentVersion) {
        logMessage(2, "Ignoring textDocument/didChange for unopened document: " + uri);
        return;
    }
    const int version = clientVersion.value_or(
        *currentVersion == std::numeric_limits<int>::max() ? *currentVersion : *currentVersion + 1);
    if (clientVersion && version <= *currentVersion) {
        logMessage(4,
                   "Ignoring stale textDocument/didChange for " + uri + " version " +
                       std::to_string(version));
        return;
    }

    // Full sync: every accepted content change must be a complete document body.
    const auto &changes = req.params["contentChanges"];
    if (changes.type() != JsonType::Array || changes.size() == 0) {
        logMessage(2, "Ignoring textDocument/didChange without full document content");
        return;
    }
    if (changes.size() != 1) {
        logMessage(2, "Ignoring textDocument/didChange with multiple full-sync content items");
        return;
    }
    const auto &change = changes.at(0);
    if (change.type() != JsonType::Object || change.has("range") || change.has("rangeLength")) {
        logMessage(2,
                   "Ignoring incremental textDocument/didChange; server only supports full sync");
        return;
    }
    const auto *textValue = stringMember(change, "text");
    if (!textValue) {
        logMessage(2, "Ignoring textDocument/didChange content item without text");
        return;
    }
    std::string text = textValue->asString();
    bridge_.updateDocument(path, text);
    store_.update(uri, version, std::move(text));

    publishDiagnostics(uri);
}

/// @brief Remove a document after @c textDocument/didClose.
/// @param req Notification containing the document URI.
/// @details Removes bridge/store state and sends an empty diagnostic set so the
///          client clears previously published problems.
void LspHandler::handleDidClose(const JsonRpcRequest &req) {
    std::string uri;
    if (!extractTextDocumentUri(req.params, uri))
        return;
    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr)) {
        logMessage(2, pathErr);
        return;
    }
    bridge_.removeDocument(path);
    store_.close(uri);

    // Clear diagnostics for the closed document
    auto params = JsonValue::object({
        {"uri", JsonValue(uri)},
        {"diagnostics", JsonValue::array({})},
    });
    try {
        transport_.writeMessage(buildNotification("textDocument/publishDiagnostics", params));
    } catch (const std::exception &) {
        // Diagnostic clearing is best-effort during shutdown/connection loss.
    }
}

// --- Completion ---

/// @brief Produce completion items for @c textDocument/completion.
/// @param req Request containing an open document URI and LSP position.
/// @return Completion-array response, an empty array for an unknown document,
///         or an invalid-parameters response for malformed coordinates or URIs.
std::string LspHandler::handleCompletion(const JsonRpcRequest &req) {
    std::string uri;
    int line = 0;
    int col = 0;
    if (!extractTextDocumentUri(req.params, uri) || !extractPosition(req.params, line, col))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/completion params");

    const std::string *content = store_.getContent(uri);
    if (!content)
        return buildResponse(req.id, JsonValue::array({}));

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr))
        return buildError(req.id, kInvalidParams, pathErr);
    int compilerLine = 0;
    int compilerCol = 0;
    if (!lspPositionToCompilerPosition(*content, line, col, compilerLine, compilerCol))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/completion position");
    auto items = bridge_.completions(*content, compilerLine, compilerCol, path);

    JsonValue::ArrayType arr;
    arr.reserve(items.size());
    for (const auto &item : items) {
        char sortBuf[32];
        std::snprintf(sortBuf, sizeof(sortBuf), "%08d", item.sortPriority);
        JsonValue::ObjectType completionMembers{
            {"label", JsonValue(item.label)},
            {"insertText", JsonValue(item.insertText)},
            {"kind", JsonValue(completionKindToLsp(item.kind))},
            {"detail", JsonValue(item.detail)},
            {"sortText", JsonValue(sortBuf)},
        };
        if (!item.documentation.empty()) {
            completionMembers.emplace_back(
                "documentation",
                JsonValue::object(
                    {{"kind", JsonValue("markdown")}, {"value", JsonValue(item.documentation)}}));
        }
        arr.emplace_back(JsonValue(std::move(completionMembers)));
    }

    return buildResponse(req.id, JsonValue(std::move(arr)));
}

// --- Hover ---

/// @brief Produce Markdown hover information for @c textDocument/hover.
/// @param req Request containing an open document URI and LSP position.
/// @return Hover response, null result when no information is available, or an
///         invalid-parameters response for malformed input.
std::string LspHandler::handleHover(const JsonRpcRequest &req) {
    std::string uri;
    int line = 0;
    int col = 0;
    if (!extractTextDocumentUri(req.params, uri) || !extractPosition(req.params, line, col))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/hover params");

    const std::string *content = store_.getContent(uri);
    if (!content)
        return buildResponse(req.id, JsonValue());

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr))
        return buildError(req.id, kInvalidParams, pathErr);
    int compilerLine = 0;
    int compilerCol = 0;
    if (!lspPositionToCompilerPosition(*content, line, col, compilerLine, compilerCol))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/hover position");
    auto result = bridge_.hover(*content, compilerLine, compilerCol, path);

    if (result.empty())
        return buildResponse(req.id, JsonValue());

    auto hover = JsonValue::object({
        {"contents",
         JsonValue::object({
             {"kind", JsonValue("markdown")},
             {"value", JsonValue(result)},
         })},
    });
    return buildResponse(req.id, hover);
}

// --- Document Symbols ---

/// @brief List symbols for @c textDocument/documentSymbol.
/// @param req Request containing the target document URI.
/// @return Symbol-information array with LSP kinds and UTF-16 declaration ranges,
///         or an error response for malformed parameters.
std::string LspHandler::handleDocumentSymbol(const JsonRpcRequest &req) {
    std::string uri;
    if (!extractTextDocumentUri(req.params, uri))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/documentSymbol params");

    const std::string *content = store_.getContent(uri);
    if (!content)
        return buildResponse(req.id, JsonValue::array({}));

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr))
        return buildError(req.id, kInvalidParams, pathErr);
    auto syms = bridge_.symbols(*content, path);

    JsonValue::ArrayType arr;
    arr.reserve(syms.size());
    for (const auto &s : syms) {
        arr.push_back(JsonValue::object({
            {"name", JsonValue(s.name)},
            {"kind", JsonValue(symbolKindToLsp(s.kind))},
            {"location",
             JsonValue::object({
                 {"uri", JsonValue(uri)},
                 {"range", rangeForSourceLocation(*content, s.name, s.line, s.column)},
             })},
        }));
    }

    return buildResponse(req.id, JsonValue(std::move(arr)));
}

// --- Definition ---

/// @brief Resolve @c textDocument/definition through the compiler bridge.
/// @param req Request containing an open document URI and LSP position.
/// @return Location response, null when unsupported or unresolved, or an
///         invalid-parameters response for malformed input.
std::string LspHandler::handleDefinition(const JsonRpcRequest &req) {
    std::string uri;
    int line = 0;
    int col = 0;
    if (!extractTextDocumentUri(req.params, uri) || !extractPosition(req.params, line, col))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/definition params");

    const std::string *content = store_.getContent(uri);
    if (!content || !bridge_.supportsDefinition())
        return buildResponse(req.id, JsonValue());

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr))
        return buildError(req.id, kInvalidParams, pathErr);

    int compilerLine = 0;
    int compilerCol = 0;
    if (!lspPositionToCompilerPosition(*content, line, col, compilerLine, compilerCol))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/definition position");

    auto location = bridge_.definition(*content, compilerLine, compilerCol, path);
    if (!location)
        return buildResponse(req.id, JsonValue());
    return buildResponse(req.id, locationToJson(*location, content, path, uri));
}

// --- References ---

/// @brief Resolve @c textDocument/references through the compiler bridge.
/// @param req Request containing a document position and optional declaration filter.
/// @return Array of converted locations, or an error response for malformed input.
std::string LspHandler::handleReferences(const JsonRpcRequest &req) {
    std::string uri;
    int line = 0;
    int col = 0;
    if (!extractTextDocumentUri(req.params, uri) || !extractPosition(req.params, line, col))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/references params");

    const std::string *content = store_.getContent(uri);
    if (!content || !bridge_.supportsReferences())
        return buildResponse(req.id, JsonValue::array({}));

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr))
        return buildError(req.id, kInvalidParams, pathErr);

    int compilerLine = 0;
    int compilerCol = 0;
    if (!lspPositionToCompilerPosition(*content, line, col, compilerLine, compilerCol))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/references position");

    bool includeDeclaration = true;
    if (const auto *context = objectMember(req.params, "context")) {
        if (const auto *includeValue = boolMember(*context, "includeDeclaration"))
            includeDeclaration = includeValue->asBool(true);
    }

    auto refs = bridge_.references(*content, compilerLine, compilerCol, path);
    JsonValue::ArrayType arr;
    arr.reserve(refs.size());
    for (const auto &ref : refs) {
        if (!includeDeclaration && ref.isDefinition)
            continue;
        arr.push_back(locationToJson(ref, content, path, uri));
    }
    return buildResponse(req.id, JsonValue(std::move(arr)));
}

// --- Rename ---

/// @brief Build a workspace edit for @c textDocument/rename.
/// @param req Request containing a document position and valid replacement identifier.
/// @return WorkspaceEdit response grouped by source URI, null when unsupported,
///         or a protocol error describing invalid input or bridge failure.
std::string LspHandler::handleRename(const JsonRpcRequest &req) {
    std::string uri;
    int line = 0;
    int col = 0;
    const auto *newName = stringMember(req.params, "newName");
    if (!newName || !extractTextDocumentUri(req.params, uri) ||
        !extractPosition(req.params, line, col)) {
        return buildError(req.id, kInvalidParams, "Invalid textDocument/rename params");
    }
    if (!isValidRenameIdentifier(newName->asString()))
        return buildError(req.id, kInvalidParams, "Rename target must be an identifier");

    const std::string *content = store_.getContent(uri);
    if (!content || !bridge_.supportsRename())
        return buildResponse(req.id, JsonValue());

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr))
        return buildError(req.id, kInvalidParams, pathErr);

    int compilerLine = 0;
    int compilerCol = 0;
    if (!lspPositionToCompilerPosition(*content, line, col, compilerLine, compilerCol))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/rename position");

    RenameResult result =
        bridge_.rename(*content, compilerLine, compilerCol, path, newName->asString());
    if (!result.success)
        return buildError(req.id, kInvalidRequest, "Rename failed: " + result.reason);

    std::map<std::string, JsonValue::ArrayType> editsByUri;
    for (const auto &edit : result.edits) {
        editsByUri[uriForSourceFile(edit.range.file, path, uri)].push_back(JsonValue::object({
            {"range", rangeForSourceRange(edit.range, content, path)},
            {"newText", JsonValue(edit.newText)},
        }));
    }

    JsonValue::ObjectType changes;
    for (auto &[editUri, edits] : editsByUri)
        changes.push_back({std::move(editUri), JsonValue(std::move(edits))});

    return buildResponse(req.id, JsonValue::object({{"changes", JsonValue(std::move(changes))}}));
}

// --- Signature Help ---

/// @brief Produce call information for @c textDocument/signatureHelp.
/// @param req Request containing an open document URI and LSP position.
/// @return SignatureHelp response, null when unavailable, or an invalid-parameters
///         response for malformed input.
std::string LspHandler::handleSignatureHelp(const JsonRpcRequest &req) {
    std::string uri;
    int line = 0;
    int col = 0;
    if (!extractTextDocumentUri(req.params, uri) || !extractPosition(req.params, line, col))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/signatureHelp params");

    const std::string *content = store_.getContent(uri);
    if (!content || !bridge_.supportsSignatureHelp())
        return buildResponse(req.id, JsonValue());

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr))
        return buildError(req.id, kInvalidParams, pathErr);

    int compilerLine = 0;
    int compilerCol = 0;
    if (!lspPositionToCompilerPosition(*content, line, col, compilerLine, compilerCol))
        return buildError(req.id, kInvalidParams, "Invalid textDocument/signatureHelp position");

    SignatureHelpInfo help = bridge_.signatureHelp(*content, compilerLine, compilerCol, path);
    if (!help.available || help.signatures.empty())
        return buildResponse(req.id, JsonValue());
    return buildResponse(req.id, signatureHelpToJson(help));
}

// --- Workspace Symbols ---

/// @brief Search bridge-indexed symbols for @c workspace/symbol.
/// @param req Request containing the string search query.
/// @return Array of symbol locations, an empty array when unsupported, or an
///         invalid-parameters response when the query is missing.
std::string LspHandler::handleWorkspaceSymbol(const JsonRpcRequest &req) {
    const auto *queryValue = stringMember(req.params, "query");
    if (!queryValue)
        return buildError(req.id, kInvalidParams, "Invalid workspace/symbol params");
    if (!bridge_.supportsWorkspaceSymbols())
        return buildResponse(req.id, JsonValue::array({}));

    auto symbols = bridge_.workspaceSymbols(queryValue->asString());
    JsonValue::ArrayType arr;
    arr.reserve(symbols.size());
    for (const auto &symbol : symbols) {
        SourceRangeInfo range;
        range.file = symbol.file;
        range.line = symbol.line;
        range.column = symbol.column;
        range.endLine = symbol.line;
        range.endColumn = symbol.column + static_cast<uint32_t>(symbol.name.size());
        arr.push_back(JsonValue::object({
            {"name", JsonValue(symbol.name)},
            {"kind", JsonValue(symbolKindToLsp(symbol.kind))},
            {"location",
             JsonValue::object({
                 {"uri", JsonValue(pathToFileUri(symbol.file))},
                 {"range", rangeForSourceRange(range, nullptr, "")},
             })},
        }));
    }
    return buildResponse(req.id, JsonValue(std::move(arr)));
}

// --- Semantic Tokens ---

/// @brief Encode a full document's semantic tokens in LSP delta form.
/// @param req Request containing the target document URI.
/// @return SemanticTokens response containing five integers per retained token,
///         an empty token set when unsupported, or an invalid-parameters response.
/// @details Bridge byte ranges are converted to UTF-16, sorted by location, and
///          filtered to valid single-line, nonoverlapping legend entries.
std::string LspHandler::handleSemanticTokensFull(const JsonRpcRequest &req) {
    std::string uri;
    if (!extractTextDocumentUri(req.params, uri))
        return buildError(
            req.id, kInvalidParams, "Invalid textDocument/semanticTokens/full params");

    const std::string *content = store_.getContent(uri);
    if (!content || !bridge_.supportsSemanticTokens())
        return buildResponse(req.id, JsonValue::object({{"data", JsonValue::array({})}}));

    std::string path;
    std::string pathErr;
    if (!DocumentStore::tryFileUriToPath(uri, path, &pathErr))
        return buildError(req.id, kInvalidParams, pathErr);

    struct EncodedToken {
        int line{0};
        int start{0};
        int length{0};
        int type{0};
        int modifiers{0};
    };

    auto tokens = bridge_.semanticTokens(*content, path);
    std::vector<EncodedToken> encoded;
    encoded.reserve(tokens.size());
    for (const auto &token : tokens) {
        int startLine = 0;
        int startCol = 0;
        int endLine = 0;
        int endCol = 0;
        if (!sourceRangeToLspSpan(*content,
                                  token.line,
                                  token.column,
                                  token.line,
                                  token.column + token.length,
                                  startLine,
                                  startCol,
                                  endLine,
                                  endCol)) {
            continue;
        }
        if (endLine != startLine || endCol <= startCol)
            continue;
        const int tokenType = static_cast<int>(token.type);
        if (tokenType < 0 || tokenType > static_cast<int>(SemanticTokenType::Operator) ||
            token.modifiers != 0) {
            continue;
        }
        encoded.push_back(
            {startLine, startCol, endCol - startCol, tokenType, static_cast<int>(token.modifiers)});
    }
    /// @brief Order semantic tokens by source line and start position.
    /// @param lhs Left-hand encoded token.
    /// @param rhs Right-hand encoded token.
    /// @return `true` when `lhs` precedes `rhs`.
    std::sort(encoded.begin(), encoded.end(), [](const EncodedToken &lhs, const EncodedToken &rhs) {
        if (lhs.line != rhs.line)
            return lhs.line < rhs.line;
        return lhs.start < rhs.start;
    });

    JsonValue::ArrayType data;
    data.reserve(encoded.size() * 5u);
    int prevLine = 0;
    int prevStart = 0;
    int prevEndOnLine = -1;
    int prevTokenLine = -1;
    for (const auto &token : encoded) {
        if (token.line == prevTokenLine && token.start < prevEndOnLine)
            continue;
        if (token.modifiers < 0)
            continue;
        const int deltaLine = token.line - prevLine;
        const int deltaStart = deltaLine == 0 ? token.start - prevStart : token.start;
        data.push_back(JsonValue(deltaLine));
        data.push_back(JsonValue(deltaStart));
        data.push_back(JsonValue(token.length));
        data.push_back(JsonValue(token.type));
        data.push_back(JsonValue(token.modifiers));
        prevLine = token.line;
        prevStart = token.start;
        prevTokenLine = token.line;
        prevEndOnLine = token.start + token.length;
    }

    return buildResponse(req.id, JsonValue::object({{"data", JsonValue(std::move(data))}}));
}

// --- Diagnostic Publishing ---

/// @brief Compile an open document and publish its current LSP diagnostics.
/// @param uri Client document URI used to find content and address the notification.
/// @details Converts source spans, severity, related notes, help links, and fix-its
///          into Diagnostic objects. Missing documents, invalid URIs, and transport
///          loss are handled as best-effort no-ops.
void LspHandler::publishDiagnostics(const std::string &uri) {
    const std::string *content = store_.getContent(uri);
    if (!content)
        return;

    std::string path;
    if (!DocumentStore::tryFileUriToPath(uri, path))
        return;
    auto diags = bridge_.check(*content, path);

    JsonValue::ArrayType diagArr;
    diagArr.reserve(diags.size());
    for (const auto &d : diags) {
        int lspSeverity;
        switch (d.severity) {
            case 0:
                lspSeverity = 3; // LSP Information (Note)
                break;
            case 1:
                lspSeverity = 2; // LSP Warning
                break;
            case 2:
                lspSeverity = 1; // LSP Error
                break;
            default:
                lspSeverity = 3;
                break;
        }

        auto range = diagnosticRangeForSpan(*content, d.line, d.column, d.endLine, d.endColumn);

        auto diag = JsonValue::object({
            {"range", std::move(range)},
            {"severity", JsonValue(lspSeverity)},
            {"source", JsonValue(config_.sourceName)},
            {"message", JsonValue(d.message)},
        });

        if (!d.code.empty()) {
            auto diagObj = diag.asObject();
            diagObj.push_back({"code", JsonValue(d.code)});
            if (!d.help.empty() &&
                (d.help.rfind("http://", 0) == 0 || d.help.rfind("https://", 0) == 0)) {
                diagObj.push_back(
                    {"codeDescription", JsonValue::object({{"href", JsonValue(d.help)}})});
            }
            diag = JsonValue(std::move(diagObj));
        }

        if (!d.notes.empty()) {
            // The single-document server only knows this document's URI. Notes
            // anchored in this document point at their own range; notes from
            // other files (or without a location) anchor at the primary range
            // with the original path preserved in the message text.
            JsonValue::ArrayType related;
            related.reserve(d.notes.size());
            for (const auto &n : d.notes) {
                const bool sameFile = n.file.empty() || n.file == path;
                const bool hasLoc = n.line > 0;
                JsonValue noteRange = (sameFile && hasLoc)
                                          ? diagnosticRangeForLocation(*content, n.line, n.column)
                                          : diagnosticRangeForSpan(
                                                *content, d.line, d.column, d.endLine, d.endColumn);
                std::string noteMessage = n.message;
                if (!sameFile && hasLoc) {
                    noteMessage = n.file + ":" + std::to_string(n.line) + ":" +
                                  std::to_string(n.column) + ": " + noteMessage;
                }
                const std::string noteUri =
                    (!sameFile && !n.file.empty()) ? pathToFileUri(n.file) : uri;
                related.push_back(JsonValue::object({
                    {"location",
                     JsonValue::object(
                         {{"uri", JsonValue(noteUri)}, {"range", std::move(noteRange)}})},
                    {"message", JsonValue(std::move(noteMessage))},
                }));
            }
            auto diagObj = diag.asObject();
            diagObj.push_back({"relatedInformation", JsonValue(std::move(related))});
            diag = JsonValue(std::move(diagObj));
        }

        if (!d.stage.empty() || !d.help.empty() || !d.fixits.empty()) {
            JsonValue::ArrayType fixits;
            fixits.reserve(d.fixits.size());
            for (const auto &f : d.fixits) {
                JsonValue fixRange =
                    (f.line > 0) ? diagnosticRangeForSpan(*content,
                                                          f.line,
                                                          f.column,
                                                          f.endLine ? f.endLine : f.line,
                                                          f.endColumn ? f.endColumn : f.column)
                                 : diagnosticRangeForSpan(
                                       *content, d.line, d.column, d.endLine, d.endColumn);
                fixits.push_back(JsonValue::object({
                    {"message", JsonValue(f.message)},
                    {"replacement", JsonValue(f.replacement)},
                    {"range", std::move(fixRange)},
                }));
            }

            auto diagObj = diag.asObject();
            diagObj.push_back({"data",
                               JsonValue::object({
                                   {"stage", JsonValue(d.stage)},
                                   {"help", JsonValue(d.help)},
                                   {"fixits", JsonValue(std::move(fixits))},
                               })});
            diag = JsonValue(std::move(diagObj));
        }

        diagArr.push_back(std::move(diag));
    }

    auto params = JsonValue::object({
        {"uri", JsonValue(uri)},
        {"diagnostics", JsonValue(std::move(diagArr))},
    });
    try {
        transport_.writeMessage(buildNotification("textDocument/publishDiagnostics", params));
    } catch (const std::exception &) {
        // Diagnostics are a notification; transport loss should not crash request handling.
    }
}

// --- Kind mapping ---

/// @brief Map a bridge completion-kind ordinal to LSP CompletionItemKind.
/// @param kind Compiler-bridge kind value defined by CompletionInfo.
/// @return Corresponding LSP enumeration value, or Text for unknown ordinals.
int LspHandler::completionKindToLsp(int kind) {
    switch (kind) {
        case 0:
            return 14; // Keyword
        case 1:
            return 15; // Snippet
        case 2:
            return 6; // Variable
        case 3:
            return 6; // Parameter → Variable
        case 4:
            return 5; // Field
        case 5:
            return 2; // Method
        case 6:
            return 3; // Function
        case 7:
            return 7; // Entity → Class
        case 8:
            return 12; // Value
        case 9:
            return 8; // Interface
        case 10:
            return 9; // Module
        case 11:
            return 7; // RuntimeClass → Class
        case 12:
            return 10; // Property
        default:
            return 1; // Text
    }
}

/// @brief Map a bridge symbol-kind name to LSP SymbolKind.
/// @param kind Lowercase language-neutral bridge kind name.
/// @return Corresponding LSP enumeration value, defaulting to Variable.
int LspHandler::symbolKindToLsp(const std::string &kind) {
    if (kind == "function")
        return 12;
    if (kind == "method")
        return 6;
    if (kind == "variable")
        return 13;
    if (kind == "parameter")
        return 13;
    if (kind == "field")
        return 8;
    if (kind == "type")
        return 5;
    if (kind == "module")
        return 2;
    return 13;
}

} // namespace zanna::server
