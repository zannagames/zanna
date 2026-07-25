//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the DiagnosticEmitter class, which formats and reports
// BASIC frontend diagnostics with rich context and source location information.
//
// The DiagnosticEmitter provides user-friendly error reporting throughout the
// BASIC compilation pipeline, transforming raw diagnostic messages into
// formatted output with:
// - Source file location (filename, line number, column)
// - Error codes for programmatic error handling
// - Source line context with caret (^) highlighting
// - Severity levels (error, warning, note)
// - Diagnostic message text
//
// Output Format:
//   program.bas:10:5: error: undefined variable 'counter' [E1001]
//   FOR counter = 1 TO 10
//       ^
//
// Key Responsibilities:
// - Diagnostic formatting: Converts internal diagnostic representations into
//   human-readable messages with source context
// - Source line extraction: Retrieves the relevant source line for each
//   diagnostic location to show the error in context
// - Caret positioning: Computes column offsets to place the ^ marker under
//   the problematic token or expression
// - Diagnostic ordering: Maintains emission order for stable, predictable
//   output across compilation runs
// - Source caching: Stores source text per file ID to enable efficient
//   repeated line lookups during diagnostic reporting
//
// Integration:
// - Used by: Lexer, Parser, SemanticAnalyzer, Lowerer to report errors
// - Wraps: DiagnosticEngine for diagnostic collection and counting
// - Queries: SourceManager for file paths and locations
// - Outputs to: std::ostream (typically std::cerr for error messages)
//
// Design Notes:
// - Borrows DiagnosticEngine and SourceManager; does not own them
// - Caches source text per file ID to avoid repeated file I/O
// - Diagnostics are accumulated and can be emitted in batch or individually
// - Emission and source registration are unsynchronized and externally serialized
//
// Usage:
//   DiagnosticEmitter emitter(diagnosticEngine, sourceManager);
//   emitter.addSource(fileId, sourceText);
//   // During compilation:
//   emitter.emit(Severity::Error, "B1001", location, 1, "undefined variable");
//   // After compilation:
//   emitter.printAll(std::cerr);
//
//===----------------------------------------------------------------------===//

/**
 * @file DiagnosticEmitter.hpp
 * @brief Declares ordered BASIC diagnostic capture and caret-rich formatting.
 *
 * Each emission is forwarded immediately to a borrowed DiagnosticEngine and
 * copied into an ordered local record for later presentation against registered
 * source snapshots and borrowed SourceManager paths.
 */

#pragma once

#include "frontends/basic/Token.hpp"
#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace il::frontends::basic {

/// @brief Formats BASIC diagnostics with error codes and caret ranges.
/// @invariant Diagnostics are emitted in order and printed with original source line.
/// @ownership Borrows DiagnosticEngine and SourceManager; copies source text per file id.
/// @warning Instances are not internally synchronized.
class DiagnosticEmitter {
  public:
    /// @brief Create emitter forwarding counts to @p de and using @p sm for file paths.
    /// @param de Diagnostic engine collecting counts.
    /// @param sm Source manager providing file paths.
    /// @warning @p de and @p sm must both outlive this emitter.
    DiagnosticEmitter(il::support::DiagnosticEngine &de, const il::support::SourceManager &sm);

    /// @brief Register source text for a file id.
    /// @param fileId Identifier from SourceManager.
    /// @param source Full contents of the source file.
    /// @post Replaces any previously cached source for @p fileId.
    void addSource(uint32_t fileId, std::string source);

    /// @brief Emit diagnostic with @p code at @p loc covering @p length characters.
    /// @param sev Severity level.
    /// @param code BASIC error code (e.g., B1001).
    /// @param loc Start location of the diagnostic.
    /// @param length Number of characters to underline (0 -> 1 caret).
    /// @param message Human-readable explanation.
    void emit(il::support::Severity sev,
              std::string code,
              il::support::SourceLoc loc,
              uint32_t length,
              std::string message);

    /// @brief Emit standardized "expected vs got" parse diagnostic.
    /// @param got Actual token encountered.
    /// @param expect Expected token kind.
    /// @param loc Source location of the unexpected token.
    void emitExpected(TokenKind got, TokenKind expect, il::support::SourceLoc loc);

    /// @brief Print all diagnostics to @p os with source snippets.
    /// @details Preserves emission order and does not clear captured entries.
    /// @param os Output stream.
    void printAll(std::ostream &os) const;

    /// @brief Number of errors reported.
    /// @return Error count from the borrowed DiagnosticEngine, including errors
    ///         reported through other clients of that engine.
    size_t errorCount() const;

    /// @brief Number of warnings reported.
    /// @return Warning count from the borrowed DiagnosticEngine, including
    ///         warnings reported through other clients of that engine.
    size_t warningCount() const;

    /// @brief Format a file:line string for a SourceLoc using SourceManager paths.
    /// @details Returns "<path>:<line>" when file and line are available, otherwise
    ///          returns an empty string.
    /// @param loc Source location to format.
    /// @return String with path and line or empty string when unavailable.
    std::string formatFileLine(il::support::SourceLoc loc) const;

  private:
    /// @brief Diagnostic record captured for later printing.
    struct Entry {
        il::support::Severity severity{il::support::Severity::Error}; ///< Diagnostic severity.
        std::string code;               ///< Error code like B1001.
        std::string message;            ///< Description text.
        il::support::SourceLoc loc;     ///< Start source location.
        uint32_t length{0};             ///< Number of characters to mark.
    };

    /// @brief Retrieve full line text for @p fileId at @p line.
    /// @param fileId Identifier from SourceManager.
    /// @param line 1-based line number to fetch.
    /// @return Line contents without trailing newline; empty for an unregistered
    ///         file, an unavailable line, or BASIC's unlabeled-line sentinel.
    std::string getLine(uint32_t fileId, uint32_t line) const;

    ///< Borrowed engine receiving immediate structured diagnostics.
    il::support::DiagnosticEngine &de_;                 ///< Underlying diagnostic engine.
    ///< Borrowed manager used only for path/location formatting.
    const il::support::SourceManager &sm_;              ///< Source manager for file paths.
    ///< Owning presentation records in emission order.
    std::vector<Entry> entries_;                        ///< Diagnostics in emission order.
    ///< Owning source snapshot keyed by file id.
    std::unordered_map<uint32_t, std::string> sources_; ///< Source text per file id.
};

} // namespace il::frontends::basic
