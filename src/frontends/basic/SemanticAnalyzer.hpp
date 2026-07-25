//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the SemanticAnalyzer class, which performs semantic
// analysis and validation of BASIC AST nodes.
//
// The semantic analyzer is the third stage of the BASIC compilation pipeline:
//   Lexer → Parser → AST → Semantic → Lowerer → IL
//
// Key Responsibilities:
// - Symbol resolution: Validates that all referenced variables, procedures,
//   and labels are properly declared before use
// - Type checking: Ensures type compatibility in expressions, assignments,
//   and procedure calls according to BASIC's type system
// - Scope management: Tracks local/global scopes, procedure parameters,
//   and nested block structures
// - Name resolution: Resolves identifiers to their declarations, handling
//   BASIC's implicit type suffix rules (%, &, !, #, $)
// - Array validation: Checks array dimension counts and bounds specifications
// - Control flow validation: Ensures proper nesting of FOR/NEXT, DO/LOOP,
//   WHILE/WEND, SELECT CASE constructs
// - Procedure validation: Verifies RETURN statements, parameter counts,
//   and function return value requirements
//
// Two-Pass Analysis:
// The analyzer operates in two passes to handle forward references:
// 1. Declaration pass: Collects all SUB/FUNCTION declarations and global
//    variables to build the symbol table
// 2. Validation pass: Checks statement bodies, resolves references, and
//    validates type consistency
//
// Type System Integration:
// - Implicit typing: Variables without explicit DIM use type suffixes
//   (%, &, !, #, $) or default to single-precision floating point
// - Explicit typing: DIM statements declare variable types explicitly
// - Type promotion: Numeric operations may promote integer types to
//   floating-point following BASIC's coercion rules
// - String operations: Validated separately from numeric operations
//
// Error Reporting:
// - Emits structured diagnostics via DiagnosticEmitter for:
//   * Undefined variables and procedures
//   * Type mismatches in expressions and assignments
//   * Invalid control flow (NEXT without FOR, etc.)
//   * Duplicate declarations
//   * Invalid array usage (wrong dimension count, bounds errors)
//
// Design Notes:
// - The analyzer does not modify the AST; it only validates and annotates
//   the symbol table for use by the lowering stage
// - Borrows DiagnosticEmitter for error reporting; does not own it
// - Uses ProcRegistry for tracking procedure signatures
// - Uses ScopeTracker for managing lexical scopes during analysis
//
//===----------------------------------------------------------------------===//

/// @file SemanticAnalyzer.hpp
/// @brief Declares the stateful semantic pass for the BASIC frontend.
/// @details The analyzer coordinates declaration collection, scoped name and
///          type resolution, statement/expression validation, procedure and OOP
///          checks, control-flow bookkeeping, and diagnostic emission. It owns
///          its analysis tables, borrows the diagnostic sink, and may update
///          mutable AST nodes with resolved names or semantic annotations.

#pragma once

