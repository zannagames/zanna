//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/SemanticAnalyzer_Procs.cpp
// Purpose: Implements procedure registration and analysis logic for the BASIC
//          semantic analyzer, covering SUB/FUNCTION bodies and user-defined
//          call validation.
// Key invariants: Procedure scope resets state between declarations; call
//                 validation consults ProcRegistry signatures.
// Ownership/Lifetime: Borrowed DiagnosticEmitter; ProcRegistry managed by the
//                     analyzer instance.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer_Procs.cpp
/// @brief Implements BASIC procedure scopes, return flow, and call resolution.
/// @details The routines transact procedure-local analyzer state, register
///          parameters, validate FUNCTION/SUB bodies, conservatively model
///          explicit and VB-style returns, orchestrate whole-program analysis,
///          resolve qualified/imported/runtime-aliased calls, and validate call
///          arguments and result types.

#include "frontends/basic/Diag.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/LineUtils.hpp"
#include "frontends/basic/SemanticAnalyzer_Internal.hpp"

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/Semantic_OOP.hpp"
#include "frontends/basic/sem/RegistryBuilder.hpp"
#include "frontends/basic/sem/RuntimeMethodIndex.hpp"
#include "il/runtime/classes/RuntimeClasses.hpp"

#include <algorithm>
#include <utility>

