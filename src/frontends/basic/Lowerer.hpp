//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/Lowerer.hpp
// Purpose: Transforms validated BASIC AST nodes into Zanna IL instructions.
//          Final stage of the pipeline: Lexer -> Parser -> AST -> Semantic -> Lowerer -> IL.
// Key invariants: Generates deterministic block names per procedure using BlockNamer.
//                 Procedures are lowered before a synthetic @main encompassing
//                 top-level statements. Runtime helpers are declared lazily.
// Ownership/Lifetime: Returns the produced Module by value, temporarily borrows
//                     an IRBuilder/module during emission, and does not own AST nodes.
// Links: docs/internals/architecture.md, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file Lowerer.hpp
 * @brief Declares the stateful BASIC AST-to-IL lowering coordinator.
 *
 * Lowerer owns helper facades, symbol/runtime bookkeeping, and OOP indexes.
 * During one lowering run it temporarily borrows an IR builder and destination
 * module; AST nodes remain caller-owned, and completed modules are returned by
 * value. The same instance may be reused because program orchestration resets
 * per-run state.
 */

#pragma once

#include "frontends/basic/AST.hpp"
#include "frontends/basic/BasicTypes.hpp"
#include "frontends/basic/EmitCommon.hpp"
#include "frontends/basic/IdentifierUtil.hpp"
#include "frontends/basic/LowerRuntime.hpp"
#include "frontends/basic/LowererContext.hpp"
#include "frontends/basic/LowererFwd.hpp"
#include "frontends/basic/LowererRuntimeHelpers.hpp"
#include "frontends/basic/LowererTypes.hpp"
#include "frontends/basic/NameMangler.hpp"
#include "frontends/basic/OopIndex.hpp"
#include "frontends/basic/StringTable.hpp"
#include "frontends/basic/SymbolTable.hpp"
#include "frontends/basic/TypeCoercionEngine.hpp"
#include "frontends/basic/TypeRules.hpp"
#include "frontends/basic/lower/MemberArrayResolver.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "zanna/il/IRBuilder.hpp"
#include "zanna/il/Module.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace il::frontends::basic {

// Forward declarations are now in LowererFwd.hpp

/// @brief Lowers BASIC AST into IL Module.
/// @invariant Generates deterministic block names per procedure using BlockNamer.
/// @invariant Owned helper facades retain valid back-references to this object.
class Lowerer {
  public:
    /// @brief Construct a lowerer.
    /// @param boundsChecks Enable debug array bounds checks.
    explicit Lowerer(bool boundsChecks = false);

    /// @brief Destroy owned lowering helpers and transient bookkeeping.
    ~Lowerer();

    /// @brief Lower @p prog into an IL module with @main entry.
    /// @details Procedures are lowered before a synthetic `@main` encompassing
    ///          the program's top-level statements.
    /// @param prog Caller-owned AST that remains valid for the duration of the call.
    /// @return Newly populated module by value.
    il::core::Module lowerProgram(const Program &prog);

    /// @brief Backward-compatibility wrapper for older call sites.
    /// @param prog Caller-owned AST to lower.
    /// @return Result of @ref lowerProgram.
    il::core::Module lower(const Program &prog);

    /// @brief Attach diagnostic emitter used for frontend errors during lowering.
    /// @param emitter Diagnostic sink; may be nullptr to disable emission.
    void setDiagnosticEmitter(DiagnosticEmitter *emitter) noexcept;

    /// @brief Access the diagnostic emitter when present.
    /// @return Borrowed sink pointer, or null when diagnostics are disabled.
    [[nodiscard]] DiagnosticEmitter *diagnosticEmitter() const noexcept;

    /// @brief Attach semantic analyzer for type information lookup during lowering.
    /// @param analyzer Semantic analyzer providing variable type information; may be nullptr.
    void setSemanticAnalyzer(const SemanticAnalyzer *analyzer) noexcept;

    /// @brief Access the semantic analyzer when present.
    /// @return Borrowed analyzer pointer, or null when none is attached.
    [[nodiscard]] const SemanticAnalyzer *semanticAnalyzer() const noexcept;

    /// @brief Map a canonical (case-insensitive) qualified class name to its declared casing.
    /// @details Uses the OOP index to find the class by case-insensitive match and returns
    ///          the qualified name as declared. Falls back to the input when not found.
    std::string resolveQualifiedClassCasing(const std::string &qname) const;

    /// @brief Mark a symbol as storing an object reference.
    /// @param name BASIC symbol name tracked in the symbol table.
    /// @param className Fully-qualified BASIC class name associated with the object.
    void setSymbolObjectType(std::string_view name, std::string className);

    /// @brief Ensure the given runtime helper is available to lowering.
    /// @param feature Runtime feature required by an auxiliary pass.
    /// @post The feature's request bit is set.
    void requestRuntimeFeature(il::runtime::RuntimeFeature feature) {
        requestHelper(feature);
    }

    // Namespace qualification support -------------------------------------------------
    /// @brief Push a namespace path onto the current qualification stack.
    /// @param path Ordered segments, e.g. {"A","B"} for A.B.
    void pushNamespace(const std::vector<std::string> &path);

    /// @brief Pop @p count segments from the namespace stack (best-effort clamp).
    /// @param count Number of trailing segments to remove.
    void popNamespace(std::size_t count);

    /// @brief Qualify a class name with the active namespace path if any.
    /// @param klass Unqualified BASIC class name.
    /// @return Fully-qualified name like "A.B.Klass" or the original when no namespace active.
    [[nodiscard]] std::string qualify(const std::string &klass) const;

    // =========================================================================
    // Source Location API
    // =========================================================================
    // These methods provide controlled access to the current source location
    // for RAII helpers and emission utilities without requiring friendship.

    /// @brief Get the current source location for IR emission.
    /// @return Reference to the mutable source location.
    [[nodiscard]] il::support::SourceLoc &sourceLocation() noexcept;

    /// @brief Get the current source location (const).
    /// @return Reference to the immutable source location.
    [[nodiscard]] const il::support::SourceLoc &sourceLocation() const noexcept;

    /// @brief Set the current source location for IR emission.
    /// @param loc New source location to use for subsequent emissions.
    void setSourceLocation(il::support::SourceLoc loc) noexcept;

  private:
    // =========================================================================
    // Friend Declarations
    // =========================================================================
    // Friends are grouped by functional category. Each friend requires access
    // to private Lowerer internals that cannot be reasonably exposed via public
    // accessors without breaking encapsulation or creating overly complex APIs.
    //
    // Categories:
    // - Pipeline Helpers: Core lowering orchestration (Program/Procedure/Statement)
    // - Expression Lowering: Numeric, logical, builtin expression handlers
    // - Statement Lowering: I/O, control flow, runtime statements
    // - OOP Helpers: Class method and field emission
    // - Scanning Infrastructure: Runtime needs and type analysis
    // - Emission Utilities: IL instruction generation helpers

    // Pipeline Helpers - orchestrate multi-phase lowering
    friend struct ProgramLowering;
    friend struct ProcedureLowering;
    friend struct StatementLowering;

    // Expression Lowering - handle specific expression categories
    friend class LowererExprVisitor;
    friend struct LogicalExprLowering;
    friend struct NumericExprLowering;
    friend struct BuiltinExprLowering;
    friend class builtins::LowerCtx;

    // Statement Lowering - handle specific statement categories
    friend class LowererStmtVisitor;
    friend class SelectCaseLowering;
    friend class IoStatementLowerer;
    friend class ControlStatementLowerer;
    friend class RuntimeStatementLowerer;

    // OOP Helpers - class/method/field emission
    friend class OopEmitHelper;
    friend struct OopLoweringContext;
    friend class MethodDispatchResolver;
    friend class BoundsCheckEmitter;

    // Scanning Infrastructure - pre-lowering analysis
    friend class lower::detail::ExprTypeScanner;
    friend class lower::detail::RuntimeNeedsScanner;

    // Emission Utilities - IL generation helpers that need private emit* methods
    friend class Emit;               // Wraps emitUnary/emitBinary with location tracking
    friend class TypeCoercionEngine; // Type coercion needs emitUnary/emitBasicLogicalI64
    friend class lower::Emitter;
    friend class lower::BuiltinLowerContext;
    friend class lower::common::CommonLowering;
    friend class RuntimeCallBuilder;
    friend class NumericTypeClassifier; // Needs access to classifyNumericType

    using Module = il::core::Module;
    using Function = il::core::Function;
    using BasicBlock = il::core::BasicBlock;
    using Value = il::core::Value;
    using Type = il::core::Type;
    using Opcode = il::core::Opcode;
    using IlValue = Value;
    using IlType = Type;
    using AstType = ::il::frontends::basic::Type;

    // Type aliases for context structures (defined in LowererContext.hpp)
    using ProcedureContext = ::il::frontends::basic::ProcedureContext;
    using BlockNamer = ::il::frontends::basic::BlockNamer;
    using ForBlocks = ::il::frontends::basic::ForBlocks;

  public:
    // Re-export types from LowererTypes.hpp for API compatibility
    using RVal = ::il::frontends::basic::RVal;
    using PrintChArgString = ::il::frontends::basic::PrintChArgString;

    // =========================================================================
    // Detail Access API (Internal)
    // =========================================================================
    // Exposes a narrow, explicit surface for modular lowering helpers without
    // granting them direct friendship to the full Lowerer class.

    /// @brief Narrow public facade providing controlled access to Lowerer internals.
    /// @details Modular lowering helpers receive a DetailAccess handle instead of
    ///          friendship to the entire Lowerer class. This limits the surface area
    ///          exposed to helper modules while still allowing them to delegate back
    ///          to core lowering routines.
    class DetailAccess {
      public:
        /// @brief Construct a detail-access handle bound to the given Lowerer.
        /// @param lowerer Borrowed Lowerer that must outlive this handle.
        explicit DetailAccess(Lowerer &lowerer) noexcept : lowerer_(&lowerer) {}

        /// @brief Retrieve the underlying Lowerer reference.
        /// @return The Lowerer instance backing this handle.
        [[nodiscard]] Lowerer &lowerer() const noexcept {
            return *lowerer_;
        }

        /// @brief Lower a variable reference expression.
        /// @param expr Variable expression AST node.
        /// @return Loaded value and its type.
        [[nodiscard]] RVal lowerVarExpr(const VarExpr &expr) {
            return lowerer_->lowerVarExpr(expr);
        }

        /// @brief Lower a unary expression (e.g. NOT, negation).
        /// @param expr Unary expression AST node.
        /// @return Resulting value and type.
        [[nodiscard]] RVal lowerUnaryExpr(const UnaryExpr &expr) {
            return lowerer_->lowerUnaryExpr(expr);
        }

