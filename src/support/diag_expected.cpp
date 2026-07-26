//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/support/diag_expected.cpp
// Purpose: Provide the Expected<void>-based diagnostic infrastructure shared by
//          front ends, passes, and tooling.
// Key invariants: Success is represented by an empty optional diagnostic; every
//                 failure stores a fully-populated @ref Diag.  Severity strings
//                 must remain lowercase to match existing command-line output.
// Ownership/Lifetime: Expected instances own their diagnostic payloads by value;
//                     printing helpers borrow output streams and optional source
//                     managers supplied by the caller.
// Links: src/support/diag_expected.hpp, docs/internals/codemap.md#support-library
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Supplies the `Expected<void>` helpers specialized for diagnostics.
/// @details The support layer leans heavily on `Expected<void>` to propagate
///          recoverable failures.  This translation unit gathers all diagnostic
///          focused utilities—constructors, severity conversions, and printers—
///          so clients get a coherent experience when emitting or displaying
///          errors from disparate subsystems.

#include "diag_expected.hpp"

#include <algorithm>
#include <cassert>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace il::support {
namespace {
/// @brief Calculate a same-line diagnostic underline length in source bytes.
/// @param diag Diagnostic whose primary range should be inspected.
/// @param snippetLoc Location selected for snippet rendering.
/// @return Number of source bytes covered by the same-line range, or one byte.
/// @details Source locations in the front ends are byte-oriented.  The snippet
///          renderer later converts the byte count to display columns so UTF-8
///          code units do not produce multiple carets for one visible character.
uint32_t sameLineRangeLength(const Diag &diag, SourceLoc snippetLoc) {
    if (!diag.range.isConcrete() || diag.range.begin.file_id != snippetLoc.file_id ||
        diag.range.begin.line != snippetLoc.line || diag.range.end.line != snippetLoc.line ||
        diag.range.begin.column == 0 || diag.range.begin.column != snippetLoc.column ||
        diag.range.end.column <= diag.range.begin.column) {
        return 1;
    }
    return diag.range.end.column - diag.range.begin.column;
}

/// @brief Check whether @p byte is a UTF-8 continuation byte.
/// @param byte Byte to inspect.
/// @return True when @p byte has the binary form 10xxxxxx.
bool isUtf8Continuation(unsigned char byte) {
    return (byte & 0xc0u) == 0x80u;
}

/// @brief Determine the valid UTF-8 sequence length at @p index.
/// @param text Byte string expected to contain UTF-8.
/// @param index Offset of the candidate leading byte.
/// @return Valid sequence length in bytes, or zero when the sequence is invalid.
/// @details Rejects overlong encodings, surrogate code points, truncated
///          sequences, and code points beyond U+10FFFF so JSON output never
///          contains malformed UTF-8.
size_t validUtf8SequenceLength(std::string_view text, size_t index) {
    /// @brief Return the unsigned byte at an offset from the candidate sequence lead.
    /// @param offset Byte offset relative to @p index.
    /// @return Unsigned byte value from @p text.
    const auto byteAt = [&](size_t offset) {
        return static_cast<unsigned char>(text[index + offset]);
    };

    const unsigned char first = byteAt(0);
    if (first < 0x80u)
        return 1;
    if (first >= 0xc2u && first <= 0xdfu) {
        if (index + 1 < text.size() && isUtf8Continuation(byteAt(1)))
            return 2;
        return 0;
    }
    if (first == 0xe0u) {
        if (index + 2 < text.size() && byteAt(1) >= 0xa0u && byteAt(1) <= 0xbfu &&
            isUtf8Continuation(byteAt(2)))
            return 3;
        return 0;
    }
    if ((first >= 0xe1u && first <= 0xecu) || (first >= 0xeeu && first <= 0xefu)) {
        if (index + 2 < text.size() && isUtf8Continuation(byteAt(1)) &&
            isUtf8Continuation(byteAt(2)))
            return 3;
        return 0;
    }
    if (first == 0xedu) {
        if (index + 2 < text.size() && byteAt(1) >= 0x80u && byteAt(1) <= 0x9fu &&
            isUtf8Continuation(byteAt(2)))
            return 3;
        return 0;
    }
    if (first == 0xf0u) {
        if (index + 3 < text.size() && byteAt(1) >= 0x90u && byteAt(1) <= 0xbfu &&
            isUtf8Continuation(byteAt(2)) && isUtf8Continuation(byteAt(3)))
            return 4;
        return 0;
    }
    if (first >= 0xf1u && first <= 0xf3u) {
        if (index + 3 < text.size() && isUtf8Continuation(byteAt(1)) &&
            isUtf8Continuation(byteAt(2)) && isUtf8Continuation(byteAt(3)))
            return 4;
        return 0;
    }
    if (first == 0xf4u) {
        if (index + 3 < text.size() && byteAt(1) >= 0x80u && byteAt(1) <= 0x8fu &&
            isUtf8Continuation(byteAt(2)) && isUtf8Continuation(byteAt(3)))
            return 4;
        return 0;
    }
    return 0;
}

/// @brief Return the snippet location that should receive the primary caret.
/// @param diag Diagnostic whose range/location pair is being printed.
/// @return Range begin for same-line concrete ranges, otherwise @ref Diag::loc.
/// @details Some diagnostics carry a primary location near a token but attach a
///          more precise range that starts elsewhere on the same line.  Rendering
///          the snippet from the range begin lets users see the full highlighted
///          span without losing the original header location.
SourceLoc primarySnippetLoc(const Diag &diag) {
    if ((diag.range.isConcrete() || diag.range.isInsertion()) &&
        diag.range.begin.file_id == diag.range.end.file_id &&
        diag.range.begin.line == diag.range.end.line && diag.range.begin.hasColumn()) {
        return diag.range.begin;
    }
    return diag.loc;
}

/// @brief Return the number of visible cells emitted for an escaped ASCII byte.
/// @param byte ASCII/control byte to classify.
/// @return Number of output characters used by the escaped source-line printer.
/// @details Source snippets escape all control bytes rather than printing them
///          directly.  The caret renderer uses the same width accounting so
///          underlines remain aligned with the escaped source text.
uint32_t escapedAsciiByteWidth(unsigned char byte) {
    switch (byte) {
        case '\n':
        case '\r':
        case '\t':
        case '\b':
        case '\f':
            return 2;
        default:
            if (byte < 0x20u || byte == 0x7fu)
                return 4;
            return 1;
    }
}

/// @brief Print one escaped ASCII/control byte from a source snippet.
/// @param os Output stream receiving the escaped byte.
/// @param byte Byte to render.
/// @details This mirrors @ref printTextEscaped for source lines but keeps the
///          implementation local so the caret width code and source rendering
///          remain tied to the same escape policy.
void printEscapedSourceByte(std::ostream &os, unsigned char byte) {
    constexpr char hex[] = "0123456789abcdef";
    switch (byte) {
        case '\n':
            os << "\\n";
            break;
        case '\r':
            os << "\\r";
            break;
        case '\t':
            os << "\\t";
            break;
        case '\b':
            os << "\\b";
            break;
        case '\f':
            os << "\\f";
            break;
        default:
            if (byte < 0x20u || byte == 0x7fu)
                os << "\\x" << hex[(byte >> 4) & 0x0f] << hex[byte & 0x0f];
            else
                os << static_cast<char>(byte);
            break;
    }
}

/// @brief Return visible output columns for a source byte span after escaping.
/// @param line Source line text.
/// @param beginByte Zero-based byte offset at which to begin counting.
/// @param requestedBytes Number of source bytes to account for.
/// @return Escaped display width, with valid UTF-8 scalar values counted as one.
/// @details Front ends currently report byte columns.  This helper walks exactly
///          those bytes but mirrors the escaped snippet printer, so tabs, ANSI
///          escape bytes, and malformed UTF-8 do not desynchronize the caret.
uint32_t escapedDisplayColumnsForByteRange(std::string_view line,
                                           size_t beginByte,
                                           size_t requestedBytes) {
    if (beginByte >= line.size() || requestedBytes == 0)
        return 0;

    const size_t endByte = std::min(line.size(), beginByte + requestedBytes);
    uint32_t columns = 0;
    for (size_t index = beginByte; index < endByte;) {
        const auto byte = static_cast<unsigned char>(line[index]);
        if (byte < 0x80u) {
            columns += escapedAsciiByteWidth(byte);
            ++index;
            continue;
        }

        const size_t sequenceLength = validUtf8SequenceLength(line, index);
        if (sequenceLength == 0) {
            columns += 4;
            ++index;
        } else {
            ++columns;
            index += sequenceLength;
        }
    }
    return columns;
}

/// @brief Emit indentation before a source caret using escaped source columns.
/// @param os Output stream receiving spaces.
/// @param line Source line text.
/// @param column One-based byte column of the caret.
/// @details Existing front ends report byte columns.  Source text is escaped
///          before printing, so the indent width is computed from the escaped
///          representation rather than from raw terminal control characters.
void printCaretIndent(std::ostream &os, std::string_view line, uint32_t column) {
    if (column <= 1)
        return;

    const size_t byteLimit = std::min<size_t>(static_cast<size_t>(column - 1), line.size());
    const uint32_t columns = escapedDisplayColumnsForByteRange(line, 0, byteLimit);
    for (uint32_t index = 0; index < columns; ++index)
        os << ' ';
}

/// @brief Print a source line using visible escapes for unsafe bytes.
/// @param os Output stream receiving the escaped line.
/// @param line Source line text without a line terminator.
/// @details Diagnostics are often rendered directly into terminals or logs.
///          Printing raw control characters lets a source file alter subsequent
///          output, so this helper escapes controls and malformed UTF-8 while
///          preserving ordinary printable ASCII and valid UTF-8 bytes.
void printSourceLineEscaped(std::ostream &os, std::string_view line) {
    for (size_t index = 0; index < line.size();) {
        const auto byte = static_cast<unsigned char>(line[index]);
        if (byte < 0x80u) {
            printEscapedSourceByte(os, byte);
            ++index;
            continue;
        }

        const size_t sequenceLength = validUtf8SequenceLength(line, index);
        if (sequenceLength == 0) {
            os << "\\x";
            constexpr char hex[] = "0123456789abcdef";
            os << hex[(byte >> 4) & 0x0f] << hex[byte & 0x0f];
            ++index;
        } else {
            os.write(line.data() + index, static_cast<std::streamsize>(sequenceLength));
            index += sequenceLength;
        }
    }
}

/// @brief Count display columns covered by a byte range in an escaped source line.
/// @param line Source line text.
/// @param beginByte Zero-based byte offset where the underline begins.
/// @param requestedBytes Number of source bytes the underline should cover.
/// @return At least one display column.
/// @details Control bytes are counted according to their escaped representation.
///          Valid UTF-8 scalar values still count as one column, matching the
///          existing byte-oriented location model used by the front ends.
uint32_t displayColumnsForByteRange(std::string_view line,
                                    size_t beginByte,
                                    uint32_t requestedBytes) {
    const uint32_t columns =
        escapedDisplayColumnsForByteRange(line, beginByte, static_cast<size_t>(requestedBytes));
    return std::max<uint32_t>(1, columns);
}

/// @brief Print one escaped source line with its diagnostic gutter.
/// @param loc Location whose line number and file id select the source line.
/// @param os Output stream receiving the rendered source line.
/// @param sm Source manager used to retrieve source text.
/// @return The line text when available; otherwise std::nullopt.
/// @details Returning the unescaped line lets callers compute byte-oriented caret
///          placement after the visible source text has been printed.
std::optional<std::string_view> printEscapedSourceGutterLine(SourceLoc loc,
                                                             std::ostream &os,
                                                             const SourceManager *sm) {
    if (!sm || loc.file_id == 0 || loc.line == 0)
        return std::nullopt;

    if (!sm->hasLine(loc.file_id, loc.line))
        return std::nullopt;

    auto srcLine = sm->getLine(loc.file_id, loc.line);
    std::string lineNumStr = std::to_string(loc.line);

    os << ' ' << lineNumStr << " | ";
    printSourceLineEscaped(os, srcLine);
    os << '\n';
    return srcLine;
}

/// @brief Print a caret/tilde marker under an already-rendered source line.
/// @param lineNumber Source line number used to size the gutter.
/// @param srcLine Raw source line text used for byte-to-column conversion.
/// @param column One-based byte column where the marker starts.
/// @param underlineLength Number of source bytes to underline.
/// @param os Output stream receiving the marker line.
/// @details Source text is printed in escaped form, so this helper mirrors that
///          escaping policy when computing indentation and underline width.
void printSourceMarker(uint32_t lineNumber,
                       std::string_view srcLine,
                       uint32_t column,
                       uint32_t underlineLength,
                       std::ostream &os) {
    std::string lineNumStr = std::to_string(lineNumber);
    std::string gutter(lineNumStr.size(), ' ');

    if (column == 0 || column > srcLine.size() + 1)
        return;

    os << ' ' << gutter << " | ";
    printCaretIndent(os, srcLine, column);
    const size_t beginByte = static_cast<size_t>(column - 1);
    const uint32_t maxLength = static_cast<uint32_t>(
        std::min<size_t>(std::numeric_limits<uint32_t>::max(),
                         srcLine.size() + 1 >= column ? srcLine.size() + 1 - column : 1));
    const uint32_t caretCount =
        displayColumnsForByteRange(srcLine, beginByte, std::min(underlineLength, maxLength));
    os << '^';
    for (uint32_t i = 1; i < caretCount; ++i)
        os << '~';
    os << '\n';
}

/// @brief Print the source line and caret marker for a diagnostic-like location.
/// @param loc Location selecting the file, line, and one-based caret column.
/// @param underlineLength Number of source bytes to cover with the marker.
/// @param os Output stream receiving the gutter, source line, and marker.
/// @param sm Optional source manager used to retrieve the source line.
/// @details Produces no output when the location cannot be resolved to a line.
///          Source control bytes are escaped and the marker width is adjusted
///          to remain aligned with the resulting visible representation.
void printSourceSnippet(SourceLoc loc,
                        uint32_t underlineLength,
                        std::ostream &os,
                        const SourceManager *sm) {
    auto srcLine = printEscapedSourceGutterLine(loc, os, sm);
    if (!srcLine)
        return;
    printSourceMarker(loc.line, *srcLine, loc.column, underlineLength, os);
}

/// @brief Print a bounded multi-line diagnostic range.
/// @param range Concrete source range whose endpoints may span multiple lines.
/// @param os Output stream receiving source snippet lines.
/// @param sm Source manager used to retrieve source text.
/// @return True when a multi-line snippet was rendered.
/// @details Very large ranges are abbreviated after a small number of lines so a
///          malicious or generated range cannot flood terminal output while still
///          showing both the beginning and end coordinates.
bool printMultilineSourceSnippet(const SourceRange &range,
                                 std::ostream &os,
                                 const SourceManager *sm) {
    if (!range.isConcrete() || range.begin.file_id != range.end.file_id ||
        range.begin.line >= range.end.line)
        return false;

    constexpr uint32_t kMaxRenderedLines = 4;
    uint32_t rendered = 0;
    for (uint32_t line = range.begin.line; line <= range.end.line; ++line) {
        if (rendered == kMaxRenderedLines && line < range.end.line) {
            os << " ... | ...\n";
            line = range.end.line;
        }

        SourceLoc lineLoc{range.begin.file_id, line, 1};
        auto srcLine = printEscapedSourceGutterLine(lineLoc, os, sm);
        if (!srcLine)
            continue;

        uint32_t startColumn = 1;
        uint32_t underlineLength = 1;
        if (line == range.begin.line) {
            startColumn = range.begin.column;
            underlineLength = static_cast<uint32_t>(std::min<size_t>(
                std::numeric_limits<uint32_t>::max(),
                srcLine->size() + 1 >= startColumn ? srcLine->size() + 1 - startColumn : 1));
        } else if (line == range.end.line) {
            underlineLength = std::max<uint32_t>(1, range.end.column - 1);
        } else {
            underlineLength = static_cast<uint32_t>(std::min<size_t>(
                std::numeric_limits<uint32_t>::max(), std::max<size_t>(1, srcLine->size())));
        }
        printSourceMarker(line, *srcLine, startColumn, underlineLength, os);
        ++rendered;
    }
    return rendered != 0;
}

/// @brief Emit text in a single diagnostic line without raw control characters.
/// @param os Output stream receiving escaped text.
/// @param text User-facing diagnostic text to sanitize.
/// @details Text diagnostics are line-oriented.  Rendering embedded newlines,
///          carriage returns, or other control bytes verbatim lets one diagnostic
///          masquerade as several messages, so this helper prints visible escape
///          sequences while leaving ordinary UTF-8 bytes unchanged.
void printTextEscaped(std::ostream &os, std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char ch : text) {
        switch (ch) {
            case '\n':
                os << "\\n";
                break;
            case '\r':
                os << "\\r";
                break;
            case '\t':
                os << "\\t";
                break;
            case '\b':
                os << "\\b";
                break;
            case '\f':
                os << "\\f";
                break;
            default:
                if (ch < 0x20u || ch == 0x7fu) {
                    os << "\\x" << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
                } else {
                    os << static_cast<char>(ch);
                }
                break;
        }
    }
}

