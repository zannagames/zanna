//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file Sema_Scope.cpp
/// @brief Lexical scope stack management, symbol lookup, visibility checks, flow-sensitive type
/// narrowing, and initialization-state helpers for Zia Sema.
///
/// @details This file was split out of Sema.cpp to keep semantic analysis
/// responsibilities navigable without changing the Sema public interface or
/// diagnostic behavior. Member functions remain declared in Sema.hpp.
///
/// @see frontends/zia/Sema.hpp
///
//===----------------------------------------------------------------------===//

#include "frontends/zia/Sema.hpp"

#include <cassert>
#include <utility>

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

} // namespace

//=============================================================================
// Scope Management
//=============================================================================

/// @brief Push a new child scope onto the scope stack.
/// @param startLoc Source location where the lexical scope begins.
/// @details Creates a tooling snapshot, assigns a monotonic scope identifier and depth, and
///          starts a parallel flow-narrowing layer.
void Sema::pushScope(SourceLoc startLoc) {
    const uint32_t scopeId = nextScopeId_++;
    const size_t depth = currentScope_ ? currentScope_->depth() + 1 : 0;
    const uint32_t parentId = currentScope_ ? currentScope_->id() : 0;
    scopes_.push_back(std::make_unique<Scope>(currentScope_, scopeId, depth));
    currentScope_ = scopes_.back().get();
    narrowedTypes_.push_back({});
    scopeSnapshots_[scopeId] = ScopeSnapshot{scopeId, parentId, depth, startLoc, {}};
}

/// @brief Pop the current scope, restoring its parent as the active scope.
/// @param endLoc Source location where the lexical scope ends.
/// @pre There must be more than the global scope remaining.
/// @details Checks for unused variables, removes scope-qualified initialization state, closes the
///          tooling snapshot, and removes the matching narrowing layer.
void Sema::popScope(SourceLoc endLoc) {
    assert(scopes_.size() > 1 && "cannot pop global scope");

    // W001: Check for unused variables/parameters in the scope being popped
    checkUnusedVariables(*currentScope_);

    const uint32_t poppedScopeId = currentScope_->id();
    for (const auto &[name, _] : currentScope_->getSymbols())
        initializedVars_.erase(std::to_string(poppedScopeId) + ":" + name);

    auto snapIt = scopeSnapshots_.find(currentScope_->id());
    if (snapIt != scopeSnapshots_.end() && endLoc.isValid())
        snapIt->second.endLoc = endLoc;

    if (!narrowedTypes_.empty())
        narrowedTypes_.pop_back();
    currentScope_ = currentScope_->parent();
    scopes_.pop_back();
    assert(currentScope_ == scopes_.back().get() && "scope stack corrupted");
}

/// @brief Define a symbol in the current scope.
/// @param name The symbol name to register.
/// @param symbol The symbol metadata to associate with the name.
/// @param locOverride Optional source location for symbols without decl (locals, params).
/// @return True when defined, or false after a duplicate-definition diagnostic.
/// @details Compatible extern redefinitions refine the existing entry. Successful definitions
///          are also copied into the position-aware tooling index.
bool Sema::defineSymbol(const std::string &name, Symbol symbol, SourceLoc locOverride) {
    SourceLoc defLoc =
        locOverride.isValid() ? locOverride : (symbol.decl ? symbol.decl->loc : SourceLoc{});
    if (Symbol *existing = currentScope_->lookupLocal(name)) {
        if (existing->decl == nullptr && symbol.decl == nullptr && existing->isExtern &&
            symbol.isExtern) {
            symbol.loc = defLoc;
            currentScope_->define(name, std::move(symbol));
            return true;
        }
    }
    if (!reportDuplicateDefinition(name, defLoc))
        return false;

    symbol.loc = defLoc;
    currentScope_->define(name, std::move(symbol));

    // Capture a snapshot for position-based hover queries.
    Symbol *defined = currentScope_->lookupLocal(name);
    if (defined) {
        ScopedSymbol ss;
        ss.symbol = *defined;
        ss.loc = defLoc;
        ss.ownerType = currentSelfType_ ? currentSelfType_->name : "";
        ss.scopeId = currentScope_->id();
        scopedSymbols_.push_back(std::move(ss));
    }
    return true;
}

