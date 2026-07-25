//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/frontends/basic/Semantic_OOP_Helpers.cpp
// Purpose: Implements focused AST walkers and control-flow predicates used
//          while constructing the BASIC object-model index.
// Key invariants:
//   - Validation is optional when no DiagnosticEmitter is supplied.
//   - Definite-return analysis succeeds only for structurally proven paths.
//   - Helper walks borrow AST nodes and never rewrite method bodies.
// Ownership/Lifetime:
//   - Walkers borrow field-name sets, diagnostics, and AST nodes for a bounded
//     synchronous traversal.
// Links: src/frontends/basic/Semantic_OOP_Builder.cpp,
//        src/frontends/basic/detail/Semantic_OOP_Internal.hpp,
//        src/frontends/basic/AstWalker.hpp,
//        src/frontends/basic/ASTUtils.hpp
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/AstWalker.hpp"
#include "frontends/basic/StringUtils.hpp"
#include "frontends/basic/detail/Semantic_OOP_Internal.hpp"

/// @file
/// @brief Implements BASIC OOP validation walkers and structural return checks.

namespace il::frontends::basic::detail {

namespace {

//===----------------------------------------------------------------------===//
// AST Walkers
//===----------------------------------------------------------------------===//

/// @brief Walker to detect local variables that shadow class fields.
/// @details Visits DIM statements and emits B2016 when a local name exactly
///          matches one of the containing class's non-static field names.
class MemberShadowCheckWalker final : public BasicAstWalker<MemberShadowCheckWalker> {
  public:
    /// @brief Binds a shadowing walk to one class and diagnostic sink.
    /// @param className Class name used to qualify the warning message.
    /// @param fields Borrowed set of field names valid throughout the walk.
    /// @param emitter Optional borrowed sink; null disables warning emission.
    MemberShadowCheckWalker(const std::string &className,
                            const std::unordered_set<std::string> &fields,
                            DiagnosticEmitter *emitter) noexcept
        : className_(className), fields_(fields), emitter_(emitter) {}

    /// @brief Checks one DIM declaration before descending into its children.
    /// @param stmt Local declaration whose name may shadow a class field.
    /// @post Emits at most one B2016 warning for @p stmt.
    void before(const DimStmt &stmt) {
        if (!emitter_ || stmt.name.empty())
            return;
        if (!fields_.contains(stmt.name))
            return;

        std::string qualifiedField = className_;
        if (!qualifiedField.empty())
            qualifiedField += '.';
        qualifiedField += stmt.name;

        std::string msg = "local '" + stmt.name + "' shadows field '" + qualifiedField +
                          "'; use Me." + stmt.name + " to access the field";
        emitter_->emit(il::support::Severity::Warning,
                       "B2016",
                       stmt.loc,
                       static_cast<uint32_t>(stmt.name.size()),
                       std::move(msg));
    }

  private:
    /// Class display name copied for stable diagnostic construction.
    std::string className_;

    /// Non-owning set of non-static field names to compare against.
    const std::unordered_set<std::string> &fields_;

    /// Optional non-owning destination for shadowing warnings.
    DiagnosticEmitter *emitter_;
};

/// @brief Walker to detect use of 'ME' in static contexts.
/// @details Every visited @ref MeExpr produces the configured error. The
///          surrounding helper skips the walk entirely when diagnostics are
///          disabled.
class MeUseWalker : public BasicAstWalker<MeUseWalker> {
  public:
    /// Optional non-owning diagnostic sink.
    DiagnosticEmitter *em;

    /// Stable diagnostic code borrowed from the caller.
    const char *errorCode;

    /// Human-readable diagnostic text borrowed from the caller.
    const char *message;

    /// @brief Creates a walker with caller-selected diagnostic metadata.
    /// @param e Optional borrowed diagnostic emitter.
    /// @param code Null-terminated code that remains valid during the walk.
    /// @param msg Null-terminated message that remains valid during the walk.
    MeUseWalker(DiagnosticEmitter *e, const char *code, const char *msg)
        : em(e), errorCode(code), message(msg) {}