/// @brief Print a diagnostic header without attached notes.
/// @param diag Diagnostic supplying the location, severity, code, and message.
/// @param os Output stream receiving the single-line header.
/// @param sm Optional source manager used to resolve the location's file path.
/// @details The optional path and coordinates precede the severity when they
///          can be resolved. Diagnostic-controlled text is escaped so embedded
///          control characters cannot forge additional output lines.
void printDiagHeader(const Diag &diag, std::ostream &os, const SourceManager *sm) {
    if (sm && diag.loc.file_id != 0) {
        auto path = sm->getPath(diag.loc.file_id);
        if (!path.empty()) {
            printTextEscaped(os, path);
            if (diag.loc.line != 0) {
                os << ':' << diag.loc.line;
                if (diag.loc.column != 0) {
                    os << ':' << diag.loc.column;
                }
            }
            os << ": ";
        }
    }
    os << detail::diagSeverityToString(diag.severity);
    if (!diag.code.empty()) {
        os << '[';
        printTextEscaped(os, diag.code);
        os << ']';
    }
    os << ": ";
    printTextEscaped(os, diag.message);
    os << '\n';
}

/// @brief Print a human-readable source coordinate for a fix-it range.
/// @param os Output stream receiving the coordinate.
/// @param range Range attached to the fix-it.
/// @param sm Optional source manager used to resolve file ids.
/// @details Text diagnostics are consumed by humans, but fix-its can reference
///          a different file than the primary diagnostic.  Including a path when
///          it is known prevents cross-file edits from looking like same-file
///          line/column pairs.
void printFixItTextRange(std::ostream &os, const SourceRange &range, const SourceManager *sm) {
    if (!range.isConcrete() && !range.isInsertion())
        return;

    /// @brief Print one endpoint, including a resolved path when it is available.
    /// @param loc Source location to render.
    const auto printPoint = [&](SourceLoc loc) {
        if (sm && loc.file_id != 0) {
            const auto path = sm->getPath(loc.file_id);
            if (!path.empty()) {
                printTextEscaped(os, path);
                os << ':';
            }
        }
        os << loc.line << ':' << loc.column;
    };

    os << " at ";
    printPoint(range.begin);
    if (range.isConcrete()) {
        os << '-';
        if (range.end.file_id != range.begin.file_id)
            printPoint(range.end);
        else
            os << range.end.line << ':' << range.end.column;
    }
}

