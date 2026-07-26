//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/diag_capture.hpp
// Purpose: Provide a capture-only diagnostic sink to bridge legacy bool plus ostream APIs.
// Key invariants: Captured lines can be normalized into multiple structured
//                 diagnostics; the legacy single-Diag bridge retains later
//                 messages as notes on the primary error.
// Ownership/Lifetime: DiagCapture is a value type that owns its buffered text; it
//                     borrows the caller's output stream only for the call.
// Links: docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares deferred diagnostic capture and legacy-result adaptation.
/// @details `DiagCapture` owns text emitted by bool-plus-ostream APIs and can
///          normalize it into one or many structured diagnostics. The templated
///          bridge invokes legacy work and returns `Expected<void>` without
///          consuming the reusable capture buffer.

#pragma once

#include "support/diag_expected.hpp"

#include <ostream>
#include <sstream>
#include <utility>
#include <vector>

namespace il::support {
/// @brief Sink that accumulates diagnostic text for later conversion.
struct DiagCapture {
    /// @brief Accumulated diagnostic text from legacy helpers.
    std::ostringstream ss;

    /// @brief Forward a diagnostic to an output stream using standard formatting.
    /// @param out Stream receiving the formatted diagnostic.
    /// @param diag Diagnostic to print.
    void printTo(std::ostream &out, const Diag &diag);

    /// @brief Convert the captured text into an error diagnostic without a location.
    /// @return Diagnostic containing the captured message.
    /// @details When multiple legacy diagnostics were captured, the first becomes
    ///          the primary diagnostic and the remaining messages are attached as
    ///          notes so callers using the historical single-diagnostic API still
    ///          retain the extra context.
    [[nodiscard]] Diag toDiag() const;

    /// @brief Convert captured text into all structured diagnostics it contains.
    /// @return Diagnostics parsed from the capture buffer, or one fallback error.
    /// @details Legacy APIs usually emit one diagnostic per line.  This helper
    ///          preserves each line as an independent structured diagnostic while
    ///          normalizing already-formatted severity/source prefixes.
    [[nodiscard]] std::vector<Diag> toDiagnostics() const;
};

/// @brief Helper bridging legacy bool-returning APIs to Expected<void>.
/// @param ok Result flag reported by the legacy call.
/// @param capture Diagnostic capture containing any emitted message.
/// @return Empty Expected on success; diagnostic payload on failure.
Expected<void> capture_to_expected_impl(bool ok, DiagCapture &capture);

/// @brief Adapt a legacy bool plus ostream diagnostic API to Expected<void>.
/// @tparam F Callable type invoked with an std::ostream& to perform the legacy work.
/// @param legacyCall Callable that returns true on success and writes diagnostics on failure.
/// @return Empty Expected on success; diagnostic payload converted from captured text on failure.
///
/// This function must remain inline because it is a template instantiated with
/// arbitrary functors across translation units; moving it to a source file
/// would break those instantiations.
template <class F> inline Expected<void> capture_to_expected(F &&legacyCall) {
    DiagCapture capture;
    bool ok = std::forward<F>(legacyCall)(capture.ss);
    return capture_to_expected_impl(ok, capture);
}

} // namespace il::support