/// @brief Find the most relevant symbol at a given cursor position.
/// @param name Symbol spelling to search.
/// @param fileId Cursor file, or zero to accept any file.
/// @param line Cursor line.
/// @param col Cursor column.
/// @return Deepest visible preceding definition, preferring the latest declaration at equal depth,
///         or nullptr.
const ScopedSymbol *Sema::findSymbolAtPosition(const std::string &name,
                                               uint32_t fileId,
                                               uint32_t line,
                                               uint32_t col) const {
    const ScopedSymbol *best = nullptr;
    const SourceLoc cursor{fileId, line, col};
    for (const auto &ss : scopedSymbols_) {
        if (ss.symbol.name != name)
            continue;
        if (!ss.loc.isValid())
            continue;
        if (fileId != 0 && ss.loc.file_id != fileId)
            continue;
        // Symbol must be defined at or before the cursor position.
        if (compareLoc(ss.loc, cursor) > 0)
            continue;

        auto scopeIt = scopeSnapshots_.find(ss.scopeId);
        if (scopeIt != scopeSnapshots_.end()) {
            const auto &scope = scopeIt->second;
            if (fileId != 0 && scope.startLoc.hasFile() && scope.startLoc.file_id != fileId)
                continue;
            if (scope.startLoc.isValid() && compareLoc(scope.startLoc, cursor) > 0)
                continue;
            if (scope.endLoc.isValid() && cursor.file_id == scope.endLoc.file_id &&
                compareLoc(cursor, scope.endLoc) > 0)
                continue;
        }

        if (!best) {
            best = &ss;
            continue;
        }

        const auto bestScopeIt = scopeSnapshots_.find(best->scopeId);
        const size_t bestDepth =
            bestScopeIt != scopeSnapshots_.end() ? bestScopeIt->second.depth : 0;
        const size_t thisDepth = scopeIt != scopeSnapshots_.end() ? scopeIt->second.depth : 0;
        if (thisDepth > bestDepth ||
            (thisDepth == bestDepth && compareLoc(ss.loc, best->loc) > 0)) {
            best = &ss;
        }
    }
    return best;
}

/// @brief Look up a symbol by name in the current scope chain.
/// @param name The symbol name to search for.
/// @return Pointer to the symbol if found, nullptr otherwise.
Symbol *Sema::lookupSymbol(const std::string &name) {
    return currentScope_->lookup(name);
}

/// @brief Recover an identifier's declared Optional surface after flow narrowing.
/// @param expr Analyzed expression, currently recognized when it is an identifier.
/// @param analyzedType Flow-sensitive result type.
/// @return Declared Optional variable/parameter type when available; otherwise @p analyzedType.
TypeRef Sema::declaredOptionalSurfaceType(Expr *expr, TypeRef analyzedType) {
    if (analyzedType && analyzedType->kind == TypeKindSem::Optional)
        return analyzedType;

    auto *ident = dynamic_cast<IdentExpr *>(expr);
    if (!ident)
        return analyzedType;

    Symbol *sym = lookupSymbol(ident->name);
    if (!sym)
        return analyzedType;

    if (sym->kind != Symbol::Kind::Variable && sym->kind != Symbol::Kind::Parameter)
        return analyzedType;

    if (sym->type && sym->type->kind == TypeKindSem::Optional)
        return sym->type;

    return analyzedType;
}

/// @brief Check cross-file visibility for a resolved symbol.
/// @param sym Symbol being referenced.
/// @param useLoc Source location of the use.
/// @param name Source spelling reserved for richer access policies.
/// @param viaQualifiedModule Whether the use was module-qualified, reserved for richer policies.
/// @return True for externs, same-file uses, non-top-level declarations, or exported top-level
///         declarations; false for private top-level declarations used from another file.
bool Sema::canAccessSymbol(const Symbol &sym,
                           SourceLoc useLoc,
                           const std::string &name,
                           bool viaQualifiedModule) const {
    (void)name;
    (void)viaQualifiedModule;

    if (sym.isExtern || !useLoc.isValid() || !sym.loc.isValid())
        return true;

    if (useLoc.file_id == 0 || sym.loc.file_id == 0 || useLoc.file_id == sym.loc.file_id)
        return true;

    if (!sym.decl)
        return true;

    switch (sym.decl->kind) {
        case DeclKind::Function:
        case DeclKind::Struct:
        case DeclKind::Class:
        case DeclKind::Interface:
        case DeclKind::GlobalVar:
        case DeclKind::Namespace:
        case DeclKind::Enum:
        case DeclKind::TypeAlias:
            break;
        default:
            return true;
    }

    if (!sym.isExported)
        return false;
    return true;
}