namespace il::frontends::basic {

using semantic_analyzer_detail::astToSemanticType;

/// @brief Enters a rollback transaction for procedure-local analyzer state.
/// @details Saves the enclosing active scope and error handler, installs this
///          scope, clears handler state for the new procedure, records FOR/loop
///          stack depths, and pushes a lexical mapping scope.
/// @param analyzer Analyzer borrowed until this scope is destroyed.
/// @post @p analyzer identifies this object as its active procedure scope.
SemanticAnalyzer::ProcedureScope::ProcedureScope(SemanticAnalyzer &analyzer) noexcept
    : analyzer_(analyzer) {
    previous_ = analyzer_.activeProcScope_;
    analyzer_.activeProcScope_ = this;
    previousHandlerActive_ = analyzer_.errorHandlerActive_;
    previousHandlerTarget_ = analyzer_.errorHandlerTarget_;
    analyzer_.errorHandlerActive_ = false;
    analyzer_.errorHandlerTarget_.reset();
    forStackDepth_ = analyzer_.forStack_.size();
    loopStackDepth_ = analyzer_.loopStack_.size();
    analyzer_.scopes_.pushScope();
}

/// @brief Rolls back every mutation recorded by this procedure scope.
/// @details Restores enclosing scope/handler state; erases inserted label
///          references, labels, and symbols; restores or erases baseline map
///          entries; restores literal channel states; truncates control stacks
///          to their captured depths; and pops the lexical scope.
/// @post Module/enclosing-procedure state matches the recorded baselines.
SemanticAnalyzer::ProcedureScope::~ProcedureScope() noexcept {
    analyzer_.activeProcScope_ = previous_;
    analyzer_.errorHandlerActive_ = previousHandlerActive_;
    analyzer_.errorHandlerTarget_ = previousHandlerTarget_;
    for (const auto &label : newLabelRefs_)
        analyzer_.labelRefs_.erase(label);
    for (const auto &label : newLabels_)
        analyzer_.labels_.erase(label);
    for (const auto &name : newSymbols_)
        analyzer_.symbols_.erase(name);
    for (const auto &delta : varTypeDeltas_) {
        if (delta.previous)
            analyzer_.varTypes_[delta.name] = *delta.previous;
        else
            analyzer_.varTypes_.erase(delta.name);
    }
    for (const auto &delta : objectClassDeltas_) {
        if (delta.previous)
            analyzer_.objectClassTypes_[delta.name] = *delta.previous;
        else
            analyzer_.objectClassTypes_.erase(delta.name);
    }
    for (const auto &delta : arrayDeltas_) {
        if (delta.previous)
            analyzer_.arrays_[delta.name] = *delta.previous;
        else
            analyzer_.arrays_.erase(delta.name);
    }
    for (const auto &delta : channelDeltas_) {
        if (delta.previouslyOpen)
            analyzer_.openChannels_.insert(delta.channel);
        else
            analyzer_.openChannels_.erase(delta.channel);
    }
    if (analyzer_.forStack_.size() > forStackDepth_)
        analyzer_.forStack_.resize(forStackDepth_);
    if (analyzer_.loopStack_.size() > loopStackDepth_)
        analyzer_.loopStack_.resize(loopStackDepth_);
    analyzer_.scopes_.popScope();
}

/// @brief Records a newly inserted symbol for unconditional rollback.
/// @param name Exact symbol-set key copied into the rollback log.
void SemanticAnalyzer::ProcedureScope::noteSymbolInserted(const std::string &name) {
    newSymbols_.push_back(name);
}

/// @brief Records the first variable-type baseline for a key.
/// @param name Exact type-map key.
/// @param previous Prior value, or no value when previously absent.
/// @note Repeated mutations of the same key retain the original baseline.
void SemanticAnalyzer::ProcedureScope::noteVarTypeMutation(const std::string &name,
                                                           std::optional<Type> previous) {
    if (!trackedVarTypes_.insert(name).second)
        return;
    varTypeDeltas_.push_back({name, previous});
}

/// @brief Records the first object-class baseline for a key.
/// @param name Exact object-class map key.
/// @param previous Prior qualified class, or no value when absent.
void SemanticAnalyzer::ProcedureScope::noteObjectClassMutation(
    const std::string &name, std::optional<std::string> previous) {
    if (!trackedObjectClasses_.insert(name).second)
        return;
    objectClassDeltas_.push_back({name, std::move(previous)});
}

/// @brief Records the first array-metadata baseline for a key.
/// @param name Exact array map key.
/// @param previous Prior metadata, or no value when absent.
void SemanticAnalyzer::ProcedureScope::noteArrayMutation(const std::string &name,
                                                         std::optional<ArrayMetadata> previous) {
    if (!trackedArrays_.insert(name).second)
        return;
    arrayDeltas_.push_back({name, previous});
}

/// @brief Records a literal channel's entry state on its first mutation.
/// @param channel Channel key.
/// @param previouslyOpen Whether the channel was present before the procedure.
void SemanticAnalyzer::ProcedureScope::noteChannelMutation(long long channel, bool previouslyOpen) {
    if (!trackedChannels_.insert(channel).second)
        return;
    channelDeltas_.push_back({channel, previouslyOpen});
}

/// @brief Records a newly inserted procedure label for rollback.
/// @param label Label-set value.
void SemanticAnalyzer::ProcedureScope::noteLabelInserted(int label) {
    newLabels_.push_back(label);
}

/// @brief Records a newly inserted procedure label reference for rollback.
/// @param label Reference-set value.
void SemanticAnalyzer::ProcedureScope::noteLabelRefInserted(int label) {
    newLabelRefs_.push_back(label);
}

/// @brief Registers one parameter in lexical, symbol, type, and array state.
/// @details Initially binds source spelling to itself. Scalar type comes from
///          the AST; arrays become object/string/integer-array categories and
///          receive unknown-shape metadata. Existing type/array baselines are
///          logged for rollback. Symbol tracking may resolve the name further,
///          in which case the source spelling is rebound to that resolved key.
/// @param param Parsed parameter metadata.
/// @pre A lexical scope is active; normally a @ref ProcedureScope is active.
void SemanticAnalyzer::registerProcedureParam(const Param &param) {
    scopes_.bind(param.name, param.name);

    std::string paramName = param.name;
    Type paramType = astToSemanticType(param.type);
    if (param.is_array) {
        if (!param.objectClass.empty())
            paramType = Type::ArrayObject;
        else if (param.type == ::il::frontends::basic::Type::Str)
            paramType = Type::ArrayString;
        else
            paramType = Type::ArrayInt;
    }

    auto itType = varTypes_.find(paramName);
    if (activeProcScope_) {
        std::optional<Type> previous;
        if (itType != varTypes_.end())
            previous = itType->second;
        activeProcScope_->noteVarTypeMutation(paramName, previous);
    }
    varTypes_[paramName] = paramType;

    if (param.is_array) {
        auto itArray = arrays_.find(paramName);
        if (activeProcScope_) {
            std::optional<ArrayMetadata> previous;
            if (itArray != arrays_.end())
                previous = itArray->second;
            activeProcScope_->noteArrayMutation(paramName, previous);
        }
        // Array parameters have unknown size
        arrays_[paramName] = ArrayMetadata();
    }

    resolveAndTrackSymbol(paramName, SymbolKind::Definition);
    if (paramName != param.name)
        scopes_.bind(param.name, paramName);
}

/// @brief Performs common scoped traversal for SUB/FUNCTION bodies.
/// @details Creates a procedure transaction, optionally pushes an EXIT context,
///          registers parameters, pre-collects only user line labels for forward
///          references, visits non-null body statements, invokes @p bodyCheck,
///          and pops the optional context before rollback at scope exit.
/// @tparam Proc AST node type modelling the procedure declaration.
/// @tparam BodyCallback Callable accepting the analyzed `const Proc &`.
/// @param proc Procedure declaration being analyzed.
/// @param bodyCheck Callback that performs procedure-specific validation.
/// @param loopKind Optional loop kind for EXIT statement validation.
template <typename Proc, typename BodyCallback>
void SemanticAnalyzer::analyzeProcedureCommon(const Proc &proc,
                                              BodyCallback &&bodyCheck,
                                              std::optional<LoopKind> loopKind) {
    ProcedureScope procScope(*this);

    // Push loop context if this is a SUB or FUNCTION (for EXIT SUB/FUNCTION support)
    std::optional<std::pair<LoopKind, size_t>> loopState;
    if (loopKind) {
        loopStack_.push_back(*loopKind);
        loopState = std::make_pair(*loopKind, loopStack_.size() - 1);
    }

    for (const auto &p : proc.params)
        registerProcedureParam(p);
    for (const auto &st : proc.body)
        if (st && hasUserLine(st->line)) {
            auto insertResult = labels_.insert(st->line);
            if (insertResult.second && activeProcScope_)
                activeProcScope_->noteLabelInserted(st->line);
        }
    for (const auto &st : proc.body)
        if (st)
            visitStmt(*st);

    std::forward<BodyCallback>(bodyCheck)(proc);

    // Pop loop context
    if (loopState) {
        loopStack_.pop_back();
    }
}

/// @brief Analyzes a FUNCTION in its declaration namespace.
/// @details Temporarily derives @ref nsStack_ from the function's qualified
///          name and installs active-function result state. A supported name
///          suffix conflicting with an explicit return type emits `B4006`.
///          Common body traversal then requires explicit/VB-style result flow,
///          emitting `B1007` at END FUNCTION or the declaration when missing.
///          Foreign (`DECLARE FOREIGN FUNCTION`) declarations are bodyless
///          imports and are exempt from the result-flow requirement.
///          All active-function and namespace state is restored afterward.
/// @param f Function declaration borrowed during analysis.
void SemanticAnalyzer::analyzeProc(const FunctionDecl &f) {
    // Preserve current namespace stack and establish the procedure's namespace context
    auto savedNs = nsStack_;
    if (!f.qualifiedName.empty()) {
        // Split qualified name into segments and drop the final identifier (the proc name)
        std::vector<std::string> segs = SplitDots(f.qualifiedName);
        if (!segs.empty())
            segs.pop_back();
        nsStack_ = segs;
    }

    const FunctionDecl *previousFunction = activeFunction_;
    BasicType previousExplicit = activeFunctionExplicitRet_;
    bool previousNameAssigned = activeFunctionNameAssigned_;
    activeFunction_ = &f;
    activeFunctionExplicitRet_ = f.explicitRetType;
    activeFunctionNameAssigned_ = false; // BUG-003: Reset for each function

    if (f.explicitRetType != BasicType::Unknown) {
        if (auto suffix = semantic_analyzer_detail::suffixBasicType(f.name)) {
            if (*suffix != f.explicitRetType) {
                std::string msg = "Conflicting return type: AS ";
                msg += semantic_analyzer_detail::uppercaseBasicTypeName(f.explicitRetType);
                msg += " vs name suffix";
                de.emit(il::support::Severity::Error,
                        "B4006",
                        f.loc,
                        static_cast<uint32_t>(f.name.size()),
                        std::move(msg));
            }
        }
    }

    /// Checks the fully analyzed function body for guaranteed result flow.
    analyzeProcedureCommon(
        f,
        [this](const FunctionDecl &func) {
            // DECLARE FOREIGN FUNCTION has no body by construction; the result
            // is produced by the defining module, so result flow is not this
            // declaration's obligation.
            if (func.isForeign)
                return;

            if (mustReturn(func.body))
                return;

            std::string msg = "missing return in FUNCTION " + func.name;
            de.emit(il::support::Severity::Error,
                    "B1007",
                    func.endLoc.isValid() ? func.endLoc : func.loc,
                    3,
                    std::move(msg));
        },
        LoopKind::Function);

    activeFunction_ = previousFunction;
    activeFunctionExplicitRet_ = previousExplicit;
    activeFunctionNameAssigned_ = previousNameAssigned; // BUG-003: Restore state

    // Restore namespace context
    nsStack_ = std::move(savedNs);
}

/// @brief Analyzes a SUB in its declaration namespace.
/// @details Temporarily derives the namespace stack from the qualified name,
///          performs common scoped body traversal under a SUB EXIT context, and
///          restores the prior namespace stack. No final return-flow callback is
///          required.
/// @param s Subroutine declaration borrowed during analysis.
void SemanticAnalyzer::analyzeProc(const SubDecl &s) {
    // Preserve current namespace stack and establish the procedure's namespace context
    auto savedNs = nsStack_;
    if (!s.qualifiedName.empty()) {
        std::vector<std::string> segs = SplitDots(s.qualifiedName);
        if (!segs.empty())
            segs.pop_back();
        nsStack_ = segs;
    }

    /// No-op final checker because SUB bodies need no result guarantee.
    analyzeProcedureCommon(s, [](const SubDecl &) {}, LoopKind::Sub);

    // Restore namespace context
    nsStack_ = std::move(savedNs);
}

namespace {

/// @brief Conservative result-flow state after a statement or statement list.
struct ReturnFlow {
    /// @brief Whether every modeled path terminates with a supplied result.
    bool alwaysReturns{false};

