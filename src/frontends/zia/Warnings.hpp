//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Warnings.hpp
/// @brief Warning codes, names, and policy for the Zia compiler.
///
/// @details Defines individual warning codes (W001–W019) with human-readable
/// names, plus a WarningPolicy struct that controls which warnings are enabled,
/// whether warnings are treated as errors, and per-warning suppression.
///
/// Each warning has:
///   - A numeric code (W001, W002, ...) for user reference and suppression
///   - A slug name ("unused-variable", "float-equality", ...) for CLI use
///   - A default-enabled state (conservative set on by default, noisy set -Wall only)
///
/// @see WarningSuppressions.hpp — inline comment-based suppression.
/// @see Sema.hpp — `warn()` method consumes this infrastructure.
///
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_set>

namespace il::frontends::zia {

/// @brief Individual warning codes for the Zia compiler.
/// @details Each code corresponds to a specific class of suspicious-but-legal
/// code that the compiler can detect and report.
enum class WarningCode : uint16_t {
    W001_UnusedVariable = 1,
    W002_UnreachableCode = 2,
    W003_ImplicitNarrowing = 3,
    W004_VariableShadowing = 4,
    W005_FloatEquality = 5,
    W006_EmptyLoopBody = 6,
    W007_AssignmentInCondition = 7,
    W008_MissingReturn = 8,
    W009_SelfAssignment = 9,
    W010_DivisionByZero = 10,
    W011_RedundantBoolComparison = 11,
    W012_DuplicateImport = 12,
    W013_EmptyBody = 13,
    W014_UnusedResult = 14,
    W015_UninitializedVariable = 15,
    W016_OptionalWithoutCheck = 16,
    W017_XorConfusion = 17,
    W018_BitwiseAndConfusion = 18,
    W019_NonExhaustiveMatch = 19,
};

/// @brief Total number of defined warning codes.
inline constexpr uint16_t kWarningCodeCount = 19;

/// @brief Get the diagnostic code string for a warning (e.g., "W001").
/// @param code The warning code.
/// @return Null-terminated string like "W001". Returns "W???" for unknown codes.
const char *warningCodeStr(WarningCode code);

/// @brief Get the human-readable slug name for a warning (e.g., "unused-variable").
/// @param code The warning code.
/// @return Null-terminated slug string. Returns "unknown" for unknown codes.
const char *warningName(WarningCode code);

/// @brief Parse a warning code from a string.
/// @details Accepts both numeric codes ("W001") and slug names ("unused-variable").
/// @param name The string to parse.
/// @return The parsed warning code, or std::nullopt if not recognized.
std::optional<WarningCode> parseWarningCode(std::string_view name);

/// @brief Policy controlling which warnings are enabled and their severity.
/// @details Default-constructed policy enables the conservative default set.
/// Use `-Wall` to enable all warnings, `-Werror` to treat warnings as errors,
/// and `-Wno-XXXX` to disable specific warnings.
struct WarningPolicy {
    /// @brief Enable all warnings (corresponds to `-Wall`).
    bool enableAll{false};

    /// @brief Treat warnings as errors (corresponds to `-Werror`).
    bool warningsAsErrors{false};

    /// @brief Treat safety-critical warnings as errors.
    bool strictSafetyWarnings{false};

    /// @brief Set of explicitly disabled warning codes (from `-Wno-XXX`).
    std::unordered_set<WarningCode> disabled;

    /// @brief Check if a specific warning code is enabled under this policy.
    /// @param code The warning code to check.
    /// @return true if the warning should be emitted.
    bool isEnabled(WarningCode code) const;

    /// @brief Get the set of warnings enabled by default (without `-Wall`).
    /// @return Reference to the static default-enabled set.
    static const std::unordered_set<WarningCode> &defaultEnabled();
};

} // namespace il::frontends::zia
