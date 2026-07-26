//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/support/diag_capture.cpp
// Purpose: Implement the deferred-diagnostic capture used by legacy-style APIs.
// Key invariants: Captured diagnostics are stored as plain text until converted
//                 back into @ref Diag objects; repeated conversions leave the
//                 buffered message intact so callers can print diagnostics
//                 multiple times.
// Ownership/Lifetime: @ref DiagCapture owns its stringstream buffer and borrows
//                     no external resources.  The bridging helper returns
//                     @ref Expected<void> values that outlive the capture object.
// Links: src/support/diag_capture.hpp, docs/internals/codemap.md#support-library
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the deferred-diagnostic sink used by text-only pipelines.
/// @details `il::support::DiagCapture` buffers formatted diagnostics in a
///          stringstream so subsystems that historically returned `bool`
///          success codes can surface richer error information.  These helpers
///          provide the bridge points that turn the buffered text back into the
///          structured `Diag` objects expected by the modern `Expected<void>`
///          workflow.

#include "support/diag_capture.hpp"

#include <sstream>
#include <string_view>
#include <utility>

namespace il::support {
namespace {
/// @brief Fallback message used when a legacy API fails silently.
constexpr std::string_view kEmptyCaptureFallback =
    "legacy operation failed without diagnostic output";

/// @brief Normalized payload extracted from captured legacy diagnostic text.
struct CapturedDiagnosticPayload {
    Severity severity = Severity::Error; ///< Severity parsed from a textual prefix.
    std::string message;                 ///< Message without redundant formatting.
};

/// @brief Strip a leading severity prefix from @p text.
/// @param text Message text that may begin with "error: ", "warning: ", or "note: ".
/// @param severity Severity to assign when @p prefix is present.
/// @param prefix Textual severity prefix to remove.
/// @return True when the prefix was found and stripped.
/// @details This helper handles diagnostics that were captured after they had
///          already been printed once, preventing converted diagnostics from
///          later rendering as "error: error: message".
bool stripSeverityPrefix(std::string &text,
                         Severity &severity,
                         Severity parsed,
                         std::string_view prefix) {
    if (text.size() >= prefix.size() && std::string_view{text}.substr(0, prefix.size()) == prefix) {
        text.erase(0, prefix.size());
        severity = parsed;
        return true;
    }
    return false;
}

/// @brief Check whether text before a severity marker looks like a source prefix.
/// @param prefix Text preceding a candidate marker such as ": error: ".
/// @return True when @p prefix resembles "file:line" or "file:line:column".
/// @details Captured legacy output may already have a compiler-style prefix, but
///          ordinary diagnostic messages can also contain the text ": error: ".
///          This helper requires trailing numeric coordinate fields before a
///          formatted-prefix strip is allowed.
bool looksLikeFormattedSourcePrefix(std::string_view prefix) {
    if (prefix.empty() || prefix.find('\n') != std::string_view::npos ||
        prefix.find('\r') != std::string_view::npos)
        return false;

    const size_t lastColon = prefix.rfind(':');
    if (lastColon == std::string_view::npos || lastColon + 1 >= prefix.size())
        return false;

    /// @brief Test whether a coordinate field contains one or more ASCII digits.
    /// @param text Candidate coordinate field.
    /// @return `true` when @p text is nonempty and entirely decimal digits.
    auto allDigits = [](std::string_view text) {
        if (text.empty())
            return false;
        for (unsigned char ch : text) {
            if (ch < '0' || ch > '9')
                return false;
        }
        return true;
    };

    const std::string_view lastField = prefix.substr(lastColon + 1);
    if (!allDigits(lastField))
        return false;

    const std::string_view beforeLast = prefix.substr(0, lastColon);
    const size_t lineColon = beforeLast.rfind(':');
    if (lineColon == std::string_view::npos || lineColon + 1 >= beforeLast.size())
        return true;
    return allDigits(beforeLast.substr(lineColon + 1));
}

/// @brief Strip a leading source prefix before a textual severity marker.
/// @param text Captured diagnostic text that may be "file:line: severity: message".
/// @param severity Severity assigned when a marker is found.
/// @param parsed Severity associated with @p marker.
/// @param marker Marker to find after a source-location prefix.
/// @return True when a marker was found and text before it was removed.
/// @details Legacy printers commonly emit fully formatted diagnostics.  Source
///          locations cannot be reconstructed here without a SourceManager, but
///          retaining the source prefix inside @ref Diag::message would duplicate
///          formatting when the diagnostic is printed again.
bool stripFormattedPrefix(std::string &text,
                          Severity &severity,
                          Severity parsed,
                          std::string_view marker) {
    const size_t pos = text.find(marker);
    if (pos == std::string::npos)
        return false;
    if (!looksLikeFormattedSourcePrefix(std::string_view{text}.substr(0, pos)))
        return false;
    text.erase(0, pos + marker.size());
    severity = parsed;
    return true;
}

/// @brief Remove formatting artifacts from captured legacy diagnostic text.
/// @param text Captured text emitted by a legacy bool-plus-ostream API.
/// @return Severity and message suitable for storing in a structured diagnostic.
/// @details Legacy helpers often write already-formatted diagnostics such as
///          "error: message\n". DiagCapture converts the text back into a
///          structured value, so this helper trims trailing line terminators and
///          strips severity/source prefixes to avoid printing duplicated headers.
CapturedDiagnosticPayload normalizeCapturedDiagnostic(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    Severity severity = Severity::Error;
    (void)(stripSeverityPrefix(text, severity, Severity::Error, "error: ") ||
           stripSeverityPrefix(text, severity, Severity::Warning, "warning: ") ||
           stripSeverityPrefix(text, severity, Severity::Note, "note: ") ||
           stripFormattedPrefix(text, severity, Severity::Error, ": error: ") ||
           stripFormattedPrefix(text, severity, Severity::Warning, ": warning: ") ||
           stripFormattedPrefix(text, severity, Severity::Note, ": note: "));

    if (text.empty())
        return {Severity::Error, std::string{kEmptyCaptureFallback}};
    return {severity, std::move(text)};
}
} // namespace

/// @brief Write the given diagnostic to the supplied output stream.
///
/// @details Simply forwards to @ref printDiag so all formatting (severity
///          strings, source location prefixes, newline handling) remains
///          centralised.  The capture's internal buffer is intentionally left
///          untouched so that emitting to multiple streams—stderr, logs, etc.—
///          is inexpensive and deterministic.
///
/// @param out Destination stream that receives the formatted diagnostic text.
/// @param diag Diagnostic instance to serialize.
void DiagCapture::printTo(std::ostream &out, const Diag &diag) {
    printDiag(diag, out);
}

/// @brief Convert the captured message into a Diagnostic value.
///
/// @details Packages the buffered string into a @ref Diag with error severity
///          using @ref makeError.  Because the stringstream remains untouched,
///          subsequent calls continue to observe the same captured payload—this
///          is important for callers that convert the message into both an error
///          return and a log entry.
///
/// @return Diagnostic containing a copy of the captured text.
Diag DiagCapture::toDiag() const {
    std::vector<Diag> diagnostics = toDiagnostics();
    Diag primary = std::move(diagnostics.front());
    for (size_t index = 1; index < diagnostics.size(); ++index) {
        primary.notes.push_back({diagnostics[index].loc, std::move(diagnostics[index].message)});
    }
    return primary;
}

/// @brief Convert every captured diagnostic line into a structured value.
///
/// @details Splits the captured stream on line boundaries and normalizes each
///          non-empty line independently.  If the legacy API failed silently, a
///          single fallback error is returned so callers never see an empty error
///          list for a failed operation.
///
/// @return Vector of diagnostics parsed from the capture buffer.
std::vector<Diag> DiagCapture::toDiagnostics() const {
    std::vector<Diag> diagnostics;
    std::istringstream input(ss.str());
    std::string line;
    while (std::getline(input, line)) {
        std::string trimmed = line;
        while (!trimmed.empty() && (trimmed.back() == '\n' || trimmed.back() == '\r'))
            trimmed.pop_back();
        if (trimmed.empty())
            continue;
        CapturedDiagnosticPayload payload = normalizeCapturedDiagnostic(std::move(line));
        diagnostics.push_back(Diag{payload.severity, std::move(payload.message), {}, {}});
    }

    if (diagnostics.empty()) {
        CapturedDiagnosticPayload payload = normalizeCapturedDiagnostic(ss.str());
        diagnostics.push_back(Diag{payload.severity, std::move(payload.message), {}, {}});
    }
    return diagnostics;
}

/// @brief Bridge a boolean success flag to an Expected<void> diagnostic result.
///
/// @details Normalises legacy APIs that report success with a boolean.  When
///          @p ok is @c true the function returns a default-constructed (success)
///          @ref Expected<void>.  Otherwise it invokes @ref DiagCapture::toDiag
///          to convert buffered text into an error payload.  The capture remains
///          unchanged so callers can still print or rewrap the diagnostic after
///          propagating the failure.
///
/// @param ok Boolean indicating whether the preceding operation succeeded.
/// @param capture Capture containing any error text produced by the operation.
/// @return Successful Expected when @p ok is true; otherwise an error payload.
Expected<void> capture_to_expected_impl(bool ok, DiagCapture &capture) {
    if (ok) {
        return Expected<void>{};
    }
    return Expected<void>{capture.toDiag()};
}
} // namespace il::support
