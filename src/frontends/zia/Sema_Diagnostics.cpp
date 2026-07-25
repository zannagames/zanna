//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Diagnostics.cpp
/// @brief Coded semantic diagnostics, warning policy handling, duplicate-definition reporting,
/// did-you-mean suggestions, and scope-end source ranges for Zia Sema.
///
/// @details This file was split out of Sema.cpp to keep semantic analysis
/// responsibilities navigable without changing the Sema public interface or
/// diagnostic behavior. Member functions remain declared in Sema.hpp.
///
/// @see frontends/zia/Sema.hpp
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"

#include <algorithm>
#include <limits>

namespace il::frontends::zia {

namespace {

/// @brief Three-way compare two source locations by (file, line, column).
/// @param a Left source location.
/// @param b Right source location.
/// @return -1 if @p a precedes @p b, +1 if it follows, 0 if identical.
int compareLoc(const SourceLoc &a, const SourceLoc &b) {
    if (a.file_id != b.file_id)
        return (a.file_id < b.file_id) ? -1 : 1;
    if (a.line != b.line)
        return (a.line < b.line) ? -1 : 1;
    if (a.column != b.column)
        return (a.column < b.column) ? -1 : 1;
    return 0;
}

/// @brief Compute Levenshtein edit distance with a two-row dynamic-programming table.
/// @param lhs Left spelling candidate.
/// @param rhs Right spelling candidate.
/// @return Number of single-character edits needed to transform @p lhs into @p rhs.
size_t editDistance(std::string_view lhs, std::string_view rhs) {
    std::vector<size_t> previous(rhs.size() + 1);
    std::vector<size_t> current(rhs.size() + 1);
    for (size_t j = 0; j <= rhs.size(); ++j)
        previous[j] = j;
    for (size_t i = 1; i <= lhs.size(); ++i) {
        current[0] = i;
        for (size_t j = 1; j <= rhs.size(); ++j) {
            const size_t substitution = previous[j - 1] + (lhs[i - 1] == rhs[j - 1] ? 0 : 1);
            current[j] = std::min({previous[j] + 1, current[j - 1] + 1, substitution});
        }
        previous.swap(current);
    }
    return previous[rhs.size()];
}

} // namespace

//=============================================================================
// Error Reporting
//=============================================================================

/// @brief Report a semantic warning at a source location (legacy).
/// @param loc Source location attached to the warning.
/// @param message Human-readable warning text.
/// @details Emits the legacy `V3001` warning without applying warning-policy or suppression
///          filtering.
void Sema::warning(SourceLoc loc, const std::string &message) {
    il::support::Diagnostic diag{il::support::Severity::Warning, message, loc, "V3001"};
    diag.stage = "sema";
    diag_.report(std::move(diag));
}

/// @brief Report a coded warning with policy and suppression checks.
/// @param code Stable warning identifier.
/// @param loc Source location attached to the warning.
/// @param message Human-readable warning text.
/// @details Disabled and locally suppressed warnings are discarded. Enabled warnings are
///          promoted to errors under `-Werror` or strict safety policy as applicable.
void Sema::warn(WarningCode code, SourceLoc loc, const std::string &message) {
    // Check policy: is this warning enabled?
    if (warningPolicy_) {
        if (!warningPolicy_->isEnabled(code))
            return;
    } else {
        // No policy set — use default conservative set
        if (WarningPolicy::defaultEnabled().count(code) == 0)
            return;
    }

    // Check inline suppression
    if (suppressions_.isSuppressed(code, loc))
        return;

    auto isSafetyCritical = [](WarningCode warning) {
        switch (warning) {
            case WarningCode::W008_MissingReturn:
            case WarningCode::W010_DivisionByZero:
            case WarningCode::W015_UninitializedVariable:
            case WarningCode::W016_OptionalWithoutCheck:
            case WarningCode::W019_NonExhaustiveMatch:
                return true;
            default:
                return false;
        }
    };

    // Determine severity: Warning or Error (-Werror or strict safety diagnostics).
    auto sev =
        (warningPolicy_ && (warningPolicy_->warningsAsErrors ||
                            (warningPolicy_->strictSafetyWarnings && isSafetyCritical(code))))
            ? il::support::Severity::Error
            : il::support::Severity::Warning;

    if (sev == il::support::Severity::Error)
        hasError_ = true;

    il::support::Diagnostic diag{sev, message, loc, warningCodeStr(code)};
    diag.stage = "sema";
    diag_.report(std::move(diag));
}

/// @brief Check for unused variables in a scope and emit W001 warnings.
/// @param scope Scope whose local symbols are inspected.
/// @details Excludes discard names, intentionally unused parameters, synthetic `self`, externs,
///          and non-variable symbols.
void Sema::checkUnusedVariables(const Scope &scope) {
    for (const auto &[name, sym] : scope.getSymbols()) {
        // Only check variables and parameters
        if (sym.kind != Symbol::Kind::Variable && sym.kind != Symbol::Kind::Parameter)
            continue;

        // Skip the discard name "_"
        if (name == "_")
            continue;

        // Parameters whose names begin with '_' are intentionally unused.
        // Keep local variables stricter so accidental dead locals still surface.
        if (sym.kind == Symbol::Kind::Parameter && !name.empty() && name[0] == '_')
            continue;

        // Instance methods permit implicit field/member access without writing
        // `self.`, so warning on the synthetic receiver parameter is noise.
        if (sym.kind == Symbol::Kind::Parameter && name == "self")
            continue;

        // Skip extern/runtime symbols
        if (sym.isExtern)
            continue;

        if (!sym.used) {
            std::string what = (sym.kind == Symbol::Kind::Parameter) ? "Parameter" : "Variable";
            SourceLoc loc = sym.loc.isValid() ? sym.loc : (sym.decl ? sym.decl->loc : SourceLoc{});
            warn(WarningCode::W001_UnusedVariable,
                 loc,
                 what + " '" + name + "' is declared but never used");
        }
    }
}

/// @brief Report a semantic error at a source location.
/// @param loc Primary error location.
/// @param message Human-readable error text.
/// @details Uses the generic `V-ZIA-SEMA` code and delegates structured construction to
///          @ref errorWithCode.
void Sema::error(SourceLoc loc, const std::string &message) {
    errorWithCode(loc, "V-ZIA-SEMA", message);
}

/// @brief Report a structured semantic error and mark analysis as failed.
/// @param loc Primary diagnostic location.
/// @param code Stable diagnostic code.
/// @param message Human-readable error text.
/// @param range Highlight range; synthesized as one column at @p loc when omitted.
/// @param notes Related diagnostic locations and explanations.
/// @param help Optional remediation guidance.
void Sema::errorWithCode(SourceLoc loc,
                         std::string code,
                         std::string message,
                         il::support::SourceRange range,
                         std::vector<il::support::DiagnosticNote> notes,
                         std::string help) {
    hasError_ = true;
    if (!range.isValid() && loc.isValid()) {
        range = il::support::SourceRange{
            loc,
            il::support::SourceLoc{loc.file_id, loc.line, loc.column + 1},
        };
    }
    il::support::Diagnostic diag{
        il::support::Severity::Error, std::move(message), loc, std::move(code)};
    diag.range = range;
    diag.notes = std::move(notes);
    diag.stage = "sema";
    diag.help = std::move(help);
    diag_.report(std::move(diag));
}

/// @brief Find the nearest visible spelling for an unresolved symbol.
/// @param name Misspelled identifier.
/// @return Candidate within the length-sensitive edit-distance threshold, or std::nullopt.
/// @details Searches lexical scopes, bound short names, and registered type names; ties retain
///          the first candidate encountered.
std::optional<std::string> Sema::suggestSymbolName(const std::string &name) const {
    std::optional<std::string> best;
    size_t bestDistance = std::numeric_limits<size_t>::max();

    auto consider = [&](const std::string &candidate) {
        if (candidate.empty() || candidate == name)
            return;
        const size_t distance = editDistance(name, candidate);
        const size_t limit = name.size() <= 4 ? 1 : 2;
        if (distance <= limit && distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    };

    for (const Scope *scope = currentScope_; scope != nullptr; scope = scope->parent()) {
        for (const auto &[candidate, _] : scope->getSymbols())
            consider(candidate);
    }
    for (const auto &[candidate, _] : importedSymbols_)
        consider(candidate);
    for (const auto &[candidate, _] : typeRegistry_)
        consider(candidate);
    return best;
}

/// @brief Diagnose a duplicate name in the current lexical scope.
/// @param name Name being defined.
/// @param loc Location of the new definition.
/// @return True when no local definition exists; false after emitting a duplicate diagnostic.
/// @details When available, the prior definition is attached as a diagnostic note.
bool Sema::reportDuplicateDefinition(const std::string &name, SourceLoc loc) {
    if (!currentScope_)
        return true;

    Symbol *existing = currentScope_->lookupLocal(name);
    if (!existing)
        return true;

    SourceLoc existingLoc = existing->loc.isValid()
                                ? existing->loc
                                : (existing->decl ? existing->decl->loc : SourceLoc{});
    if (!existingLoc.isValid()) {
        for (auto it = scopedSymbols_.rbegin(); it != scopedSymbols_.rend(); ++it) {
            if (it->scopeId == currentScope_->id() && it->symbol.name == name) {
                existingLoc = it->loc;
                break;
            }
        }
    }

    std::string message = "Duplicate definition of '" + name + "'";
    std::vector<il::support::DiagnosticNote> notes;
    if (existingLoc.isValid()) {
        message += " (previous definition at line " + std::to_string(existingLoc.line) +
                   ", column " + std::to_string(existingLoc.column) + ")";
        notes.push_back({existingLoc, "previous definition of '" + name + "' is here"});
    }
    errorWithCode(loc,
                  "V-ZIA-DUPLICATE",
                  std::move(message),
                  {},
                  std::move(notes),
                  "Rename one declaration or move it to a different scope.");
    return false;
}

/// @brief Compute the last source location covered by a statement tree.
/// @param stmt Statement whose lexical end is needed.
/// @return End of the deepest applicable branch/body, the statement location as fallback, or an
///         invalid location for null.
/// @details Compound control flow selects the latest branch, catch, or finally endpoint so scope
///          snapshots cover the complete source construct.
SourceLoc Sema::scopeEndForStmt(const Stmt *stmt) {
    if (!stmt)
        return {};

    switch (stmt->kind) {
        case StmtKind::Block: {
            auto *block = static_cast<const BlockStmt *>(stmt);
            if (block->endLoc.isValid())
                return block->endLoc;
            if (block->statements.empty())
                return stmt->loc;
            return scopeEndForStmt(block->statements.back().get());
        }
        case StmtKind::If: {
            auto *ifStmt = static_cast<const IfStmt *>(stmt);
            SourceLoc end = scopeEndForStmt(ifStmt->thenBranch.get());
            if (ifStmt->elseBranch) {
                SourceLoc elseEnd = scopeEndForStmt(ifStmt->elseBranch.get());
                if (!end.isValid() || (elseEnd.isValid() && compareLoc(elseEnd, end) > 0))
                    end = elseEnd;
            }
            return end.isValid() ? end : stmt->loc;
        }
        case StmtKind::While:
            return scopeEndForStmt(static_cast<const WhileStmt *>(stmt)->body.get());
        case StmtKind::For:
            return scopeEndForStmt(static_cast<const ForStmt *>(stmt)->body.get());
        case StmtKind::ForIn:
            return scopeEndForStmt(static_cast<const ForInStmt *>(stmt)->body.get());
        case StmtKind::Defer:
            return scopeEndForStmt(static_cast<const DeferStmt *>(stmt)->action.get());
        case StmtKind::Try: {
            auto *tryStmt = static_cast<const TryStmt *>(stmt);
            SourceLoc end = scopeEndForStmt(tryStmt->tryBody.get());
            for (const auto &catchClause : tryStmt->catches) {
                SourceLoc catchEnd = scopeEndForStmt(catchClause.body.get());
                if (!end.isValid() || (catchEnd.isValid() && compareLoc(catchEnd, end) > 0))
                    end = catchEnd;
            }
            if (tryStmt->finallyBody) {
                SourceLoc finallyEnd = scopeEndForStmt(tryStmt->finallyBody.get());
                if (!end.isValid() || (finallyEnd.isValid() && compareLoc(finallyEnd, end) > 0))
                    end = finallyEnd;
            }
            return end.isValid() ? end : stmt->loc;
        }
        default:
            return stmt->loc;
    }
}

/// @brief Report an "undefined identifier" error for the given name.
/// @param loc Identifier location.
/// @param name Unresolved source spelling.
/// @details Adds a nearest-name suggestion and replacement fix-it when a sufficiently close
///          visible symbol exists.
void Sema::errorUndefined(SourceLoc loc, const std::string &name) {
    std::string message = "Undefined identifier: " + name;
    std::vector<il::support::DiagnosticNote> notes;
    std::vector<il::support::DiagnosticFixIt> fixits;
    il::support::SourceRange range{};
    if (loc.isValid()) {
        range = il::support::SourceRange{
            loc,
            il::support::SourceLoc{loc.file_id,
                                   loc.line,
                                   loc.column +
                                       static_cast<uint32_t>(std::max<size_t>(1, name.size()))},
        };
    }
    if (auto suggestion = suggestSymbolName(name)) {
        message += "; did you mean '" + *suggestion + "'?";
        notes.push_back({loc, "candidate symbol '" + *suggestion + "' is visible here"});
        fixits.push_back({range, *suggestion, "replace with '" + *suggestion + "'"});
    }

    hasError_ = true;
    il::support::Diagnostic diag{
        il::support::Severity::Error,
        std::move(message),
        loc,
        "V-ZIA-UNDEFINED",
    };
    diag.range = range;
    diag.notes = std::move(notes);
    diag.stage = "sema";
    diag.help = "Declare the symbol, import it, or correct the spelling.";
    diag.fixits = std::move(fixits);
    diag_.report(std::move(diag));
}

/// @brief Report a runtime method accessed without call parentheses. See header.
/// @param expr Field-style method access being rejected.
/// @param className Runtime receiver class.
/// @param candidates Runtime method candidates encoded with arity suffixes.
/// @details Offers an `()` fix-it only when a zero-argument overload exists; otherwise the
///          diagnostic uses `(...)` guidance without a misleading automatic edit.
void Sema::errorRuntimeMethodNeedsCall(FieldExpr *expr, const std::string &className,
                                       const std::vector<std::string> &candidates) {
    // A zero-arg overload means `()` fully fixes the access, so we offer a fix-it.
    // With only arg-taking overloads we guide but omit the fix-it — a fix that
    // produces a fresh "wrong argument count" error would not be a fix.
    bool hasZeroArg = false;
    for (const auto &candidate : candidates) {
        if (candidate.size() >= 2 && candidate.compare(candidate.size() - 2, 2, "/0") == 0) {
            hasZeroArg = true;
            break;
        }
    }
    const std::string call = hasZeroArg ? "()" : "(...)";
    std::string message = "'" + expr->field + "' is a method of '" + className +
                          "'; call it with '" + expr->field + call + "'";

    // The field-name span. FieldExpr::loc is the `.` token and the field name
    // immediately follows it, so the name occupies [column+1, column+1+len).
    // Used both to underline the access and to attach the "add ()" fix-it.
    il::support::SourceRange range{};
    std::vector<il::support::DiagnosticFixIt> fixits;
    if (expr->loc.isValid()) {
        const uint32_t fieldColumn = expr->loc.column + 1;
        range = il::support::SourceRange{
            il::support::SourceLoc{expr->loc.file_id, expr->loc.line, fieldColumn},
            il::support::SourceLoc{expr->loc.file_id, expr->loc.line,
                                   fieldColumn + static_cast<uint32_t>(expr->field.size())},
        };
        if (hasZeroArg) {
            fixits.push_back({range, expr->field + "()", "add '()' to call the method"});
        }
    }

    hasError_ = true;
    il::support::Diagnostic diag{
        il::support::Severity::Error,
        std::move(message),
        expr->loc,
        "V-ZIA-METHOD-CALL",
    };
    diag.range = range;
    diag.stage = "sema";
    diag.help =
        "Runtime methods are invoked with parentheses; write '" + expr->field + call + "'.";
    diag.fixits = std::move(fixits);
    diag_.report(std::move(diag));
}

/// @brief Report a type mismatch error showing expected vs actual types.
/// @param loc Expression location.
/// @param expected Required semantic type, or null for unknown.
/// @param actual Observed semantic type, or null for unknown.
void Sema::errorTypeMismatch(SourceLoc loc, TypeRef expected, TypeRef actual) {
    std::string expectedStr = expected ? expected->toDisplayString() : "unknown";
    std::string actualStr = actual ? actual->toDisplayString() : "unknown";
    errorWithCode(loc,
                  "V-ZIA-TYPE-MISMATCH",
                  "Type mismatch: expected " + expectedStr + ", got " + actualStr);
}

} // namespace il::frontends::zia
