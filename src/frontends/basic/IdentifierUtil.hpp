//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file provides utilities for canonicalizing BASIC identifiers and
// qualified names according to BASIC's case-insensitive language semantics.
//
// BASIC Identifier Rules:
// BASIC is a case-insensitive language where identifiers like "Counter",
// "COUNTER", and "counter" all refer to the same variable. The frontend
// canonicalizes identifiers through the C character-classification APIs for
// consistent symbol table lookups and IL name generation.
//
// Key Utilities:
// - CanonicalizeIdent / Canon: validate and case-fold one identifier
// - JoinQualified / JoinDots: join segments with '.' separators
// - CanonicalizeQualified / CanonJoin: canonicalize and join segments
// - SplitDots: split qualified names while preserving empty segments
// - StripTypeSuffix: remove one trailing BASIC type suffix
//
// Canonicalization:
// Accepted identifier bytes are folded with std::tolower for:
// - Symbol table lookups (case-insensitive matching)
// - IL name generation (deterministic output)
// - Namespace resolution (consistent qualified names)
//
// Validation:
// The first byte must satisfy std::isalpha or be '_'; later bytes must satisfy
// std::isalnum or be '_'. Type suffixes are handled separately.
//
// Qualified Names:
// For namespace support (NAMESPACE...END NAMESPACE), qualified names use
// dot notation:
//   MyNamespace.MyModule.MyFunction
//
// The canonicalization preserves namespace hierarchy while applying lowercase
// conversion to each segment.
//
// Error Handling:
// Validation is conservative: functions return empty string on failure,
// leaving detailed error reporting to callers (parser, semantic analyzer).
//
// Integration:
// - Used by: Parser for identifier processing
// - Used by: SemanticAnalyzer for symbol table operations
// - Used by: Lowerer for IL name generation
//
// Design Notes:
// - Header-only implementation for zero overhead
// - No dynamic state; all functions are pure
// - Validation failures return empty string for caller handling
//
//===----------------------------------------------------------------------===//

/**
 * @file IdentifierUtil.hpp
 * @brief Defines BASIC identifier validation, case folding, and path utilities.
 *
 * The header contains stateless, owning transformations. Character validity and
 * case conversion follow `std::isalpha`, `std::isalnum`, and `std::tolower`
 * after conversion to unsigned char.
 */

#pragma once

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace il::frontends::basic {

/// @brief Canonicalize a single identifier through locale-aware byte folding.
/// @details The first byte must satisfy `std::isalpha` or be `_`; remaining
///          bytes must satisfy `std::isalnum` or be `_`. Accepted bytes are
///          folded with `std::tolower`. Empty input and validation failure both
///          return an empty string.
/// @param ident Input identifier text.
/// @return Lowercased identifier or empty on invalid characters.
inline std::string CanonicalizeIdent(std::string_view ident) {
    if (ident.empty())
        return std::string{};
    std::string out;
    out.reserve(ident.size());
    bool first = true;
    for (unsigned char ch : ident) {
        if ((first && (std::isalpha(ch) || ch == '_')) ||
            (!first && (std::isalnum(ch) || ch == '_'))) {
            out.push_back(static_cast<char>(std::tolower(ch)));
        } else {
            return std::string{}; // invalid
        }
        first = false;
    }
    return out;
}

/// @brief Canonicalize a single identifier (alias of CanonicalizeIdent).
/// @param ident Identifier bytes to validate and fold.
/// @return CanonicalizeIdent(@p ident).
inline std::string Canon(std::string_view ident) {
    return CanonicalizeIdent(ident);
}

/// @brief Join qualified name segments with '.' separators.
/// @details Does not validate or canonicalize segments and preserves empty
///          segments as adjacent, leading, or trailing dots.
/// @param parts Qualified path segments in declaration order.
/// @return Dot-joined name; empty when @p parts is empty.
inline std::string JoinQualified(const std::vector<std::string> &parts) {
    if (parts.empty()) {
        return std::string{};
    }
    size_t total = 0;
    for (const auto &p : parts)
        total += p.size();
    std::string out;
    out.reserve(total + (parts.size() - 1));
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i)
            out.push_back('.');
        out.append(parts[i]);
    }
    return out;
}

/// @brief Join qualified name segments with '.' (alias of JoinQualified).
/// @param parts Path segments to join verbatim.
/// @return JoinQualified(@p parts).
inline std::string JoinDots(const std::vector<std::string> &parts) {
    return JoinQualified(parts);
}

/// @brief Canonicalize each segment then join as a fully-qualified name.
/// @details Each nonempty segment is validated via @ref CanonicalizeIdent.
///          Empty segments are accepted and preserved by JoinQualified().
/// @param parts Qualified path segments in declaration order.
/// @return Folded, dot-joined name; empty when a nonempty segment is invalid or
///         when @p parts itself is empty.
inline std::string CanonicalizeQualified(const std::vector<std::string> &parts) {
    std::vector<std::string> canon;
    canon.reserve(parts.size());
    for (const auto &p : parts) {
        std::string c = CanonicalizeIdent(p);
        if (c.empty() && !p.empty()) {
            return std::string{};
        }
        canon.emplace_back(std::move(c));
    }
    return JoinQualified(canon);
}

/// @brief Canonicalize each segment and join with '.' separators.
/// @details Equivalent to CanonicalizeQualified.
/// @param parts Qualified path segments.
/// @return CanonicalizeQualified(@p parts).
inline std::string CanonJoin(const std::vector<std::string> &parts) {
    return CanonicalizeQualified(parts);
}

/// @brief Split a dot-joined string into segments, preserving empty segments.
/// @param dotted Qualified path like "A.B.C".
/// @return Vector of segments in order. Empty input returns one empty segment;
///         adjacent, leading, and trailing dots also produce empty segments.
inline std::vector<std::string> SplitDots(std::string_view dotted) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : dotted) {
        if (ch == '.') {
            out.emplace_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    out.emplace_back(std::move(cur));
    return out;
}

/// @brief Strip BASIC type suffix from an identifier.
/// @details Removes trailing %, &, !, #, or $ if present.
///          These suffixes denote types in BASIC (Integer, Long, Single, Double, String).
///          At most one byte is removed and the remaining text is not validated.
/// @param ident Input identifier with possible type suffix.
/// @return Identifier without the type suffix.
inline std::string StripTypeSuffix(std::string_view ident) {
    if (ident.empty())
        return std::string{};
    char last = ident.back();
    if (last == '$' || last == '%' || last == '#' || last == '!' || last == '&')
        return std::string{ident.substr(0, ident.size() - 1)};
    return std::string{ident};
}

} // namespace il::frontends::basic