    /// @brief Emits the configured diagnostic for one ME expression.
    /// @param expr Invalid instance-receiver expression in a static body.
    /// @post Emits exactly one diagnostic when @ref em is non-null.
    void visit(const MeExpr &expr) {
        if (!em)
            return;
        em->emit(il::support::Severity::Error, errorCode, expr.loc, 1, message);
    }
};

//===----------------------------------------------------------------------===//
// Return Analysis Helpers
//===----------------------------------------------------------------------===//

/// @brief Check if a statement definitely returns a value.
/// @details A RETURN with a value succeeds. A statement list delegates to its
///          final statement, and an IF succeeds only when its THEN, every
///          ELSEIF, and mandatory ELSE branch all succeed. Loops and all other
///          statement forms conservatively fail.
/// @param stmt Statement subtree to classify structurally.
/// @return @c true only when every modeled path through @p stmt returns a
///         value.
[[nodiscard]] bool methodMustReturn(const Stmt &stmt) {
    if (const auto *lst = as<const StmtList>(stmt))
        return !lst->stmts.empty() && methodMustReturn(*lst->stmts.back());
    if (const auto *ret = as<const ReturnStmt>(stmt))
        return ret->value != nullptr;
    if (const auto *ifs = as<const IfStmt>(stmt)) {
        if (!ifs->then_branch || !methodMustReturn(*ifs->then_branch))
            return false;
        for (const auto &elifBranch : ifs->elseifs)
            if (!elifBranch.then_branch || !methodMustReturn(*elifBranch.then_branch))
                return false;
        if (!ifs->else_branch)
            return false;
        return methodMustReturn(*ifs->else_branch);
    }
    if (is<WhileStmt>(stmt) || is<ForStmt>(stmt))
        return false;
    return false;
}

} // namespace

//===----------------------------------------------------------------------===//
// Public Helper Functions
//===----------------------------------------------------------------------===//

/// @brief Reports local DIM declarations that shadow fields of @p klass.
/// @details Walks every non-null statement recursively with
///          @ref MemberShadowCheckWalker. Static fields are absent from
///          @p fieldNames by construction and therefore are not considered.
/// @param body Method, constructor, or destructor body to inspect.
/// @param klass Class supplying the name used in warning messages.
/// @param fieldNames Borrowed set of instance-field names.
/// @param emitter Optional diagnostic sink; null or an empty field set makes
///                this function an exact no-op.
void checkMemberShadowing(const std::vector<StmtPtr> &body,
                          const ClassDecl &klass,
                          const std::unordered_set<std::string> &fieldNames,
                          DiagnosticEmitter *emitter) {
    if (!emitter || fieldNames.empty())
        return;

    MemberShadowCheckWalker walker(klass.name, fieldNames, emitter);
    for (const auto &stmt : body)
        if (stmt)
            walker.walkStmt(*stmt);
}

/// @brief Reports every ME expression within a static member body.
/// @param body Statements traversed recursively.
/// @param emitter Optional diagnostic sink; null makes the function a no-op.
/// @param errorCode Null-terminated diagnostic code used for each occurrence.
/// @param message Null-terminated message used for each occurrence.
void checkMeInStaticContext(const std::vector<StmtPtr> &body,
                            DiagnosticEmitter *emitter,
                            const char *errorCode,
                            const char *message) {
    if (!emitter)
        return;
    MeUseWalker walker(emitter, errorCode, message);
    for (const auto &s : body)
        if (s)
            walker.walkStmt(*s);
}

/// @brief Determines whether a method body's final statement definitely returns.
/// @details Earlier statements cannot establish the property because execution
///          may continue past them; only the final non-null subtree is
///          classified by @ref methodMustReturn.
/// @param stmts Ordered top-level method statements.
/// @return @c true when the body is non-empty and its final statement returns
///         a value on every modeled path.
bool methodBodyMustReturn(const std::vector<StmtPtr> &stmts) {
    if (stmts.empty())
        return false;
    const StmtPtr &tail = stmts.back();
    return tail && methodMustReturn(*tail);
}

/// @brief Detects VB-style return by assignment to the function's own name.
/// @details Searches direct statements, nested statement lists, and every IF
///          branch using case-insensitive name comparison. This is an
///          existence check, not a proof that every control-flow path assigns
///          the result.
/// @param method Method declaration whose body and name are inspected.
/// @return @c true when a modeled assignment targets @p method's name.
bool methodHasImplicitReturn(const MethodDecl &method) {
    /// Recognizes one LET whose simple variable target matches the method name.
    auto isNameAssign = [&](const Stmt &s) -> bool {
        if (auto *let = as<const LetStmt>(s)) {
            if (let->target) {
                if (auto *v = as<VarExpr>(*let->target))
                    return string_utils::iequals(v->name, method.name);
            }
        }
        return false;
    };

    /// Recursively searches statement lists and conditional branches.
    std::function<bool(const Stmt &)> walk = [&](const Stmt &s) -> bool {
        if (isNameAssign(s))
            return true;
        if (auto *list = as<const StmtList>(s)) {
            for (const auto &sp : list->stmts)
                if (sp && walk(*sp))
                    return true;
        }
        if (auto *ifs = as<const IfStmt>(s)) {
            if (ifs->then_branch && walk(*ifs->then_branch))
                return true;
            for (const auto &e : ifs->elseifs)
                if (e.then_branch && walk(*e.then_branch))
                    return true;
            if (ifs->else_branch && walk(*ifs->else_branch))
                return true;
        }
        return false;
    };
    for (const auto &sp : method.body)
        if (sp && walk(*sp))
            return true;
    return false;
}

/// @brief Emits B1007 for a value-returning method without an explicit or
///        implicit result.
/// @details Procedures without a return type are ignored. A diagnostic is also
///          suppressed when structural analysis proves a value-returning tail
///          or when assignment to the function name supplies VB-style return
///          semantics.
/// @param klass Containing class used to qualify the method in the message.
/// @param method Method declaration to validate.
/// @param emitter Optional diagnostic sink; null makes the function a no-op.
/// @post Emits at most one B1007 diagnostic.
void emitMissingReturn(const ClassDecl &klass,
                       const MethodDecl &method,
                       DiagnosticEmitter *emitter) {
    if (!emitter)
        return;
    if (!method.ret)
        return;
    if (methodBodyMustReturn(method.body))
        return;
    if (methodHasImplicitReturn(method))
        return;

    std::string qualified = klass.name;
    if (!qualified.empty())
        qualified += '.';
    qualified += method.name;

    std::string msg = "missing return in FUNCTION " + qualified;
    emitter->emit(il::support::Severity::Error, "B1007", method.loc, 3, std::move(msg));
}

/// @brief Joins qualified-name segments with period separators.
/// @param segs Ordered segments; empty strings are retained as empty segments.
/// @return Dot-separated spelling, or an empty string for no segments.
std::string joinQualified(const std::vector<std::string> &segs) {
    std::string out;
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i)
            out.push_back('.');
        out += segs[i];
    }
    return out;
}

} // namespace il::frontends::basic::detail
