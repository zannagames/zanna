//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the diagnostic sink helpers shared across verifier components.
// These helpers convert verifier-specific diagnostic codes into user-facing
// strings and provide a simple sink that collects diagnostics for later
// inspection.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Diagnostic sinks and helpers for the IL verifier.
/// @details The functions here translate enumerated diagnostic codes into
///          textual prefixes, build @ref il::support::Diag instances tagged with
///          the verifier namespace, and provide a basic sink implementation that
///          retains diagnostics in arrival order.

#include "il/verify/DiagSink.hpp"

#include <utility>

namespace {
/// @brief Map a verifier diagnostic code to its string prefix.
/// @details The verifier exposes a small catalogue of diagnostic codes that are
///          grouped by subsystem.  This helper translates the enumerator into a
///          stable string used as part of the user-facing diagnostic identifier
///          (for example "verify.eh.underflow").
/// @param code Verifier diagnostic category to translate.
/// @return Stable prefix for a known code, or an empty view for `Unknown` and
///         unrecognized enum values.
std::string_view diagCodeToPrefix(il::verify::VerifyDiagCode code) {
    using il::verify::VerifyDiagCode;
    switch (code) {
        case VerifyDiagCode::Unknown:
            return {};
        case VerifyDiagCode::EhStackUnderflow:
            return "verify.eh.underflow";
        case VerifyDiagCode::EhStackLeak:
            return "verify.eh.unreleased";
        case VerifyDiagCode::EhResumeTokenMissing:
            return "verify.eh.resume_token_missing";
        case VerifyDiagCode::EhResumeTokenMismatch:
            return "verify.eh.resume_token_mismatch";
        case VerifyDiagCode::EhHandlerInvalidEntry:
            return "verify.eh.handler_invalid_entry";
        case VerifyDiagCode::EhResumeTokenEscape:
            return "verify.eh.resume_token_escape";
        case VerifyDiagCode::EhResumeLabelInvalidTarget:
            return "verify.eh.resume_label_target";
        case VerifyDiagCode::EhHandlerNotDominant:
            return "verify.eh.handler_not_dominant";
        case VerifyDiagCode::EhHandlerUnreachable:
            return "verify.eh.handler_unreachable";
    }
    return {};
}
} // namespace

namespace il::verify {

/// @brief Convert a diagnostic code into its string representation.
/// @details Thin wrapper that forwards to @ref diagCodeToPrefix so external
///          callers do not need to reach into the anonymous namespace when they
///          only care about the string form of the code.
/// @param code Verifier diagnostic category to translate.
/// @return Stable textual prefix, or an empty view when no prefix is defined.
std::string_view toString(VerifyDiagCode code) {
    return diagCodeToPrefix(code);
}

/// @brief Construct a diagnostic value tagged with the verifier namespace.
/// @details Uses the Diagnostic struct's code field to store the verifier code
///          prefix, ensuring consistent formatting through the shared printDiag()
///          function.  The code appears as `[verify.eh.underflow]` in the output.
/// @param code Verifier category converted into the diagnostic code field.
/// @param severity Severity assigned to the resulting diagnostic.
/// @param loc Source location copied into the diagnostic.
/// @param message Human-readable payload moved into the diagnostic.
/// @return Diagnostic with its stage set to `verify`.
il::support::Diag makeVerifierDiag(VerifyDiagCode code,
                                   il::support::Severity severity,
                                   il::support::SourceLoc loc,
                                   std::string message) {
    const std::string_view prefix = diagCodeToPrefix(code);
    il::support::Diag diag{severity, std::move(message), loc, std::string(prefix)};
    diag.stage = "verify";
    return diag;
}

/// @brief Convenience wrapper that always reports an error severity.
/// @details Calls @ref makeVerifierDiag with @ref il::support::Severity::Error,
///          saving callers from repeating the severity constant at each call
///          site.
/// @param code Verifier category converted into the diagnostic code field.
/// @param loc Source location copied into the diagnostic.
/// @param message Human-readable payload moved into the diagnostic.
/// @return Error-severity diagnostic with its stage set to `verify`.
il::support::Diag makeVerifierError(VerifyDiagCode code,
                                    il::support::SourceLoc loc,
                                    std::string message) {
    return makeVerifierDiag(code, il::support::Severity::Error, loc, std::move(message));
}

/// @brief Append a diagnostic to the collection in arrival order.
/// @details The sink simply stores diagnostics in a vector, preserving the order
///          they were reported so clients can iterate deterministically.
/// @param diag Diagnostic moved into the retained collection.
void CollectingDiagSink::report(il::support::Diag diag) {
    diags_.push_back(std::move(diag));
}

/// @brief Access the accumulated diagnostics without copying.
/// @details Returns a reference to the internal vector so callers can inspect or
///          iterate the collected diagnostics without incurring a copy.
/// @return Const reference valid until the sink is destroyed or a subsequent
///         mutating operation invalidates the vector's storage.
const std::vector<il::support::Diag> &CollectingDiagSink::diagnostics() const {
    return diags_;
}

/// @brief Clear all stored diagnostics.
/// @details Empties the backing container, allowing the same sink instance to be
///          reused across multiple verification runs.
void CollectingDiagSink::clear() {
    diags_.clear();
}

} // namespace il::verify