        /// @brief Lower a binary expression (arithmetic, comparison, etc.).
        /// @param expr Binary expression AST node.
        /// @return Resulting value and type.
        [[nodiscard]] RVal lowerBinaryExpr(const BinaryExpr &expr) {
            return lowerer_->lowerBinaryExpr(expr);
        }

        /// @brief Lower a UBOUND query expression.
        /// @param expr UBOUND expression AST node naming the array.
        /// @return Resulting value and type.
        [[nodiscard]] RVal lowerUBoundExpr(const UBoundExpr &expr) {
            return lowerer_->lowerUBoundExpr(expr);
        }

        /// @brief Lower an IF/ELSEIF/ELSE statement.
        /// @param stmt IF statement AST node.
        void lowerIf(const IfStmt &stmt) {
            lowerer_->lowerIf(stmt);
        }

        /// @brief Lower a WHILE/WEND loop statement.
        /// @param stmt WHILE statement AST node.
        void lowerWhile(const WhileStmt &stmt) {
            lowerer_->lowerWhile(stmt);
        }

        /// @brief Lower a DO/LOOP statement.
        /// @param stmt DO statement AST node.
        void lowerDo(const DoStmt &stmt) {
            lowerer_->lowerDo(stmt);
        }

        /// @brief Lower a FOR/NEXT loop statement.
        /// @param stmt FOR statement AST node.
        void lowerFor(const ForStmt &stmt) {
            lowerer_->lowerFor(stmt);
        }

        /// @brief Lower a FOR EACH iteration statement.
        /// @param stmt FOR EACH statement AST node.
        void lowerForEach(const ForEachStmt &stmt) {
            lowerer_->lowerForEach(stmt);
        }

        /// @brief Lower a SELECT CASE statement.
        /// @param stmt SELECT CASE statement AST node.
        void lowerSelectCase(const SelectCaseStmt &stmt) {
            lowerer_->lowerSelectCase(stmt);
        }

        /// @brief Lower a NEXT statement (FOR loop increment/termination).
        /// @param stmt NEXT statement AST node.
        void lowerNext(const NextStmt &stmt) {
            lowerer_->lowerNext(stmt);
        }

        /// @brief Lower an EXIT statement (loop early termination).
        /// @param stmt EXIT statement AST node.
        void lowerExit(const ExitStmt &stmt) {
            lowerer_->lowerExit(stmt);
        }

        /// @brief Lower a GOTO statement (unconditional branch).
        /// @param stmt GOTO statement AST node.
        void lowerGoto(const GotoStmt &stmt) {
            lowerer_->lowerGoto(stmt);
        }

        /// @brief Lower a GOSUB statement (subroutine call with return address).
        /// @param stmt GOSUB statement AST node.
        void lowerGosub(const GosubStmt &stmt) {
            lowerer_->lowerGosub(stmt);
        }

        /// @brief Lower a GOSUB RETURN statement.
        /// @param stmt RETURN statement AST node.
        void lowerGosubReturn(const ReturnStmt &stmt) {
            lowerer_->lowerGosubReturn(stmt);
        }

        /// @brief Lower an ON ERROR GOTO statement (error handler registration).
        /// @param stmt ON ERROR GOTO statement AST node.
        void lowerOnErrorGoto(const OnErrorGoto &stmt) {
            lowerer_->lowerOnErrorGoto(stmt);
        }

        /// @brief Lower a RESUME statement (error handler continuation).
        /// @param stmt RESUME statement AST node.
        void lowerResume(const Resume &stmt) {
            lowerer_->lowerResume(stmt);
        }

        /// @brief Lower an END statement (program termination).
        /// @param stmt END statement AST node.
        void lowerEnd(const EndStmt &stmt) {
            lowerer_->lowerEnd(stmt);
        }

        /// @brief Lower a TRY/CATCH statement.
        /// @param stmt TRY/CATCH statement AST node.
        void lowerTryCatch(const TryCatchStmt &stmt) {
            lowerer_->lowerTryCatch(stmt);
        }

        /// @brief Lower a NEW expression allocating a BASIC object instance.
        /// @param expr NEW expression AST node.
        /// @return Resulting object pointer value and type.
        [[nodiscard]] RVal lowerNewExpr(const NewExpr &expr) {
            return lowerer_->lowerNewExpr(expr);
        }

        /// @brief Lower a NEW expression with an explicit OOP context.
        /// @param expr NEW expression AST node.
        /// @param ctx OOP lowering context providing cached metadata.
        /// @return Resulting object pointer value and type.
        [[nodiscard]] RVal lowerNewExpr(const NewExpr &expr, OopLoweringContext &ctx) {
            return lowerer_->lowerNewExpr(expr, ctx);
        }

        /// @brief Lower a ME expression referencing the implicit instance slot.
        /// @param expr ME expression AST node.
        /// @return Resulting self-pointer value and type.
        [[nodiscard]] RVal lowerMeExpr(const MeExpr &expr) {
            return lowerer_->lowerMeExpr(expr);
        }

        /// @brief Lower a ME expression with an explicit OOP context.
        /// @param expr ME expression AST node.
        /// @param ctx OOP lowering context providing cached metadata.
        /// @return Resulting self-pointer value and type.
        [[nodiscard]] RVal lowerMeExpr(const MeExpr &expr, OopLoweringContext &ctx) {
            return lowerer_->lowerMeExpr(expr, ctx);
        }

        /// @brief Lower a member access expression (field read).
        /// @param expr Member access expression AST node.
        /// @return Loaded field value and its type.
        [[nodiscard]] RVal lowerMemberAccessExpr(const MemberAccessExpr &expr) {
            return lowerer_->lowerMemberAccessExpr(expr);
        }

        /// @brief Lower a member access expression with an explicit OOP context.
        /// @param expr Member access expression AST node.
        /// @param ctx OOP lowering context providing cached metadata.
        /// @return Loaded field value and its type.
        [[nodiscard]] RVal lowerMemberAccessExpr(const MemberAccessExpr &expr,
                                                 OopLoweringContext &ctx) {
            return lowerer_->lowerMemberAccessExpr(expr, ctx);
        }

        /// @brief Lower a method call expression.
        /// @param expr Method call expression AST node.
        /// @return Resulting return value and its type.
        [[nodiscard]] RVal lowerMethodCallExpr(const MethodCallExpr &expr) {
            return lowerer_->lowerMethodCallExpr(expr);
        }

        /// @brief Lower a method call expression with an explicit OOP context.
        /// @param expr Method call expression AST node.
        /// @param ctx OOP lowering context providing cached metadata.
        /// @return Resulting return value and its type.
        [[nodiscard]] RVal lowerMethodCallExpr(const MethodCallExpr &expr,
                                               OopLoweringContext &ctx) {
            return lowerer_->lowerMethodCallExpr(expr, ctx);
        }

        /// @brief Lower a DELETE statement releasing an object reference.
        /// @param stmt DELETE statement AST node.
        void lowerDelete(const DeleteStmt &stmt) {
            lowerer_->lowerDelete(stmt);
        }

        /// @brief Lower a DELETE statement with an explicit OOP context.
        /// @param stmt DELETE statement AST node.
        /// @param ctx OOP lowering context providing cached metadata.
        void lowerDelete(const DeleteStmt &stmt, OopLoweringContext &ctx) {
            lowerer_->lowerDelete(stmt, ctx);
        }

        /// @brief Scan program OOP constructs to populate class layouts.
        /// @param prog Program AST root node.
        void scanOOP(const Program &prog) {
            lowerer_->scanOOP(prog);
        }

        /// @brief Emit constructor, destructor, and method bodies for CLASS declarations.
        /// @param prog Program AST root node.
        void emitOopDeclsAndBodies(const Program &prog) {
            lowerer_->emitOopDeclsAndBodies(prog);
        }

        /// @brief Lower a LET assignment statement.
        /// @param stmt LET statement AST node.
        void lowerLet(const LetStmt &stmt) {
            lowerer_->lowerLet(stmt);
        }

        /// @brief Lower a CONST declaration statement.
        /// @param stmt CONST statement AST node.
        void lowerConst(const ConstStmt &stmt) {
            lowerer_->lowerConst(stmt);
        }

        /// @brief Lower a STATIC variable declaration statement.
        /// @param stmt STATIC statement AST node.
        void lowerStatic(const StaticStmt &stmt) {
            lowerer_->lowerStatic(stmt);
        }

        /// @brief Lower a DIM array/variable declaration statement.
        /// @param stmt DIM statement AST node.
        void lowerDim(const DimStmt &stmt) {
            lowerer_->lowerDim(stmt);
        }

        /// @brief Lower a REDIM array resizing statement.
        /// @param stmt REDIM statement AST node.
        void lowerReDim(const ReDimStmt &stmt) {
            lowerer_->lowerReDim(stmt);
        }

        /// @brief Lower a RANDOMIZE statement (seed random number generator).
        /// @param stmt RANDOMIZE statement AST node.
        void lowerRandomize(const RandomizeStmt &stmt) {
            lowerer_->lowerRandomize(stmt);
        }

        /// @brief Lower a SWAP statement (exchange two variables).
        /// @param stmt SWAP statement AST node.
        void lowerSwap(const SwapStmt &stmt) {
            lowerer_->lowerSwap(stmt);
        }

        /// @brief Access the centralized type coercion engine.
        /// @return Reference to the TypeCoercionEngine owned by the Lowerer.
        TypeCoercionEngine &coercion() noexcept {
            return lowerer_->coercion();
        }

      private:
        /// @brief Non-owning backing lowerer; non-null for the handle's lifetime.
        Lowerer *lowerer_; ///< Non-owning pointer to the backing Lowerer.
    };

    /// @brief Create a detail-access handle for modular lowering helpers.
    /// @return A lightweight facade exposing a controlled subset of Lowerer internals.
    [[nodiscard]] DetailAccess detailAccess() noexcept {
        return DetailAccess(*this);
    }

    /// @brief Permit the I/O helper to format one PRINT# argument.
    /// @param self I/O lowerer providing access to the owning Lowerer.
    /// @param expr Source expression used for type-sensitive formatting.
    /// @param value Already lowered expression value.
    /// @param quoteStrings Whether strings receive record-format quotes.
    /// @return String handle and optional runtime feature requirement.
    friend PrintChArgString lowerPrintChArgToString(IoStatementLowerer &self,
                                                    const Expr &expr,
                                                    RVal value,
                                                    bool quoteStrings);
    /// @brief Permit the I/O helper to assemble a PRINT# record.
    /// @param self I/O lowerer providing emission services.
    /// @param stmt PRINT# statement containing record items.
    /// @return Runtime string handle for the assembled record.
    friend Value buildPrintChWriteRecord(IoStatementLowerer &self, const PrintChStmt &stmt);

    // Re-export more types from LowererTypes.hpp
    using ArrayAccess = ::il::frontends::basic::ArrayAccess;
    using ArrayAccessKind = ::il::frontends::basic::ArrayAccessKind;
    using SymbolInfo = ::il::frontends::basic::SymbolInfo;