/// @brief Print optional diagnostic metadata that has no source snippet of its own.
/// @param diag Diagnostic whose secondary fields should be rendered.
/// @param os Output stream receiving text diagnostics.
/// @param sm Optional source manager used to resolve fix-it file identifiers.
/// @details The support diagnostic model carries structured fields for stage,
///          help text, and fix-it suggestions.  Keeping these in the text output
///          makes command-line diagnostics match the information available to JSON
///          consumers without changing the primary header format.
void printDiagDetails(const Diag &diag, std::ostream &os, const SourceManager *sm) {
    if (!diag.stage.empty()) {
        os << "  stage: ";
        printTextEscaped(os, diag.stage);
        os << '\n';
    }
    if (!diag.help.empty()) {
        os << "  help: ";
        printTextEscaped(os, diag.help);
        os << '\n';
    }
    for (const auto &fixit : diag.fixits) {
        os << "  fix-it";
        if (!fixit.message.empty()) {
            os << ": ";
            printTextEscaped(os, fixit.message);
        }
        printFixItTextRange(os, fixit.range, sm);
        os << " -> ";
        if (fixit.replacement.empty())
            os << "<empty>";
        else
            printTextEscaped(os, fixit.replacement);
        os << '\n';
    }
}

/// @brief Emit a string as a JSON-escaped, double-quoted literal.
/// @details Escapes the characters mandated by the JSON grammar (quote,
///          backslash, and the `\b`/`\f`/`\n`/`\r`/`\t` shorthands) and renders
///          any remaining control byte below 0x20 as a `\u00XX` sequence so the
///          output is always valid UTF-8 JSON.
/// @param os Output stream receiving the quoted literal.
/// @param text Raw text to escape and quote.
void printJsonEscaped(std::ostream &os, std::string_view text) {
    os << '"';
    for (size_t index = 0; index < text.size();) {
        const unsigned char ch = static_cast<unsigned char>(text[index]);
        switch (ch) {
            case '"':
                os << "\\\"";
                ++index;
                break;
            case '\\':
                os << "\\\\";
                ++index;
                break;
            case '\b':
                os << "\\b";
                ++index;
                break;
            case '\f':
                os << "\\f";
                ++index;
                break;
            case '\n':
                os << "\\n";
                ++index;
                break;
            case '\r':
                os << "\\r";
                ++index;
                break;
            case '\t':
                os << "\\t";
                ++index;
                break;
            default:
                if (ch < 0x20) {
                    constexpr char hex[] = "0123456789abcdef";
                    os << "\\u00" << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
                    ++index;
                } else if (ch < 0x80) {
                    os << static_cast<char>(ch);
                    ++index;
                } else {
                    const size_t length = validUtf8SequenceLength(text, index);
                    if (length == 0) {
                        os << "\\ufffd";
                        ++index;
                    } else {
                        os.write(text.data() + index, static_cast<std::streamsize>(length));
                        index += length;
                    }
                }
                break;
        }
    }
    os << '"';
}

