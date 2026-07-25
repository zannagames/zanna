//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticDiagUtil.hpp
// Purpose: Small helpers to standardize formatting and emission of common
//          semantic diagnostics across the BASIC frontend.
// Key invariants:
//   - Candidate lists are sorted case-insensitively for determinism.
//   - Diagnostic codes and severities come from the generated catalog.
// Ownership/Lifetime:
//   - Header-only helpers own only their local strings and vectors.
//   - Diagnostic emission borrows the caller-supplied emitter for the call.
// Links: src/frontends/basic/DiagnosticEmitter.hpp,
//        include/zanna/diag/BasicDiag.hpp,
//        src/frontends/basic/SemanticAnalyzer_Namespace.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "zanna/diag/BasicDiag.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

/// @file
/// @brief Defines deterministic formatting and emission helpers for BASIC
///        semantic diagnostics.

namespace il::frontends::basic::semutil {

/// @brief Format a candidate list for ambiguity diagnostics.
/// @details Sorts candidates by a byte-wise, case-insensitive comparison,
///          converts each byte through the C locale's uppercase mapping, and
///          joins the results with @c ", ". The vector is accepted by value so
///          the caller's order and spellings are unchanged.
/// @param candidates Candidate names to normalize and format; duplicates are
///                   retained.
/// @return Uppercase, comma-separated candidates in case-insensitive order, or
///         an empty string when @p candidates is empty.
/// @note Characters are converted through @c unsigned @c char before calling
///       the C classification functions, avoiding undefined behavior for
///       negative signed-byte values.
inline std::string formatCandidateList(std::vector<std::string> candidates) {
    auto toLower = [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    };
    std::sort(
        candidates.begin(), candidates.end(), [&](const std::string &a, const std::string &b) {
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) {
                char ca = toLower(a[i]);
                char cb = toLower(b[i]);
                if (ca != cb)
                    return ca < cb;
            }
            return a.size() < b.size();
        });
    // Uppercase and join
    std::string out;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (i)
            out += ", ";
        for (char ch : candidates[i])
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(ch))));
    }
    return out;
}

/// @brief Emit E_NS_003 ambiguous type diagnostic via the shared emitter.
/// @details Looks up the severity, stable code, and message template for
///          @ref diag::BasicDiag::NsAmbiguousType, formats @p candidates with
///          @ref formatCandidateList, substitutes the type and candidate
///          fields, and forwards the result to @p emitter.
/// @param emitter Diagnostic sink that receives the completed record.
/// @param loc Source location at which the ambiguous type name begins.
/// @param length Number of source columns to underline.
/// @param typeName Type spelling inserted into the catalog message.
/// @param candidates Fully qualified candidates considered by name resolution.
/// @post Exactly one catalogued ambiguous-type diagnostic is submitted to
///       @p emitter.
inline void emitAmbiguousType(il::frontends::basic::DiagnosticEmitter &emitter,
                              il::support::SourceLoc loc,
                              uint32_t length,
                              const std::string &typeName,
                              const std::vector<std::string> &candidates) {
    using il::frontends::basic::diag::BasicDiag;
    const auto sev = il::frontends::basic::diag::getSeverity(BasicDiag::NsAmbiguousType);
    const auto code = std::string(il::frontends::basic::diag::getCode(BasicDiag::NsAmbiguousType));
    const auto cand = formatCandidateList(candidates);
    const auto msg = il::frontends::basic::diag::formatMessage(
        BasicDiag::NsAmbiguousType, {{"type", typeName}, {"candidates", cand}});
    emitter.emit(sev, code, loc, length, msg);
}

} // namespace il::frontends::basic::semutil