  private:
    /// @brief Aggregates state shared across helper stages of program lowering.
    /// @details Statement pointers borrow nodes from the input Program; block
    ///          pointers borrow storage from the synthetic function.
    struct ProgramEmitContext {
        std::vector<const Stmt *> mainStmts; ///< Flattened top-level statements for @main.
        Function *function{nullptr};         ///< The synthetic @main IL function.
        BasicBlock *entry{nullptr};          ///< Entry basic block of @main.
    };

    // Re-export private types from LowererTypes.hpp
    using SlotType = ::il::frontends::basic::SlotType;
    using VariableStorage = ::il::frontends::basic::VariableStorage;
    using ProcedureSignature = ::il::frontends::basic::ProcedureSignature;
    using IfBlocks = ::il::frontends::basic::IfBlocks;
    using CtrlState = ::il::frontends::basic::CtrlState;

    // Note: ProcedureContext, BlockNamer, and ForBlocks are defined in LowererContext.hpp

  public:
    /// Configuration callbacks and return type used by procedure lowering.
    struct ProcedureConfig;

  private:
    /// @brief Derived metadata used while constructing one procedure.
    struct ProcedureMetadata {
        /// Non-owning body statement pointers in source order.
        std::vector<const Stmt *> bodyStmts;
        /// Canonical parameter names excluded from local allocation.
        std::unordered_set<std::string> paramNames;
        /// IL parameters forming the emitted function signature.
        std::vector<il::core::Param> irParams;
        /// Number of source-level parameters.
        size_t paramCount{0};
    };

    /// @brief Collect parameter and body metadata for one procedure.
    /// @param params Source-level formal parameters.
    /// @param body Owned AST statement pointers whose nodes are borrowed.
    /// @param config Procedure-specific return and callback configuration.
    /// @return Metadata consumed by skeleton and emission stages.
    ProcedureMetadata collectProcedureMetadata(const std::vector<Param> &params,
                                               const std::vector<StmtPtr> &body,
                                               const ProcedureConfig &config);

    /// @brief Initialize function blocks and procedure context from metadata.
    /// @param f Function object receiving the procedure skeleton.
    /// @param name Source-level procedure name used for block naming.
    /// @param metadata Collected parameters and body statement pointers.
    void buildProcedureSkeleton(Function &f,
                                const std::string &name,
                                const ProcedureMetadata &metadata);

    /// @brief Allocate storage for eligible symbols in three ordered passes.
    /// @param paramNames Names treated as formal parameters.
    /// @param includeParams Whether formal parameters are eligible for allocation.
    void allocateLocalSlots(const std::unordered_set<std::string> &paramNames, bool includeParams);

    /// @brief Allocate stack slots for boolean scalars (Pass 1 of slot allocation).
    /// @param paramNames Names treated as formal parameters.
    /// @param includeParams Whether those parameter names are eligible.
    void allocateBooleanSlots(const std::unordered_set<std::string> &paramNames,
                              bool includeParams);

    /// @brief Allocate stack slots for arrays and non-boolean scalars (Pass 2).
    /// @param paramNames Names treated as formal parameters.
    /// @param includeParams Whether those parameter names are eligible.
    void allocateNonBooleanSlots(const std::unordered_set<std::string> &paramNames,
                                 bool includeParams);

    /// @brief Allocate auxiliary slots for array length tracking (Pass 3).
    /// @param paramNames Names treated as formal parameters.
    /// @param includeParams Whether those parameter names are eligible.
    void allocateArrayLengthSlots(const std::unordered_set<std::string> &paramNames,
                                  bool includeParams);

    /// @brief Check if a symbol should have a slot allocated.
    /// @param name Symbol name under consideration.
    /// @param info Collected symbol metadata.
    /// @param paramNames Names treated as formal parameters.
    /// @param includeParams Whether parameter symbols may receive slots.
    /// @return True when this allocation pass should materialize the symbol.
    [[nodiscard]] bool shouldAllocateSlot(const std::string &name,
                                          const SymbolInfo &info,
                                          const std::unordered_set<std::string> &paramNames,
                                          bool includeParams) const;

    /// @brief Compute the IL type for a procedure parameter.
    /// @param p Source-level parameter declaration.
    /// @return IL type used in the function signature.
    [[nodiscard]] static il::core::Type computeParamILType(const Param &p);

    /// @brief Materialize a single parameter into a stack slot.
    /// @param p Source-level parameter declaration.
    /// @param index Source parameter ordinal.
    /// @param ilParamOffset Offset from source ordinals to IL parameter ordinals.
    void materializeSingleParam(const Param &p, size_t index, size_t ilParamOffset);

    /// @brief Resolve storage for a STATIC variable.
    /// @param name Symbol name to resolve.
    /// @param slotInfo Expected storage type and traits.
    /// @return Runtime-backed variable storage, or empty when unresolved.
    [[nodiscard]] std::optional<VariableStorage> resolveStaticVariableStorage(
        std::string_view name, const SlotType &slotInfo);

    /// @brief Resolve storage for a module-level global variable.
    /// @param name Symbol name to resolve.
    /// @param slotInfo Expected storage type and traits.
    /// @return Runtime-backed variable storage, or empty when unresolved.
    [[nodiscard]] std::optional<VariableStorage> resolveModuleLevelStorage(
        std::string_view name, const SlotType &slotInfo);

    /// @brief Select the appropriate rt_modvar_addr_* helper based on type kind.
    /// @param kind IL scalar/storage kind.
    /// @return Runtime helper symbol for that kind.
    [[nodiscard]] std::string selectModvarAddrHelper(il::core::Type::Kind kind);

    /// @brief Lower statement pointers in order with optional termination stopping.
    /// @param stmts Non-owning statement pointers; null entries are skipped.
    /// @param stopOnTerminated Whether to stop after the current block terminates.
    /// @param beforeBranch Optional callback invoked for each statement before lowering.
    void lowerStatementSequence(const std::vector<const Stmt *> &stmts,
                                bool stopOnTerminated,
                                const std::function<void(const Stmt &)> &beforeBranch = {});

    /// @brief Pre-register procedure signatures for forward call resolution.
    /// @param prog Program containing procedure declarations.
    void collectProcedureSignatures(const Program &prog);

    /// @brief Discover symbols referenced throughout a complete program.
    /// @param prog Program whose procedure and main statements are scanned.
    void collectVars(const Program &prog);

    /// @brief Discover symbols referenced by an arbitrary statement list.
    /// @param stmts Non-owning statement pointers; null entries are ignored.
    void collectVars(const std::vector<const Stmt *> &stmts);

    /// @brief Obtain or synthesize the control-flow line identifier for a statement.
    /// @param s Statement whose line identity is requested.
    /// @return Positive user line or negative synthetic line identifier.
    int virtualLine(const Stmt &s);

    /// @brief Lower one FUNCTION declaration and its body.
    /// @param decl Function AST declaration.
    void lowerFunctionDecl(const FunctionDecl &decl);

    /// @brief Lower one SUB declaration and its body.
    /// @param decl Subroutine AST declaration.
    void lowerSubDecl(const SubDecl &decl);

  public:
    /// @brief Configuration shared by FUNCTION and SUB lowering.
    struct ProcedureConfig {
        Type retType{Type(Type::Kind::Void)};  ///< IL return type for the procedure.
        std::function<void()> postCollect;     ///< Hook after variable discovery.
        std::function<void()> emitEmptyBody;   ///< Emit return path for empty bodies.
        std::function<void()> emitFinalReturn; ///< Emit return in the synthetic exit block.
    };

  private:
    /// @brief Lower shared procedure scaffolding for FUNCTION/SUB declarations.
    /// @param name Source-level procedure name.
    /// @param params Ordered formal parameters.
    /// @param body Procedure body statements.
    /// @param config Return type and stage-specific callbacks.
    void lowerProcedure(const std::string &name,
                        const std::vector<Param> &params,
                        const std::vector<StmtPtr> &body,
                        const ProcedureConfig &config);

    /// @brief Collect declarations and cache main statement pointers.
    /// @param prog Program whose declarations are emitted before main.
    /// @return Context borrowing the program's main statements.
    ProgramEmitContext collectProgramDeclarations(const Program &prog);

    /// @brief Create the main function skeleton and block mappings.
    /// @param state Context updated with function and entry pointers.
    void buildMainFunctionSkeleton(ProgramEmitContext &state);

    /// @brief Discover global variables referenced by the main body.
    /// @param state Context containing main statement pointers.
    void collectMainVariables(ProgramEmitContext &state);

    /// @brief Materialise stack storage for main locals and bookkeeping.
    /// @param state Context whose entry block becomes the insertion point.
    void allocateMainLocals(ProgramEmitContext &state);

    /// @brief Emit the main body along with epilogue clean-up.
    /// @param state Main function, entry, exit bookkeeping, and statements.
    void emitMainBodyAndEpilogue(ProgramEmitContext &state);

    /// @brief Stack-allocate parameters and seed local map.
    /// @param params Ordered source-level parameter declarations.
    void materializeParams(const std::vector<Param> &params);

    /// @brief Reset procedure-level lowering state.
    /// @post Procedure symbols, block naming, and deferred releases are cleared.
    void resetLoweringState();

    /// @brief Dispatch one statement to its category-specific lowerer.
    /// @param stmt Statement AST node to emit.
    void lowerStmt(const Stmt &stmt);

    /// @brief Dispatch one expression to its category-specific lowerer.
    /// @param expr Expression AST node to emit.
    /// @return Lowered IL value and type.
    RVal lowerExpr(const Expr &expr);
    /// @brief Lower and normalize one expression for scalar contexts.
    /// @param expr Expression AST node to emit.
    /// @return Scalar-compatible value and type.
    RVal lowerScalarExpr(const Expr &expr);
    /// @brief Normalize an already lowered value for scalar contexts.
    /// @param value Lowered value to normalize.
    /// @param loc Source location for any emitted conversion.
    /// @return Scalar-compatible value and type.
    RVal lowerScalarExpr(RVal value, il::support::SourceLoc loc);

    /// @brief Lower a variable reference expression.
    /// @param expr Variable expression node.
    /// @return Loaded value and its type.
    RVal lowerVarExpr(const VarExpr &expr);

    /// @brief Lower a unary expression (e.g. NOT).
    /// @param expr Unary expression node.
    /// @return Resulting value and type.
    RVal lowerUnaryExpr(const UnaryExpr &expr);

    /// @brief Lower a binary expression.
    /// @param expr Binary expression node.
    /// @return Resulting value and type.
    RVal lowerBinaryExpr(const BinaryExpr &expr);

    /// @brief Translate a BASIC return type hint into an IL type.
    /// @param fnName BASIC function name used for suffix inference.
    /// @param hint Explicit BASIC return type annotation, if any.
    /// @return IL type matching the BASIC semantics for the function return.
    Type functionRetTypeFromHint(const std::string &fnName, BasicType hint) const;