/// @brief Cache SourceManager file-id resolutions while printing one JSON diagnostic.
/// @details A single diagnostic object can reference the same file in its primary
///          location, range endpoints, notes, fix-its, and source snippet.  The
///          SourceManager protects lookups with a mutex, so resolving once per
///          file id keeps JSON serialization cheaper without changing output.
struct JsonFileResolver {
    /// @brief Source manager borrowed for the duration of JSON serialization.
    const SourceManager *sm = nullptr;

    /// @brief Cached resolved path by file id; std::nullopt means "unknown".
    std::unordered_map<uint32_t, std::optional<std::string>> paths;

    /// @brief Construct a resolver for an optional SourceManager.
    /// @param sourceManager Manager used to resolve file ids, or nullptr.
    explicit JsonFileResolver(const SourceManager *sourceManager) : sm(sourceManager) {}

    /// @brief Resolve @p fileId to a display path if possible.
    /// @param fileId SourceManager file identifier.
    /// @return Optional display path string cached for repeated use.
    const std::optional<std::string> &resolve(uint32_t fileId) {
        auto [it, inserted] = paths.emplace(fileId, std::nullopt);
        if (!inserted)
            return it->second;
        if (sm && fileId != 0) {
            const auto path = sm->getPath(fileId);
            if (!path.empty())
                it->second = std::string(path);
        }
        return it->second;
    }