#include "frontends/basic/BasicTypes.hpp"
#include "frontends/basic/OopIndex.hpp"
#include "frontends/basic/ProcRegistry.hpp"
#include "frontends/basic/ScopeTracker.hpp"
#include "frontends/basic/SemanticDiagnostics.hpp"
#include "frontends/basic/ast/NodeFwd.hpp"
#include "frontends/basic/sem/NamespaceRegistry.hpp"
#include "frontends/basic/sem/TypeResolver.hpp"
#include "frontends/basic/sem/UsingContext.hpp"
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il::frontends::basic {
/// @brief Stateful BASIC semantic pass declared below.
class SemanticAnalyzer;

namespace sem {
/// @brief RAII/helper context granted access to control-flow state.
class ControlCheckContext;

/// @brief RAII/helper context granted access to expression-analysis state.
class ExprCheckContext;
} // namespace sem

/// @brief Tracked shape metadata for a BASIC array symbol.
/// @details Extents are stored in declaration order. A negative
///          @ref totalSize represents a dynamic or otherwise unknown element
///          count; constructors do not validate extents or recompute products.
struct ArrayMetadata {
    /// @brief Extent value for each known dimension.
    /// @details An empty vector represents a dynamic or unknown shape.
    std::vector<long long> extents;

    /// @brief Precomputed element count, or `-1` when dynamic/unknown.
    long long totalSize{-1};

    /// @brief Constructs metadata for an unknown or dynamic array shape.
    /// @post @ref extents is empty and @ref totalSize is `-1`.
    ArrayMetadata() = default;

    /// @brief Constructs one-dimensional metadata from a supplied extent.
    /// @param size Extent and total count to store verbatim.
    /// @note No range validation is performed.
    explicit ArrayMetadata(long long size) : extents{size}, totalSize{size} {}

    /// @brief Constructs metadata from extents and a caller-computed total.
    /// @param exts Ordered extents moved into the metadata.
    /// @param total Precomputed element count stored without verification.
    ArrayMetadata(std::vector<long long> exts, long long total)
        : extents(std::move(exts)), totalSize(total) {}
};

/// @brief Expression visitor allowed to invoke private analyzer helpers.
class SemanticAnalyzerExprVisitor;

/// @brief Statement visitor allowed to invoke private analyzer helpers.
class SemanticAnalyzerStmtVisitor;

namespace semantic_analyzer_detail {
/// @brief Static type/operand rule for one binary-expression operator.
struct ExprRule;

/// @brief Shared statement-analysis helper with private analyzer access.
class StmtShared;

/// @brief Returns the immutable rule associated with a binary operator.
/// @param op Operator whose semantic rule is requested.
/// @return Reference to static rule metadata.
const ExprRule &exprRule(BinaryExpr::Op op);
} // namespace semantic_analyzer_detail

/// @brief Performs semantic analysis and annotation of a BASIC program.
/// @details The analyzer owns symbol, type, array, procedure, namespace, OOP,
///          label, import, control-flow, and implicit-conversion state. Analysis
///          is split across visitors and specialized translation units but uses
///          this object as the shared state carrier.
/// @invariant @ref de and @ref procReg_ remain bound to the diagnostic emitter
///            supplied at construction.
/// @invariant @ref activeProcScope_ is either null or points at the innermost
///            live @ref ProcedureScope.
/// @note The program AST is borrowed. Mutable nodes may be canonicalized or
///       annotated during analysis.
class SemanticAnalyzer {
  public:
    /// @brief Diagnostic code for non-boolean conditional expressions.
    static constexpr std::string_view DiagNonBooleanCondition = "E1001";

    /// @brief Diagnostic code for non-boolean logical operands.
    static constexpr std::string_view DiagNonBooleanLogicalOperand = "E1002";

    /// @brief Diagnostic code for non-boolean NOT operand.
    static constexpr std::string_view DiagNonBooleanNotOperand = "E1003";

    /// @brief Diagnostic code for CASE labels that exceed the 32-bit signed range.
    static constexpr std::string_view DiagSelectCaseLabelRange = "B2012";

    /// @brief Forward declaration of the analyzer's value-type classification.
    enum class Type;

    /// @brief SELECT selector classification defined by control-analysis code.
    struct SelectCaseSelectorInfo;

    /// @brief Mutable validation state shared across CASE arms.
    struct SelectCaseArmContext;

    /// @brief Constructs an analyzer that reports through @p emitter.
    /// @param emitter Diagnostic sink borrowed by the analyzer and its
    ///                procedure registry; it must outlive this object.
    explicit SemanticAnalyzer(DiagnosticEmitter &emitter);

    /// @brief Runs full semantic analysis over a parsed program.
    /// @details Coordinates declaration/index passes, symbol and procedure
    ///          collection, statement/expression validation, and final
    ///          cross-reference checks. Diagnostics are emitted rather than
    ///          returned.
    /// @param prog Program AST borrowed during the pass.
    void analyze(const Program &prog);

    /// @brief Exposes symbol names retained after analysis.
    /// @return Const reference to analyzer-owned storage.
    const std::unordered_set<std::string> &symbols() const;

    /// @brief Exposes numeric labels declared by the analyzed program.
    /// @return Const reference to analyzer-owned storage.
    const std::unordered_set<int> &labels() const;

    /// @brief Exposes numeric labels referenced by control-flow statements.
    /// @return Const reference to analyzer-owned storage.
    const std::unordered_set<int> &labelRefs() const;

    /// @brief Exposes registered procedure signatures.
    /// @return Const reference owned by @ref procReg_.
    const ProcTable &procs() const;

    /// @brief Copies canonical namespace imports from the active USING scope.
    /// @return Imported namespace paths in unspecified order; aliases are
    ///         excluded.
    std::vector<std::string> getUsingImports() const;

    /// @brief Looks up array metadata using an exact resolved name.
    /// @param name Map key to query without additional canonicalization.
    /// @return Pointer to analyzer-owned metadata, or @c nullptr when absent.
    /// @warning Subsequent map mutation may invalidate the returned pointer.
    const ArrayMetadata *lookupArrayMetadata(const std::string &name) const;

    /// @brief Exposes the class/interface index populated during analysis.
    /// @return Const reference owned by this analyzer.
    const OopIndex &oopIndex() const noexcept {
        return oopIndex_;
    }

    /// @brief Reports the class supplying the active implicit `ME` receiver.
    /// @return Qualified class name by value, or @c std::nullopt outside an
    ///         instance-member context.
    [[nodiscard]] std::optional<std::string> activeInstanceClassQName() const;

    /// @brief Records the reserved raw-pointer compatibility setting.
    /// @details The current safety checker does not consult this flag and
    ///          continues to enforce its typed callback-bridge allowlist. The
    ///          value is retained only as a compatibility hook.
    /// @param allow Value to retain for compatibility.
    /// @post @ref allowUnsafePointers_ equals @p allow.
    void setAllowUnsafePointers(bool allow) noexcept {
        allowUnsafePointers_ = allow;
    }

  private:
    friend class sem::ControlCheckContext;
    friend class sem::ExprCheckContext;
    friend class SemanticAnalyzerExprVisitor;
    friend class SemanticAnalyzerStmtVisitor;
    friend class semantic_analyzer_detail::StmtShared;

    /// @brief Dispatches a mutable statement to the semantic statement visitor.
    /// @param s Statement node that may receive semantic annotations.
    void visitStmt(Stmt &s);

    /// @brief Validates a `CLS` statement.
    /// @param s Statement to inspect.
    void visit(const ClsStmt &s);
    /// @brief Validates `COLOR` operands.
    /// @param s Statement to inspect.
    void visit(const ColorStmt &s);
    /// @brief Validates a `SLEEP` duration.
    /// @param s Statement to inspect.
    void visit(const SleepStmt &s);
    /// @brief Validates `LOCATE` coordinates.
    /// @param s Statement to inspect.
    void visit(const LocateStmt &s);
    /// @brief Validates a cursor-control statement.
    /// @param s Statement to inspect.
    void visit(const CursorStmt &s);
    /// @brief Validates TRY/CATCH structure and nested bodies.
    /// @param s Statement to inspect.
    void visit(const TryCatchStmt &s);
    /// @brief Validates a runtime-resource `USING` statement and its body.
    /// @param s Statement to inspect.
    void visit(const UsingStmt &s);

    /// @brief Analyzes each statement in a statement-list node.
    /// @param s List whose non-null children are visited in order.
    void analyzeStmtList(const StmtList &s);
    /// @brief Analyzes expressions in a terminal `PRINT`.
    /// @param s Statement to inspect.
    void analyzePrint(const PrintStmt &s);
    /// @brief Analyzes a channel `PRINT #` operation.
    /// @param s Statement to inspect.
    void analyzePrintCh(const PrintChStmt &s);
    /// @brief Resolves and validates a procedure-call statement.
    /// @param s Mutable statement that may receive resolved call metadata.
    void analyzeCallStmt(CallStmt &s);
    /// @brief Validates and annotates an assignment.
    /// @param s Mutable assignment statement.
    void analyzeLet(LetStmt &s);
    /// @brief Validates an `OPEN` and updates literal-channel tracking.
    /// @param s Mutable statement whose expressions are analyzed.
    void analyzeOpen(OpenStmt &s);
    /// @brief Validates a `CLOSE` and updates literal-channel tracking.
    /// @param s Mutable statement whose expressions are analyzed.
    void analyzeClose(CloseStmt &s);
    /// @brief Validates channel and offset expressions for `SEEK`.
    /// @param s Mutable statement whose expressions are analyzed.
    void analyzeSeek(SeekStmt &s);
    /// @brief Validates an IF condition and all branches.
    /// @param s Statement to inspect.
    void analyzeIf(const IfStmt &s);
    /// @brief Validates a SELECT selector, CASE arms, and bodies.
    /// @param s Statement to inspect.
    void analyzeSelectCase(const SelectCaseStmt &s);
    /// @brief Analyzes statements contained by one CASE or CASE ELSE body.
    /// @param body Ordered AST statement pointers; null entries are tolerated by
    ///             statement dispatch.
    void analyzeSelectCaseBody(const std::vector<StmtPtr> &body);

    /// @brief Classifies the selector for string-versus-numeric CASE rules.
    /// @param s SELECT statement whose selector is inspected.
    /// @return Selector classification and validity information.
    SelectCaseSelectorInfo classifySelectCaseSelector(const SelectCaseStmt &s);

    /// @brief Validates one CASE arm under shared SELECT state.
    /// @param arm Arm whose labels, ranges, and relations are inspected.
    /// @param ctx Selector and duplicate/overlap state updated by validation.
    /// @return Whether the arm is semantically acceptable for model creation.
    bool validateSelectCaseArm(const CaseArm &arm, SelectCaseArmContext &ctx);

    /// @brief Applies string-selector rules to one CASE arm.
    /// @param arm Arm to validate.
    /// @param ctx Shared SELECT validation state.
    /// @return Whether string labels in the arm are valid.
    bool validateSelectCaseStringArm(const CaseArm &arm, SelectCaseArmContext &ctx);

    /// @brief Applies numeric-selector rules to one CASE arm.
    /// @param arm Arm to validate.
    /// @param ctx Shared SELECT validation state.
    /// @return Whether numeric labels, ranges, and relations are valid.
    bool validateSelectCaseNumericArm(const CaseArm &arm, SelectCaseArmContext &ctx);

    /// @brief Validates a WHILE condition and body.
    /// @param s Statement to inspect.
    void analyzeWhile(const WhileStmt &s);
    /// @brief Validates a DO loop's condition and body.
    /// @param s Statement to inspect.
    void analyzeDo(const DoStmt &s);
    /// @brief Validates and annotates a numeric FOR loop.
    /// @param s Mutable statement containing loop-variable expressions.
    void analyzeFor(ForStmt &s);
    /// @brief Validates and annotates a FOR EACH loop.
    /// @param s Mutable statement containing iterator/container expressions.
    void analyzeForEach(ForEachStmt &s);
    /// @brief Records and validates a GOTO target.
    /// @param s Statement to inspect.
    void analyzeGoto(const GotoStmt &s);
    /// @brief Records a GOSUB target and subroutine-return protocol.
    /// @param s Statement to inspect.
    void analyzeGosub(const GosubStmt &s);
    /// @brief Validates and installs an `ON ERROR GOTO` target.
    /// @param s Statement to inspect.
    void analyzeOnErrorGoto(const OnErrorGoto &s);
    /// @brief Validates a NEXT statement against active FOR variables.
    /// @param s Statement to inspect.
    void analyzeNext(const NextStmt &s);
    /// @brief Validates an EXIT target against active loop/procedure contexts.
    /// @param s Statement to inspect.
    void analyzeExit(const ExitStmt &s);
    /// @brief Analyzes an END statement.
    /// @param s Statement to inspect.
    void analyzeEnd(const EndStmt &s);
    /// @brief Validates RESUME against active error-handler state.
    /// @param s Statement to inspect.
    void analyzeResume(const Resume &s);
    /// @brief Validates and annotates RETURN in the current procedure context.
    /// @param s Mutable return statement.
    void analyzeReturn(ReturnStmt &s);
    /// @brief Validates a RANDOMIZE seed expression.
    /// @param s Statement to inspect.
    void analyzeRandomize(const RandomizeStmt &s);
    /// @brief Validates terminal INPUT destinations.
    /// @param s Mutable statement whose destinations may be resolved.
    void analyzeInput(InputStmt &s);
    /// @brief Validates channel INPUT destinations.
    /// @param s Mutable statement whose destinations may be resolved.
    void analyzeInputCh(InputChStmt &s);
    /// @brief Validates a channel line-input destination.
    /// @param s Mutable statement whose destination may be resolved.
    void analyzeLineInputCh(LineInputChStmt &s);
    /// @brief Validates a DIM declaration and records type/array metadata.
    /// @param s Mutable declaration whose names and expressions are analyzed.
    void analyzeDim(DimStmt &s);
    /// @brief Validates a CONST declaration and records its symbol/value.
    /// @param s Mutable declaration whose initializer is analyzed.
    void analyzeConst(ConstStmt &s);
    /// @brief Validates procedure-static declarations.
    /// @param s Mutable declaration to analyze.
    void analyzeStatic(StaticStmt &s);

    /// @brief Validates module-shared declarations.
    /// @param s Mutable declaration to analyze.
    void analyzeShared(SharedStmt &s);

    /// @brief Validates REDIM and updates array metadata.
    /// @param s Mutable statement to analyze.
    void analyzeReDim(ReDimStmt &s);
    /// @brief Validates the two assignable operands of SWAP.
    /// @param s Mutable statement to analyze.
    void analyzeSwap(SwapStmt &s);

    /// @brief Analyzes a namespace declaration and its members.
    /// @param decl Mutable declaration whose qualified context is established.
    void analyzeNamespaceDecl(NamespaceDecl &decl);
    /// @brief Analyzes a class declaration and member bodies.
    /// @param decl Mutable declaration to resolve and validate.
    void analyzeClassDecl(ClassDecl &decl);
    /// @brief Analyzes an interface declaration and signatures.
    /// @param decl Mutable declaration to resolve and validate.
    void analyzeInterfaceDecl(InterfaceDecl &decl);
    /// @brief Validates and records a namespace USING declaration.
    /// @param decl Mutable declaration that may receive canonical resolution.
    void analyzeUsingDecl(UsingDecl &decl);

    /// @brief Resolve a type reference and emit diagnostics if not found.
    /// @param typeName Type name to resolve (simple or qualified).
    /// @param currentNsChain Current namespace context (segments).
    /// @param loc Source location for diagnostics.
    /// @param length Source length for diagnostics.
    /// @return Fully-qualified type name if resolved; empty string on error.
    std::string resolveTypeRef(const std::string &typeName,
                               const std::vector<std::string> &currentNsChain,
                               il::support::SourceLoc loc,
                               uint32_t length);

    /// @brief Validates assignment to a simple variable.
    /// @param v Mutable destination variable.
    /// @param s Assignment supplying the source value and location.
    void analyzeVarAssignment(VarExpr &v, const LetStmt &s);
    /// @brief Validates assignment to an indexed array element.
    /// @param a Mutable indexed destination.
    /// @param s Assignment supplying the source value and location.
    void analyzeArrayAssignment(ArrayExpr &a, const LetStmt &s);
    /// @brief Validates assignment to an object member.
    /// @param m Mutable member-access destination.
    /// @param s Assignment supplying the source value and location.
    void analyzeMemberAssignment(MemberAccessExpr &m, const LetStmt &s);
    /// @brief Diagnoses assignment to a non-assignable expression.
    /// @param s Invalid assignment statement.
    void analyzeConstExpr(const LetStmt &s);

  public:
    /// @brief Coarse value categories used by BASIC semantic checks.
    enum class Type {
        /// @brief Scalar integer value.
        Int,
        /// @brief Scalar floating-point value.
        Float,
        /// @brief Scalar string value.
        String,
        /// @brief Scalar boolean value.
        Bool,
        /// @brief Array whose elements are integers.
        ArrayInt,
        /// @brief Array whose elements are strings.
        ArrayString,
        /// @brief Array whose elements are object references.
        ArrayObject,
        /// @brief Object reference or class instance.
        Object,
        /// @brief Unresolved, invalid, or not-yet-known type.
        Unknown
    };

    /// @brief Looks up a tracked type by exact or canonicalized spelling.
    /// @param name Symbol spelling to query; lexical aliases are not resolved.
    /// @return Recorded type, or @c std::nullopt when absent.
    std::optional<Type> lookupVarType(const std::string &name) const;

    /// @brief Looks up an object's declared class by exact or canonical spelling.
    /// @param name Symbol spelling to query.
    /// @return Copy of the qualified class name, or @c std::nullopt when absent.
    std::optional<std::string> lookupObjectClassQName(const std::string &name) const;

    /// @brief Tests whether a symbol remains in module-level analyzer state.
    /// @details Procedure-scope rollback removes locals, allowing the lowerer
    ///          to distinguish persistent module storage from procedure slots.
    ///          Exact and canonicalized spellings are checked.
    /// @param name Symbol spelling to query.
    /// @return @c true when the symbol set contains either spelling.
    [[nodiscard]] bool isModuleLevelSymbol(const std::string &name) const;

    /// @brief Tests whether a symbol is a retained module-level constant.
    /// @details Exact and canonicalized spellings are checked. The lowerer uses
    ///          this distinction when selecting module versus local storage.
    /// @param name Symbol spelling to query.
    /// @return @c true when the constant set contains either spelling.
    [[nodiscard]] bool isConstSymbol(const std::string &name) const;

  private:
    /// @brief RAII transaction for procedure-local analyzer mutations.
    /// @details On entry, installs itself as the active procedure scope, saves
    ///          error-handler and control-stack state, clears the procedure's
    ///          handler state, and pushes a lexical scope. Recorded mutations
    ///          are reversed on destruction so module-level analysis state is
    ///          not polluted by locals.
    /// @invariant Each keyed mutation category records at most one baseline per
    ///            key, even when that key changes repeatedly inside the scope.
    class ProcedureScope {
      public:
        /// @brief Enters a new procedure-local transaction.
        /// @param analyzer Analyzer whose mutable state is captured and updated;
        ///                 it must outlive this scope.
        explicit ProcedureScope(SemanticAnalyzer &analyzer) noexcept;

        /// @brief Procedure transactions cannot be copied.
        ProcedureScope(const ProcedureScope &) = delete;

        /// @brief Procedure transactions cannot be copy-assigned.
        ProcedureScope &operator=(const ProcedureScope &) = delete;

        /// @brief Restores every recorded analyzer state category.
        /// @details Reinstates the enclosing procedure pointer and prior error
        ///          handler, removes procedure-only symbols/labels/references,
        ///          restores map/channel baselines, truncates control stacks to
        ///          their entry depths, and pops the lexical scope.
        ~ProcedureScope() noexcept;

        /// @brief Records a symbol newly inserted in this procedure.
        /// @param name Exact symbol key to erase during rollback.
        void noteSymbolInserted(const std::string &name);

        /// @brief Records a variable-type baseline on first mutation.
        /// @param name Exact map key.
        /// @param previous Value to restore, or no value when the key was absent.
        void noteVarTypeMutation(const std::string &name, std::optional<Type> previous);

        /// @brief Records an object-class baseline on first mutation.
        /// @param name Exact map key.
        /// @param previous Class to restore, or no value when the key was absent.
        void noteObjectClassMutation(const std::string &name, std::optional<std::string> previous);

        /// @brief Records an array-metadata baseline on first mutation.
        /// @param name Exact map key.
        /// @param previous Metadata to restore, or no value when absent.
        void noteArrayMutation(const std::string &name, std::optional<ArrayMetadata> previous);

        /// @brief Records a channel's entry state on first mutation.
        /// @param channel Literal channel number.
        /// @param previouslyOpen Whether it was open before this procedure.
        void noteChannelMutation(long long channel, bool previouslyOpen);

        /// @brief Records a procedure-local label newly inserted globally.
        /// @param label Label to erase during rollback.
        void noteLabelInserted(int label);

        /// @brief Records a procedure-local label reference newly inserted.
        /// @param label Reference to erase during rollback.
        void noteLabelRefInserted(int label);

      private:
        /// @brief Baseline for one variable-type map key.
        struct VarTypeDelta {
            /// @brief Exact mutated key.
            std::string name;
            /// @brief Value present at scope entry, if any.
            std::optional<Type> previous;
        };

        /// @brief Baseline for one object-class map key.
        struct ObjectClassDelta {
            /// @brief Exact mutated key.
            std::string name;
            /// @brief Qualified class present at scope entry, if any.
            std::optional<std::string> previous;
        };

        /// @brief Baseline for one array-metadata map key.
        struct ArrayDelta {
            /// @brief Exact mutated key.
            std::string name;
            /// @brief Metadata present at scope entry, if any.
            std::optional<ArrayMetadata> previous;
        };

        /// @brief Baseline for one literal file-channel state.
        struct ChannelDelta {
            /// @brief Channel whose state changed.
            long long channel;
            /// @brief Whether the channel was open at scope entry.
            bool previouslyOpen;
        };

        /// @brief Analyzer being transacted.
        SemanticAnalyzer &analyzer_;
        /// @brief Previously active procedure scope for nested restoration.
        ProcedureScope *previous_{nullptr};
        /// @brief FOR-variable stack depth at entry.
        size_t forStackDepth_{0};
        /// @brief EXIT-context stack depth at entry.
        size_t loopStackDepth_{0};
        /// @brief Symbol keys inserted by this procedure.
        std::vector<std::string> newSymbols_;
        /// @brief Label definitions inserted by this procedure.
        std::vector<int> newLabels_;
        /// @brief Label references inserted by this procedure.
        std::vector<int> newLabelRefs_;
        /// @brief First-seen variable-type baselines.
        std::vector<VarTypeDelta> varTypeDeltas_;
        /// @brief First-seen object-class baselines.
        std::vector<ObjectClassDelta> objectClassDeltas_;
        /// @brief First-seen array baselines.
        std::vector<ArrayDelta> arrayDeltas_;
        /// @brief First-seen literal-channel baselines.
        std::vector<ChannelDelta> channelDeltas_;
        /// @brief Keys already represented in @ref varTypeDeltas_.
        std::unordered_set<std::string> trackedVarTypes_;
        /// @brief Keys already represented in @ref objectClassDeltas_.
        std::unordered_set<std::string> trackedObjectClasses_;
        /// @brief Keys already represented in @ref arrayDeltas_.
        std::unordered_set<std::string> trackedArrays_;
        /// @brief Channels already represented in @ref channelDeltas_.
        std::unordered_set<long long> trackedChannels_;
        /// @brief Error-handler active flag saved at entry.
        bool previousHandlerActive_{false};
        /// @brief Error-handler target saved at entry.
        std::optional<int> previousHandlerTarget_;
    };

    /// @brief Describes the caller's intent when resolving/tracking a name.
    /// @details The current implementation distinguishes references from both
    ///          definition categories; `Definition` and `InputTarget` follow
    ///          the same insertion/default-type path.
    enum class SymbolKind {
        Reference,  ///< Existing symbol use; do not declare or type.
        Definition, ///< Implicit definition; apply suffix defaults if unset.
        InputTarget ///< INPUT destination; force suffix-based defaults.
    };

    /// @brief Construct kinds tracked for validating EXIT statements.
    /// @details Values represent FOR, WHILE/WEND, DO/LOOP, SUB, and FUNCTION
    ///          contexts, respectively.
    enum class LoopKind { For, While, Do, Sub, Function };

    /// @brief Resolves a scoped alias and optionally tracks definition state.
    /// @details References stop after alias resolution. Definition and input
    ///          targets insert the name and assign a suffix-derived type only
    ///          when no explicit or inferred type is already present.
    /// @param name Identifier updated in place when a mapping is found.
    /// @param kind Resolution/tracking behavior.
    void resolveAndTrackSymbol(std::string &name, SymbolKind kind);

    /// @brief Dispatches expression analysis and returns its inferred type.
    /// @param e Mutable expression that may receive resolved names/annotations.
    /// @return Semantic type, including @ref Type::Unknown after errors.
    Type visitExpr(Expr &e);

    /// @brief Requires a condition to produce a boolean.
    /// @param expr Mutable condition expression to analyze and annotate.
    void checkConditionExpr(Expr &expr);

    /// @brief Diagnoses an expression that is not accepted as numeric.
    /// @param expr Mutable expression to analyze.
    /// @param message Diagnostic text used on type failure.
    void requireNumeric(Expr &expr, std::string_view message);

    /// @brief Resolves a variable reference and returns its tracked/default type.
    /// @param v Mutable variable expression.
    /// @return Inferred variable type.
    Type analyzeVar(VarExpr &v);

    /// @brief Validates a unary expression.
    /// @param u Expression to inspect.
    /// @return Result type after operand/operator checking.
    Type analyzeUnary(const UnaryExpr &u);

    /// @brief Validates a binary expression.
    /// @param b Expression to inspect.
    /// @return Result type after rule-based operand checking.
    Type analyzeBinary(const BinaryExpr &b);

    /// @brief Validates an `LBOUND` query.
    /// @param expr Mutable query expression.
    /// @return Integer on a valid query, otherwise the recovery type.
    Type analyzeLBound(LBoundExpr &expr);

    /// @brief Validates a `UBOUND` query.
    /// @param expr Mutable query expression.
    /// @return Integer on a valid query, otherwise the recovery type.
    Type analyzeUBound(UBoundExpr &expr);

    /// @brief Validates a builtin invocation through signature metadata.
    /// @param c Call expression to inspect.
    /// @return Builtin result type or recovery type after diagnostics.
    Type analyzeBuiltinCall(const BuiltinCallExpr &c);

  public:
    /// @brief Static validation metadata for one builtin argument position.
    struct BuiltinArgSpec {
        /// @brief Whether the argument may be omitted at this position.
        bool optional{false};         ///< Whether the argument may be omitted.
        /// @brief Pointer to static allowed-type storage, or null for none.
        const Type *allowed{nullptr}; ///< Pointer to allowed types array.
        /// @brief Number of readable elements beginning at @ref allowed.
        std::size_t allowedCount{0};  ///< Number of entries in @ref allowed.
    };

    /// @brief Static arity, per-position, and result metadata for a builtin.
    struct BuiltinSignature {
        /// @brief Minimum required argument count.
        std::size_t requiredArgs{0};              ///< Number of mandatory arguments.
        /// @brief Additional arguments accepted beyond the required prefix.
        std::size_t optionalArgs{0};              ///< Number of optional arguments.
        /// @brief Pointer to per-position static metadata.
        const BuiltinArgSpec *arguments{nullptr}; ///< Per-position argument specs.
        /// @brief Number of readable entries beginning at @ref arguments.
        std::size_t argumentCount{0};             ///< Total number of entries in @ref arguments.
        /// @brief Semantic result type for the generic signature path.
        Type result{Type::Unknown};               ///< Result type reported when checks pass.
    };

    /// @brief Pointer-to-member type for builtin-specific validation handlers.
    /// @details A handler receives the call, pre-analyzed argument types, and
    ///          signature metadata and returns the inferred result type.
    using BuiltinAnalyzer = Type (SemanticAnalyzer::*)(const BuiltinCallExpr &,
                                                       const std::vector<Type> &,
                                                       const BuiltinSignature &);

    /// @brief Returns semantic signature metadata for a builtin identifier.
    /// @param builtin Builtin whose registry metadata is translated.
    /// @return Reference to thread-local/static signature storage; callers must
    ///         consume it before a later call on the same thread overwrites
    ///         reused backing slots.
    static const BuiltinSignature &builtinSignature(BuiltinCallExpr::Builtin builtin);

    /// @brief Applies ABS-specific argument and result-type rules.
    /// @param c Parsed builtin call.
    /// @param args Pre-analyzed argument types.
    /// @param signature ABS signature metadata.
    /// @return Integer or float matching the numeric operand, or recovery type.
    Type analyzeAbs(const BuiltinCallExpr &c,
                    const std::vector<Type> &args,
                    const BuiltinSignature &signature);

    /// @brief Applies INSTR-specific optional-argument rules.
    /// @param c Parsed builtin call.
    /// @param args Pre-analyzed argument types.
    /// @param signature INSTR signature metadata.
    /// @return Semantic result type after validation.
    Type analyzeInstr(const BuiltinCallExpr &c,
                      const std::vector<Type> &args,
                      const BuiltinSignature &signature);

  private:
    /// @brief Validates builtin arity against an inclusive interval.
    /// @param c Call supplying source location/name for diagnostics.
    /// @param args Already analyzed argument types; its size is checked.
    /// @param min Minimum accepted count.
    /// @param max Maximum accepted count.
    /// @return @c true when the count lies in [@p min, @p max].
    bool checkArgCount(const BuiltinCallExpr &c,
                       const std::vector<Type> &args,
                       size_t min,
                       size_t max);
    /// @brief Validates one builtin argument against allowed types.
    /// @param c Call supplying diagnostic context.
    /// @param idx Zero-based argument position.
    /// @param argTy Actual semantic type.
    /// @param allowed Accepted types.
    /// @return @c true when @p argTy is accepted or is a recoverable unknown.
    bool checkArgType(const BuiltinCallExpr &c,
                      size_t idx,
                      Type argTy,
                      std::span<const Type> allowed);
    /// @brief Applies per-position signature checks to analyzed arguments.
    /// @param c Call supplying diagnostic context.
    /// @param args Actual argument types.
    /// @param signature Metadata defining arity and allowed types.
    /// @return @c true when all applicable checks pass.
    bool validateBuiltinArgs(const BuiltinCallExpr &c,
                             const std::vector<Type> &args,
                             const BuiltinSignature &signature);
    /// @brief Executes the generic signature-driven builtin analysis path.
    /// @param c Parsed call.
    /// @param args Pre-analyzed argument types.
    /// @param signature Metadata to validate.
    /// @return Signature result on success or the implementation's recovery
    ///         type after diagnostics.
    Type analyzeBuiltinWithSignature(const BuiltinCallExpr &c,
                                     const std::vector<Type> &args,
                                     const BuiltinSignature &signature);
    /// @brief Resolves a user-defined callee of the required procedure kind.
    /// @param c Call whose name and context are inspected.
    /// @param expectedKind Required SUB/FUNCTION classification.
    /// @return Pointer to registry-owned signature, or @c nullptr after failure.
    const ProcSignature *resolveCallee(const CallExpr &c, ProcSignature::Kind expectedKind);

    /// @brief Analyzes and validates arguments to a user-defined procedure.
    /// @param c Call whose argument expressions are inspected.
    /// @param sig Resolved signature, or null for recovery-only traversal.
    /// @return Actual semantic argument types in source order.
    std::vector<Type> checkCallArgs(const CallExpr &c, const ProcSignature *sig);

    /// @brief Infers the result type of a resolved call.
    /// @param c Call expression supplying contextual metadata.
    /// @param sig Resolved signature, or null on lookup failure.
    /// @return Semantic result type or @ref Type::Unknown.
    Type inferCallType(const CallExpr &c, const ProcSignature *sig);

    /// @brief Resolves and validates a user-defined function call expression.
    /// @param c Call to analyze.
    /// @return Inferred call result type.
    Type analyzeCall(const CallExpr &c);

    /// @brief Resolves and validates object construction.
    /// @param expr Mutable `NEW` expression that may receive resolved type data.
    /// @return Object on success or recovery type on failure.
    Type analyzeNew(NewExpr &expr);

    /// @brief Resolves an array and validates index expressions/dimensions.
    /// @param a Mutable array-access expression.
    /// @return Element semantic type or recovery type.
    Type analyzeArray(ArrayExpr &a);

    /// @brief Enforces safe-BASIC rules for raw-pointer runtime signatures.
    /// @details Calls without raw-pointer returns or parameters pass. Raw
    ///          returns are rejected. Raw callback arguments may pass when an
    ///          address-of expression occupies a recognized callback bridge
    ///          slot; a following payload bridge may pass when paired with such
    ///          a callback and is not itself address-of. The first violation
    ///          emits `B2131`.
    /// @param target Canonical runtime API name, possibly empty.
    /// @param rawPointerReturn Whether the runtime result is a raw pointer.
    /// @param rawPointerParams Per-position raw-pointer flags.
    /// @param args Parsed arguments used to recognize safe bridge patterns.
    /// @param loc Fallback diagnostic location.
    /// @param displayName Source-facing name and fallback API name.
    /// @return @c true when the signature/arguments are accepted.
    bool checkRuntimePointerSafety(std::string_view target,
                                   bool rawPointerReturn,
                                   const std::vector<bool> &rawPointerParams,
                                   const std::vector<ExprPtr> &args,
                                   il::support::SourceLoc loc,
                                   std::string_view displayName);

    /// @brief Records a lowering-time implicit conversion for an expression.
    /// @param expr Expression whose stable address becomes the metadata key.
    /// @param targetType Semantic destination type.
    /// @post Any previous conversion recorded for @p expr is replaced.
    void markImplicitConversion(const Expr &expr, Type targetType);

    /// @brief Request that @p expr be wrapped in an implicit cast to @p target.
    ///
    /// @details Until the BASIC AST grows an explicit cast node this utility
    ///          records the intent as an implicit conversion so that lowering
    ///          can inject the appropriate runtime helper when emitting IL.
    ///
    /// @param expr Expression slated for conversion.
    /// @param target Semantic type to coerce the expression to.
    void insertImplicitCast(Expr &expr, Type target);

    /// @brief Determines whether a statement list supplies a function result.
    /// @details Accepts either explicit all-path returns or VB-style assignment
    ///          to the active function name on all relevant paths.
    /// @param stmts Ordered function-body statements.
    /// @return @c true when return-flow analysis guarantees a result.
    bool mustReturn(const std::vector<StmtPtr> &stmts) const;

    /// @brief Determines whether one statement supplies a function result.
    /// @param s Statement analyzed under @ref activeFunction_.
    /// @return @c true for guaranteed explicit return or qualifying function-name
    ///         assignment flow.
    bool mustReturn(const Stmt &s) const;

    /// @brief Activates the current scope's error handler.
    /// @param label Numeric target recorded for later RESUME/handler checks.
    void installErrorHandler(int label);

    /// @brief Deactivates the current error handler and clears its target.
    void clearErrorHandler();

    /// @brief Reports current error-handler activation.
    /// @return Value of @ref errorHandlerActive_.
    bool hasActiveErrorHandler() const noexcept;

    /// @brief Pushes an active construct for EXIT validation.
    /// @param kind Construct being entered.
    void pushLoop(LoopKind kind);

    /// @brief Pops the innermost tracked construct when one exists.
    void popLoop();

    /// @brief Pushes an active FOR control-variable spelling.
    /// @param name Name copied into the stack.
    void pushForVariable(std::string_view name);

    /// @brief Pops the innermost FOR variable when one exists.
    void popForVariable();

    /// @brief Tests whether a name occurs anywhere in the FOR-variable stack.
    /// @param name Case-sensitive spelling to search.
    /// @return @c true when any active loop uses the exact spelling.
    bool isLoopVariableActive(std::string_view name) const noexcept;

    /// @brief Tests whether a construct kind occurs in the EXIT-context stack.
    /// @param kind Construct kind to search.
    /// @return @c true when found at any nesting depth.
    bool hasLoopOfKind(LoopKind kind) const noexcept;

    /// @brief Reports whether pre-analysis found GOSUB in the module body.
    /// @return Value cached in @ref mainHasGosub_.
    bool mainHasGosub() const noexcept;

    /// @brief Performs common scoped traversal for SUB and FUNCTION bodies.
    /// @details Creates a rollback scope, optionally pushes a procedure EXIT
    ///          context, registers parameters and user line labels, visits body
    ///          statements, then invokes the procedure-specific callback.
    /// @tparam Proc Procedure AST type exposing `params` and `body`.
    /// @tparam BodyCallback Callable accepting `const Proc &`.
    /// @param proc Declaration to analyze.
    /// @param bodyCheck Final procedure-specific validation callback.
    /// @param loopKind Optional SUB/FUNCTION context for EXIT checks.
    template <typename Proc, typename BodyCallback>
    void analyzeProcedureCommon(const Proc &proc,
                                BodyCallback &&bodyCheck,
                                std::optional<LoopKind> loopKind = std::nullopt);

    /// @brief Registers one procedure parameter in scoped symbol/type state.
    /// @param param Parsed parameter including type, array, and object metadata.
    /// @pre A @ref ProcedureScope and lexical scope are active.
    void registerProcedureParam(const Param &param);

    /// @brief Analyzes a FUNCTION body and verifies its result flow.
    /// @param f Function declaration to analyze.
    void analyzeProc(const FunctionDecl &f);

    /// @brief Analyzes a SUB body in a rollback procedure scope.
    /// @param s Subroutine declaration to analyze.
    void analyzeProc(const SubDecl &s);

    /// @brief Diagnostic facade bound to the caller's emitter.
    SemanticDiagnostics de; ///< Diagnostic sink.

    /// @brief Lexical mapping stack used during name resolution.
    ScopeTracker scopes_;

    /// @brief Registry of runtime and user procedure signatures.
    ProcRegistry procReg_;

    /// @brief Class, interface, enum, and member index built for OOP checks.
    OopIndex oopIndex_;

    /// @brief Declared namespace/type registry for qualified resolution.
    NamespaceRegistry ns_; ///< Registry for declared namespaces and types.

    /// @brief File-level USING declarations used to seed resolution.
    UsingContext usings_;  ///< File-scoped USING imports.

    /// @brief Imports and aliases visible in one namespace-analysis scope.
    struct UsingScope {
        /// @brief Canonical lowercase imported namespace names.
        std::unordered_set<std::string> imports; ///< Canonical lowercase qualified namespaces

        /// @brief Canonical lowercase alias to qualified-namespace mapping.
        std::unordered_map<std::string, std::string> aliases; ///< alias (lower) -> qualified ns

        /// @brief Declaration location for each alias key.
        std::unordered_map<std::string, il::support::SourceLoc> aliasLoc; ///< alias -> loc
    };

    /// @brief Nested namespace USING scopes; imports may be inherited.
    std::vector<UsingScope> usingStack_;     ///< Scoped USING contexts (inherit on namespace entry)

    /// @brief Current namespace path segments.
    std::vector<std::string> nsStack_;       ///< Current namespace path during analysis.

    /// @brief Resolver created after namespace declaration collection.
    std::unique_ptr<TypeResolver> resolver_; ///< Type resolver (constructed after declare pass).

    /// @brief Whether a declaration has appeared before a USING placement check.
    bool sawDecl_{false}; ///< Track if any declarations seen (for USING placement).

    /// @brief Resolved symbol names retained in current/module analysis state.
    std::unordered_set<std::string> symbols_;

    /// @brief Names declared with CONST.
    std::unordered_set<std::string> constants_; ///< Constant names declared with CONST.

    /// @brief Compile-time integer values available for constant evaluation.
    std::unordered_map<std::string, long long>
        constantValues_; ///< Integer values of CONST declarations.

    /// @brief Inferred or declared semantic type by resolved symbol name.
    std::unordered_map<std::string, Type> varTypes_;

    /// @brief Qualified class name by resolved object variable.
    std::unordered_map<std::string, std::string> objectClassTypes_;

    /// @brief Shape metadata by resolved array symbol.
    std::unordered_map<std::string, ArrayMetadata>
        arrays_;                                 ///< Array metadata with extents and total size

    /// @brief Literal file channels currently modeled as open.
    std::unordered_set<long long> openChannels_; ///< Channels opened by literal handles.

    /// @brief Numeric line labels visible in the current analysis context.
    std::unordered_set<int> labels_;

    /// @brief Numeric line labels referenced by control-flow statements.
    std::unordered_set<int> labelRefs_;

    /// @brief Active FOR control variables, outermost to innermost.
    std::vector<std::string> forStack_; ///< Active FOR loop variables.

    /// @brief Active EXIT target kinds, outermost to innermost.
    std::vector<LoopKind> loopStack_;   ///< Active loop constructs for EXIT validation.

    /// @brief Lowering-time conversion target keyed by borrowed AST node address.
    std::unordered_map<const Expr *, Type> implicitConversions_;

    /// @brief Innermost active procedure rollback scope, or null at module level.
    ProcedureScope *activeProcScope_{nullptr};

    /// @brief Whether the current procedure/module error handler is active.
    bool errorHandlerActive_{false};

    /// @brief Numeric target of the active error handler, when any.
    std::optional<int> errorHandlerTarget_;

    /// @brief Whether the main module contains any GOSUB construct.
    bool mainHasGosub_{false};

    /// @brief Qualified class whose member is currently being analyzed.
    std::string activeClassQName_;

    /// @brief Whether the active member has an implicit `ME` receiver.
    bool activeMemberHasMe_{false};

    /// @brief Function whose body/result flow is currently being analyzed.
    const FunctionDecl *activeFunction_{nullptr};

    /// @brief Explicit return annotation of @ref activeFunction_.
    BasicType activeFunctionExplicitRet_{BasicType::Unknown};

    /// @brief Compatibility flag controlling raw-pointer runtime API checks.
    bool allowUnsafePointers_{false};

    /// @brief Whether the active FUNCTION assigns its own name as a result.
    bool activeFunctionNameAssigned_{
        false}; ///< Track if function name was assigned (VB-style implicit return). (BUG-003)
};

} // namespace il::frontends::basic
