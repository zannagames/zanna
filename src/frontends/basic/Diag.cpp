//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Diag.cpp
// Purpose: Implement standardized BASIC procedure, type, and alias diagnostics.
// Ownership/Lifetime: Messages are built locally and moved into a borrowed emitter.
// Links: src/frontends/basic/Diag.hpp,
//        src/frontends/basic/DiagnosticEmitter.hpp,
//        src/frontends/basic/IdentifierUtil.hpp
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements BASIC frontend diagnostic helpers.
/// @details The helpers in this file construct consistent, actionable error and
///          note messages for common semantic scenarios. Each routine formats
///          text, computes appropriate source spans, and delegates emission to
///          the provided diagnostic emitter.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/Diag.hpp"
#include "frontends/basic/IdentifierUtil.hpp"

#include <algorithm>

namespace il::frontends::basic::diagx {

/// @brief Report a duplicate procedure definition with both source locations.
/// @details Emits a single error that names the procedure, lists the original
///          definition location, and places the caret at the second definition
///          to direct users to the conflicting declaration.
/// @param emitter Diagnostic sink used to emit the error.
/// @param qname Fully-qualified procedure name.
/// @param first Source location of the first definition.
/// @param second Source location of the duplicate definition.
void ErrorDuplicateProc(DiagnosticEmitter &emitter,
                        std::string_view qname,
                        il::support::SourceLoc first,
                        il::support::SourceLoc second) {
    // Compose single actionable message including both definition locations.
    std::string whereFirst = emitter.formatFileLine(first);
    std::string whereSecond = emitter.formatFileLine(second);
    std::string msg = std::string("duplicate procedure '") + std::string(qname) +
                      "' first defined at " + (whereFirst.empty() ? "?" : whereFirst) +
                      ", again at " + (whereSecond.empty() ? "?" : whereSecond);
    emitter.emit(il::support::Severity::Error,
                 "B1004",
                 second,
                 static_cast<uint32_t>(qname.size()),
                 std::move(msg));
}

/// @brief Report an unknown procedure reference with candidate names.
/// @details Canonicalizes the identifier when possible and appends a list of
///          fully-qualified candidates that were attempted during lookup. The
///          caret location is anchored at the unresolved reference.
/// @param emitter Diagnostic sink used to emit the error.
/// @param loc Source location of the unresolved identifier.
/// @param ident Identifier text as written in source.
/// @param tried Fully-qualified procedure names that were considered.
void ErrorUnknownProc(DiagnosticEmitter &emitter,
                      il::support::SourceLoc loc,
                      std::string_view ident,
                      const std::vector<std::string> &tried) {
    // Canonicalize head identifier if possible to maintain consistency.
    std::string head = CanonicalizeIdent(ident);
    if (head.empty())
        head.assign(ident.begin(), ident.end());

    std::string msg = std::string("unknown procedure '") + head + "'";
    if (!tried.empty()) {
        msg += " (tried: ";
        for (std::size_t i = 0; i < tried.size(); ++i) {
            if (i)
                msg += ", ";
            msg += tried[i];
        }
        msg += ')';
    }
    emitter.emit(il::support::Severity::Error,
                 "B1006",
                 loc,
                 static_cast<uint32_t>(head.size() > 0 ? head.size() : ident.size()),
                 std::move(msg));
}

/// @brief Report an unknown qualified procedure reference.
/// @details Emits a concise error for a fully-qualified name without attempting
///          any canonicalization or candidate listing.
/// @param emitter Diagnostic sink used to emit the error.
/// @param loc Source location of the unresolved reference.
/// @param qname Fully-qualified procedure name as written/resolved.
void ErrorUnknownProcQualified(DiagnosticEmitter &emitter,
                               il::support::SourceLoc loc,
                               std::string_view qname) {
    emitter.emit(il::support::Severity::Error,
                 "B1006",
                 loc,
                 static_cast<uint32_t>(qname.size()),
                 std::string("unknown procedure '") + std::string(qname) + "'");
}

/// @brief Format a truncated "tried" list for diagnostic messages.
/// @details Produces a comma-separated list capped at @p limit entries and
///          appends a "+N more" suffix when additional candidates exist.
/// @param tried Candidate strings to format.
/// @param limit Maximum number of entries to include before truncating.
/// @return Parenthesized suffix suitable for appending to an error message.
static std::string formatTriedList(const std::vector<std::string> &tried, std::size_t limit = 8) {
    if (tried.empty())
        return {};
    std::string s = " (tried: ";
    std::size_t n = std::min(tried.size(), limit);
    for (std::size_t i = 0; i < n; ++i) {
        if (i)
            s += ", ";
        s += tried[i];
    }
    if (tried.size() > limit) {
        s += ", +" + std::to_string(tried.size() - limit) + " more";
    }
    s += ")";
    return s;
}

/// @brief Report an unknown procedure with a potentially long tried list.
/// @details Canonicalizes the identifier if possible and appends a truncated
///          candidate list generated by @ref formatTriedList so diagnostics stay
///          readable even when many overloads are present.
/// @param emitter Diagnostic sink used to emit the error.
/// @param loc Source location of the unresolved identifier.
/// @param ident Identifier text as written in source.
/// @param tried Fully-qualified procedure names that were considered.
void ErrorUnknownProcWithTries(DiagnosticEmitter &emitter,
                               il::support::SourceLoc loc,
                               std::string_view ident,
                               const std::vector<std::string> &tried) {
    std::string head = CanonicalizeIdent(ident);
    if (head.empty())
        head.assign(ident.begin(), ident.end());
    std::string msg = std::string("unknown procedure '") + head + "'" + formatTriedList(tried);
    emitter.emit(il::support::Severity::Error,
                 "B1006",
                 loc,
                 static_cast<uint32_t>(head.size() > 0 ? head.size() : ident.size()),
                 std::move(msg));
}

/// @brief Report an ambiguous procedure reference with sorted matches.
/// @details Sorts the candidate list deterministically and emits a single error
///          that lists all possible matches so users can disambiguate.
/// @param emitter Diagnostic sink used to emit the error.
/// @param loc Source location of the ambiguous reference.
/// @param ident Identifier text as written in source.
/// @param matches Candidate procedure names that match the call site.
void ErrorAmbiguousProc(DiagnosticEmitter &emitter,
                        il::support::SourceLoc loc,
                        std::string_view ident,
                        std::vector<std::string> matches) {
    std::sort(matches.begin(), matches.end());
    std::string msg = std::string("ambiguous procedure '") + std::string(ident) + "' — matches: ";
    for (std::size_t i = 0; i < matches.size(); ++i) {
        if (i)
            msg += ", ";
        msg += matches[i];
    }
    emitter.emit(il::support::Severity::Error,
                 "B2009",
                 loc,
                 static_cast<uint32_t>(ident.size()),
                 std::move(msg));
}

/// @brief Report an unknown type name with a truncated candidate list.
/// @details Formats the identifier as written and appends the formatted list of
///          namespace-qualified candidates that were attempted.
/// @param emitter Diagnostic sink used to emit the error.
/// @param loc Source location of the unresolved type identifier.
/// @param ident Type name as written in source.
/// @param tried Fully-qualified type names that were considered.
void ErrorUnknownTypeWithTries(DiagnosticEmitter &emitter,
                               il::support::SourceLoc loc,
                               std::string_view ident,
                               const std::vector<std::string> &tried) {
    std::string head(ident.begin(), ident.end());
    std::string msg = std::string("unknown type '") + head + "'" + formatTriedList(tried);
    emitter.emit(il::support::Severity::Error,
                 "B2111",
                 loc,
                 static_cast<uint32_t>(head.size()),
                 std::move(msg));
}

/// @brief Emit a note explaining how an alias expanded.
/// @details Used to show the fully-qualified name substituted for a namespace
///          alias when reporting related errors.
/// @param emitter Diagnostic sink used to emit the note.
/// @param alias The alias as written in source.
/// @param targetQn Fully-qualified namespace substituted for the alias.
void NoteAliasExpansion(DiagnosticEmitter &emitter,
                        std::string_view alias,
                        std::string_view targetQn) {
    std::string msg = std::string("alias '") + std::string(alias) + "' -> " + std::string(targetQn);
    emitter.emit(il::support::Severity::Note, "N0001", {}, 0, std::move(msg));
}

/// @brief Report a user procedure that shadows a builtin extern.
/// @details Emitted when a user-defined procedure collides with a seeded
///          `Zanna.*` runtime helper, which would otherwise hide the builtin.
/// @param emitter Diagnostic sink used to emit the error.
/// @param qname Fully-qualified procedure name that caused the conflict.
/// @param loc Source location of the user-defined declaration.
void ErrorBuiltinShadow(DiagnosticEmitter &emitter,
                        std::string_view qname,
                        il::support::SourceLoc loc) {
    // Emit a distinct diagnostic when a user-defined procedure attempts to shadow
    // a builtin extern (seeded from Zanna.* runtime registry). Keep the message
    // concise and actionable.
    std::string msg =
        std::string("user procedure shadows builtin extern '") + std::string(qname) + "'";
    emitter.emit(il::support::Severity::Error,
                 "E_ZANNA_BUILTIN_SHADOW",
                 loc,
                 static_cast<uint32_t>(qname.size()),
                 std::move(msg));
}

} // namespace il::frontends::basic::diagx