    /// @brief Print the JSON value for a file id's resolved path.
    /// @param os Output stream receiving either a quoted path or null.
    /// @param fileId SourceManager file identifier to serialize.
    void printFile(std::ostream &os, uint32_t fileId) {
        const auto &path = resolve(fileId);
        if (path)
            printJsonEscaped(os, *path);
        else
            os << "null";
    }
};

/// @brief Emit a source location as a JSON `"location"` object.
/// @details Writes the raw `file_id`/`line`/`column` triple alongside a resolved
///          `file` path; the path is `null` when no SourceManager is available or
///          the location carries no file identifier.
/// @param os Output stream receiving the object.
/// @param loc Source location to serialize.
/// @param files Resolver used to cache SourceManager path lookups.
void printJsonLoc(std::ostream &os, SourceLoc loc, JsonFileResolver &files) {
    os << "\"location\":{";
    os << "\"file_id\":" << loc.file_id << ',';
    os << "\"line\":" << loc.line << ',';
    os << "\"column\":" << loc.column << ',';
    os << "\"file\":";
    files.printFile(os, loc.file_id);
    os << '}';
}

/// @brief Emit a string value, or JSON `null` when it is empty.
/// @details Distinguishes "absent" from "present but empty" for optional textual
///          fields (stage, help, fix-it message) by mapping an empty view to the
///          literal `null` and otherwise delegating to @ref printJsonEscaped.
/// @param os Output stream receiving the value.
/// @param text Candidate text; empty means the field is absent.
void printJsonOptionalString(std::ostream &os, std::string_view text) {
    if (text.empty())
        os << "null";
    else
        printJsonEscaped(os, text);
}

