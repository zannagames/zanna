//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/verify/DiagSink.hpp
// Purpose: Diagnostic infrastructure for the IL verifier -- defines the
//          DiagSink interface for decoupled error/warning reporting,
//          VerifyDiagCode enum for structured diagnostic codes,
//          CollectingDiagSink for in-memory accumulation, and factory
//          functions for constructing verifier diagnostics.
// Key invariants:
//   - DiagSink is a pure interface (virtual report()); implementations own
//     their storage strategy.
//   - CollectingDiagSink appends diagnostics; order is preserved.
// Ownership/Lifetime: DiagSink is a polymorphic base; callers own sinks.
//          CollectingDiagSink owns its diagnostic vector.
// Links: support/diag_expected.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares verifier diagnostic codes, factories, and sink abstractions.
/// @details Verifier components use these APIs to attach stable subsystem codes
///          and a `verify` stage to shared diagnostics while choosing whether
///          diagnostics are streamed, collected, or handled by another sink.

#pragma once

#include "support/diag_expected.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace il::verify {

/// @brief Identifier for structured verifier diagnostics.
enum class VerifyDiagCode {
    Unknown = 0,                ///< Unclassified diagnostic.
    EhStackUnderflow,           ///< Encountered eh.pop with an empty handler stack.
    EhStackLeak,                ///< Execution left a function with handlers still active.
    EhResumeTokenMissing,       ///< Resume.* executed without an active resume token.
    EhResumeTokenMismatch,      ///< Resume token does not match active EH provenance.
    EhHandlerInvalidEntry,      ///< Handler block entered without verified token forwarding.
    EhResumeTokenEscape,        ///< Resume token used outside allowed EH control flow.
    EhResumeLabelInvalidTarget, ///< resume.label target does not postdominate the faulting block.
    EhHandlerNotDominant,       ///< Handler block does not dominate a protected faulting block.
    EhHandlerUnreachable        ///< Handler block is not reachable from function entry.
};

/// @brief Convert a verifier diagnostic code to its textual prefix.
/// @param code Diagnostic code to translate.
/// @return Stable string view describing @p code.
std::string_view toString(VerifyDiagCode code);

/// @brief Construct a diagnostic tagged with a verifier code.
/// @param code Structured diagnostic code.
/// @param severity Diagnostic severity classification.
/// @param loc Optional source location associated with the diagnostic.
/// @param message Human readable payload appended after the code prefix.
/// @return Diagnostic ready for reporting through sinks or Expected values.
il::support::Diag makeVerifierDiag(VerifyDiagCode code,
                                   il::support::Severity severity,
                                   il::support::SourceLoc loc,
                                   std::string message);

/// @brief Convenience wrapper that constructs an error diagnostic for verifier failures.
/// @param code Structured diagnostic code.
/// @param loc Optional source location associated with the diagnostic.
/// @param message Human readable description appended after the code prefix.
/// @return Error severity diagnostic referencing the provided verifier code.
il::support::Diag makeVerifierError(VerifyDiagCode code,
                                    il::support::SourceLoc loc,
                                    std::string message);

/// @brief Interface for verifier components to report diagnostics without coupling to storage.
class DiagSink {
  public:
    /// @brief Destroy a sink through the polymorphic interface.
    virtual ~DiagSink() = default;

    /// @brief Report a diagnostic to the sink.
    /// @param diag Diagnostic to forward or store.
    virtual void report(il::support::Diag diag) = 0;
};

/// @brief Concrete sink that stores diagnostics in-memory for later inspection.
class CollectingDiagSink : public DiagSink {
  public:
    /// @brief Append @p diag to the collection.
    /// @param diag Diagnostic to store.
    void report(il::support::Diag diag) override;

    /// @brief Access the accumulated diagnostics.
    /// @return Immutable view of the recorded diagnostics.
    [[nodiscard]] const std::vector<il::support::Diag> &diagnostics() const;

    /// @brief Remove all stored diagnostics.
    void clear();

  private:
    /// @brief Diagnostics retained in report order.
    std::vector<il::support::Diag> diags_;
};

} // namespace il::verify
