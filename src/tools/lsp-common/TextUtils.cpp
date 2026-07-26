//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements compiler-coordinate text scanning for hover and cursor
///        operations.
///
/// Coordinates are one-based. Scanning is byte-oriented and recognizes ASCII
/// alphanumeric/underscore identifier components plus dot-separated receiver
/// chains. Returned context owns all extracted text.
///
/// @see TextUtils.hpp
//
//===----------------------------------------------------------------------===//

#include "tools/lsp-common/TextUtils.hpp"

#include <cctype>

namespace zanna::server {

/// @brief Test whether a source byte belongs to an identifier.
/// @param c Candidate byte.
/// @return @c true for alphanumeric bytes or underscore.
/// @note Classification converts through unsigned char before calling the C
///       character API, avoiding undefined behavior for negative char values.
bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

/// @brief Extract an identifier and its dot-chain receiver at a source position.
/// @param source Full source buffer.
/// @param line One-based requested line.
/// @param col One-based byte column; positions past line end are clamped.
/// @return Owned hover context. @c valid is false for missing lines, nonpositive
///         coordinates, or positions that do not touch an identifier.
HoverContext extractIdentifierAtCursor(const std::string &source, int line, int col) {
    HoverContext ctx;
    if (line <= 0 || col <= 0)
        return ctx;

    // Find the start of the requested line (1-based).
    size_t lineStart = 0;
    int curLine = 1;
    for (size_t i = 0; i < source.size() && curLine < line; ++i) {
        if (source[i] == '\n') {
            ++curLine;
            lineStart = i + 1;
        }
    }
    if (curLine != line)
        return ctx;

    // Find line end.
    size_t lineEnd = lineStart;
    while (lineEnd < source.size() && source[lineEnd] != '\n')
        ++lineEnd;

    // col is 1-based; convert to 0-based offset within line.
    size_t cursorOff = lineStart + static_cast<size_t>(col - 1);
    if (cursorOff > lineEnd)
        cursorOff = lineEnd;

    // Expand left from cursor to find identifier start.
    size_t idStart = cursorOff;
    while (idStart > lineStart && isIdentChar(source[idStart - 1]))
        --idStart;

    // Expand right from cursor to find identifier end.
    size_t idEnd = cursorOff;
    while (idEnd < lineEnd && isIdentChar(source[idEnd]))
        ++idEnd;

    if (idStart == idEnd)
        return ctx; // cursor on whitespace/operator

    ctx.identifier = source.substr(idStart, idEnd - idStart);
    ctx.valid = true;

    // Scan left past dots to capture dot-chain prefix (e.g., "shell.app").
    if (idStart > lineStart && source[idStart - 1] == '.') {
        size_t prefixEnd = idStart - 1; // position of the '.'
        size_t prefixStart = prefixEnd;
        // Walk backwards through identifiers and dots.
        while (prefixStart > lineStart) {
            size_t p = prefixStart - 1;
            if (isIdentChar(source[p])) {
                while (p > lineStart && isIdentChar(source[p - 1]))
                    --p;
                prefixStart = p;
                if (p > lineStart && source[p - 1] == '.')
                    prefixStart = p - 1;
                else
                    break;
            } else {
                break;
            }
        }
        ctx.dotPrefix = source.substr(prefixStart, prefixEnd - prefixStart);
    }

    return ctx;
}

} // namespace zanna::server