/// @brief Emit the source text of a location's line as a JSON `"source"` field.
/// @details Resolves the line through the SourceManager and escapes it; produces
///          `null` when there is no manager, the location lacks a file or line,
///          or the resolved line is empty. Lets JSON consumers render an inline
///          code excerpt without re-reading the file.
/// @param os Output stream receiving the field.
/// @param loc Source location whose line should be quoted.
/// @param sm Optional source manager used to fetch the line text.
void printJsonSourceLine(std::ostream &os, SourceLoc loc, const SourceManager *sm) {
    os << "\"source\":";
    if (!sm || loc.file_id == 0 || loc.line == 0) {
        os << "null";
        return;
    }
    if (!sm->hasLine(loc.file_id, loc.line)) {
        os << "null";
        return;
    }
    const std::string line{sm->getLine(loc.file_id, loc.line)};
    printJsonEscaped(os, line);
}

/// @brief Emit a source range as a JSON `"range"` object with begin/end points.
/// @details Serializes both endpoints as `file_id`/`line`/`column`/`file`
///          records, mirroring @ref printJsonLoc for each end; the whole field is
///          `null` when the range is invalid.
/// @param os Output stream receiving the object.
/// @param range Source range to serialize.
/// @param files Resolver used to cache SourceManager path lookups.
void printJsonRange(std::ostream &os, const SourceRange &range, JsonFileResolver &files) {
    os << "\"range\":";
    if (!range.isConcrete() && !range.isInsertion()) {
        os << "null";
        return;
    }
    os << '{';
    os << "\"begin\":{";
    os << "\"file_id\":" << range.begin.file_id << ',';
    os << "\"line\":" << range.begin.line << ',';
    os << "\"column\":" << range.begin.column << ',';
    os << "\"file\":";
    files.printFile(os, range.begin.file_id);
    os << "},\"end\":{";
    os << "\"file_id\":" << range.end.file_id << ',';
    os << "\"line\":" << range.end.line << ',';
    os << "\"column\":" << range.end.column << ',';
    os << "\"file\":";
    files.printFile(os, range.end.file_id);
    os << "}}";
}

/// @brief Emit a fix-it range, falling back to insertion at the diagnostic location.
/// @param os Output stream receiving the JSON field.
/// @param fixitRange Range stored on the fix-it suggestion.
/// @param diagLoc Primary diagnostic location used for implicit insertions.
/// @param files Resolver used to cache SourceManager path lookups.
/// @details A default-constructed @ref DiagnosticFixIt::range means "insert at
///          the diagnostic location".  JSON consumers need concrete coordinates,
///          so this helper materialises that implicit insertion when possible.
void printJsonFixItRange(std::ostream &os,
                         const SourceRange &fixitRange,
                         SourceLoc diagLoc,
                         JsonFileResolver &files) {
    if (fixitRange.isConcrete() || fixitRange.isInsertion()) {
        printJsonRange(os, fixitRange, files);
        return;
    }

    if (diagLoc.hasFile() && diagLoc.hasLine() && diagLoc.hasColumn()) {
        printJsonRange(os, SourceRange{diagLoc, diagLoc}, files);
        return;
    }

    printJsonRange(os, fixitRange, files);
}