    /// @brief Whether every continuing modeled path has assigned the active
    ///        FUNCTION name.
    bool assignedAfter{false};
};

/// @brief Tests for VB-style assignment to the active FUNCTION name.
/// @param stmt Candidate LET statement.
/// @param activeFunction Function whose name is compared case-insensitively.
/// @return @c true only for a variable-target LET matching the active name.
bool assignsActiveFunctionName(const Stmt &stmt, const FunctionDecl *activeFunction) {
    if (!activeFunction)
        return false;
    const auto *let = as<const LetStmt>(stmt);
    if (!let || !let->target)
        return false;
    const auto *target = as<const VarExpr>(*let->target);
    return target && string_utils::iequals(target->name, activeFunction->name);
}

/// @brief Computes conservative result flow for one statement.
/// @param stmt Statement to classify.
/// @param activeFunction Function used for VB-style name assignment.
/// @param assignedBefore Whether every incoming path already has a result.
/// @return Return/assignment state after @p stmt.
ReturnFlow flowForStmt(const Stmt &stmt, const FunctionDecl *activeFunction, bool assignedBefore);

/// @brief Propagates result flow through an ordered statement list.
/// @details Null statements are skipped. Traversal stops at the first statement
///          that always returns; otherwise the assignment guarantee threads
///          into the next statement.
/// @param stmts Statements in execution order.
/// @param activeFunction Function used for name-assignment recognition.
/// @param assignedBefore Incoming assignment guarantee.
/// @return Flow after the first guaranteed return or the complete list.
ReturnFlow flowForStmtList(const std::vector<StmtPtr> &stmts,
                           const FunctionDecl *activeFunction,
                           bool assignedBefore) {
    bool assigned = assignedBefore;
    for (const auto &stmt : stmts) {
        if (!stmt)
            continue;
        ReturnFlow flow = flowForStmt(*stmt, activeFunction, assigned);
        if (flow.alwaysReturns)
            return flow;
        assigned = flow.assignedAfter;
    }
    return {false, assigned};
}

/// @brief Computes branch flow while tolerating a missing branch node.
/// @param stmt Optional branch statement.
/// @param activeFunction Function used for name-assignment recognition.
/// @param assignedBefore Incoming assignment guarantee.
/// @return Unchanged continuing flow for null, otherwise @ref flowForStmt.
ReturnFlow flowForBranchStmt(const StmtPtr &stmt,
                             const FunctionDecl *activeFunction,
                             bool assignedBefore) {
    if (!stmt)
        return {false, assignedBefore};
    return flowForStmt(*stmt, activeFunction, assignedBefore);
}

/// @brief Merges result guarantees across IF/ELSEIF/ELSE branches.
/// @details Guaranteed return requires an ELSE and every branch returning.
///          Guaranteed assignment after the IF requires every continuing branch
///          to assign or return; without ELSE the incoming assignment state
///          represents the unselected path.
/// @param stmt IF statement to inspect.
/// @param activeFunction Function used for name-assignment recognition.
/// @param assignedBefore Incoming assignment guarantee.
/// @return Merged branch flow.
ReturnFlow flowForIf(const IfStmt &stmt, const FunctionDecl *activeFunction, bool assignedBefore) {
    std::vector<ReturnFlow> branches;
    branches.push_back(flowForBranchStmt(stmt.then_branch, activeFunction, assignedBefore));
    for (const auto &elseif : stmt.elseifs)
        branches.push_back(flowForBranchStmt(elseif.then_branch, activeFunction, assignedBefore));

    const bool hasElse = stmt.else_branch != nullptr;
    if (hasElse)
        branches.push_back(flowForBranchStmt(stmt.else_branch, activeFunction, assignedBefore));

    bool allReturn = hasElse;
    bool assignedAfter = hasElse ? true : assignedBefore;
    for (const ReturnFlow &branch : branches) {
        allReturn = allReturn && branch.alwaysReturns;
        assignedAfter = assignedAfter && (branch.alwaysReturns || branch.assignedAfter);
    }
    return {allReturn, assignedAfter};
}

/// @brief Merges result guarantees across SELECT CASE arms.
/// @details A non-empty CASE ELSE body is required for exhaustive return or new
///          assignment guarantees. Every explicit arm and the else body are
///          analyzed as statement lists from the same incoming state.
/// @param stmt SELECT statement to inspect.
/// @param activeFunction Function used for name-assignment recognition.
/// @param assignedBefore Incoming assignment guarantee.
/// @return Merged arm flow.
ReturnFlow flowForSelect(const SelectCaseStmt &stmt,
                         const FunctionDecl *activeFunction,
                         bool assignedBefore) {
    bool allReturn = !stmt.elseBody.empty();
    bool assignedAfter = stmt.elseBody.empty() ? assignedBefore : true;

    for (const auto &arm : stmt.arms) {
        ReturnFlow armFlow = flowForStmtList(arm.body, activeFunction, assignedBefore);
        allReturn = allReturn && armFlow.alwaysReturns;
        assignedAfter = assignedAfter && (armFlow.alwaysReturns || armFlow.assignedAfter);
    }

    if (!stmt.elseBody.empty()) {
        ReturnFlow elseFlow = flowForStmtList(stmt.elseBody, activeFunction, assignedBefore);
        allReturn = allReturn && elseFlow.alwaysReturns;
        assignedAfter = assignedAfter && (elseFlow.alwaysReturns || elseFlow.assignedAfter);
    }

    return {allReturn, assignedAfter};
}

/// @brief Merges result flow across TRY, optional CATCH, and FINALLY.
/// @details An always-returning FINALLY dominates. Otherwise normal-return
///          status requires TRY and any non-empty CATCH to return, while the
///          assignment guarantee entering FINALLY requires both possible
///          pre-finally paths to have returned or assigned.
/// @param stmt TRY/CATCH/FINALLY statement.
/// @param activeFunction Function used for name-assignment recognition.
/// @param assignedBefore Incoming assignment guarantee.
/// @return Flow after FINALLY.
ReturnFlow flowForTry(const TryCatchStmt &stmt,
                      const FunctionDecl *activeFunction,
                      bool assignedBefore) {
    ReturnFlow tryFlow = flowForStmtList(stmt.tryBody, activeFunction, assignedBefore);
    ReturnFlow catchFlow = stmt.catchBody.empty()
                               ? ReturnFlow{false, assignedBefore}
                               : flowForStmtList(stmt.catchBody, activeFunction, assignedBefore);

    bool normalReturn = tryFlow.alwaysReturns;
    if (!stmt.catchBody.empty())
        normalReturn = normalReturn && catchFlow.alwaysReturns;

    bool assignedBeforeFinally =
        (tryFlow.alwaysReturns || tryFlow.assignedAfter) &&
        (stmt.catchBody.empty() || catchFlow.alwaysReturns || catchFlow.assignedAfter);
    ReturnFlow finallyFlow =
        flowForStmtList(stmt.finallyBody, activeFunction, assignedBeforeFinally);
    if (finallyFlow.alwaysReturns)
        return finallyFlow;
    return {normalReturn, finallyFlow.assignedAfter};
}

/// @brief Classifies one statement for function-result flow.
/// @details Handles statement lists, value/assigned RETURN, EXIT FUNCTION,
///          function-name assignment, IF, SELECT, and TRY. Loops are
///          conservatively treated as possibly unexecuted and do not establish
///          new guarantees; all other statements preserve incoming assignment.
/// @param stmt Statement to inspect.
/// @param activeFunction Function used for name-assignment recognition.
/// @param assignedBefore Incoming assignment guarantee.
/// @return Conservative post-statement flow.
ReturnFlow flowForStmt(const Stmt &stmt, const FunctionDecl *activeFunction, bool assignedBefore) {
    if (const auto *lst = as<const StmtList>(stmt))
        return flowForStmtList(lst->stmts, activeFunction, assignedBefore);
    if (const auto *ret = as<const ReturnStmt>(stmt))
        return {ret->value != nullptr || assignedBefore, assignedBefore};
    if (const auto *exitStmt = as<const ExitStmt>(stmt)) {
        if (exitStmt->kind == ExitStmt::LoopKind::Function)
            return {assignedBefore, assignedBefore};
        return {false, assignedBefore};
    }
    if (assignsActiveFunctionName(stmt, activeFunction))
        return {false, true};
    if (const auto *ifs = as<const IfStmt>(stmt))
        return flowForIf(*ifs, activeFunction, assignedBefore);
    if (const auto *select = as<const SelectCaseStmt>(stmt))
        return flowForSelect(*select, activeFunction, assignedBefore);
    if (const auto *tryCatch = as<const TryCatchStmt>(stmt))
        return flowForTry(*tryCatch, activeFunction, assignedBefore);
    if (is<WhileStmt>(stmt) || is<ForStmt>(stmt) || is<DoStmt>(stmt) || is<ForEachStmt>(stmt))
        return {false, assignedBefore};
    return {false, assignedBefore};
}

/// @brief Tests whether a statement subtree contains GOSUB.
/// @param stmt Root statement.
/// @return @c true when a recognized nested construct contains GOSUB.
bool containsGosub(const Stmt &stmt);

/// @brief Tests a statement list for any nested GOSUB.
/// @param stmts List whose null entries are skipped.
/// @return @c true on the first matching subtree.
bool containsGosubList(const std::vector<StmtPtr> &stmts) {
    for (const auto &stmt : stmts)
        if (stmt && containsGosub(*stmt))
            return true;
    return false;
}

/// @brief Recursively tests one statement for GOSUB.
/// @details Descends through lists, IF branches, SELECT arms/else, all TRY
///          bodies, loop bodies, and namespace bodies. Other constructs are not
///          traversed.
/// @param stmt Statement subtree root.
/// @return @c true when GOSUB is found.
bool containsGosub(const Stmt &stmt) {
    if (is<GosubStmt>(stmt))
        return true;
    if (const auto *lst = as<const StmtList>(stmt))
        return containsGosubList(lst->stmts);
    if (const auto *ifs = as<const IfStmt>(stmt)) {
        if (ifs->then_branch && containsGosub(*ifs->then_branch))
            return true;
        for (const auto &elseif : ifs->elseifs)
            if (elseif.then_branch && containsGosub(*elseif.then_branch))
                return true;
        return ifs->else_branch && containsGosub(*ifs->else_branch);
    }
    if (const auto *select = as<const SelectCaseStmt>(stmt)) {
        for (const auto &arm : select->arms)
            if (containsGosubList(arm.body))
                return true;
        return containsGosubList(select->elseBody);
    }
    if (const auto *tryCatch = as<const TryCatchStmt>(stmt)) {
        return containsGosubList(tryCatch->tryBody) || containsGosubList(tryCatch->catchBody) ||
               containsGosubList(tryCatch->finallyBody);
    }
    if (const auto *whileStmt = as<const WhileStmt>(stmt))
        return containsGosubList(whileStmt->body);
    if (const auto *doStmt = as<const DoStmt>(stmt))
        return containsGosubList(doStmt->body);
    if (const auto *forStmt = as<const ForStmt>(stmt))
        return containsGosubList(forStmt->body);
    if (const auto *forEach = as<const ForEachStmt>(stmt))
        return containsGosubList(forEach->body);
    if (const auto *ns = as<const NamespaceDecl>(stmt))
        return containsGosubList(ns->body);
    return false;
}

} // namespace

/// @brief Determines whether a statement list guarantees a function result.
/// @param stmts Body evaluated with no incoming function-name assignment.
/// @return @c true when modeled flow always returns or every continuing path
///         assigns the active function name.
bool SemanticAnalyzer::mustReturn(const std::vector<StmtPtr> &stmts) const {
    ReturnFlow flow = flowForStmtList(stmts, activeFunction_, false);
    return flow.alwaysReturns || flow.assignedAfter;
}

/// @brief Determines whether one statement guarantees a function result.
/// @param s Statement evaluated with no incoming function-name assignment.
/// @return @c true for guaranteed return or guaranteed name assignment.
bool SemanticAnalyzer::mustReturn(const Stmt &s) const {
    ReturnFlow flow = flowForStmt(s, activeFunction_, false);
    return flow.alwaysReturns || flow.assignedAfter;
}

/// @brief Reports the main-module GOSUB pre-scan result.
/// @return Cached value populated by @ref analyze.
bool SemanticAnalyzer::mainHasGosub() const noexcept {
    return mainHasGosub_;
}

/// @brief Orchestrates semantic analysis for an entire BASIC program.
/// @details Clears core symbol/control/procedure state and creates a root USING
///          scope. It registers top-level and namespace-nested procedures before
///          calls are checked, rebuilds OOP and namespace/USING indexes, seeds
///          non-alias file imports, and constructs the type resolver. Main
///          user-line labels and GOSUB presence are pre-collected before the
///          module body is visited. Top-level procedures are then analyzed, and
///          a recursive namespace pass analyzes nested procedure bodies under
///          inherited imports plus namespace-local USING directives.
/// @param prog Program whose shared AST nodes may receive semantic annotations.
/// @post Procedure-local mutations have been rolled back; module-level query
///       state and rebuilt registries remain available.
void SemanticAnalyzer::analyze(const Program &prog) {
    symbols_.clear();
    labels_.clear();
    labelRefs_.clear();
    forStack_.clear();
    loopStack_.clear();
    varTypes_.clear();
    objectClassTypes_.clear();
    arrays_.clear();
    openChannels_.clear();
    errorHandlerActive_ = false;
    errorHandlerTarget_.reset();
    mainHasGosub_ = false;
    activeClassQName_.clear();
    activeMemberHasMe_ = false;
    procReg_.clear();
    scopes_.reset();
    sawDecl_ = false;
    usingStack_.clear();
    usingStack_.push_back(UsingScope{}); // root scope

    // Register procedure signatures first (needed for call resolution)
    for (const auto &p : prog.procs)
        if (p) {
            if (auto *f = as<FunctionDecl>(*p))
                procReg_.registerProc(*f);
            else if (auto *s = as<SubDecl>(*p))
                procReg_.registerProc(*s);
        }

    // Also register procedures declared inside namespace blocks using their
    // fully-qualified names (assigned by CollectProcedures).
    /// Recursively registers FUNCTION/SUB declarations inside namespaces.
    std::function<void(const std::vector<StmtPtr> &)> scan;
    scan = [&](const std::vector<StmtPtr> &stmts) {
        for (const auto &stmtPtr : stmts) {
            if (!stmtPtr)
                continue;
            switch (stmtPtr->stmtKind()) {
                case Stmt::Kind::NamespaceDecl: {
                    const auto &ns = static_cast<const NamespaceDecl &>(*stmtPtr);
                    scan(ns.body);
                    break;
                }
                case Stmt::Kind::FunctionDecl:
                    procReg_.registerProc(static_cast<const FunctionDecl &>(*stmtPtr));
                    break;
                case Stmt::Kind::SubDecl:
                    procReg_.registerProc(static_cast<const SubDecl &>(*stmtPtr));
                    break;
                default:
                    break;
            }
        }
    };
    scan(prog.main);

    oopIndex_.clear();
    buildOopIndex(prog, oopIndex_, &de.emitter());

    // Build namespace registry and USING context.
    buildNamespaceRegistry(prog, ns_, usings_, &de.emitter());

    // Seed root USING scope from file-scoped UsingContext (declaration order preserved).
    if (!usings_.imports().empty()) {
        UsingScope &root = usingStack_.front();
        for (const auto &imp : usings_.imports()) {
            // Seed only non-alias imports here; alias entries are validated and
            // applied by analyzeUsingDecl during statement analysis (so shadowing
            // checks run correctly).
            if (imp.alias.empty()) {
                std::string nsCanon = CanonicalizeQualified(SplitDots(imp.ns));
                root.imports.insert(std::move(nsCanon));
            }
        }
    }

    // Construct TypeResolver after declare pass completes.
    resolver_ = std::make_unique<TypeResolver>(ns_, usings_);

    // Pre-collect all labels (line numbers) from main block for GOTO/GOSUB validation.
    // This must happen before analysis since GOTO can jump forward.
    for (const auto &stmt : prog.main)
        if (stmt && hasUserLine(stmt->line))
            labels_.insert(stmt->line);
    mainHasGosub_ = containsGosubList(prog.main);

    // Analyze main module body so module-level variables are registered
    // in symbols_ before procedures are analyzed. This allows procedures to
    // reference module-level globals without explicit parameters.
    for (const auto &stmt : prog.main)
        if (stmt)
            visitStmt(*stmt);

    // Now analyze procedure bodies - they can reference module-level symbols
    for (const auto &p : prog.procs)
        if (p) {
            if (auto *f = as<FunctionDecl>(*p))
                analyzeProc(*f);
            else if (auto *s = as<SubDecl>(*p))
                analyzeProc(*s);
        }

    // Additionally analyze procedures declared inside namespace blocks.
    /// Recursively analyzes namespace-nested procedure bodies while maintaining
    /// namespace and USING stacks.
    std::function<void(const std::vector<StmtPtr> &)> scanBodies;
    scanBodies = [&](const std::vector<StmtPtr> &stmts) {
        for (const auto &stmtPtr : stmts) {
            if (!stmtPtr)
                continue;
            switch (stmtPtr->stmtKind()) {
                case Stmt::Kind::NamespaceDecl: {
                    const auto &ns = static_cast<const NamespaceDecl &>(*stmtPtr);
                    // Establish namespace context
                    for (const auto &seg : ns.path)
                        nsStack_.push_back(seg);
                    // Establish USING scope for this namespace and apply any
                    // USING directives declared inside the block before
                    // analyzing procedure bodies.
                    UsingScope childScope;
                    if (!usingStack_.empty()) {
                        // Inherit imports from parent scope; aliases are local.
                        childScope.imports = usingStack_.back().imports;
                    }
                    usingStack_.push_back(std::move(childScope));
                    // First pass: apply USINGs declared in this namespace.
                    for (const auto &s : ns.body) {
                        if (s && s->stmtKind() == Stmt::Kind::UsingDecl)
                            analyzeUsingDecl(static_cast<UsingDecl &>(*s));
                    }
                    scanBodies(ns.body);
                    if (nsStack_.size() >= ns.path.size())
                        nsStack_.resize(nsStack_.size() - ns.path.size());
                    else
                        nsStack_.clear();
                    if (!usingStack_.empty())
                        usingStack_.pop_back();
                    break;
                }
                case Stmt::Kind::FunctionDecl:
                    analyzeProc(static_cast<const FunctionDecl &>(*stmtPtr));
                    break;
                case Stmt::Kind::SubDecl:
                    analyzeProc(static_cast<const SubDecl &>(*stmtPtr));
                    break;
                default:
                    break;
            }
        }
    };
    scanBodies(prog.main);
}

/// @brief Resolves a call target and enforces its callable category.
/// @details BASIC type suffixes are stripped before lookup. Qualified calls
///          expand an innermost scoped alias first, then a file alias, and try a
///          canonical registry name. A registered runtime class-method alias may
///          rewrite the mutable call to its canonical target. Unqualified calls
///          choose the nearest enclosing namespace match, then global, then
///          collect import matches; multiple imports emit an ambiguity
///          diagnostic with display-cased names. Failed qualified alias
///          expansion adds an alias note, while other failures report qualified
///          or attempted names. Functions are permitted where a SUB statement
///          call is expected; a SUB in expression context emits `B2005`.
/// @param c Call expression. Runtime alias fallback may update its callee fields
///          through the mutable AST despite the const reference.
/// @param expectedKind FUNCTION for expression calls or SUB for statement calls.
/// @return Pointer to registry-owned signature, or @c nullptr after resolution,
///         ambiguity, or kind diagnostics.
const ProcSignature *SemanticAnalyzer::resolveCallee(const CallExpr &c,
                                                     ProcSignature::Kind expectedKind) {
    /// Removes one recognized BASIC type suffix from a non-empty name.
    auto stripSuffix = [](std::string_view name) -> std::string_view {
        if (name.empty())
            return name;
        char last = name.back();
        switch (last) {
            case '$':
            case '#':
            case '!':
            case '&':
            case '%':
                return name.substr(0, name.size() - 1);
            default:
                return name;
        }
    };

    // Build candidate list.
    std::vector<std::string> attempts;
    const ProcSignature *sig = nullptr;

    if (!c.calleeQualified.empty()) {
        // Prepare raw segments with last-suffix stripped, then expand alias if present.
        std::vector<std::string> segs = c.calleeQualified;
        if (!segs.empty()) {
            std::string &last = segs.back();
            if (!last.empty()) {
                char t = last.back();
                if (t == '$' || t == '#' || t == '!' || t == '&' || t == '%')
                    last.pop_back();
            }
        }

        // Alias expansion: if first segment matches an alias, replace it with the target path.
        bool usedAlias = false;
        std::string usedAliasName;
        std::string usedAliasTarget;
        if (!segs.empty()) {
            std::string firstCanon = CanonicalizeIdent(segs[0]);
            // 1) Prefer the innermost scoped USING aliases (namespace-scoped or file-scoped).
            if (!usingStack_.empty()) {
                const auto &aliases = usingStack_.back().aliases;
                auto itAlias = aliases.find(firstCanon);
                if (itAlias != aliases.end()) {
                    std::vector<std::string> expanded = SplitDots(itAlias->second);
                    expanded.insert(expanded.end(), segs.begin() + 1, segs.end());
                    segs = std::move(expanded);
                    usedAlias = true;
                    usedAliasName = firstCanon;
                    usedAliasTarget = itAlias->second;
                }
            }
            // 2) If not found in the scoped USINGs, consult file-scoped UsingContext aliases.
            if (!usedAlias && usings_.hasAlias(firstCanon)) {
                std::string target = usings_.resolveAlias(firstCanon);
                if (!target.empty()) {
                    std::vector<std::string> expanded = SplitDots(target);
                    expanded.insert(expanded.end(), segs.begin() + 1, segs.end());
                    segs = std::move(expanded);
                    usedAlias = true;
                    usedAliasName = firstCanon;
                    usedAliasTarget = std::move(target);
                }
            }
        }

        // Canonicalize
        std::vector<std::string> canonSegs;
        canonSegs.reserve(segs.size());
        for (const auto &seg : segs) {
            std::string cseg = CanonicalizeIdent(seg);
            if (cseg.empty() && !seg.empty()) {
                canonSegs.clear();
                break;
            }
            canonSegs.emplace_back(std::move(cseg));
        }
        std::string q;
        if (!canonSegs.empty())
            q = JoinQualified(canonSegs);
        if (q.empty()) {
            q = c.callee; // fallback to original text when canonicalization fails
        }
        attempts.push_back(q);
        sig = procReg_.lookup(q);
        if (!sig && segs.size() >= 2) {
            // Runtime class-method alias fallback: classes may expose methods that
            // alias functions registered under a different canonical name (e.g.
            // Zanna.Data.Json.GetStr aliases Zanna.Collections.Map.GetStr). Resolve
            // the class surface and rewrite the call to the alias target so the
            // rest of the pipeline (argument checks, lowering) sees the real
            // function, keeping BASIC's callable surface equal to Zia's.
            std::vector<std::string> classSegs(segs.begin(), segs.end() - 1);
            const std::string classQ = JoinDots(classSegs);
            if (il::runtime::findRuntimeClassByQName(classQ)) {
                if (auto alias = runtimeMethodIndex().find(classQ, segs.back(), c.args.size())) {
                    if (const auto *aliasSig = procReg_.lookup(alias->target)) {
                        auto &mutableCall = const_cast<CallExpr &>(c);
                        mutableCall.calleeQualified = SplitDots(alias->target);
                        mutableCall.callee = alias->target;
                        sig = aliasSig;
                    }
                }
            }
        }
        if (!sig && usedAlias) {
            // Unknown after alias expansion: show canonical and note alias mapping.
            diagx::ErrorUnknownProcQualified(de.emitter(), c.loc, q);
            diagx::NoteAliasExpansion(de.emitter(), usedAliasName, usedAliasTarget);
            return nullptr;
        }
    } else {
        // Unqualified resolution: parent-walk, then USING imports.
        std::vector<std::string> prefixCanon;
        prefixCanon.reserve(nsStack_.size());
        for (const auto &seg : nsStack_) {
            std::string canon = CanonicalizeIdent(seg);
            prefixCanon.push_back(canon.empty() ? seg : canon);
        }
        std::string ident = std::string{stripSuffix(c.callee)};
        ident = CanonicalizeIdent(ident);

        // Parent-walk precedence: choose the nearest match only. Do not
        // accumulate multiple hits along the chain; if multiple levels define
        // the same name, the nearest wins deterministically.
        for (std::size_t n = prefixCanon.size(); n > 0 && !sig; --n) {
            std::vector<std::string> parts(prefixCanon.begin(),
                                           prefixCanon.begin() + static_cast<std::ptrdiff_t>(n));
            parts.push_back(ident);
            std::string q = JoinQualified(parts);
            attempts.push_back(q);
            if (const auto *s = procReg_.lookup(q)) {
                sig = s;
            }
        }
        if (!sig) {
            // Try global unqualified ident.
            attempts.push_back(ident);
            sig = procReg_.lookup(ident);
        }

        if (!sig) {
            // No parent/global hit: try imported namespaces (USING imports only, no aliases).
            std::vector<std::string> importHits;
            if (!usingStack_.empty()) {
                const UsingScope &cur = usingStack_.back();
                for (const auto &ns : cur.imports) {
                    std::string q = ns;
                    if (!q.empty())
                        q.push_back('.');
                    q += ident;
                    attempts.push_back(q);
                    if (procReg_.lookup(q)) {
                        // Build a display name preserving namespace casing and, when possible,
                        // the original function spelling. This keeps diagnostics stable and
                        // user-friendly.
                        std::string displayNs = ns;
                        if (const auto *info = ns_.info(ns))
                            displayNs = info->full;

                        std::string display = displayNs + "." + ident;
                        // Attempt to find an original-cased key in the proc table that
                        // canonicalizes to the same qualified name.
                        const auto &procs = procReg_.procs();
                        int bestScore = -1; // -1: none, 0: lowercase fallback, 1: mixed-case, 2:
                                            // preferred ns + mixed-case
                        for (const auto &kv : procs) {
                            const std::string &key = kv.first;
                            // Canonicalize key and compare against q (already canonicalized
                            // ns+ident).
                            std::vector<std::string> parts = SplitDots(key);
                            std::string keyCanon = CanonicalizeQualified(parts);
                            if (keyCanon == q) {
                                // Score this candidate: prefer matching namespace + mixed-case key.
                                bool hasUpper = false;
                                for (char ch : key)
                                    if (std::isupper(static_cast<unsigned char>(ch))) {
                                        hasUpper = true;
                                        break;
                                    }
                                int score = hasUpper ? 1 : 0;
                                // Check namespace prefix equality when available.
                                auto lastDot = key.rfind('.');
                                std::string keyNs = lastDot != std::string::npos
                                                        ? key.substr(0, lastDot)
                                                        : std::string{};
                                if (hasUpper && keyNs == displayNs)
                                    score = 2;
                                if (score > bestScore) {
                                    bestScore = score;
                                    display = key;
                                    if (bestScore == 2)
                                        break; // optimal
                                }
                            }
                        }
                        importHits.push_back(std::move(display));
                    }
                }
            }
            if (importHits.size() == 1) {
                sig = procReg_.lookup(importHits[0]);
            } else if (importHits.size() > 1) {
                // Remap matches to display case using namespace registry for the prefix
                // and the original typed callee for the suffix.
                std::vector<std::string> displayHits;
                displayHits.reserve(importHits.size());
                /// Title-cases the first byte of each dotted namespace segment
                /// for fallback ambiguity-display text.
                auto titleCaseNs = [](const std::string &ns) {
                    std::string out;
                    out.reserve(ns.size());
                    bool start = true;
                    for (char ch : ns) {
                        if (ch == '.') {
                            out.push_back('.');
                            start = true;
                        } else {
                            if (start)
                                out.push_back(static_cast<char>(
                                    std::toupper(static_cast<unsigned char>(ch))));
                            else
                                out.push_back(static_cast<char>(
                                    std::tolower(static_cast<unsigned char>(ch))));
                            start = false;
                        }
                    }
                    return out;
                };
                for (const auto &qcanon : importHits) {
                    std::size_t dot = qcanon.rfind('.');
                    std::string nsPart =
                        dot == std::string::npos ? std::string{} : qcanon.substr(0, dot);
                    std::string displayNs = nsPart;
                    if (const auto *info = ns_.info(nsPart))
                        displayNs = info->full;
                    // Normalise namespace display to TitleCase for readability.
                    std::string nsTitle = titleCaseNs(displayNs);
                    std::string display =
                        nsTitle.empty() ? std::string(c.callee) : nsTitle + "." + c.callee;
                    displayHits.push_back(std::move(display));
                }
                diagx::ErrorAmbiguousProc(de.emitter(), c.loc, c.callee, displayHits);
                return nullptr;
            }
        }
    }
    if (!sig) {
        if (!c.calleeQualified.empty()) {
            std::string q = CanonicalizeQualified(c.calleeQualified);
            diagx::ErrorUnknownProcQualified(de.emitter(), c.loc, q.empty() ? c.callee : q);
        } else {
            diagx::ErrorUnknownProcWithTries(de.emitter(), c.loc, c.callee, attempts);
        }
        return nullptr;
    }
    if (sig->kind != expectedKind) {
        // Allow calling functions as statements by accepting FUNCTION where SUB is expected.
        if (expectedKind == ProcSignature::Kind::Sub &&
            sig->kind == ProcSignature::Kind::Function) {
            return sig;
        }
        if (expectedKind == ProcSignature::Kind::Function) {
            std::string msg = "subroutine '" + c.callee +
                              "' used in expression; convert to FUNCTION or call as a statement";
            de.emit(il::support::Severity::Error,
                    "B2005",
                    c.loc,
                    static_cast<uint32_t>(c.callee.size()),
                    std::move(msg));
        } else {
            std::string msg = "function '" + c.callee + "' cannot be called as a statement";
            de.emit(il::support::Severity::Error,
                    "B2015",
                    c.loc,
                    static_cast<uint32_t>(c.callee.size()),
                    std::move(msg));
        }
        return nullptr;
    }
    return sig;
}

/// @brief Evaluates and validates arguments against a resolved signature.
/// @details Every argument is visited first, using unknown for null nodes, even
///          when @p sig is null. Exact arity is required. Array parameters
///          require a direct variable present in array metadata. Runtime object
///          parameters reject integer/float/boolean primitives except literal
///          integer zero; object, string, and unknown pass. Scalar compatibility
///          permits integer-to-float, float-to-integer, numeric-to-boolean, and
///          object-to-I64. Canonical `Zanna.Text.StringBuilder.*` calls relax
///          mismatches for I64 parameters. Runtime signatures receive a final
///          raw-pointer safety check whose boolean result is intentionally not
///          used to suppress the returned type vector.
/// @param c Call supplying AST arguments, callee text, and location.
/// @param sig Registry-owned signature, or null after unresolved lookup.
/// @return Actual semantic argument types in source order.
std::vector<SemanticAnalyzer::Type> SemanticAnalyzer::checkCallArgs(const CallExpr &c,
                                                                    const ProcSignature *sig) {
    std::vector<Type> argTys;
    for (auto &a : c.args)
        argTys.push_back(a ? visitExpr(*a) : Type::Unknown);

    if (!sig)
        return argTys;

    if (c.args.size() != sig->params.size()) {
        std::string msg = "argument count mismatch for '" + c.callee + "': expected " +
                          std::to_string(sig->params.size()) + ", got " +
                          std::to_string(c.args.size());
        de.emit(il::support::Severity::Error,
                "B2008",
                c.loc,
                static_cast<uint32_t>(c.callee.size()),
                std::move(msg));
        return argTys;
    }

    size_t n = std::min(argTys.size(), sig->params.size());
    for (size_t i = 0; i < n; ++i) {
        auto expectTy = sig->params[i].type;
        auto argTy = argTys[i];
        if (sig->params[i].is_array) {
            auto *argExpr = c.args[i].get();
            auto *v = argExpr ? as<VarExpr>(*argExpr) : nullptr;
            if (!v || !arrays_.contains(v->name)) {
                il::support::SourceLoc loc = argExpr ? argExpr->loc : c.loc;
                std::string msg = "argument " + std::to_string(i + 1) + " to " + c.callee +
                                  " must be an array variable (ByRef)";
                de.emit(il::support::Severity::Error, "B2006", loc, 1, std::move(msg));
            }
            continue;
        }
        // Object parameters of runtime builtins are seeded as I64 (the AST type
        // enum cannot express objects); reject primitives explicitly so an i64
        // never reaches IL verification as an object-pointer operand. A literal
        // 0 is permitted as the null-object idiom (e.g. Thread.Start payloads).
        if (sig->isRuntimeBuiltin && i < sig->objectParams.size() && sig->objectParams[i]) {
            if (argTy == Type::Int || argTy == Type::Float || argTy == Type::Bool) {
                const auto *lit = c.args[i] ? as<const IntExpr>(*c.args[i]) : nullptr;
                if (!(argTy == Type::Int && lit && lit->value == 0)) {
                    std::string msg = "argument " + std::to_string(i + 1) + " to " + c.callee +
                                      " expects an OBJECT; box primitives with Zanna.Core.Box";
                    de.emit(il::support::Severity::Error, "B2001", c.loc, 1, std::move(msg));
                }
            }
            continue; // Object/String/Unknown arguments are acceptable object payloads.
        }
        if (expectTy == ::il::frontends::basic::Type::F64 && argTy == Type::Int)
            continue;
        // Float arguments can be implicitly converted to integer parameters (truncation)
        if (expectTy == ::il::frontends::basic::Type::I64 && argTy == Type::Float)
            continue;
        // Integer/float arguments can be truncated to boolean parameters
        if (expectTy == ::il::frontends::basic::Type::Bool &&
            (argTy == Type::Int || argTy == Type::Float))
            continue;
        Type want = Type::Int;
        if (expectTy == ::il::frontends::basic::Type::F64)
            want = Type::Float;
        else if (expectTy == ::il::frontends::basic::Type::Str)
            want = Type::String;
        else if (expectTy == ::il::frontends::basic::Type::Bool)
            want = Type::Bool;
        // Object-typed arguments can match I64 parameters (objects are pointers)
        if (argTy == Type::Object && expectTy == ::il::frontends::basic::Type::I64)
            continue;
        // Relax type checking for selected runtime helpers that operate on opaque pointers.
        // Permit pointer-typed values to match integer-typed parameters for
        // Zanna.Text.StringBuilder.* canonical helpers.
        bool relaxPtrArg = false;
        if (!c.calleeQualified.empty()) {
            // Build canonical lowercase name
            std::string qcanon = CanonicalizeQualified(c.calleeQualified);
            if (qcanon.rfind("zanna.text.stringbuilder.", 0) == 0) {
                relaxPtrArg = true;
            }
        }
        if (argTy != Type::Unknown && argTy != want) {
            if (!(relaxPtrArg && want == Type::Int)) {
                std::string msg = "argument type mismatch";
                de.emit(il::support::Severity::Error, "B2001", c.loc, 1, std::move(msg));
            }
        }
    }

    if (sig->isRuntimeBuiltin) {
        std::string display = sig->runtimeTarget;
        if (display.empty())
            display = !c.calleeQualified.empty() ? JoinQualified(c.calleeQualified) : c.callee;
        checkRuntimePointerSafety(sig->runtimeTarget,
                                  sig->rawPointerReturn,
                                  sig->rawPointerParams,
                                  c.args,
                                  c.loc,
                                  display);
    }
    return argTys;
}

/// @brief Maps a procedure signature's return metadata to semantic type space.
/// @param c Call expression retained for interface symmetry but not inspected.
/// @param sig Resolved signature, or null.
/// @return Unknown without a return type, object when @c objectReturn is set,
///         float/string/boolean for those AST types, and integer for every
///         remaining return type.
SemanticAnalyzer::Type SemanticAnalyzer::inferCallType([[maybe_unused]] const CallExpr &c,
                                                       const ProcSignature *sig) {
    if (!sig || !sig->retType)
        return Type::Unknown;
    if (sig->objectReturn)
        return Type::Object;
    if (*sig->retType == ::il::frontends::basic::Type::F64)
        return Type::Float;
    if (*sig->retType == ::il::frontends::basic::Type::Str)
        return Type::String;
    if (*sig->retType == ::il::frontends::basic::Type::Bool)
        return Type::Bool;
    return Type::Int;
}

/// @brief Delegates expression-call analysis to the shared semantic helper.
/// @param c Function-call expression to resolve and validate.
/// @return Active-instance array-field element type, resolved function result,
///         or unknown recovery type.
SemanticAnalyzer::Type SemanticAnalyzer::analyzeCall(const CallExpr &c) {
    return sem::analyzeCallExpr(*this, c);
}

} // namespace il::frontends::basic