/// @brief Emit the access diagnostic corresponding to a rejected symbol.
/// @param useLoc Source location of the use.
/// @param name Source-visible declaration name.
/// @param sym Inaccessible symbol metadata.
/// @param viaQualifiedModule Whether lookup was module-qualified.
void Sema::reportInaccessibleSymbol(SourceLoc useLoc,
                                    const std::string &name,
                                    const Symbol &sym,
                                    bool viaQualifiedModule) {
    (void)viaQualifiedModule;
    if (!sym.isExported) {
        error(useLoc,
              "Cannot access private top-level declaration '" + name + "' from another file");
        return;
    }
}

/// @brief Look up a symbol and enforce cross-file export visibility.
/// @param name Semantic symbol name.
/// @param useLoc Source location of the use.
/// @param viaQualifiedModule Whether lookup followed a module qualifier.
/// @return Accessible symbol, or nullptr when absent or after an access diagnostic.
Symbol *Sema::lookupAccessibleSymbol(const std::string &name,
                                     SourceLoc useLoc,
                                     bool viaQualifiedModule) {
    Symbol *sym = lookupSymbol(name);
    if (!sym)
        return nullptr;
    if (canAccessSymbol(*sym, useLoc, name, viaQualifiedModule))
        return sym;
    reportInaccessibleSymbol(useLoc, name, *sym, viaQualifiedModule);
    return nullptr;
}

/// @brief Look up the type of a variable, respecting flow-sensitive type narrowing.
/// @details Checks narrowed types first (from null-check analysis), then falls back
///          to the declared type in scope.
/// @param name The variable name to look up.
/// @return The narrowed or declared type, or nullptr if not found.
TypeRef Sema::lookupVarType(const std::string &name) {
    if (TypeRef narrowed = lookupNarrowedType(name))
        return narrowed;

    // Fall back to declared type
    Symbol *sym = currentScope_->lookup(name);
    if (sym && (sym->kind == Symbol::Kind::Variable || sym->kind == Symbol::Kind::Parameter ||
                sym->kind == Symbol::Kind::Field)) {
        return sym->type;
    }
    return nullptr;
}

/// @brief Look up a flow-sensitive type refinement by narrowing key.
/// @param key Identifier, member path, or supported literal-index path.
/// @return Innermost active refinement, or nullptr when none exists.
TypeRef Sema::lookupNarrowedType(const std::string &key) const {
    if (key.empty())
        return nullptr;

    for (auto it = narrowedTypes_.rbegin(); it != narrowedTypes_.rend(); ++it) {
        auto found = it->find(key);
        if (found != it->end())
            return found->second;
    }
    return nullptr;
}

//=============================================================================
// Type Narrowing (Flow-Sensitive Type Analysis)
//=============================================================================

/// @brief Push a new type narrowing scope for flow-sensitive analysis.
/// @details Refinements added afterward shadow outer layers until @ref popNarrowingScope.
void Sema::pushNarrowingScope() {
    narrowedTypes_.push_back({});
}

/// @brief Pop the current type narrowing scope.
/// @details A call on an empty stack is tolerated for diagnostic recovery.
void Sema::popNarrowingScope() {
    if (!narrowedTypes_.empty()) {
        narrowedTypes_.pop_back();
    }
}

/// @brief Narrow the type of a variable in the current narrowing scope.
/// @param name The variable whose type is being narrowed.
/// @param narrowedType The narrowed type to record.
void Sema::narrowType(const std::string &name, TypeRef narrowedType) {
    if (!narrowedTypes_.empty()) {
        narrowedTypes_.back()[name] = narrowedType;
    }
}

/// @brief Mark a variable as definitely initialized.
/// @param name Source variable name; lexical ownership is encoded into the stored key.
void Sema::markInitialized(const std::string &name) {
    initializedVars_.insert(initializedSymbolKey(name));
}

/// @brief Check if a variable has been definitely initialized.
/// @param name Source variable name resolved under normal shadowing rules.
/// @return True when the current control-flow state contains its scope-qualified key.
bool Sema::isInitialized(const std::string &name) const {
    return initializedVars_.count(initializedSymbolKey(name)) > 0;
}