/// @brief Emit a complete diagnostic as a single JSON object.
/// @details Assembles every diagnostic field — severity, code, stage, message,
///          location, range, source line, help text, notes, and fix-its — into
///          one object using the helpers above. Notes and fix-its are emitted as
///          nested arrays so machine consumers receive the full diagnostic in a
///          single record. This is the per-element building block used by
///          @ref printDiagnosticsJson.
/// @param diag Diagnostic to serialize.
/// @param os Output stream receiving the object.
/// @param sm Optional source manager used to resolve file identifiers.
void printJsonDiagObject(const Diag &diag, std::ostream &os, const SourceManager *sm) {
    const SourceLoc snippetLoc = primarySnippetLoc(diag);
    JsonFileResolver files(sm);
    os << '{';
    os << "\"severity\":";
    printJsonEscaped(os, detail::diagSeverityToString(diag.severity));
    os << ",\"code\":";
    if (diag.code.empty()) {
        os << "null";
    } else {
        printJsonEscaped(os, diag.code);
    }
    os << ",\"stage\":";
    printJsonOptionalString(os, diag.stage);
    os << ",\"message\":";
    printJsonEscaped(os, diag.message);
    os << ',';
    printJsonLoc(os, diag.loc, files);
    os << ',';
    printJsonRange(os, diag.range, files);
    os << ',';
    printJsonSourceLine(os, snippetLoc, sm);
    os << ",\"help\":";
    printJsonOptionalString(os, diag.help);
    os << ",\"notes\":[";
    for (size_t index = 0; index < diag.notes.size(); ++index) {
        if (index != 0)
            os << ',';
        const auto &note = diag.notes[index];
        os << '{';
        os << "\"message\":";
        printJsonEscaped(os, note.message);
        os << ',';
        printJsonLoc(os, note.loc, files);
        os << '}';
    }
    os << "],\"fixits\":[";
    for (size_t index = 0; index < diag.fixits.size(); ++index) {
        if (index != 0)
            os << ',';
        const auto &fixit = diag.fixits[index];
        os << '{';
        os << "\"message\":";
        printJsonOptionalString(os, fixit.message);
        os << ',';
        printJsonFixItRange(os, fixit.range, diag.loc, files);
        os << ",\"replacement\":";
        printJsonEscaped(os, fixit.replacement);
        os << '}';
    }
    os << "]}";
}
} // namespace

/// @brief Emit text as a JSON-escaped, double-quoted string literal.
/// @param os Output stream receiving the complete JSON string literal.
/// @param text Raw byte string to validate, escape, and quote.
/// @details Delegates to the diagnostic JSON escaper so command-line tools and
///          diagnostic serializers share identical handling of controls,
///          quotes, backslashes, valid UTF-8, and malformed byte sequences.
void printJsonStringEscaped(std::ostream &os, std::string_view text) {
    printJsonEscaped(os, text);
}

/// @brief Construct an Expected<void> that stores a diagnostic error state.
///
/// @details The constructor moves the provided diagnostic into the optional
///          error slot, thereby marking the instance as failed.  A
///          default-constructed `Expected` contains no diagnostic payload and
///          represents success.  Moving into the error slot avoids copies while
///          ensuring the caller relinquishes ownership of the diagnostic.
///
/// @param diag Diagnostic to transfer into the error payload.
Expected<void>::Expected(Diag diag) : error_(std::move(diag)) {}

/// @brief Report whether the Expected<void> represents a successful outcome.
///
/// @details Success is indicated by the absence of a stored diagnostic.  The
///          helper performs no side effects, allowing callers to branch on
///          success without extracting or modifying the payload.  It is the
///          canonical way to test the error flag prior to retrieving the
///          diagnostic via @ref error().
///
/// @return True if the instance holds no diagnostic (success), otherwise false.
bool Expected<void>::hasValue() const noexcept {
    return !error_.has_value();
}

/// @brief Allow Expected<void> to participate directly in boolean tests.
///
/// @details Delegates to @ref hasValue() so callers can write idiomatic
///          conditionals such as `if (auto ok = doThing())`.  The conversion is
///          explicit enough to avoid accidental narrowing yet terse enough to be
///          pleasant in control flow.
///
/// @return True when the operation succeeded and no diagnostic is stored.
Expected<void>::operator bool() const noexcept {
    return hasValue();
}

/// @brief Access the diagnostic that describes the recorded failure.
///
/// @details Callers must ensure the `Expected` represents an error before
///          invoking this accessor; doing so is undefined behaviour otherwise.
///          The returned reference remains valid for the lifetime of the
///          `Expected` instance and allows callers to inspect severity, message
///          text, and locations without copying.
///
/// @return Reference to the stored diagnostic payload.
const Diag &Expected<void>::error() const & {
    assert(error_.has_value());
    return error_.value();
}

/// @brief Move-access the diagnostic that describes the recorded failure.
///
/// @details Mirrors the const lvalue overload but lets callers efficiently
///          propagate a diagnostic out of a temporary Expected<void> without an
///          extra copy.  Calling this on a successful Expected remains a contract
///          violation and is guarded by an assertion in debug builds.
///
/// @return Rvalue reference to the stored diagnostic payload.
Diag &&Expected<void>::error() && {
    assert(error_.has_value());
    return std::move(error_.value());
}

namespace detail {
/// @brief Map a diagnostic severity to a lowercase string used for printing.
///
/// @details The helper keeps the conversion in one location so diagnostic
///          formatting stays consistent across the codebase. Unrecognised
///          enumerators fall back to "unknown" so malformed or future severity
///          values still produce parseable diagnostic text.
///
/// @param severity Severity enumeration value to translate.
/// @return Null-terminated string naming the severity level.
const char *diagSeverityToString(Severity severity) {
    switch (severity) {
        case Severity::Note:
            return "note";
        case Severity::Warning:
            return "warning";
        case Severity::Error:
            return "error";
    }
    return "unknown";
}
} // namespace detail