    /// @brief Lower a boolean expression using explicit branch bodies.
    /// @param cond Boolean condition selecting THEN branch.
    /// @param loc Source location to attribute to control flow.
    /// @param emitThen Lambda emitting THEN branch body, storing into result slot.
    /// @param emitElse Lambda emitting ELSE branch body, storing into result slot.
    /// @param thenLabelBase Optional label base for THEN block naming.
    /// @param elseLabelBase Optional label base for ELSE block naming.
    /// @param joinLabelBase Optional label base for join block naming.
    /// @return Resulting value and type.
    RVal lowerBoolBranchExpr(Value cond,
                             il::support::SourceLoc loc,
                             const std::function<void(Value)> &emitThen,
                             const std::function<void(Value)> &emitElse,
                             std::string_view thenLabelBase = {},
                             std::string_view elseLabelBase = {},
                             std::string_view joinLabelBase = {});

    /// @brief Lower logical (`AND`/`OR`) expressions with short-circuiting.
    /// @param expr Binary expression node.
    /// @return Resulting value and type.
    RVal lowerLogicalBinary(const BinaryExpr &expr);

    /// @brief Lower integer division and modulo with divide-by-zero check.
    /// @param expr Binary expression node.
    /// @return Resulting value and type.
    RVal lowerDivOrMod(const BinaryExpr &expr);

    /// @brief Lower string concatenation and relational comparisons.
    /// @param expr Binary expression node.
    /// @param lhs Pre-lowered left-hand side.
    /// @param rhs Pre-lowered right-hand side.
    /// @return Resulting value and type.
    RVal lowerStringBinary(const BinaryExpr &expr, RVal lhs, RVal rhs);

    /// @brief Lower numeric arithmetic and comparisons.
    /// @param expr Binary expression node.
    /// @param lhs Pre-lowered left-hand side.
    /// @param rhs Pre-lowered right-hand side.
    /// @return Resulting value and type.
    RVal lowerNumericBinary(const BinaryExpr &expr, RVal lhs, RVal rhs);

    /// @brief Lower the power operator by invoking the runtime helper.
    /// @param expr Binary expression node.
    /// @param lhs Pre-lowered left-hand side.
    /// @param rhs Pre-lowered right-hand side.
    /// @return Resulting double-precision value.
    RVal lowerPowBinary(const BinaryExpr &expr, RVal lhs, RVal rhs);

    /// @brief Lower a built-in call expression.
    /// @param expr Built-in call expression node.
    /// @return Resulting value and type.
    RVal lowerBuiltinCall(const BuiltinCallExpr &expr);

    /// @brief Classify the numeric type of @p expr using BASIC promotion rules.
    /// @param expr Expression to inspect.
    /// @return Numeric type describing INTEGER, LONG, SINGLE, or DOUBLE semantics.
    TypeRules::NumericType classifyNumericType(const Expr &expr);

    /// @brief Lower a UBOUND query expression.
    /// @param expr UBOUND expression node naming the array.
    /// @return Resulting value and type.
    RVal lowerUBoundExpr(const UBoundExpr &expr);

    /// @brief Lower a NEW expression allocating a BASIC object instance.
    /// @param expr Object-construction expression.
    /// @return Newly allocated object pointer and type.
    RVal lowerNewExpr(const NewExpr &expr);
    /// @brief Lower object construction using an explicit OOP context.
    /// @param expr Object-construction expression.
    /// @param ctx Cached OOP layouts and dispatch metadata.
    /// @return Newly allocated object pointer and type.
    RVal lowerNewExpr(const NewExpr &expr, OopLoweringContext &ctx);

    /// @brief Lower a ME expression referencing the implicit instance slot.
    /// @param expr ME expression carrying source location.
    /// @return Loaded self pointer and object type.
    RVal lowerMeExpr(const MeExpr &expr);
    /// @brief Lower ME using an explicit OOP context.
    /// @param expr ME expression carrying source location.
    /// @param ctx Cached OOP layouts and dispatch metadata.
    /// @return Loaded self pointer and object type.
    RVal lowerMeExpr(const MeExpr &expr, OopLoweringContext &ctx);

    /// @brief Lower a member access reading a field from an object instance.
    /// @param expr Member access to resolve and load.
    /// @return Loaded field value and type.
    RVal lowerMemberAccessExpr(const MemberAccessExpr &expr);
    /// @brief Lower member access using an explicit OOP context.
    /// @param expr Member access to resolve and load.
    /// @param ctx Cached OOP layouts and dispatch metadata.
    /// @return Loaded field value and type.
    RVal lowerMemberAccessExpr(const MemberAccessExpr &expr, OopLoweringContext &ctx);

    /// @brief Lower an object method invocation expression.
    /// @param expr Method call including receiver and arguments.
    /// @return Call result and emitted IL type.
    RVal lowerMethodCallExpr(const MethodCallExpr &expr);
    /// @brief Lower a method call using an explicit OOP context.
    /// @param expr Method call including receiver and arguments.
    /// @param ctx Cached OOP layouts and dispatch metadata.
    /// @return Call result and emitted IL type.
    RVal lowerMethodCallExpr(const MethodCallExpr &expr, OopLoweringContext &ctx);

    /// @brief Scan a method call's argument expressions into runtime BasicTypes.
    /// @param expr Method call whose arguments are classified.
    /// @return Runtime-facing type category for each argument.
    std::vector<BasicType> methodCallRuntimeArgTypes(const MethodCallExpr &expr);
    /// @brief Scan a method call's argument expressions into AST Types.
    /// @param expr Method call whose arguments are classified.
    /// @return AST type category for each argument.
    std::vector<::il::frontends::basic::Type> methodCallAstArgTypes(const MethodCallExpr &expr);
    /// @brief Try to lower a static `Class.Method(...)` call (no receiver).
    /// @param expr Method-call expression under consideration.
    /// @return The lowered result, or std::nullopt if @p expr is an instance call.
    std::optional<RVal> tryLowerStaticMethodCall(const MethodCallExpr &expr);

    /// @brief Lower a DELETE statement releasing an object reference.
    /// @param stmt DELETE statement to lower.
    void lowerDelete(const DeleteStmt &stmt);
    /// @brief Lower DELETE using an explicit OOP context.
    /// @param stmt DELETE statement to lower.
    /// @param ctx Cached OOP layouts and dispatch metadata.
    void lowerDelete(const DeleteStmt &stmt, OopLoweringContext &ctx);

    /// @brief Coerce a value to `i64`.
    /// @param v Value and current IL type.
    /// @param loc Source location for conversion instructions.
    /// @return Converted or fallback value reported by the coercion engine.
    RVal coerceToI64(RVal v, il::support::SourceLoc loc);

    /// @brief Coerce a value to `f64`.
    /// @param v Value and current IL type.
    /// @param loc Source location for conversion instructions.
    /// @return Converted or fallback value reported by the coercion engine.
    RVal coerceToF64(RVal v, il::support::SourceLoc loc);

    /// @brief Coerce a value to an `i1` condition.
    /// @param v Value and current IL type.
    /// @param loc Source location for conversion instructions.
    /// @return Boolean value and `i1` type.
    RVal coerceToBool(RVal v, il::support::SourceLoc loc);

    /// @brief Route a value through the `i64` coercion contract.
    /// @param v Value and current IL type.
    /// @param loc Source location for conversion instructions.
    /// @return `i64`-compatible result.
    RVal ensureI64(RVal v, il::support::SourceLoc loc);

    /// @brief Route a value through the `f64` coercion contract.
    /// @param v Value and current IL type.
    /// @param loc Source location for conversion instructions.
    /// @return `f64`-compatible result.
    RVal ensureF64(RVal v, il::support::SourceLoc loc);

    /// @brief Convert one PRINT# value to runtime string form.
    /// @param expr Source expression used by type-sensitive formatting.
    /// @param value Already lowered expression value.
    /// @param quoteStrings Whether string values receive record-format quotes.
    /// @return String handle and optional runtime feature dependency.
    PrintChArgString lowerPrintChArgToString(const Expr &expr, RVal value, bool quoteStrings);

    /// @brief Build the comma-separated record payload for PRINT#.
    /// @param stmt PRINT# statement containing output items.
    /// @return Runtime string handle for the assembled record.
    Value buildPrintChWriteRecord(const PrintChStmt &stmt);

    // -------------------------------------------------------------------------
    // Core statement lowering (formerly LowerStmt_Core.hpp)
    // -------------------------------------------------------------------------
    /// @brief Lower a statement list in source order.
    /// @param stmt Statement-list node.
    void lowerStmtList(const StmtList &stmt);
    /// @brief Lower a call used in statement position.
    /// @param stmt CALL statement node.
    void lowerCallStmt(const CallStmt &stmt);
    /// @brief Lower a procedure or GOSUB RETURN statement.
    /// @param stmt RETURN statement node.
    void lowerReturn(const ReturnStmt &stmt);
    /// @brief Normalize a runtime channel operand to `i32`.
    /// @param channel Lowered channel value.
    /// @param loc Source location for coercion and narrowing.
    /// @return Channel value carrying `i32` type.
    RVal normalizeChannelToI32(RVal channel, il::support::SourceLoc loc);
    /// @brief Branch on a nonzero runtime error code.
    /// @param err Runtime `i32` error value.
    /// @param loc Source location for generated control flow.
    /// @param labelStem Stem for unique fail and continuation block names.
    /// @param onFailure Callback emitted in the failure block.
    void emitRuntimeErrCheck(Value err,
                             il::support::SourceLoc loc,
                             std::string_view labelStem,
                             const std::function<void(Value)> &onFailure);