/// @brief Build the scope-qualified initialization-state key for @p name.
/// @details Walks from the current lexical scope outward and uses the first
///          scope that owns @p name, matching normal shadowing lookup. If no
///          symbol is available because recovery is continuing after an
///          earlier error, the raw name remains as a fallback key.
/// @param name Source symbol name.
/// @return `scopeId:name` for the nearest owning scope, or the raw name.
std::string Sema::initializedSymbolKey(const std::string &name) const {
    for (Scope *scope = currentScope_; scope != nullptr; scope = scope->parent()) {
        if (scope->lookupLocal(name))
            return std::to_string(scope->id()) + ":" + name;
    }
    return name;
}

/// @brief Save the current initialization state for branching analysis.
/// @return Copy of the active scope-qualified initialized-variable set.
std::unordered_set<std::string> Sema::saveInitState() const {
    return initializedVars_;
}

/// @brief Intersect two branch initialization states.
/// Only variables initialized in BOTH branches remain initialized.
/// @param branchA Initialization set from the first control-flow branch.
/// @param branchB Initialization set from the second control-flow branch.
void Sema::intersectInitState(const std::unordered_set<std::string> &branchA,
                              const std::unordered_set<std::string> &branchB) {
    std::unordered_set<std::string> result;
    for (const auto &name : branchA) {
        if (branchB.count(name) > 0)
            result.insert(name);
    }
    initializedVars_ = std::move(result);
}

/// @brief Try to extract a null-check pattern from a condition expression.
/// @details Recognizes patterns: x != null, x == null, null != x, null == x.
/// @param[in] cond The condition expression to analyze.
/// @param[out] varName The variable name being null-checked.
/// @param[out] isNotNull True if the pattern is != null, false if == null.
/// @param[out] checkedType Optional destination for the operand's declared optional surface type.
/// @return True if a null-check pattern was recognized.
bool Sema::tryExtractNullCheck(Expr *cond,
                               std::string &varName,
                               bool &isNotNull,
                               TypeRef *checkedType) {
    // Pattern: x != null or x == null
    if (cond->kind != ExprKind::Binary)
        return false;

    auto *binary = static_cast<BinaryExpr *>(cond);
    if (binary->op != BinaryOp::Ne && binary->op != BinaryOp::Eq)
        return false;

    isNotNull = (binary->op == BinaryOp::Ne);

    auto captureOperand = [&](Expr *operand) {
        varName = narrowingKeyForExpr(operand);
        if (varName.empty())
            return false;
        if (checkedType) {
            TypeRef ty = typeOf(operand);
            *checkedType = declaredOptionalSurfaceType(operand, ty);
        }
        return true;
    };

    // Check for "x != null" or "obj.field != null" pattern
    if (binary->right->kind == ExprKind::NullLiteral) {
        return captureOperand(binary->left.get());
    }

    // Check for "null != x" or "null != obj.field" pattern
    if (binary->left->kind == ExprKind::NullLiteral) {
        return captureOperand(binary->right.get());
    }

    return false;
}

/// @brief Build a stable flow-narrowing key for an expression.
/// @param expr Identifier, `self`, dotted field chain, or literal-index chain.
/// @return Reconstructable access path, or an empty string for unsupported expressions/indexes.
std::string Sema::narrowingKeyForExpr(Expr *expr) const {
    if (!expr)
        return {};

    if (auto *ident = dynamic_cast<IdentExpr *>(expr))
        return ident->name;

    if (dynamic_cast<SelfExpr *>(expr))
        return "self";

    if (auto *field = dynamic_cast<FieldExpr *>(expr)) {
        std::string base = narrowingKeyForExpr(field->base.get());
        if (base.empty())
            return {};
        return base + "." + field->field;
    }

    if (auto *index = dynamic_cast<IndexExpr *>(expr)) {
        std::string base = narrowingKeyForExpr(index->base.get());
        if (base.empty())
            return {};
        if (auto *intLit = dynamic_cast<IntLiteralExpr *>(index->index.get()))
            return base + "[" + std::to_string(intLit->value) + "]";
        if (auto *strLit = dynamic_cast<StringLiteralExpr *>(index->index.get()))
            return base + "[\"" + strLit->value + "\"]";
        return {};
    }

    return {};
}

} // namespace il::frontends::zia
