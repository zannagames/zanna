//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BasicDiagnosticMessages.hpp
// Purpose: Defines reusable diagnostic message identifiers for the BASIC front end.
// Key invariants: Message identifiers are stable and uniquely map to human-readable text.
// Ownership/Lifetime: Header-only constants; no dynamic ownership.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file BasicDiagnosticMessages.hpp
 * @brief Defines stable identifiers and default text for BASIC diagnostics.
 *
 * Each inline constant contains string views into static string literals and
 * can be shared across parser and semantic-analysis translation units without
 * allocation or initialization-order dependencies.
 */

#pragma once

#include <string_view>

namespace il::frontends::basic::diag {

/// @brief Compile-time description of a diagnostic message.
/// @invariant Both views refer to static storage for every declaration below.
struct Message {
    std::string_view id;   ///< Stable identifier printed by the diagnostic emitter.
    std::string_view text; ///< Primary human-readable message text.
};

/// @brief SELECT CASE selector must be numeric or string-compatible.
inline constexpr Message ERR_SelectCase_NonIntegerSelector{
    "ERR_SelectCase_NonIntegerSelector", "SELECT CASE selector must be numeric or string"};

/// @brief Duplicate CASE label encountered inside a SELECT CASE.
inline constexpr Message ERR_SelectCase_DuplicateLabel{"ERR_SelectCase_DuplicateLabel",
                                                       "Duplicate CASE label"};

/// @brief CASE range lower bound exceeds its upper bound.
inline constexpr Message ERR_SelectCase_InvalidRange{
    "ERR_SelectCase_InvalidRange", "CASE range lower bound must be <= upper bound"};

/// @brief CASE range overlaps an existing CASE label or range.
inline constexpr Message ERR_SelectCase_OverlappingRange{
    "ERR_SelectCase_OverlappingRange", "CASE range overlaps existing CASE labels or ranges"};

/// @brief Multiple CASE ELSE arms were found in the same SELECT CASE.
inline constexpr Message ERR_SelectCase_DuplicateElse{"ERR_SelectCase_DuplicateElse",
                                                      "duplicate CASE ELSE"};

/// @brief SELECT CASE statement was not terminated by END SELECT.
inline constexpr Message ERR_SelectCase_MissingEndSelect{
    "ERR_SelectCase_MissingEndSelect", "SELECT CASE missing END SELECT terminator"};

/// @brief String selector requires CASE labels to be string literals.
inline constexpr Message ERR_SelectCase_StringSelectorLabels{
    "ERR_SelectCase_StringSelectorLabels",
    "SELECT CASE on a string selector requires string literal CASE labels"};

/// @brief String CASE labels require a string selector.
inline constexpr Message ERR_SelectCase_StringLabelSelector{
    "ERR_SelectCase_StringLabelSelector",
    "String CASE labels require a string SELECT CASE selector"};

/// @brief Numeric and string CASE labels may not be mixed within a SELECT CASE.
inline constexpr Message ERR_SelectCase_MixedLabelTypes{"ERR_SelectCase_MixedLabelTypes",
                                                        "mixed-type SELECT CASE"};

/// @brief CASE statement lacked any labels before the body.
inline constexpr Message ERR_Case_EmptyLabelList{"ERR_Case_EmptyLabelList",
                                                 "CASE arm requires at least one label"};

} // namespace il::frontends::basic::diag
