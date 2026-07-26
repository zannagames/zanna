//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/lsp-common/DiagnosticUtils.cpp
// Purpose: Implementation of shared diagnostic extraction utility.
// Key invariants:
//   - Severity mapping: Note=0, Warning=1, Error=2
//   - End range fields are populated only for concrete, ordered ranges that
//     live in the same file as the primary location
// Ownership/Lifetime:
//   - All returned data is fully owned
// Links: tools/lsp-common/DiagnosticUtils.hpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements conversion from compiler diagnostics to server value types.

#include "tools/lsp-common/DiagnosticUtils.hpp"

#include "support/diagnostics.hpp"
#include "support/source_manager.hpp"

namespace zanna::server {

/// @brief Resolve a file id to its registered path, or empty when unknown.
/// @param sm Optional source manager owning file registrations.
/// @param fileId One-based source file id; zero denotes no file.
/// @return Registered path copied into owned storage, or empty when unavailable.
static std::string resolvePath(const il::support::SourceManager *sm, uint32_t fileId) {
    if (!sm || fileId == 0)
        return {};
    return std::string(sm->getPath(fileId));
}

/// @brief Convert all engine diagnostics into fully owning server records.
/// @param diag Diagnostic engine to traverse in emission order.
/// @param sm Optional source manager for primary and note file paths.
/// @return Converted diagnostics preserving messages, ranges, notes, and fix-its.
std::vector<DiagnosticInfo> extractDiagnostics(const il::support::DiagnosticEngine &diag,
                                               const il::support::SourceManager *sm) {
    std::vector<DiagnosticInfo> result;
    for (const auto &d : diag.diagnostics()) {
        DiagnosticInfo info;
        switch (d.severity) {
            case il::support::Severity::Note:
                info.severity = 0;
                break;
            case il::support::Severity::Warning:
                info.severity = 1;
                break;
            case il::support::Severity::Error:
                info.severity = 2;
                break;
        }
        info.message = d.message;
        info.file = resolvePath(sm, d.loc.file_id);
        info.line = d.loc.line;
        info.column = d.loc.column;
        info.code = d.code;
        info.stage = d.stage;
        info.help = d.help;

        // Only forward machine-usable ranges: concrete (strictly ordered) and
        // anchored in the same file as the primary location.
        if (d.range.isConcrete() &&
            (d.loc.file_id == 0 || d.range.begin.file_id == d.loc.file_id)) {
            info.endLine = d.range.end.line;
            info.endColumn = d.range.end.column;
        }

        info.notes.reserve(d.notes.size());
        for (const auto &n : d.notes) {
            DiagnosticNoteInfo note;
            note.message = n.message;
            note.file = resolvePath(sm, n.loc.file_id);
            note.line = n.loc.line;
            note.column = n.loc.column;
            info.notes.push_back(std::move(note));
        }

        info.fixits.reserve(d.fixits.size());
        for (const auto &f : d.fixits) {
            DiagnosticFixItInfo fix;
            fix.message = f.message;
            fix.replacement = f.replacement;
            if (f.range.isConcrete()) {
                fix.line = f.range.begin.line;
                fix.column = f.range.begin.column;
                fix.endLine = f.range.end.line;
                fix.endColumn = f.range.end.column;
            } else if (f.range.isInsertion()) {
                fix.line = f.range.begin.line;
                fix.column = f.range.begin.column;
            } else {
                // Fall back to the diagnostic's primary location as an insertion point.
                fix.line = d.loc.line;
                fix.column = d.loc.column;
            }
            info.fixits.push_back(std::move(fix));
        }

        result.push_back(std::move(info));
    }
    return result;
}

} // namespace zanna::server
