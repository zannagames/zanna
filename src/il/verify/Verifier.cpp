//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/verify/Verifier.cpp
// Purpose: Drive the multi-stage IL verifier and collate diagnostics into a
//          single Expected result.
// Links: docs/il/il-guide.md#verification
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the top-level IL verifier orchestration logic.
/// @details The verifier coordinates subsystem checks for externs, globals,
///          functions, and exception handlers before returning a consolidated
///          diagnostic outcome to tooling.

#include "il/verify/Verifier.hpp"

#include "il/core/Module.hpp"
#include "il/verify/DiagSink.hpp"
#include "il/verify/EhVerifier.hpp"
#include "il/verify/ExternVerifier.hpp"
#include "il/verify/FunctionVerifier.hpp"
#include "il/verify/GlobalVerifier.hpp"
#include "support/diag_expected.hpp"

#include <algorithm>
#include <string>
#include <utility>

using namespace il::core;

namespace il::verify {
namespace {
using il::support::Diag;
using il::support::Expected;

/// @brief Fill default verifier code and stage fields when absent.
/// @param diag Diagnostic to normalize by value.
/// @return Normalized diagnostic preserving any explicit fields.
Diag normalizeVerifierDiag(Diag diag) {
    if (diag.code.empty()) {
        diag.code = diag.severity == il::support::Severity::Warning ? "V-IL-WARN" : "V-IL-VERIFY";
    }
    if (diag.stage.empty())
        diag.stage = "verify";
    return diag;
}

/// @brief Compare diagnostics for exact deduplication.
/// @param lhs First diagnostic.
/// @param rhs Second diagnostic.
/// @return `true` when severity, code, message, and source coordinates match.
bool sameDiagnostic(const Diag &lhs, const Diag &rhs) {
    return lhs.severity == rhs.severity && lhs.code == rhs.code && lhs.message == rhs.message &&
           lhs.loc.file_id == rhs.loc.file_id && lhs.loc.line == rhs.loc.line &&
           lhs.loc.column == rhs.loc.column;
}

} // namespace

/// @brief Run the full IL verifier pipeline over a module.
///
/// @details Executes the extern, global, function, and exception-handler
/// verifiers through @ref verifyAll with a 50-diagnostic budget. The first error
/// becomes the returned failure and every other collected diagnostic is attached
/// as a note; warning-only output does not fail verification.
///
/// @param m Module to verify.
/// @return @c Expected success on clean modules; otherwise an aggregated error diagnostic.
Expected<void> Verifier::verify(const Module &m) {
    auto diagnostics = verifyAll(m, 50);
    /// @brief Select the first error-severity diagnostic.
    auto primaryIt = std::find_if(diagnostics.begin(), diagnostics.end(), [](const Diag &diag) {
        return diag.severity == il::support::Severity::Error;
    });
    if (primaryIt == diagnostics.end())
        return {};

    Diag primary = *primaryIt;
    if (diagnostics.size() > 1) {
        primary.message += "\n";
        primary.message += std::to_string(diagnostics.size() - 1);
        primary.message += " additional verifier diagnostic";
        if (diagnostics.size() != 2)
            primary.message += "s";
        primary.message += ":";
        for (const auto &diag : diagnostics) {
            if (sameDiagnostic(diag, primary))
                continue;
            primary.notes.push_back({diag.loc, diag.message});
        }
    }
    return Expected<void>{std::move(primary)};
}

/// @brief Run verifier stages and collect a bounded set of unique diagnostics.
/// @details Each stage may emit through a sink and return a failure. Both
///          channels are normalized and deduplicated before the next stage runs;
///          collection stops once @p maxDiagnostics is reached.
/// @param m Module to verify.
/// @param maxDiagnostics Maximum number of diagnostics retained.
/// @return Collected diagnostics in stage/report order.
std::vector<Diag> Verifier::verifyAll(const Module &m, size_t maxDiagnostics) {
    CollectingDiagSink sink;
    std::vector<Diag> diagnostics;

    /// @brief Drain one stage's sink and returned error into the result vector.
    /// @param result Structured success or failure returned by the stage.
    /// @return `true` when the collection budget has been reached.
    auto appendDiagnostics = [&](const Expected<void> &result) {
        bool capped = false;
        const auto captured = sink.diagnostics();
        sink.clear();
        for (const auto &diag : captured) {
            if (diagnostics.size() >= maxDiagnostics) {
                capped = true;
                continue;
            }
            Diag normalized = normalizeVerifierDiag(diag);
            /// @brief Detect an already collected diagnostic equal to @p normalized.
            const bool duplicate =
                std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diag &existing) {
                    return sameDiagnostic(existing, normalized);
                });
            if (!duplicate)
                diagnostics.push_back(std::move(normalized));
        }
        if (!result && diagnostics.size() < maxDiagnostics) {
            Diag resultDiag = normalizeVerifierDiag(result.error());
            /// @brief Detect an already collected diagnostic equal to @p resultDiag.
            const bool duplicate =
                std::any_of(diagnostics.begin(), diagnostics.end(), [&](const Diag &existing) {
                    return sameDiagnostic(existing, resultDiag);
                });
            if (!duplicate)
                diagnostics.push_back(std::move(resultDiag));
        } else if (!result) {
            capped = true;
        }
        return capped || diagnostics.size() >= maxDiagnostics;
    };

    ExternVerifier externVerifier;
    if (appendDiagnostics(externVerifier.run(m, sink)))
        return diagnostics;

    GlobalVerifier globalVerifier;
    if (appendDiagnostics(globalVerifier.run(m, sink)))
        return diagnostics;

    FunctionVerifier functionVerifier(externVerifier.externs(), globalVerifier.globals());
    if (appendDiagnostics(functionVerifier.run(m, sink)))
        return diagnostics;

    EhVerifier ehVerifier;
    appendDiagnostics(ehVerifier.run(m, sink));

    return diagnostics;
}

} // namespace il::verify