    // -------------------------------------------------------------------------
    // Runtime statement lowering (formerly LowerStmt_Runtime.hpp)
    // -------------------------------------------------------------------------
    /// @brief Lower a LET assignment.
    /// @param stmt LET statement node.
    void lowerLet(const LetStmt &stmt);
    /// @brief Lower a CONST declaration initializer.
    /// @param stmt CONST statement node.
    void lowerConst(const ConstStmt &stmt);
    /// @brief Process a STATIC declaration; storage was prepared during scanning.
    /// @param stmt STATIC statement node.
    void lowerStatic(const StaticStmt &stmt);
    /// @brief Store a value into scalar storage with coercion and lifetime handling.
    /// @param slotInfo Target storage metadata.
    /// @param slot Target address.
    /// @param value Right-hand value.
    /// @param loc Source location for emitted instructions.
    void assignScalarSlot(const SlotType &slotInfo,
                          Value slot,
                          RVal value,
                          il::support::SourceLoc loc);
    /// @brief Store a value into a resolved array element.
    /// @param target Array expression describing base and indices.
    /// @param value Right-hand value.
    /// @param loc Source location for emitted instructions.
    void assignArrayElement(const ArrayExpr &target, RVal value, il::support::SourceLoc loc);
    /// @brief Lower DIM allocation.
    /// @param stmt DIM statement node.
    void lowerDim(const DimStmt &stmt);
    /// @brief Lower REDIM resizing.
    /// @param stmt REDIM statement node.
    void lowerReDim(const ReDimStmt &stmt);
    /// @brief Lower RANDOMIZE seeding.
    /// @param stmt RANDOMIZE statement node.
    void lowerRandomize(const RandomizeStmt &stmt);
    /// @brief Lower TRY/CATCH/FINALLY control flow.
    /// @param stmt TRY statement node.
    void lowerTryCatch(const TryCatchStmt &stmt);
    /// @brief Lower a USING statement and scoped cleanup.
    /// @param stmt USING statement node.
    void lowerUsingStmt(const UsingStmt &stmt);
    /// @brief Lower SWAP for supported l-values.
    /// @param stmt SWAP statement node.
    void lowerSwap(const SwapStmt &stmt);
    /// @brief Lower BEEP.
    /// @param stmt BEEP statement node.
    void visit(const BeepStmt &stmt);
    /// @brief Lower CLS.
    /// @param stmt CLS statement node.
    void visit(const ClsStmt &stmt);
    /// @brief Lower COLOR.
    /// @param stmt COLOR statement node.
    void visit(const ColorStmt &stmt);
    /// @brief Lower SLEEP.
    /// @param stmt SLEEP statement node.
    void visit(const SleepStmt &stmt);
    /// @brief Lower LOCATE.
    /// @param stmt LOCATE statement node.
    void visit(const LocateStmt &stmt);
    /// @brief Lower CURSOR visibility.
    /// @param stmt CURSOR statement node.
    void visit(const CursorStmt &stmt);
    /// @brief Lower ALTSCREEN.
    /// @param stmt ALTSCREEN statement node.
    void visit(const AltScreenStmt &stmt);
    /// @brief Convert an inclusive array upper bound to a checked length.
    /// @param bound Lowered upper-bound value.
    /// @param loc Source location for checks.
    /// @param labelBase Stem for failure and continuation blocks.
    /// @return Length value, passed through a block parameter when control flow exists.
    Value emitArrayLengthCheck(Value bound, il::support::SourceLoc loc, std::string_view labelBase);

    // -------------------------------------------------------------------------
    // I/O statement lowering (formerly LowerStmt_IO.hpp)
    // -------------------------------------------------------------------------
    /// @brief Lower OPEN.
    /// @param stmt OPEN statement node.
    void lowerOpen(const OpenStmt &stmt);
    /// @brief Lower CLOSE.
    /// @param stmt CLOSE statement node.
    void lowerClose(const CloseStmt &stmt);
    /// @brief Lower SEEK.
    /// @param stmt SEEK statement node.
    void lowerSeek(const SeekStmt &stmt);
    /// @brief Lower terminal PRINT.
    /// @param stmt PRINT statement node.
    void lowerPrint(const PrintStmt &stmt);
    /// @brief Lower channel PRINT#.
    /// @param stmt PRINT# statement node.
    void lowerPrintCh(const PrintChStmt &stmt);
    /// @brief Lower terminal INPUT.
    /// @param stmt INPUT statement node.
    void lowerInput(const InputStmt &stmt);
    /// @brief Lower channel INPUT#.
    /// @param stmt INPUT# statement node.
    void lowerInputCh(const InputChStmt &stmt);
    /// @brief Lower LINE INPUT#.
    /// @param stmt LINE INPUT# statement node.
    void lowerLineInputCh(const LineInputChStmt &stmt);

    // -------------------------------------------------------------------------
    // Control flow lowering (formerly LowerStmt_Control.hpp)
    // -------------------------------------------------------------------------

    /// @brief Emit blocks for an IF/ELSEIF chain.
    /// @param conds Number of conditions (IF + ELSEIFs).
    /// @return Indices for test/then blocks and ELSE/exit blocks.
    IfBlocks emitIfBlocks(size_t conds);
    /// @brief Lower one IF condition and branch to true/false targets.
    /// @param cond Condition expression.
    /// @param testBlk Block receiving condition evaluation.
    /// @param thenBlk True destination.
    /// @param falseBlk False destination.
    /// @param loc Source location for emitted control flow.
    void lowerIfCondition(const Expr &cond,
                          BasicBlock *testBlk,
                          BasicBlock *thenBlk,
                          BasicBlock *falseBlk,
                          il::support::SourceLoc loc);
    /// @brief Lower a boolean expression directly into a conditional branch.
    /// @param expr Boolean expression.
    /// @param trueBlk True destination.
    /// @param falseBlk False destination.
    /// @param loc Source location for emitted control flow.
    void lowerCondBranch(const Expr &expr,
                         BasicBlock *trueBlk,
                         BasicBlock *falseBlk,
                         il::support::SourceLoc loc);
    /// @brief Lower one IF branch body and connect fallthrough to the join.
    /// @param stmt Optional branch body.
    /// @param thenBlk Block receiving that body.
    /// @param exitIdx Stable active-function index of the join block.
    /// @param loc Source location for emitted branches.
    /// @return True when the branch remains able to fall through.
    bool lowerIfBranch(const Stmt *stmt,
                       BasicBlock *thenBlk,
                       size_t exitIdx,
                       il::support::SourceLoc loc);
    /// @brief Emit the complete IF/ELSEIF/ELSE graph.
    /// @param stmt IF statement node.
    /// @return Current/join state after emission.
    CtrlState emitIf(const IfStmt &stmt);
    /// @brief Lower an IF statement and install its continuation.
    /// @param stmt IF statement node.
    void lowerIf(const IfStmt &stmt);
    /// @brief Lower loop-body statements in order.
    /// @param body Owned statement pointers comprising the body.
    void lowerLoopBody(const std::vector<StmtPtr> &body);
    /// @brief Emit a WHILE/WEND graph.
    /// @param stmt WHILE statement node.
    /// @return Current/join state after emission.
    CtrlState emitWhile(const WhileStmt &stmt);
    /// @brief Lower WHILE/WEND.
    /// @param stmt WHILE statement node.
    void lowerWhile(const WhileStmt &stmt);
    /// @brief Emit a pre- or post-tested DO/LOOP graph.
    /// @param stmt DO statement node.
    /// @return Current/join state after emission.
    CtrlState emitDo(const DoStmt &stmt);
    /// @brief Lower DO/LOOP.
    /// @param stmt DO statement node.
    void lowerDo(const DoStmt &stmt);
    /// @brief Allocate blocks needed by a FOR loop.
    /// @param varStep Whether positive and negative runtime heads are required.
    /// @return Stable block indexes for loop construction.
    ForBlocks setupForBlocks(bool varStep);
    /// @brief Lower FOR using a compile-time step direction.
    /// @param stmt FOR statement node.
    /// @param slot Induction-variable storage.
    /// @param end Previously lowered inclusive bound.
    /// @param step Previously lowered step value.
    /// @param stepConst Constant step used to choose comparison direction.
    void lowerForConstStep(const ForStmt &stmt, Value slot, RVal end, RVal step, int64_t stepConst);
    /// @brief Lower FOR using a runtime step-sign branch.
    /// @param stmt FOR statement node.
    /// @param slot Induction-variable storage.
    /// @param end Previously lowered inclusive bound.
    /// @param step Previously lowered, reusable step value.
    void lowerForVarStep(const ForStmt &stmt, Value slot, RVal end, RVal step);
    /// @brief Emit a FOR loop and report its continuation.
    /// @param stmt FOR statement node.
    /// @param slot Induction-variable storage.
    /// @param end Previously lowered inclusive bound.
    /// @param step Previously lowered step value.
    /// @return Current/join state after emission.
    CtrlState emitFor(const ForStmt &stmt, Value slot, RVal end, RVal step);
    /// @brief Lower a FOR statement from its AST operands.
    /// @param stmt FOR statement node.
    void lowerFor(const ForStmt &stmt);
    /// @brief Lower object/collection FOR EACH iteration.
    /// @param stmt FOR EACH statement node.
    void lowerForEach(const ForEachStmt &stmt);
    /// @brief Add a FOR step to the induction variable.
    /// @param slot Induction-variable storage.
    /// @param step Step value to add.
    void emitForStep(Value slot, Value step);
    /// @brief Emit SELECT CASE branching.
    /// @param stmt SELECT CASE statement node.
    /// @return Current/join state after emission.
    CtrlState emitSelect(const SelectCaseStmt &stmt);
    /// @brief Lower NEXT.
    /// @param stmt NEXT statement node.
    void lowerNext(const NextStmt &stmt);
    /// @brief Lower EXIT for the selected construct.
    /// @param stmt EXIT statement node.
    void lowerExit(const ExitStmt &stmt);
    /// @brief Lower GOSUB stack setup and branch.
    /// @param stmt GOSUB statement node.
    void lowerGosub(const GosubStmt &stmt);
    /// @brief Lower an unconditional GOTO.
    /// @param stmt GOTO statement node.
    void lowerGoto(const GotoStmt &stmt);
    /// @brief Lower RETURN from a GOSUB.
    /// @param stmt RETURN statement node.
    void lowerGosubReturn(const ReturnStmt &stmt);
    /// @brief Install or clear an ON ERROR GOTO handler.
    /// @param stmt ON ERROR statement node.
    void lowerOnErrorGoto(const OnErrorGoto &stmt);
    /// @brief Lower RESUME within an error handler.
    /// @param stmt RESUME statement node.
    void lowerResume(const Resume &stmt);
    /// @brief Lower END using the active function's return type.
    /// @param stmt END statement node.
    void lowerEnd(const EndStmt &stmt);

    /// @brief Lower SELECT CASE and install its continuation.
    /// @param stmt SELECT CASE statement node.
    void lowerSelectCase(const SelectCaseStmt &stmt);

    /// @brief Return the IL boolean type.
    /// @return `i1` type descriptor.
    IlType ilBoolTy();

    /// @brief Materialize an `i1` boolean constant.
    /// @param v Boolean value.
    /// @return Constant IL value.
    IlValue emitBoolConst(bool v);

    /// @brief Build then/else blocks that store and reload one boolean result.
    /// @param emitThen Callback populating the shared result slot in the true block.
    /// @param emitElse Callback populating the shared result slot in the false block.
    /// @param thenLabelBase Stem for the true block.
    /// @param elseLabelBase Stem for the false block.
    /// @param joinLabelBase Stem for the join block.
    /// @return Boolean loaded in the join block.
    IlValue emitBoolFromBranches(const std::function<void(Value)> &emitThen,
                                 const std::function<void(Value)> &emitElse,
                                 std::string_view thenLabelBase = "bool_then",
                                 std::string_view elseLabelBase = "bool_else",
                                 std::string_view joinLabelBase = "bool_join");

