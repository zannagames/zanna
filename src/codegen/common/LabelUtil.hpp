//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/LabelUtil.hpp
// Purpose: Small helpers for generating assembler-safe labels.
// Key invariants: Output labels are always non-empty and valid for GAS/NASM;
//                 no digit-leading labels are emitted.
// Ownership/Lifetime: Header-only stateless utilities with no global state.
// Links: codegen/x86_64/EmitAsm.cpp, codegen/aarch64/EmitAsm.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>
#include <string_view>

/// @file
/// @brief Provides deterministic assembler-label sanitization helpers.

/// @brief Utilities for generating assembler-compatible labels from arbitrary strings.
///
/// Assembly language has strict requirements for label names that differ from
/// high-level language identifiers. This namespace provides functions to transform
/// IL function names, block labels, and other identifiers into valid assembly labels.
namespace zanna::codegen::common {

/// @brief Append assembler-safe characters from @p fragment to @p out.
/// @details Applies the same character policy used by @ref sanitizeLabel:
///          alphanumerics, underscore, period, and dollar are preserved, while
///          hyphens and all other characters are converted to underscores. This
///          helper intentionally does not apply the leading-digit rule; callers
///          apply that rule once to the full label stem before appending any
///          suffix.
/// @param out Destination label buffer.
/// @param fragment Input label fragment to sanitize and append.
/// @post @p out retains its existing prefix and gains exactly one output
///       character per byte in @p fragment.
inline void appendSanitizedLabelFragment(std::string &out, std::string_view fragment) {
    for (unsigned char ch : fragment) {
        const bool isAlpha = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        const bool isDigit = (ch >= '0' && ch <= '9');
        if (isAlpha || isDigit || ch == '_' || ch == '.' || ch == '$')
            out.push_back(static_cast<char>(ch));
        else
            out.push_back('_');
    }
}

/// @brief Transforms an arbitrary string into a valid assembler label.
///
/// Assembly labels typically have restrictions on allowed characters and cannot
/// start with digits. This function sanitizes input strings to comply with common
/// assembler requirements (GAS, NASM, LLVM assembly, etc.):
///
/// **Character Handling:**
/// - Alphanumeric characters [A-Za-z0-9] are preserved as-is
/// - Underscores (_), periods (.), and dollar signs ($) are preserved
/// - Hyphens (-) are replaced with underscores (common in IL block names like "entry-0")
/// - All other characters are replaced with underscores (_)
///
/// **Label Validity:**
/// - If the result would start with a digit, an 'L' prefix is prepended
/// - Empty input produces "L" as the output
///
/// **Suffix Support:**
/// - An optional suffix is sanitized with the same character rules and appended
/// - Useful for generating unique labels (e.g., "_entry", "_exit", "_123")
///
/// @par Example Usage:
/// @code
/// sanitizeLabel("main")           // Returns "main"
/// sanitizeLabel("entry-0")        // Returns "entry_0" (hyphen to underscore)
/// sanitizeLabel("123start")       // Returns "L123start" (L prefix added)
/// sanitizeLabel("foo::bar")       // Returns "foo__bar" (colons -> underscores)
/// sanitizeLabel("loop", "_42")    // Returns "loop_42" (suffix appended)
/// @endcode
///
/// @param in The input string to sanitize (function name, block label, etc.).
/// @param suffix Optional suffix to append to the sanitized label.
/// @return A valid assembler label string.
///
/// @note The resulting label is always non-empty due to the 'L' prefix rule.
/// @see IL function names use Zanna.Namespace.Function format which becomes
///      Zanna_Namespace_Function after sanitization.
inline std::string sanitizeLabel(std::string_view in, std::string_view suffix = {}) {
    std::string out;
    out.reserve(in.size() + suffix.size() + 2);

    appendSanitizedLabelFragment(out, in);
    if (out.empty() || (out[0] >= '0' && out[0] <= '9'))
        out.insert(out.begin(), 'L');
    if (!suffix.empty()) {
        appendSanitizedLabelFragment(out, suffix);
    }
    return out;
}

} // namespace zanna::codegen::common
