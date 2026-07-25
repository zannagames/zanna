//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticDiagnostics.hpp
// Purpose: Declares the semantic-analysis diagnostic facade used to forward
//          raw and catalogued BASIC diagnostics through a shared emitter.
// Key invariants:
//   - Raw emission preserves every caller-supplied diagnostic field.
//   - Typed emission derives severity and stable code from BasicDiag metadata.
//   - Error and warning counts remain owned by the underlying emitter.
// Ownership/Lifetime:
//   - SemanticDiagnostics stores a non-owning reference; the emitter must
//     outlive the facade.
// Links: src/frontends/basic/SemanticDiagnostics.cpp,
//        src/frontends/basic/DiagnosticEmitter.hpp,
//        include/zanna/diag/BasicDiag.hpp,
//        src/frontends/basic/SemanticAnalyzer.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "zanna/diag/BasicDiag.hpp"

#include <initializer_list>
#include <string>
#include <string_view>

/// @file
/// @brief Declares BASIC semantic diagnostic formatting and forwarding helpers.

namespace il::frontends::basic {

/// @brief Thin semantic-analysis facade over a shared diagnostic emitter.
/// @details Supports direct diagnostic forwarding, generated-catalog emission,
///          counter access, and the legacy non-boolean-condition formatter.
///          The facade does not buffer, suppress, or otherwise reinterpret
///          diagnostics.
/// @invariant @ref emitter_ refers to a live @ref DiagnosticEmitter.
class SemanticDiagnostics {
  public:
    /// @brief Binds the facade to an externally owned diagnostic emitter.
    /// @param emitter Sink borrowed for the complete lifetime of this facade.
    explicit SemanticDiagnostics(DiagnosticEmitter &emitter);

    /// @brief Message template for non-boolean conditions.
    /// @details @c "{type}" and @c "{expr}" are replaced by
    ///          @ref formatNonBooleanCondition.
    static constexpr std::string_view NonBooleanConditionMessage =
        "Expected BOOLEAN condition, got {type}. Suggestion: use {expr} <> 0.";

    /// @brief Forwards one fully specified diagnostic to the bound emitter.
    /// @param sev Severity assigned by the caller.
    /// @param code Stable diagnostic code transferred into the emitter.
    /// @param loc Source location at which the diagnostic begins.
    /// @param length Number of source columns to underline.
    /// @param message Human-readable message transferred into the emitter.
    void emit(il::support::Severity sev,
              std::string code,
              il::support::SourceLoc loc,
              uint32_t length,
              std::string message);

    /// @brief Formats and emits one generated-catalog BASIC diagnostic.
    /// @param diag Catalog entry supplying severity, code, and message template.
    /// @param loc Source location at which the diagnostic begins.
    /// @param length Number of source columns to underline.
    /// @param replacements Placeholder substitutions for the catalog message.
    void emit(diag::BasicDiag diag,
              il::support::SourceLoc loc,
              uint32_t length,
              std::initializer_list<diag::Replacement> replacements = {});

    /// @brief Returns the cumulative number of emitted errors.
    /// @return Current error count from the bound emitter.
    [[nodiscard]] size_t errorCount() const;

    /// @brief Returns the cumulative number of emitted warnings.
    /// @return Current warning count from the bound emitter.
    [[nodiscard]] size_t warningCount() const;

    /// @brief Format the NonBooleanCondition diagnostic message.
    /// @param typeName Describes the inferred BASIC type.
    /// @param exprText Expression text used for suggestions.
    /// @return Newly allocated message with the template placeholders
    ///         substituted.
    [[nodiscard]] static std::string formatNonBooleanCondition(std::string_view typeName,
                                                               std::string_view exprText);

    /// @brief Emit the NonBooleanCondition diagnostic with formatted message.
    /// @param code Diagnostic code to emit (e.g., E1001).
    /// @param loc Source location associated with the condition.
    /// @param length Number of characters to underline.
    /// @param typeName BASIC type name for the offending expression.
    /// @param exprText Textual representation of the expression.
    void emitNonBooleanCondition(std::string code,
                                 il::support::SourceLoc loc,
                                 uint32_t length,
                                 std::string_view typeName,
                                 std::string_view exprText);

    /// @brief Exposes the bound emitter for operations outside this facade.
    /// @return Borrowed mutable reference with the emitter's original lifetime.
    [[nodiscard]] DiagnosticEmitter &emitter();

  private:
    /// Non-owning diagnostic sink shared with the semantic analyzer.
    DiagnosticEmitter &emitter_;
};

} // namespace il::frontends::basic