    /// @brief Emit a stack allocation.
    /// @param bytes Requested allocation size in bytes.
    /// @return Pointer to the allocated storage.
    Value emitAlloca(int bytes);

    /// @brief Emit a typed load.
    /// @param ty Value type to load.
    /// @param addr Source address.
    /// @return Loaded SSA value.
    Value emitLoad(Type ty, Value addr);

    /// @brief Emit a typed store.
    /// @param ty Stored value type.
    /// @param addr Destination address.
    /// @param val Value to write.
    void emitStore(Type ty, Value addr, Value val);

    /// @brief Emit a typed binary instruction.
    /// @param op Opcode to emit.
    /// @param ty Instruction result type.
    /// @param lhs Left operand.
    /// @param rhs Right operand.
    /// @return Instruction result value.
    Value emitBinary(Opcode op, Type ty, Value lhs, Value rhs);

    /// @brief Emit unary instruction of @p op on @p val producing @p ty.
    /// @param op Opcode to emit.
    /// @param ty Instruction result type.
    /// @param val Operand value.
    /// @return Instruction result value.
    Value emitUnary(Opcode op, Type ty, Value val);

    /// @brief Materialize a signed 64-bit integer constant.
    /// @param v Constant value.
    /// @return IL constant operand.
    Value emitConstI64(std::int64_t v);

    /// @brief Zero-extend an `i1` value to `i64`.
    /// @param val Boolean bit.
    /// @return `i64` zero or one.
    Value emitZext1ToI64(Value val);

    /// @brief Emit integer subtraction.
    /// @param lhs Left operand.
    /// @param rhs Right operand.
    /// @return Subtraction result.
    Value emitISub(Value lhs, Value rhs);

    /// @brief Convert an `i1` truth value to BASIC's `i64` -1/0 form.
    /// @param b1 Boolean bit.
    /// @return Canonical BASIC logical word.
    Value emitBasicLogicalI64(Value b1);
    /// @brief Create an emission facade at the current source location.
    /// @return Lightweight facade borrowing this Lowerer.
    Emit emitCommon() noexcept;
    /// @brief Create an emission facade at an explicit source location.
    /// @param loc Location applied by the facade.
    /// @return Lightweight facade borrowing this Lowerer.
    Emit emitCommon(il::support::SourceLoc loc) noexcept;

    /// @brief Get the type coercion engine for this lowerer.
    /// @return Reference to the coercion engine.
    TypeCoercionEngine &coercion() noexcept;

    /// @brief Narrow a 64-bit value to 32 bits for runtime calls.
    /// @details Convenience wrapper around emitCommon().to_iN(value, 32) that reduces
    ///          boilerplate when preparing arguments for 32-bit runtime functions.
    /// @param value The 64-bit value to narrow.
    /// @param loc Source location for the narrowing operation.
    /// @return Narrowed 32-bit value.
    Value narrow32(Value value, il::support::SourceLoc loc);

    /// @brief Emit checked integer negation for @p val producing type @p ty.
    /// @param ty Integer operand/result type.
    /// @param val Value to negate.
    /// @return Checked negation result.
    Value emitCheckedNeg(Type ty, Value val);

    /// @brief Emit an unconditional branch.
    /// @param target Destination block.
    void emitBr(BasicBlock *target);

    /// @brief Emit a conditional branch.
    /// @param cond `i1` condition.
    /// @param t True destination.
    /// @param f False destination.
    void emitCBr(Value cond, BasicBlock *t, BasicBlock *f);

    /// @brief Emit a direct value-returning call.
    /// @param ty Callee return type.
    /// @param callee Emitted symbol name.
    /// @param args Ordered argument values.
    /// @return Call result.
    Value emitCallRet(Type ty, const std::string &callee, const std::vector<Value> &args);

    /// @brief Emit a direct void call.
    /// @param callee Emitted symbol name.
    /// @param args Ordered argument values.
    void emitCall(const std::string &callee, const std::vector<Value> &args);

    /// @brief Request runtime helper and emit call in a single operation.
    /// @details Combines requestHelper() and emitCallRet() to reduce boilerplate in
    ///          lowering code that needs to call runtime functions.
    /// @param feature Runtime feature to request (ensures helper is linked).
    /// @param callee Name of the runtime function to call.
    /// @param returnType Return type of the runtime function.
    /// @param args Arguments to pass to the runtime function.
    /// @return Value representing the call result.
    Value emitRuntimeHelper(il::runtime::RuntimeFeature feature,
                            const std::string &callee,
                            Type returnType,
                            const std::vector<Value> &args);

    /// @brief Emit an indirect call where the callee is a value operand.
    /// @param ty Callee return type.
    /// @param callee Function-pointer value.
    /// @param args Ordered argument values.
    /// @return Call result.
    Value emitCallIndirectRet(Type ty, Value callee, const std::vector<Value> &args);

    /// @brief Emit a void-typed indirect call where the callee is a value operand.
    /// @param callee Function-pointer value.
    /// @param args Ordered argument values.
    void emitCallIndirect(Value callee, const std::vector<Value> &args);

    /// @brief Materialize a pointer to interned global string data.
    /// @param globalName Global symbol name.
    /// @return Pointer value referencing that global.
    Value emitConstStr(const std::string &globalName);
    /// @brief Store a non-object array handle into a slot.
    /// @param slot Destination address.
    /// @param value Array handle.
    /// @param elementType BASIC element type used for lifetime handling.
    void storeArray(Value slot, Value value, AstType elementType = AstType::I64);
    /// @brief Store an array handle with explicit object-array classification.
    /// @param slot Destination address.
    /// @param value Array handle.
    /// @param elementType BASIC element type.
    /// @param isObjectArray Whether object-array retain/release rules apply.
    void storeArray(Value slot, Value value, AstType elementType, bool isObjectArray);
    /// @brief Release local array handles except named parameters.
    /// @param paramNames Names excluded from local cleanup.
    void releaseArrayLocals(const std::unordered_set<std::string> &paramNames);
    /// @brief Release array handles materialized from parameters.
    /// @param paramNames Parameter names eligible for cleanup.
    void releaseArrayParams(const std::unordered_set<std::string> &paramNames);
    /// @brief Release local object handles except named parameters.
    /// @param paramNames Names excluded from local cleanup.
    void releaseObjectLocals(const std::unordered_set<std::string> &paramNames);
    /// @brief Release object handles materialized from parameters.
    /// @param paramNames Parameter names eligible for cleanup.
    void releaseObjectParams(const std::unordered_set<std::string> &paramNames);
    /// @brief Emit releases for all deferred temporary handles.
    /// @post Deferred string and object lists are empty.
    void releaseDeferredTemps();
    /// @brief Discard deferred temporary bookkeeping without emitting releases.
    /// @post Deferred string and object lists are empty.
    void clearDeferredTemps();
    /// @brief Queue a temporary string handle for epilogue release.
    /// @param v String handle.
    void deferReleaseStr(Value v);
    /// @brief Queue a temporary object handle for epilogue release.
    /// @param v Object handle.
    /// @param className Optional class name used for destructor lookup.
    void deferReleaseObj(Value v, const std::string &className = {});

    /// @brief Emit the generic trap path.
    void emitTrap();

    /// @brief Emit a trap carrying a runtime error code.
    /// @param errCode Runtime `i32` error value.
    void emitTrapFromErr(Value errCode);

    /// @brief Push an error-handler block onto the runtime EH stack.
    /// @param handler Handler destination.
    void emitEhPush(BasicBlock *handler);
    /// @brief Pop the current runtime error handler.
    void emitEhPop();
    /// @brief Pop handlers required before returning from the active procedure.
    void emitEhPopForReturn();
    /// @brief Clear ON ERROR handler bookkeeping for the active procedure.
    void clearActiveErrorHandler();
    /// @brief Find or build the handler block for a target line.
    /// @param targetLine BASIC line label.
    /// @return Borrowed block pointer in the active function.
    BasicBlock *ensureErrorHandlerBlock(int targetLine);

    /// @brief Emit a value-returning terminator after EH cleanup.
    /// @param v Return value.
    void emitRet(Value v);

    /// @brief Emit a void return terminator after EH cleanup.
    void emitRetVoid();

    /// @brief Intern string data and obtain its global label.
    /// @param s String payload.
    /// @return Stable label for the current string table.
    std::string getStringLabel(const std::string &s);

    /// @brief Allocate the next procedure-local temporary identifier.
    /// @return Monotonically increasing temporary ID.
    unsigned nextTempId();

    /// @brief Generate a unique fallback block label.
    /// @return Mangled label containing the next fallback ID.
    std::string nextFallbackBlockLabel();

    /// @brief Access the mutable owned emitter facade.
    /// @return Emitter reference valid for this Lowerer's lifetime.
    lower::Emitter &emitter() noexcept;
    /// @brief Access the immutable owned emitter facade.
    /// @return Const emitter reference valid for this Lowerer's lifetime.
    const lower::Emitter &emitter() const noexcept;

    /// @brief Lower array indexing for a load or store.
    /// @param expr Array expression containing base and indices.
    /// @param kind Access mode controlling helper selection.
    /// @return Base/index/type information for the element access.
    ArrayAccess lowerArrayAccess(const ArrayExpr &expr, ArrayAccessKind kind);

    /// @brief Emit the row-major flattened index for a multi-dimensional array
    ///        access from per-index values and declared (inclusive) extents.
    /// @param idxVals One lowered i64 value per dimension.
    /// @param extents Declared per-dimension upper bounds (length = extent + 1).
    /// @return Flattened row-major index.
    Value emitRowMajorFlatIndex(const std::vector<Value> &idxVals,
                                const std::vector<long long> &extents);

    /// @brief Emit procedure declarations and the synthetic main function.
    /// @param prog Program AST to lower.
    void emitProgram(const Program &prog);

    /// @brief Current expression lowering depth for recursion guard.
    unsigned exprLowerDepth_{0};
    /// @brief Current statement lowering depth for recursion guard.
    unsigned stmtLowerDepth_{0};
    /// @brief Maximum allowed lowering recursion depth.
    static constexpr unsigned kMaxLowerDepth = 512;

    /// @brief Ensure fixed-depth GOSUB stack storage exists in the active function.
    void ensureGosubStack();

    /// Program-level orchestration facade owned for this Lowerer's lifetime.
    std::unique_ptr<ProgramLowering> programLowering;
    /// Procedure-level orchestration facade owned for this Lowerer's lifetime.
    std::unique_ptr<ProcedureLowering> procedureLowering;
    /// Statement-dispatch facade owned for this Lowerer's lifetime.
    std::unique_ptr<StatementLowering> statementLowering;

