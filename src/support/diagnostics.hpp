//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: support/diagnostics.hpp
// Purpose: Declares diagnostic engine for errors and warnings.
// Key invariants: errorCount()/warningCount() always equal the number of stored
//                 diagnostics at each severity; printAll preserves report order.
// Ownership/Lifetime: Engine owns collected diagnostics.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares Zanna's ordered, in-memory diagnostic representation.
/// @details Diagnostics retain both human-readable text and structured source
///          metadata for terminal and machine-oriented renderers. The engine
///          owns reported records, preserves insertion order, and maintains
///          constant-time severity counts for pipeline decisions.

#pragma once

#include "source_location.hpp"
#include <ostream>
#include <string>
#include <vector>

/// @brief Records diagnostics and prints them later.
/// @invariant Counts reflect reported diagnostics.
/// @ownership Owns stored diagnostic messages.
namespace il::support {

class SourceManager;

/// @brief Severity levels assigned to primary and secondary diagnostics.
/// @details The ordering has no ranking semantics; consumers should switch on
///          the Note, Warning, and Error enumerators rather than comparing
///          their underlying values.
enum class Severity { Note, Warning, Error };

/// @brief Secondary diagnostic note attached to a primary diagnostic.
/// @details Notes preserve related context without being counted as independent
///          errors or warnings by @ref DiagnosticEngine.
struct DiagnosticNote {
    /// @brief Optional location associated with the note.
    SourceLoc loc{};

    /// @brief Human-readable explanatory text.
    std::string message;
};

/// @brief Machine-readable source replacement hint attached to a diagnostic.
/// @details A consumer applies @ref replacement to @ref range. An invalid range
///          denotes insertion at the owning diagnostic's primary location.
struct DiagnosticFixIt {
    /// @brief Source range to replace. Empty/invalid means insert at the diagnostic location.
    SourceRange range{};

    /// @brief Replacement text.
    std::string replacement;

    /// @brief Human-readable description of the fix.
    std::string message;
};

/// @brief Single diagnostic message with location.
/// @details The primary severity, message, and location can be augmented with a
///          stable code, precise range, explanatory notes, pipeline stage, help
///          text, and zero or more independently described source replacements.
struct Diagnostic {
    Severity severity = Severity::Error; ///< Message severity.
    std::string message;                 ///< Human-readable problem description.
    SourceLoc loc;                       ///< Optional primary source location.
    std::string code;                    ///< Optional stable code such as `"B1001"`.
    SourceRange range{};                 ///< Optional source span to underline.
    std::vector<DiagnosticNote> notes{}; ///< Related explanatory locations.
    std::string stage{}; ///< Optional producing pipeline stage.
    std::string help{};  ///< Optional remediation text or URL.
    std::vector<DiagnosticFixIt> fixits{}; ///< Optional machine-readable fix suggestions.
};

/// @brief Collects diagnostics and prints them in order.
/// @details Reporting transfers a diagnostic into engine-owned storage and
///          updates cached error or warning counts. Notes do not affect either
///          count because only independently reported records are tallied.
class DiagnosticEngine {
  public:
    /// @brief Record diagnostic @p d.
    /// @param d Diagnostic to store.
    /// @details Appends the record without reordering existing diagnostics and
    ///          increments the counter corresponding to its primary severity.
    void report(Diagnostic d);

    /// @brief Print all recorded diagnostics to stream @p os.
    /// @param os Output stream receiving diagnostics in report order.
    /// @param sm Optional source manager used to resolve locations and snippets.
    /// @details Delegates each record to the canonical diagnostic text printer.
    void printAll(std::ostream &os, const SourceManager *sm = nullptr) const;

    /// @brief Number of errors reported.
    /// @return Count of records reported with error severity.
    size_t errorCount() const;

    /// @brief Number of warnings reported.
    /// @return Count of records reported with warning severity.
    size_t warningCount() const;

    /// @brief Access collected diagnostics for inspection.
    /// @return Const reference to the internal diagnostics vector.
    /// @warning The reference and its elements can be invalidated by a later
    ///          call to @ref report that reallocates the vector.
    [[nodiscard]] const std::vector<Diagnostic> &diagnostics() const {
        return diags_;
    }

  private:
    std::vector<Diagnostic> diags_;
    size_t errors_ = 0;
    size_t warnings_ = 0;
};
} // namespace il::support
