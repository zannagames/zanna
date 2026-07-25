//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/DiagnosticEmitter.cpp
// Purpose: Capture, forward, format, and print BASIC diagnostics with source
//          snippets, caret annotations, and codes.
// Ownership/Lifetime: Owns presentation entries and source snapshots; borrows
//                     the diagnostic engine and source manager.
// Links: src/frontends/basic/DiagnosticEmitter.hpp,
//        src/frontends/common/DiagnosticFormatter.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Diagnostic emitter used by the BASIC front end.
/// @details Wraps @ref il::support::DiagnosticEngine to capture diagnostics,
///          store full source text, and later print caret-highlighted output.

#include "frontends/basic/DiagnosticEmitter.hpp"
#include "frontends/basic/LineUtils.hpp"
#include "frontends/common/DiagnosticFormatter.hpp"
#include <algorithm>
#include <sstream>

namespace il::frontends::basic {

/// @brief Initialize an emitter bound to a diagnostic engine and source manager.
/// @details Stores references to the engine and manager so later emissions can
///          print caret information without additional wiring.
/// @param de Engine used to report diagnostics immediately.
/// @param sm Manager providing file path lookups for caret output.
DiagnosticEmitter::DiagnosticEmitter(il::support::DiagnosticEngine &de,
                                     const il::support::SourceManager &sm)
    : de_(de), sm_(sm) {}

/// @brief Register full source text for a file identifier.
/// @details Caches the provided buffer so caret printing can fetch the
///          surrounding line quickly.
/// @param fileId Key used by diagnostics to reference this source.
/// @param source Contents of the file to enable caret printing.
void DiagnosticEmitter::addSource(uint32_t fileId, std::string source) {
    sources_[fileId] = std::move(source);
}

/// @brief Report a diagnostic and store it for later printing.
/// @details Immediately forwards the diagnostic to the underlying engine and
///          records a copy so caret-formatted output can be produced in order of
///          emission.
/// @param sev Severity level of the diagnostic.
/// @param code Project-defined diagnostic code.
/// @param loc Source location associated with the issue.
/// @param length Highlight length; zero defaults to one caret.
/// @param message Human-readable explanation of the problem.
void DiagnosticEmitter::emit(il::support::Severity sev,
                             std::string code,
                             il::support::SourceLoc loc,
                             uint32_t length,
                             std::string message) {
    il::support::SourceRange range{};
    if (loc.isValid()) {
        const uint32_t caretLength = length == 0 ? 1 : length;
        range = il::support::SourceRange{
            loc,
            il::support::SourceLoc{loc.file_id, loc.line, loc.column + caretLength},
        };
    }
    il::support::Diagnostic diag{sev, message, loc, code};
    diag.range = range;
    diag.stage = "basic";
    de_.report(std::move(diag));
    entries_.push_back({sev, std::move(code), std::move(message), loc, length});
}

/// @brief Emit an error when a different token was encountered than expected.
/// @param got Token actually observed.
/// @param expect Token that was required.
/// @param loc Source location of the unexpected token.
/// @details Formats diagnostic B0001, describing the mismatch between expected
///          and observed tokens.
void DiagnosticEmitter::emitExpected(TokenKind got, TokenKind expect, il::support::SourceLoc loc) {
    std::string msg;
    if (expect == TokenKind::Number && got == TokenKind::Identifier) {
        msg = "expected label or number";
    } else {
        msg = std::string("expected ") + tokenKindToString(expect) + ", got " +
              tokenKindToString(got);
    }
    emit(il::support::Severity::Error, "B0001", loc, 0, std::move(msg));
}

// Severity-to-string and source-line extraction are provided by the common
// DiagnosticFormatter header. The namespace alias keeps call sites concise.
namespace dfmt = common::diag_fmt;

/// @brief Retrieve a specific line from stored source text.
/// @details Delegates to the common DiagnosticFormatter utility after checking
///          for unlabeled BASIC lines.
/// @param fileId Source file identifier.
/// @param line 1-based line number to fetch.
/// @return Line contents or empty string if unavailable.
std::string DiagnosticEmitter::getLine(uint32_t fileId, uint32_t line) const {
    if (isUnlabeledLine(line))
        return {};

    auto it = sources_.find(fileId);
    if (it == sources_.end())
        return {};
    return dfmt::getSourceLine(it->second, line);
}

/// @brief Print all collected diagnostics with caret markers.
/// @details Delegates to the common DiagnosticFormatter for the actual formatting
///          of each entry, using a BASIC-specific line-skip predicate for
///          unlabeled lines.
/// @param os Output stream receiving formatted diagnostics.
void DiagnosticEmitter::printAll(std::ostream &os) const {
    for (const auto &e : entries_) {
        /// Suppress location text for BASIC's synthetic unlabeled-line sentinel.
        std::string locStr =
            dfmt::formatLocation(sm_, e.loc, [](uint32_t line) { return isUnlabeledLine(line); });
        std::string line = getLine(e.loc.file_id, e.loc.line);
        dfmt::printDiagnostic(
            os, e.severity, e.code, e.message, locStr, line, e.loc.column, e.length);
    }
}

/// @brief Retrieve the number of errors emitted so far.
/// @details Forwards to the underlying diagnostic engine so callers can decide
///          whether the compilation pipeline should continue.
/// @return Count of error-severity diagnostics from the underlying engine.
size_t DiagnosticEmitter::errorCount() const {
    return de_.errorCount();
}

/// @brief Retrieve the number of warnings emitted so far.
/// @details Mirrors @ref errorCount but for warning-severity diagnostics.
/// @return Count of warning-severity diagnostics from the underlying engine.
size_t DiagnosticEmitter::warningCount() const {
    return de_.warningCount();
}

/// @brief Format a path:line string for a source location.
/// @details Delegates to the common DiagnosticFormatter utility.
/// @param loc Source location whose file and line are formatted.
/// @return Path-and-line text or an empty string when unavailable.
std::string DiagnosticEmitter::formatFileLine(il::support::SourceLoc loc) const {
    return dfmt::formatFileLine(sm_, loc);
}

} // namespace il::frontends::basic