    /// Builder borrowed only while ProgramLowering::run is active.
    build::IRBuilder *builder{nullptr};
    /// Destination module borrowed only while ProgramLowering::run is active.
    Module *mod{nullptr};
    /// Deterministic symbol and block-name generator for the current run.
    NameMangler mangler;
    SymbolTable symbolTable_;        ///< Unified symbol table.
    SymbolTable::SymbolMap &symbols; ///< Legacy alias (references symbolTable_.raw()).
    StringTable stringTable_;        ///< String literal interning.
    il::support::SourceLoc curLoc{}; ///< current source location for emitted IR
    /// Whether lowering emits optional array bounds metadata and checks.
    bool boundsChecks{false};
    /// Maximum number of nested GOSUB return addresses.
    static constexpr int kGosubStackDepth = 128;
    /// Counter used when structured block naming is unavailable.
    size_t nextFallbackBlockId{0};
    /// Procedure signatures indexed by canonical source name.
    std::unordered_map<std::string, ProcedureSignature> procSignatures;
    /// Alternate source spellings mapped to emitted procedure names.
    std::unordered_map<std::string, std::string> procNameAliases;

    /// Cached virtual line identifier for each borrowed statement node.
    std::unordered_map<const Stmt *, int> stmtVirtualLines_;
    /// Starting negative range reserved for synthetic line identifiers.
    int synthLineBase_{-1000000000};
    /// Sequence offset within the synthetic line range.
    int synthSeq_{0};

    /// Mutable state for the procedure currently being emitted.
    ProcedureContext context_;

    /// Low-level IL emission facade owned by this Lowerer.
    std::unique_ptr<lower::Emitter> emitter_;
    /// Shared scalar type-coercion service owned by this Lowerer.
    std::unique_ptr<TypeCoercionEngine> coercionEngine_;

    /// Optional diagnostic sink borrowed from the compilation driver.
    DiagnosticEmitter *diagnosticEmitter_{nullptr};
    /// Optional semantic analyzer borrowed from the compilation driver.
    const SemanticAnalyzer *semanticAnalyzer_{nullptr};

    /// Module variables routed through runtime-backed cross-procedure storage.
    std::unordered_set<std::string> crossProcGlobals_;

  public:
    /// @brief Mark a module-level variable as accessed from within a procedure.
    /// @details Variables marked cross-proc are routed through runtime-backed
    ///          storage so that main and procedure scopes share the same value.
    /// @param name BASIC symbol name to mark.
    void markCrossProcGlobal(const std::string &name) {
        crossProcGlobals_.insert(name);
        std::string canon = CanonicalizeIdent(name);
        if (!canon.empty())
            crossProcGlobals_.insert(std::move(canon));
    }

    /// @brief Check whether a variable has been marked as cross-procedure global.
    /// @param name BASIC symbol name to query.
    /// @return True when the symbol was previously marked via markCrossProcGlobal.
    bool isCrossProcGlobal(const std::string &name) const {
        if (crossProcGlobals_.find(name) != crossProcGlobals_.end())
            return true;
        std::string canon = CanonicalizeIdent(name);
        return !canon.empty() && crossProcGlobals_.find(canon) != crossProcGlobals_.end();
    }

    /// Runtime feature enumeration shared with the descriptor registry.
    using RuntimeFeature = il::runtime::RuntimeFeature;

    /// Feature requests, ordered uses, and exact runtime callee spellings.
    RuntimeHelperTracker runtimeTracker;

    /// Manual helper enumeration re-exported for lowering subsystems.
    using ManualRuntimeHelper = ::il::frontends::basic::ManualRuntimeHelper;
    /// @brief Total number of manual runtime helpers tracked by the lowerer.
    static constexpr std::size_t manualRuntimeHelperCount = kManualRuntimeHelperCount;

    /// @brief Map a ManualRuntimeHelper enum to its array index.
    /// @param helper The manual runtime helper to index.
    /// @return Zero-based index suitable for array subscript.
    static constexpr std::size_t manualRuntimeHelperIndex(ManualRuntimeHelper helper) noexcept {
        return ::il::frontends::basic::manualRuntimeHelperIndex(helper);
    }

    /// Requirement bits for helpers selected outside registry feature lowering.
    ManualHelperRequirements manualHelperRequirements_{};

    /// @brief Flag a manual runtime helper as required for the current compilation.
    /// @param helper The manual runtime helper to require.
    void setManualHelperRequired(ManualRuntimeHelper helper);

    /// @brief Check whether a manual runtime helper has been flagged as required.
    /// @param helper The manual runtime helper to query.
    /// @return True when the helper was previously required.
    [[nodiscard]] bool isManualHelperRequired(ManualRuntimeHelper helper) const;

    /// @brief Reset all manual helper requirement flags.
    void resetManualHelpers();

    // ═══ Runtime Requirement Declarations ═══
    // These one-liner require*() methods are in a separate header for readability.
#include "lowerer/LowererRuntimeRequirements.hpp"
    /// @brief Request that a runtime feature be declared and linked.
    /// @param feature Runtime feature to make available in the emitted module.
    void requestHelper(RuntimeFeature feature);

    /// @brief Check whether a runtime feature has been requested.
    /// @param feature Runtime feature to query.
    /// @return True when the feature was previously requested.
    bool isHelperNeeded(RuntimeFeature feature) const;

    /// @brief Record that a runtime feature is used without immediately declaring it.
    /// @param feature Runtime feature to track for later declaration.
    void trackRuntime(RuntimeFeature feature);

    /// @brief Emit IL declarations for all runtime helpers that have been requested.
    /// @param b IRBuilder used to emit function declarations into the module.
    void declareRequiredRuntime(build::IRBuilder &b);
#include "frontends/basic/LowerScan.hpp"
  public:
    // Re-export OOP types from LowererTypes.hpp
    using ClassLayout = ::il::frontends::basic::ClassLayout;
    using MemberFieldAccess = ::il::frontends::basic::MemberFieldAccess;
    using FieldScope = ::il::frontends::basic::FieldScope;

  private:
    /// @brief Scan program OOP constructs to populate class layouts and runtime requests.
    /// @param prog Program containing class/interface declarations.
    void scanOOP(const Program &prog);

    /// @brief Emit constructor, destructor, and method bodies for CLASS declarations.
    /// @param prog Program containing class/interface declarations.
    void emitOopDeclsAndBodies(const Program &prog);

    /// @brief Emit one interface-registration thunk per declared interface.
    /// @details Adds the thunk functions to the bound module.
    /// @return The mangled names of the emitted thunks (for the module init).
    std::vector<std::string> emitInterfaceRegThunks();

    /// @brief Emit class→interface itable binding thunks (allocate + populate).
    /// @details Adds one binding function for each discovered implementation.
    /// @return The mangled names of the emitted thunks (for the module init).
    std::vector<std::string> emitInterfaceBindThunks();

    /// @brief Emit one class constructor function.
    /// @param klass Owning class declaration and layout identity.
    /// @param ctor Constructor signature and body.
    void emitClassConstructor(const ClassDecl &klass, const ConstructorDecl &ctor);

    /// @brief Emit one class destructor, synthesizing cleanup as required.
    /// @param klass Owning class declaration and layout identity.
    /// @param userDtor Optional user-written destructor body.
    void emitClassDestructor(const ClassDecl &klass, const DestructorDecl *userDtor);

    /// @brief Emit one declared class method.
    /// @param klass Owning class declaration and layout identity.
    /// @param method Method signature and body.
    void emitClassMethod(const ClassDecl &klass, const MethodDecl &method);

    /// @brief Emit IL for a BASIC class method, supplying an explicit body.
    /// @details Variant used by PROPERTY synthesis to reuse accessor bodies
    ///          without attempting to transfer ownership of AST nodes.
    /// @param klass Owning class declaration and layout identity.
    /// @param method Method signature metadata.
    /// @param bodyStmts Non-owning statement pointers forming the selected body.
    void emitClassMethodWithBody(const ClassDecl &klass,
                                 const MethodDecl &method,
                                 const std::vector<const Stmt *> &bodyStmts);

    /// @brief Materialize and record the implicit self parameter slot.
    /// @param className Qualified receiver class.
    /// @param fn Active method function.
    /// @return Slot identifier containing the self pointer.
    unsigned materializeSelfSlot(const std::string &className, Function &fn);

    /// @brief Load the implicit self pointer from a materialized slot.
    /// @param slotId Slot identifier returned by @ref materializeSelfSlot.
    /// @return Object pointer value.
    Value loadSelfPointer(unsigned slotId);

    /// @brief Release managed fields described by a class layout.
    /// @param selfPtr Receiver object pointer.
    /// @param layout Field offsets and lifetime traits.
    void emitFieldReleaseSequence(Value selfPtr, const ClassLayout &layout);

    /// @brief I/O statement lowering subsystem.
    std::unique_ptr<IoStatementLowerer> ioStmtLowerer_;

    /// @brief Control flow statement lowering subsystem.
    std::unique_ptr<ControlStatementLowerer> ctrlStmtLowerer_;

    /// @brief Runtime statement lowering subsystem.
    std::unique_ptr<RuntimeStatementLowerer> runtimeStmtLowerer_;

    /// @brief Cached layout table indexed by class or TYPE name.
    std::unordered_map<std::string, ClassLayout, StringHash, std::equal_to<>> classLayouts_;

    /// @brief Indexed CLASS metadata collected during scanning.
    OopIndex oopIndex_;

    /// @brief OOP lowering context for passing through the pipeline.
    std::unique_ptr<OopLoweringContext> oopContext_;

    // Field scopes are now managed internally by symbolTable_.

    /// @brief Active namespace path segments used for qualification during lowering.
    std::vector<std::string> nsStack_;

    /// @brief Stack of fully-qualified class names for access checking during lowering.
    std::vector<std::string> classStack_;

    /// @brief Determine the BASIC class associated with an object expression.
    /// @param expr Object-valued expression to classify.
    /// @return Qualified class name, or empty when unknown.
    [[nodiscard]] std::string resolveObjectClass(const Expr &expr) const;

    /// @brief Resolve pointer and type information for a member access expression.
    /// @param expr Member access to resolve with the current OOP context.
    /// @return Field access metadata, or empty when unresolved.
    [[nodiscard]] std::optional<MemberFieldAccess> resolveMemberField(const MemberAccessExpr &expr);
    /// @brief Resolve member access with an explicit OOP context.
    /// @param expr Member access to resolve.
    /// @param ctx Cached layouts and dispatch metadata.
    /// @return Field access metadata, or empty when unresolved.
    [[nodiscard]] std::optional<MemberFieldAccess> resolveMemberField(const MemberAccessExpr &expr,
                                                                      OopLoweringContext &ctx);

