//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares compiler-independent text scanning used by language-server
///        hover and cursor operations.
///
/// Coordinates are one-based byte positions matching compiler conventions.
/// Helpers operate directly on source text without AST dependencies, and every
/// returned string owns its contents.
///
/// @see ICompilerBridge.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

namespace zanna::server {

/// @brief Context extracted from cursor position for hover resolution.
struct HoverContext {
    std::string identifier; ///< Identifier under the cursor.
    std::string dotPrefix;  ///< Receiver chain, e.g. @c shell.app for @c shell.app.run.
    bool valid{false};      ///< False when no identifier intersects the cursor.
};

/// @brief Test whether a source byte belongs to an identifier.
/// @param c Candidate byte.
/// @return @c true for alphanumeric bytes or underscore.
bool isIdentChar(char c);

/// @brief Extract the identifier and dot-chain prefix at a cursor position.
/// @param source Full source text.
/// @param line One-based line number.
/// @param col One-based byte column, clamped to line end.
/// @return Owned context with identifier and optional receiver prefix; @c valid
///         is false when the coordinates or cursor target are unsuitable.
HoverContext extractIdentifierAtCursor(const std::string &source, int line, int col);

} // namespace zanna::server
