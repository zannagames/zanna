//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticDiagnostics.cpp
// Purpose: Provide convenience wrappers that format and forward BASIC semantic
//          diagnostics to the shared DiagnosticEmitter.
// Key invariants:
//   - Catalogued diagnostics preserve the catalog's code and severity.
//   - Count queries always reflect the shared emitter's current state.
// Ownership/Lifetime:
//   - SemanticDiagnostics borrows an externally owned DiagnosticEmitter, which
//     must outlive the wrapper.
// Links: src/frontends/basic/SemanticDiagnostics.hpp,
//        src/frontends/basic/DiagnosticEmitter.hpp,
//        include/zanna/diag/BasicDiag.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements formatting helpers for BASIC semantic diagnostics.
/// @details Centralises message templates and emission logic so semantic
///          analysis code can focus on correctness checks while delegating user
///          messaging to this component.

#include "frontends/basic/SemanticDiagnostics.hpp"

#include <string>
#include <utility>

namespace il::frontends::basic {

/// @brief Construct the helper that forwards diagnostics to @p emitter.
/// @details Stores a reference to the provided emitter so later convenience
///          calls can forward diagnostics without additional wiring.  The
///          caller retains ownership of the emitter and must guarantee it
///          outlives the helper.
/// @param emitter Diagnostic sink borrowed for this object's lifetime.
SemanticDiagnostics::SemanticDiagnostics(DiagnosticEmitter &emitter) : emitter_(emitter) {}

/// @brief Emit a diagnostic by delegating to the shared emitter.
/// @details Forwards all arguments verbatim to `DiagnosticEmitter::emit`,
///          preserving severity, code, source range, and message formatting.
///          The helper exists primarily so callers do not need to include the
///          emitter header when only semantic diagnostics are required.
/// @param sev Severity recorded for the diagnostic.
/// @param code Stable diagnostic code transferred to the emitter.
/// @param loc Source location at which the diagnostic begins.
/// @param length Number of source columns to underline.
/// @param message Human-readable message transferred to the emitter.
/// @post The underlying emitter has processed exactly one diagnostic request.
void SemanticDiagnostics::emit(il::support::Severity sev,
                               std::string code,
                               il::support::SourceLoc loc,
                               uint32_t length,
                               std::string message) {
    emitter_.emit(sev, std::move(code), loc, length, std::move(message));
}

/// @brief Emit a catalogued BASIC diagnostic identified by @p diag.
/// @details Retrieves severity, code, and message template from the generated
///          catalog before forwarding the formatted diagnostic to the shared
///          emitter.  Callers supply placeholder substitutions via
///          @p replacements; unspecified placeholders are left intact so specs
///          can enforce required fields.
/// @param diag Catalog identifier describing the diagnostic to emit.
/// @param loc Source location associated with the diagnostic.
/// @param length Number of characters to underline.
/// @param replacements Placeholder substitutions applied to the message.
/// @post Exactly one diagnostic request is forwarded with catalog-derived
///       severity and code.
void SemanticDiagnostics::emit(diag::BasicDiag diag,
                               il::support::SourceLoc loc,
                               uint32_t length,
                               std::initializer_list<diag::Replacement> replacements) {
    auto message = diag::formatMessage(diag, replacements);
    emit(
        diag::getSeverity(diag), std::string(diag::getCode(diag)), loc, length, std::move(message));
}

/// @brief Retrieve the number of error diagnostics recorded so far.
/// @details Pass-through convenience wrapper over
///          `DiagnosticEmitter::errorCount()` that keeps semantic analysis
///          consumers decoupled from the emitter API.
/// @return Current cumulative error count reported by the borrowed emitter.
size_t SemanticDiagnostics::errorCount() const {
    return emitter_.errorCount();
}

/// @brief Retrieve the number of warning diagnostics recorded so far.
/// @details Mirrors @ref errorCount by forwarding to the underlying emitter to
///          keep count queries consistent and centralised.
/// @return Current cumulative warning count reported by the borrowed emitter.
size_t SemanticDiagnostics::warningCount() const {
    return emitter_.warningCount();
}

/// @brief Produce a formatted error message for a non-boolean conditional expression.
/// @details Expands the NonBooleanConditionMessage template by replacing the
///          `{type}` and `{expr}` placeholders.  The helper isolates the string
///          manipulation steps so diagnostic emission sites remain concise and
///          uniform.
/// @param typeName Inferred BASIC type inserted into the message.
/// @param exprText Suggested expression text inserted into the message.
/// @return Newly allocated message with the first occurrence of each template
///         placeholder replaced.
std::string SemanticDiagnostics::formatNonBooleanCondition(std::string_view typeName,
                                                           std::string_view exprText) {
    std::string message(NonBooleanConditionMessage);
    const auto replace =
        [](std::string &subject, std::string_view placeholder, std::string_view value) {
            if (auto pos = subject.find(placeholder); pos != std::string::npos) {
                subject.replace(pos, placeholder.size(), value);
            }
        };
    replace(message, "{type}", typeName);
    replace(message, "{expr}", exprText);
    return message;
}

/// @brief Emit a diagnostic indicating that a conditional expression was not boolean.
/// @details Relies on @ref formatNonBooleanCondition to prepare the message and
///          then issues an error severity diagnostic.  The wrapper ensures both
///          the formatting and emission logic stay consistent across all call
///          sites.
/// @param code Stable diagnostic code transferred to the emitter.
/// @param loc Source location at which the condition begins.
/// @param length Number of source columns to underline.
/// @param typeName Inferred BASIC type used to format the message.
/// @param exprText Suggested expression text used to format the message.
/// @post Exactly one error-severity diagnostic request is forwarded.
void SemanticDiagnostics::emitNonBooleanCondition(std::string code,
                                                  il::support::SourceLoc loc,
                                                  uint32_t length,
                                                  std::string_view typeName,
                                                  std::string_view exprText) {
    emit(il::support::Severity::Error,
         std::move(code),
         loc,
         length,
         formatNonBooleanCondition(typeName, exprText));
}

/// @brief Access the underlying diagnostic emitter.
/// @details Provides mutable access for scenarios where clients need more than
///          the thin wrappers supplied here (for example, to install listeners).
/// @return Borrowed mutable reference valid while the emitter and this wrapper
///         remain alive.
DiagnosticEmitter &SemanticDiagnostics::emitter() {
    return emitter_;
}

} // namespace il::frontends::basic