    /// @brief Resolve an unqualified name as a field of the active class.
    /// @param name Candidate field name.
    /// @param loc Source location for diagnostics/emission.
    /// @return Field access metadata, or empty when the name is not an implicit field.
    [[nodiscard]] std::optional<MemberFieldAccess> resolveImplicitField(std::string_view name,
                                                                        il::support::SourceLoc loc);
    /// @brief Resolve an implicit field with an explicit OOP context.
    /// @param name Candidate field name.
    /// @param loc Source location for diagnostics/emission.
    /// @param ctx Cached layouts and dispatch metadata.
    /// @return Field access metadata, or empty when unresolved.
    [[nodiscard]] std::optional<MemberFieldAccess> resolveImplicitField(std::string_view name,
                                                                        il::support::SourceLoc loc,
                                                                        OopLoweringContext &ctx);
    /// @brief Resolve local, static, module, or implicit-field storage by name.
    /// @param name Symbol name.
    /// @param loc Source location for diagnostics and address emission.
    /// @return Storage address and slot metadata, or empty on failure.
    [[nodiscard]] std::optional<VariableStorage> resolveVariableStorage(std::string_view name,
                                                                        il::support::SourceLoc loc);
    /// @brief Access the currently active class-field scope.
    /// @return Borrowed scope pointer, or null outside class member lowering.
    [[nodiscard]] const FieldScope *activeFieldScope() const;

    // --- BUG-109: Canonical class-layout resolution helpers ---
  public:
    /// @brief Resolve a class layout by canonicalizing name (qualified/unqualified, casing).
    /// @param className Class name as discovered (may be qualified or different casing).
    /// @return Pointer to layout when found; nullptr otherwise.
    [[nodiscard]] const ClassLayout *findClassLayout(std::string_view className) const;

    /// @brief Resolve member array field information for a variable name.
    /// @details Consolidates the repeated field/array/object checks previously
    ///          scattered across RuntimeStatementLowerer_Assign, Emit_Expr,
    ///          Lowerer_Expr, and Lower_OOP_Helpers (BUG-056, BUG-058, BUG-089,
    ///          BUG-108). Handles dotted member arrays, implicit field arrays in
    ///          methods, and local variable shadowing.
    /// @param name Variable name to resolve (may contain a dot for member access).
    /// @return MemberArrayInfo describing the resolution result.
    [[nodiscard]] MemberArrayInfo resolveMemberArrayField(std::string_view name) const;

  private:
    /// @brief Compute the canonical layout key (unqualified, declared casing) for a class name.
    /// @details Uses the OOP index to normalize qualified names and casing, then returns the
    ///          unqualified identifier used as the key in classLayouts_. Falls back to the last
    ///          dotted segment when lookup fails.
    /// @param className Qualified or unqualified candidate class name.
    /// @return Key used by classLayouts_.
    [[nodiscard]] std::string canonicalLayoutKey(std::string_view className) const;

  public:
    /// @brief Ensure a symbol entry exists, creating one with defaults if absent.
    /// @param name BASIC symbol name (case-insensitive lookup).
    /// @return Mutable reference to the existing or newly-created SymbolInfo.
    SymbolInfo &ensureSymbol(std::string_view name);

    /// @brief Find a symbol by name.
    /// @param name BASIC symbol name (case-insensitive lookup).
    /// @return Pointer to the SymbolInfo, or nullptr when not found.
    SymbolInfo *findSymbol(std::string_view name);

    /// @brief Find a symbol by name (const overload).
    /// @param name BASIC symbol name (case-insensitive lookup).
    /// @return Pointer to the SymbolInfo, or nullptr when not found.
    const SymbolInfo *findSymbol(std::string_view name) const;

    /// @brief Check whether a name corresponds to a field in the active class scope.
    /// @param name Identifier to check against the current field scope.
    /// @return True when the name matches a field in the active class layout.
    [[nodiscard]] bool isFieldInScope(std::string_view name) const;

    /// @brief Push a new field scope for the given class during method lowering.
    /// @param className Fully-qualified class name whose fields become visible.
    void pushFieldScope(const std::string &className);

    /// @brief Pop the current field scope after completing class method lowering.
    void popFieldScope();

    /// @brief Mark a symbol as referenced during lowering.
    /// @param name BASIC symbol name to mark.
    void markSymbolReferenced(std::string_view name);

    /// @brief Mark a symbol as an array variable.
    /// @param name BASIC symbol name to mark.
    void markArray(std::string_view name);

    /// @brief Mark a symbol as a STATIC procedure-local variable.
    /// @param name BASIC symbol name to mark.
    void markStatic(std::string_view name);

    /// @brief Set the explicit BASIC type for a symbol.
    /// @param name BASIC symbol name to update.
    /// @param type BASIC type to assign.
    void setSymbolType(std::string_view name, AstType type);

  private:
    /// @brief Derive IL slot traits for a symbol.
    /// @param name Symbol name to classify.
    /// @return Slot type and lifetime metadata.
    SlotType getSlotType(std::string_view name) const;

    /// @brief Clear procedure-local symbol and field-scope state.
    void resetSymbolState();

    // --- Module-level object array typing cache (BUG-097) ---
    /// @brief Map of module-level array names to their element class (qualified).
    /// @details Populated from the main-body variable discovery so procedure
    ///          scopes can recover object element types for global arrays even
    ///          after per-procedure symbol resets.
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>
        moduleObjArrayElemClass_;
    /// @brief BUG-107 fix: Cache for module-level scalar object types.
    std::unordered_map<std::string, std::string, StringHash, std::equal_to<>> moduleObjectClass_;
    /// @brief BUG-OOP-011 fix: Set of module-level string array names.
    std::unordered_set<std::string, StringHash, std::equal_to<>> moduleStrArrayNames_;
    /// @brief BUG-BAS-002 fix: Track current procedure parameter names.
    /// @details Parameters should not be resolved via moduleObjectClass_ cache
    ///          to prevent module-level object variables from polluting parameter types.
    std::unordered_set<std::string, StringHash, std::equal_to<>> currentProcParamNames_;

  public:
    /// @brief Register a parameter name for the current procedure.
    /// @details Called during parameter initialization to prevent module-level
    ///          object variable lookups from overriding parameter types.
    /// @param name Non-empty parameter name to record.
    void registerProcParam(std::string_view name) {
        if (!name.empty())
            currentProcParamNames_.emplace(name);
    }

    /// @brief Clear current procedure parameter tracking.
    /// @post isProcParam() returns false for every previously recorded name.
    void clearProcParams() {
        currentProcParamNames_.clear();
    }

    /// @brief Check if a name is a parameter in the current procedure.
    /// @param name Parameter name to query.
    /// @return True when the exact name is present in currentProcParamNames_.
    [[nodiscard]] bool isProcParam(std::string_view name) const {
        return currentProcParamNames_.find(name) != currentProcParamNames_.end();
    }

  public:
    /// @brief Clear cached module-level object array and scalar object typing.
    /// @post Object-array, scalar-object, and string-array caches are empty.
    void clearModuleObjectArrayCache() {
        moduleObjArrayElemClass_.clear();
        moduleObjectClass_.clear();
        moduleStrArrayNames_.clear();
    }

    /// @brief Scan AST for module-level DIM object arrays and cache their types.
    /// @details Called before lowering procedures so object array element classes
    ///          are available for method dispatch resolution in procedure bodies.
    ///          BUG-097 fix: Populate cache from AST rather than symbol table.
    /// @param main Main body statements from the Program AST.
    void cacheModuleObjectArraysFromAST(const std::vector<StmtPtr> &main);

    /// @brief Snapshot object-array typings from current symbol table.
    /// @details Intended to be called after collecting main variables so
    ///          global DIM object arrays are recorded for later procedure use.
    void cacheModuleObjectArraysFromSymbols();

    /// @brief Lookup element class for a module-level array by name.
    /// @param name Module-level array name.
    /// @return Qualified class name when known, empty otherwise.
    [[nodiscard]] std::string lookupModuleArrayElemClass(std::string_view name) const;

    /// @brief BUG-OOP-011 fix: Check if name is a module-level string array.
    /// @param name Module-level array name.
    /// @return True if the name is a cached module-level string array.
    [[nodiscard]] bool isModuleStrArray(std::string_view name) const;

  public:
    /// @brief Lookup a cached procedure signature by BASIC name.
    /// @param name Procedure name or known alias.
    /// @return Pointer to the signature when present, nullptr otherwise.
    const ProcedureSignature *findProcSignature(const std::string &name) const;

    /// @brief Resolve the emitted callee name to a qualified form when known.
    /// @param name Source-level callee name or alias.
    /// @return Emission name from the alias table, or a fallback derived from input.
    [[nodiscard]] std::string resolveCalleeName(const std::string &name) const;

    /// @brief Lookup the AST return type recorded for a class method.
    /// @param className Qualified user-defined class.
    /// @param methodName Method map key.
    /// @return Explicit or suffix-inferred type, otherwise empty.
    std::optional<::il::frontends::basic::Type> findMethodReturnType(
        std::string_view className, std::string_view methodName) const;

    /// @brief Lookup the return class name for a method that returns an object.
    /// @param className Qualified user-defined or runtime class.
    /// @param methodName Method name.
    /// @param arity Optional overload arity.
    /// @return Qualified class name if method returns object, empty string otherwise.
    [[nodiscard]] std::string findMethodReturnClassName(
        std::string_view className,
        std::string_view methodName,
        std::optional<std::size_t> arity = std::nullopt) const;

    /// @brief Lookup the AST type for a class field.
    /// @param className Qualified user-defined class.
    /// @param fieldName Field name.
    /// @return Field type, otherwise empty.
    std::optional<::il::frontends::basic::Type> findFieldType(std::string_view className,
                                                              std::string_view fieldName) const;

    /// @brief Check if a class field is an array.
    /// @param className Qualified user-defined class.
    /// @param fieldName Field name.
    /// @return Array flag, or false when unresolved.
    [[nodiscard]] bool isFieldArray(std::string_view className, std::string_view fieldName) const;

    /// @brief Access the mutable procedure-level lowering context.
    /// @return Reference to the active ProcedureContext.
    [[nodiscard]] ProcedureContext &context() noexcept;

    /// @brief Access the immutable procedure-level lowering context.
    /// @return Const reference to the active ProcedureContext.
    [[nodiscard]] const ProcedureContext &context() const noexcept;

    // Class access control context -----------------------------------------------------
    /// @brief Push the fully-qualified class name currently being lowered.
    /// @param qname Qualified class name to append.
    void pushClass(const std::string &qname) {
        classStack_.push_back(qname);
    }

    /// @brief Pop the current class lowering context.
    /// @details An empty stack is left unchanged.
    void popClass() {
        if (!classStack_.empty())
            classStack_.pop_back();
    }

    /// @brief Return the active class lowering context, or empty when not in a class.
    /// @return Copy of the top qualified class name, or an empty string.
    [[nodiscard]] std::string currentClass() const {
        return classStack_.empty() ? std::string() : classStack_.back();
    }
};

} // namespace il::frontends::basic

#include "frontends/basic/LoweringPipeline.hpp"