/// @brief Build an error diagnostic with the provided location and message.
///
/// @details This convenience function standardises the error severity used by
///          several call sites.  The location is optional and defaults to an
///          unknown value when not supplied by the caller, preserving the
///          invariant that missing source metadata is explicitly marked.  The
///          message string is moved into the diagnostic to avoid needless copies
///          when callers forward freshly constructed text.
///
/// @param loc Source location that triggered the diagnostic, or unknown.
/// @param msg Human-readable description of the problem.
/// @return Diagnostic populated with error severity and provided context.
Diag makeError(SourceLoc loc, std::string msg) {
    return Diag{Severity::Error, std::move(msg), loc, {}};
}

/// @brief Build an error diagnostic with the provided location, code, and message.
///
/// @details This convenience function creates a diagnostic with both a code and
///          message.  The code appears in the formatted output as `[CODE]` after
///          the severity level, enabling programmatic filtering while preserving
///          human readability.
///
/// @param loc Source location that triggered the diagnostic, or unknown.
/// @param code Diagnostic code for programmatic identification.
/// @param msg Human-readable description of the problem.
/// @return Diagnostic populated with error severity, code, and provided context.
Diag makeErrorWithCode(SourceLoc loc, std::string code, std::string msg) {
    return Diag{Severity::Error, std::move(msg), loc, std::move(code)};
}

/// @brief Print a diagnostic to the provided output stream.
///
/// @details The printer optionally queries a SourceManager to resolve file
///          identifiers into normalized paths.  When a valid location is
///          available the message is prefixed with
///          "<path>:<line>:<column>:" following the common compiler diagnostic
///          style.  The formatted severity string comes from
///          `detail::diagSeverityToString()` to keep wording consistent.
///
///          Canonical output format:
///            <path>:<line>:<column>: <severity>[<code>]: <message>
///          When no code is present:
///            <path>:<line>:<column>: <severity>: <message>
///
///          The function always emits a trailing newline so multiple diagnostics
///          appear as a contiguous block.
///
/// @param diag Diagnostic to render.
/// @param os Output stream receiving the textual representation.
/// @param sm Optional source manager for mapping file identifiers to paths.
void printDiag(const Diag &diag, std::ostream &os, const SourceManager *sm) {
    printDiagHeader(diag, os, sm);
    const bool renderedMultiline = printMultilineSourceSnippet(diag.range, os, sm);
    if (!renderedMultiline) {
        const SourceLoc snippetLoc = primarySnippetLoc(diag);
        printSourceSnippet(snippetLoc, sameLineRangeLength(diag, snippetLoc), os, sm);
    }
    printDiagDetails(diag, os, sm);

    for (const auto &note : diag.notes) {
        Diag noteDiag{Severity::Note, note.message, note.loc, {}};
        printDiagHeader(noteDiag, os, sm);
        printSourceSnippet(note.loc, 1, os, sm);
    }
}

/// @brief Serialize a contiguous diagnostic sequence as the standard JSON envelope.
/// @param diagnostics Diagnostics to place in the top-level array, in order.
/// @param os Output stream receiving the compact JSON document.
/// @param sm Optional source manager used to resolve paths and source lines.
/// @details Every element includes its structured notes, fix-its, range, and
///          optional source excerpt. The document ends with a newline for
///          convenient command-line streaming, including for an empty span.
void printDiagnosticsJson(std::span<const Diag> diagnostics,
                          std::ostream &os,
                          const SourceManager *sm) {
    os << "{\"diagnostics\":[";
    for (size_t index = 0; index < diagnostics.size(); ++index) {
        if (index != 0)
            os << ',';
        printJsonDiagObject(diagnostics[index], os, sm);
    }
    os << "]}\n";
}

/// @brief Serialize a vector of diagnostics as the standard JSON envelope.
/// @param diagnostics Diagnostics to encode in vector order.
/// @param os Output stream receiving the compact JSON document.
/// @param sm Optional source manager used to resolve paths and source lines.
/// @details Adapts the vector to a non-owning span and delegates to the common
///          serializer, avoiding duplicate JSON formatting behavior.
void printDiagnosticsJson(const std::vector<Diag> &diagnostics,
                          std::ostream &os,
                          const SourceManager *sm) {
    printDiagnosticsJson(std::span<const Diag>{diagnostics.data(), diagnostics.size()}, os, sm);
}

/// @brief Serialize one diagnostic in the standard diagnostics JSON envelope.
/// @param diag Diagnostic to place in the single-element top-level array.
/// @param os Output stream receiving the compact JSON document.
/// @param sm Optional source manager used to resolve paths and source lines.
/// @details Uses the same per-diagnostic object representation as the sequence
///          overloads and terminates the complete document with a newline.
void printDiagJson(const Diag &diag, std::ostream &os, const SourceManager *sm) {
    os << "{\"diagnostics\":[";
    printJsonDiagObject(diag, os, sm);
    os << "]}\n";
}
} // namespace il::support
