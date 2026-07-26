//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
// File: src/frontends/basic/lower/Scan_RuntimeNeeds.cpp
// Purpose: Tracks runtime feature requirements during BASIC scan passes.
// Key invariants: Traversal requests helpers and bookkeeping only; no IR
//                 emission occurs.
// Ownership/Lifetime: Operates on Lowerer state without owning AST or module.
// Links: docs/internals/codemap.md, docs/tutorials/basic-tutorial.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the pre-emission scan that records BASIC runtime
///        dependencies and ownership helpers.

#include "frontends/basic/ASTUtils.hpp"
#include "frontends/basic/AstWalker.hpp"
#include "frontends/basic/BasicSymbolQuery.hpp"
#include "frontends/basic/BuiltinRegistry.hpp"
#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/ProcedureSymbolTracker.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/TypeRules.hpp"
#include "frontends/basic/TypeSuffix.hpp"
#include "frontends/basic/ast/DeclNodes.hpp"
#include "frontends/basic/ast/StmtNodes.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace il::frontends::basic::lower {
namespace detail {

/// @brief AST walker that records runtime helper requirements for BASIC code.
///
/// @details The scanner traverses expressions and statements, invoking helper
///          routines on @ref Lowerer to record required runtime bridges,
///          builtin helpers, and symbol bookkeeping.  It performs no IR
///          emission, making it safe to run during the scan phase.
class RuntimeNeedsScanner final : public BasicAstWalker<RuntimeNeedsScanner> {
  public:
    /// @brief Construct a scanner bound to the lowering context.
    ///
    /// @param lowerer Lowerer used to accumulate runtime requirements.
    explicit RuntimeNeedsScanner(Lowerer &lowerer) noexcept
        : lowerer_(lowerer), tracker_(lowerer, /*trackCrossProc=*/false), query_(lowerer) {}

    /// @brief Analyse a single statement and record runtime requirements.
    ///
    /// @param stmt Statement node to visit.
    void evaluateStmt(const Stmt &stmt) {
        stmt.accept(*this);
    }

    /// @brief Analyse an entire program, scanning declarations and main body.
    ///
    /// @param prog BASIC program containing procedure declarations and main.
    void evaluateProgram(const Program &prog) {
        for (const auto &decl : prog.procs)
            if (decl)
                decl->accept(*this);
        for (const auto &stmt : prog.main)
            if (stmt)
                stmt->accept(*this);
    }

    /// @brief Defer builtin call traversal to bespoke runtime logic.
    /// The borrowed call is processed manually by @ref after.
    /// @return False to suppress automatic child traversal.
    bool shouldVisitChildren(const BuiltinCallExpr &) {
        return false;
    }

    /// @brief Skip procedure call traversal; arguments are processed manually.
    /// The borrowed call is processed manually by @ref after.
    /// @return False to suppress automatic child traversal.
    bool shouldVisitChildren(const CallExpr &) {
        return false;
    }

    /// @brief Skip constructor arguments; runtime tracking occurs explicitly.
    /// The borrowed construction expression is processed manually by @ref after.
    /// @return False to suppress automatic child traversal.
    bool shouldVisitChildren(const NewExpr &) {
        return false;
    }

    /// @brief Skip member access traversal; base expressions handled explicitly.
    /// The borrowed access expression is processed manually by @ref after.
    /// @return False to suppress automatic child traversal.
    bool shouldVisitChildren(const MemberAccessExpr &) {
        return false;
    }

    /// @brief Skip method call traversal; helper handles base and args.
    /// The borrowed call is processed manually by @ref after.
    /// @return False to suppress automatic child traversal.
    bool shouldVisitChildren(const MethodCallExpr &) {
        return false;
    }

    // Expression hooks --------------------------------------------------
    /// @brief Track runtime helpers required for array element access.
    ///
    /// @param expr Array expression encountered in rvalue position.
    void after(const ArrayExpr &expr) {
        if (lvalueDepth_ > 0)
            return;
        tracker_.trackArray(expr.name);

        // Determine array element type and require appropriate runtime functions
        auto elemType = query_.getArrayElementType(expr.name);
        if (elemType && *elemType == Type::Str) {
            // String array
            lowerer_.requireArrayStrLen();
            lowerer_.requireArrayStrGet();
        } else {
            // Integer/numeric array
            lowerer_.requireArrayI64Len();
            lowerer_.requireArrayI64Get();
        }
        lowerer_.requireArrayOobPanic();
    }

    /// @brief Mark array usage and ensure LBOUND helpers are tracked.
    ///
    /// @param expr LBOUND expression referencing an array.
    void after(const LBoundExpr &expr) {
        tracker_.trackArray(expr.name);
    }

    /// @brief Mark array usage and request runtime support for UBOUND.
    ///
    /// @param expr UBOUND expression referencing an array.
    void after(const UBoundExpr &expr) {
        tracker_.trackArray(expr.name);
        lowerer_.requireArrayI64Len();
    }

    /// @brief Delegate binary expression analysis to helper logic.
    ///
    /// @param expr Binary expression whose operands may require helpers.
    void after(const BinaryExpr &expr) {
        applyBinaryRuntimeNeeds(expr);
    }

    /// @brief Record runtime needs for builtin calls and consume arguments.
    ///
    /// @param expr Builtin call expression.
    void after(const BuiltinCallExpr &expr) {
        std::vector<std::optional<Lowerer::ExprType>> argTypes(expr.args.size());
        for (std::size_t i = 0; i < expr.args.size(); ++i) {
            const auto &arg = expr.args[i];
            if (!arg)
                continue;
            argTypes[i] = lowerer_.scanExpr(*arg);
            consumeExpr(*arg);
        }
        applyBuiltinRuntimeNeeds(expr, argTypes);
    }

    /// @brief Consume procedure call arguments to avoid leaking temporaries.
    ///
    /// @param expr Procedure call expression.
    void after(const CallExpr &expr) {
        for (const auto &arg : expr.args) {
            if (!arg)
                continue;
            consumeExpr(*arg);
        }
    }

    /// @brief Consume constructor arguments to track nested runtime needs.
    ///
    /// @param expr New-expression node whose args may require helpers.
    void after(const NewExpr &expr) {
        for (const auto &arg : expr.args) {
            if (!arg)
                continue;
            consumeExpr(*arg);
        }
    }

    /// @brief Consume the base expression of member access to maintain balance.
    ///
    /// @param expr Member access expression encountered during scanning.
    void after(const MemberAccessExpr &expr) {
        if (expr.base)
            consumeExpr(*expr.base);
    }

    /// @brief Consume method call base and arguments to surface nested helpers.
    ///
    /// @param expr Method call expression.
    void after(const MethodCallExpr &expr) {
        if (expr.base)
            consumeExpr(*expr.base);
        for (const auto &arg : expr.args) {
            if (!arg)
                continue;
            consumeExpr(*arg);
        }
    }

    // Statement hooks ---------------------------------------------------

    /// @brief Ensure PRINT# statements request channel-aware runtime helpers.
    /// @param stmt Channel output statement whose mode, arity, and newline
    ///        behavior select error-reporting helpers.
    void before(const PrintChStmt &stmt) {
        std::size_t actualArgs = 0;
        for (const auto &arg : stmt.args) {
            if (arg)
                ++actualArgs;
        }

        if (stmt.mode == PrintChStmt::Mode::Write) {
            lowerer_.requirePrintlnChErr();
            return;
        }

        if (stmt.trailingNewline)
            lowerer_.requirePrintlnChErr();

        if (actualArgs > 0 && (!stmt.trailingNewline || actualArgs > 1))
            lowerer_.requireWriteChErr();
    }

    /// @brief Track string comparison helpers required by SELECT CASE arms.
    /// @param stmt SELECT CASE statement whose arm labels are inspected.
    void after(const SelectCaseStmt &stmt) {
        for (const auto &arm : stmt.arms) {
            if (!arm.str_labels.empty()) {
                lowerer_.requestHelper(Lowerer::RuntimeFeature::StrEq);
                break;
            }
        }
    }

    /// @brief Analyse PRINT# arguments and request supporting helpers.
    ///
    /// @param stmt PRINT# statement containing arguments and mode.
    void after(const PrintChStmt &stmt) {
        for (const auto &arg : stmt.args) {
            if (!arg)
                continue;
            auto ty = lowerer_.scanExpr(*arg);
            handlePrintChArg(*arg, ty, stmt.mode);
        }
        if (stmt.mode == PrintChStmt::Mode::Write && stmt.args.size() > 1)
            lowerer_.requestHelper(Lowerer::RuntimeFeature::Concat);
    }

    /// @brief Analyse PRINT arguments (non-channel variant) for runtime helpers.
    ///
    /// @param stmt PRINT statement containing arguments.
    void after(const PrintStmt &stmt) {
        for (const auto &item : stmt.items) {
            if (item.kind != PrintItem::Kind::Expr || !item.expr)
                continue;
            auto ty = lowerer_.scanExpr(*item.expr);

            if (ty == Lowerer::ExprType::Str) {
                // Check if this is an lvalue (variable, array element, or member access)
                // If so, we'll need retain/release helpers during lowering
                const bool isLvalue = as<const VarExpr>(*item.expr) ||
                                      as<const ArrayExpr>(*item.expr) ||
                                      as<const MemberAccessExpr>(*item.expr);
                if (isLvalue) {
                    lowerer_.requireStrRetainMaybe();
                    lowerer_.requireStrReleaseMaybe();
                }
            }
            // Note: Numeric values in regular PRINT are printed directly via
            // rt_print_i64/rt_print_f64, not converted to strings, so we don't
            // need to request string conversion helpers here.
        }
    }

    /// @brief GOSUB needs trap handling when used with error recovery.
    void before(const GosubStmt &) {
        lowerer_.requireTrap();
    }

    /// @brief RETURN interacts with trap-based unwinding.
    void before(const ReturnStmt &) {
        lowerer_.requireTrap();
    }

    /// @brief CLS requires terminal helper support.
    void after(const ClsStmt &) {
        lowerer_.requestHelper(il::runtime::RuntimeFeature::TermCls);
    }

    /// @brief COLOR requires terminal colour helper support.
    void after(const ColorStmt &) {
        lowerer_.requestHelper(il::runtime::RuntimeFeature::TermColor);
    }

    /// @brief LOCATE requires terminal cursor helper support.
    void after(const LocateStmt &) {
        lowerer_.requestHelper(il::runtime::RuntimeFeature::TermLocate);
    }

    /// @brief CURSOR requires terminal cursor visibility helper support.
    void after(const CursorStmt &) {
        lowerer_.requestHelper(il::runtime::RuntimeFeature::TermCursor);
    }

    /// @brief ALTSCREEN requires terminal alternate screen buffer helper support.
    void after(const AltScreenStmt &) {
        lowerer_.requestHelper(il::runtime::RuntimeFeature::TermAltScreen);
    }

    /// @brief SLEEP requires the time helper to be declared.
    void after(const SleepStmt &) {
        lowerer_.requireSleepMs();
    }

    /// @brief Track runtime needs stemming from LET targets and values.
    ///
    /// @param stmt LET statement being analysed.
    void after(const LetStmt &stmt) {
        if (!stmt.target)
            return;
        if (auto *var = as<const VarExpr>(*stmt.target.get()))
            handleLetVarTarget(*var, stmt.expr.get());
        else if (auto *arr = as<const ArrayExpr>(*stmt.target.get()))
            handleLetArrayTarget(*arr);
        else if (auto *member = as<const MemberAccessExpr>(*stmt.target))
            handleLetMemberTarget(*member);
    }

    /// @brief Prime symbol tracking and runtime helpers for DIM statements.
    ///
    /// @param stmt DIM statement describing variable storage.
    void before(const DimStmt &stmt) {
        if (stmt.name.empty())
            return;

        // BUG-107 fix: Handle object type declarations (DIM frog AS Frog)
        if (!stmt.explicitClassQname.empty()) {
            std::string className;
            for (size_t i = 0; i < stmt.explicitClassQname.size(); ++i) {
                if (i > 0)
                    className += ".";
                className += stmt.explicitClassQname[i];
            }
            lowerer_.setSymbolObjectType(stmt.name,
                                         lowerer_.resolveQualifiedClassCasing(className));
        } else {
            lowerer_.setSymbolType(stmt.name, stmt.type);
        }

        // Use tracker for unified symbol tracking
        tracker_.track(stmt.name, stmt.isArray);

        if (stmt.isArray) {
            if (stmt.type == Type::Str) {
                // String array
                lowerer_.requireArrayStrAlloc();
                lowerer_.requireArrayStrRelease();
            } else {
                // Integer/numeric array
                lowerer_.requireArrayI64New();
                lowerer_.requireArrayI64Retain();
                lowerer_.requireArrayI64Release();
            }
        }
    }

    /// @brief Track helpers needed for REDIM resizing operations.
    ///
    /// @param stmt REDIM statement with array metadata.
    void before(const ReDimStmt &stmt) {
        if (stmt.name.empty())
            return;
        tracker_.trackArray(stmt.name);
        lowerer_.requireArrayI64Resize();
        lowerer_.requireArrayI64Retain();
        lowerer_.requireArrayI64Release();
    }

    /// @brief Track helpers needed for CONST string declarations.
    ///
    /// @param stmt CONST statement with initializer.
    void before(const ConstStmt &stmt) {
        if (stmt.name.empty())
            return;
        lowerer_.setSymbolType(stmt.name, stmt.type);
        tracker_.trackScalar(stmt.name);
        if (stmt.type == Type::Str) {
            lowerer_.requireStrRetainMaybe();
            lowerer_.requireStrReleaseMaybe();
        }
    }

    /// @brief RANDOMIZE requires the random number helper.
    void before(const RandomizeStmt &) {
        lowerer_.trackRuntime(Lowerer::RuntimeFeature::RandomizeI64);
    }

    /// @brief Ensure FOR loop variables have inferred types for runtime helpers.
    ///
    /// @param stmt FOR loop under analysis.
    void before(const ForStmt &stmt) {
        // BUG-081 fix: Extract variable name from expression
        if (stmt.varExpr) {
            if (auto *varExpr = as<VarExpr>(*stmt.varExpr)) {
                const auto *info = lowerer_.findSymbol(varExpr->name);
                if (!info || !info->hasType)
                    lowerer_.setSymbolType(varExpr->name, inferVariableType(varExpr->name));
            }
            // For complex expressions (member access, array), type inference happens
            // during expression lowering
        }
    }

    /// @brief OPEN requires file runtime error helpers.
    void before(const OpenStmt &) {
        lowerer_.requireOpenErrVstr();
    }

    /// @brief CLOSE requires runtime helpers for closing file handles.
    void before(const CloseStmt &) {
        lowerer_.requireCloseErr();
    }

    /// @brief SEEK needs runtime helpers for repositioning file channels.
    void before(const SeekStmt &) {
        lowerer_.requireSeekChErr();
    }

    /// @brief Prepare runtime helpers and bookkeeping before INPUT executes.
    ///
    /// @param stmt INPUT statement referencing destination variables.
    void before(const InputStmt &stmt) {
        // Note: Zanna.Terminal.ReadLine is declared via trackCalleeName when called.
        // No need to request RuntimeFeature::InputLine since that was for the legacy
        // rt_input_line helper which is no longer used by BASIC frontend.
        if (stmt.vars.size() > 1) {
            lowerer_.requestHelper(Lowerer::RuntimeFeature::SplitFields);
            lowerer_.requireStrReleaseMaybe();
        }
        inputVarNames_ = stmt.vars;
    }

    /// @brief Post-process INPUT to request conversions for destination types.
    void after(const InputStmt &) {
        for (const auto &name : inputVarNames_) {
            if (name.empty())
                continue;
            Type astTy = inferVariableType(name);
            switch (astTy) {
                case Type::Str:
                    break;
                case Type::F64:
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::ToDouble);
                    lowerer_.requireStrReleaseMaybe();
                    break;
                default:
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::ToInt);
                    lowerer_.requireStrReleaseMaybe();
                    break;
            }

            const auto *info = lowerer_.findSymbol(name);
            if (!info || !info->hasType)
                lowerer_.setSymbolType(name, astTy);
        }
        inputVarNames_.clear();
    }

    /// @brief Prepare helpers for INPUT# channel reads.
    void before(const InputChStmt &) {
        lowerer_.requireLineInputChErr();
        lowerer_.requestHelper(Lowerer::RuntimeFeature::SplitFields);
        lowerer_.requireStrReleaseMaybe();
    }

    /// @brief Request conversions required after INPUT# completes.
    ///
    /// @param stmt INPUT# statement describing the destination variable.
    void after(const InputChStmt &stmt) {
        for (const auto &ref : stmt.targets) {
            const auto &name = ref.name;
            if (name.empty())
                continue;
            Type astTy = inferVariableType(name);
            switch (astTy) {
                case Type::Str:
                    break;
                case Type::F64:
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::ParseDouble);
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::Val);
                    break;
                default:
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::ParseInt64);
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::Val);
                    break;
            }
            const auto *info = lowerer_.findSymbol(name);
            if (!info || !info->hasType)
                lowerer_.setSymbolType(name, astTy);
        }
    }

    /// @brief Ensure LINE INPUT# requests error-reporting helpers.
    void before(const LineInputChStmt &) {
        lowerer_.requireLineInputChErr();
    }

    /// @brief Register SUB parameter types before scanning its body.
    /// @details Procedure parameters with object types (e.g., `p AS MyClass`) must
    ///          be registered in the symbol table before the body is scanned so that
    ///          member access expressions like `p.field` can resolve the class type.
    /// @param decl SUB declaration whose ordered parameters seed symbol metadata.
    void before(const SubDecl &decl) {
        for (const auto &p : decl.params) {
            if (!p.objectClass.empty()) {
                lowerer_.setSymbolObjectType(p.name,
                                             lowerer_.resolveQualifiedClassCasing(p.objectClass));
            } else {
                lowerer_.setSymbolType(p.name, p.type);
            }
        }
    }

    /// @brief Register FUNCTION parameter types before scanning its body.
    /// @details Ensures object and scalar parameter types are available to
    ///          nested expression and member-resolution scans.
    /// @param decl FUNCTION declaration whose ordered parameters seed symbols.
    void before(const FunctionDecl &decl) {
        for (const auto &p : decl.params) {
            if (!p.objectClass.empty()) {
                lowerer_.setSymbolObjectType(p.name,
                                             lowerer_.resolveQualifiedClassCasing(p.objectClass));
            } else {
                lowerer_.setSymbolType(p.name, p.type);
            }
        }
    }

  private:
    /// @brief Evaluate an expression solely to surface nested runtime needs.
    ///
    /// @param expr Expression subtree to visit.
    void consumeExpr(const Expr &expr) {
        expr.accept(*this);
    }

    /// @brief Record helpers required by binary operators such as POW or string addition.
    ///
    /// @param expr Binary expression currently being analysed.
    void applyBinaryRuntimeNeeds(const BinaryExpr &expr) {
        using Op = BinaryExpr::Op;
        if (expr.op == Op::Pow) {
            lowerer_.trackRuntime(Lowerer::RuntimeFeature::Pow);
            return;
        }
        if (expr.op == Op::Add) {
            auto lhsType = expr.lhs ? lowerer_.scanExpr(*expr.lhs) : Lowerer::ExprType::I64;
            auto rhsType = expr.rhs ? lowerer_.scanExpr(*expr.rhs) : Lowerer::ExprType::I64;
            if (lhsType == Lowerer::ExprType::Str && rhsType == Lowerer::ExprType::Str)
                lowerer_.requestHelper(Lowerer::RuntimeFeature::Concat);
            return;
        }
        if (expr.op == Op::Eq || expr.op == Op::Ne || expr.op == Op::Lt || expr.op == Op::Le ||
            expr.op == Op::Gt || expr.op == Op::Ge) {
            auto lhsType = expr.lhs ? lowerer_.scanExpr(*expr.lhs) : Lowerer::ExprType::I64;
            auto rhsType = expr.rhs ? lowerer_.scanExpr(*expr.rhs) : Lowerer::ExprType::I64;
            if (lhsType == Lowerer::ExprType::Str || rhsType == Lowerer::ExprType::Str) {
                // Request appropriate string comparison helper based on operator
                if (expr.op == Op::Eq || expr.op == Op::Ne)
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::StrEq);
                else if (expr.op == Op::Lt)
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::StrLt);
                else if (expr.op == Op::Le)
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::StrLe);
                else if (expr.op == Op::Gt)
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::StrGt);
                else if (expr.op == Op::Ge)
                    lowerer_.requestHelper(Lowerer::RuntimeFeature::StrGe);
            }
        }
    }

    /// @brief Apply builtin-specific runtime tracking based on scan rules.
    ///
    /// @param expr Builtin expression being analysed.
    /// @param argTypes Cached argument classifications gathered earlier.
    void applyBuiltinRuntimeNeeds(const BuiltinCallExpr &expr,
                                  const std::vector<std::optional<Lowerer::ExprType>> &argTypes) {
        if (expr.builtin == BuiltinCallExpr::Builtin::Str && !expr.args.empty() && expr.args[0])
            lowerer_.requestHelper(
                strFeatureForNumeric(lowerer_.classifyNumericType(*expr.args[0])));

        const auto &rule = getBuiltinScanRule(expr.builtin);
        /// @brief Tests whether a builtin argument slot is present.
        /// @param idx Argument index to query.
        /// @return `true` when the index exists and contains an expression.
        auto hasArg = [&](std::size_t idx) {
            return idx < expr.args.size() && expr.args[idx] != nullptr;
        };
        /// @brief Retrieves a cached builtin argument classification.
        /// @param idx Argument index to query.
        /// @return Cached type, or `std::nullopt` when unavailable.
        auto argType = [&](std::size_t idx) -> std::optional<Lowerer::ExprType> {
            return idx < argTypes.size() ? argTypes[idx] : std::nullopt;
        };

        using Feature = BuiltinScanRule::Feature;
        for (const auto &feature : rule.features) {
            bool apply = false;
            switch (feature.condition) {
                case Feature::Condition::Always:
                    apply = true;
                    break;
                case Feature::Condition::IfArgPresent:
                    apply = hasArg(feature.argIndex);
                    break;
                case Feature::Condition::IfArgMissing:
                    apply = !hasArg(feature.argIndex);
                    break;
                case Feature::Condition::IfArgTypeIs: {
                    auto ty = argType(feature.argIndex);
                    apply = ty && *ty == feature.type;
                    break;
                }
                case Feature::Condition::IfArgTypeIsNot: {
                    auto ty = argType(feature.argIndex);
                    apply = ty && *ty != feature.type;
                    break;
                }
            }

            if (!apply)
                continue;

            switch (feature.action) {
                case Feature::Action::Request:
                    lowerer_.requestHelper(feature.feature);
                    break;
                case Feature::Action::Track:
                    lowerer_.trackRuntime(feature.feature);
                    break;
            }
        }

        // --- begin: ensure manual helpers for file-position builtins ---
        switch (expr.builtin) {
            case BuiltinCallExpr::Builtin::Eof:
                lowerer_.requireEofCh();
                break;
            case BuiltinCallExpr::Builtin::Lof:
                lowerer_.requireLofCh();
                break;
            case BuiltinCallExpr::Builtin::Loc:
                lowerer_.requireLocCh();
                break;
            case BuiltinCallExpr::Builtin::Timer:
                lowerer_.requireTimerMs();
                break;
            default:
                break;
        }
        // --- end: ensure manual helpers for file-position builtins ---
    }

    /// @brief Request helpers needed to print an expression via PRINT#.
    ///
    /// @param expr Expression being printed.
    /// @param ty Expression classification from scan-time inference.
    /// @param mode PRINT# mode (WRITE vs PRINT) guiding formatting.
    void handlePrintChArg(const Expr &expr, Lowerer::ExprType ty, PrintChStmt::Mode mode) {
        if (ty == Lowerer::ExprType::Str) {
            if (mode == PrintChStmt::Mode::Write)
                lowerer_.requestHelper(Lowerer::RuntimeFeature::CsvQuote);

            // Check if this is an lvalue (variable, array element, or member access)
            // If so, we'll need retain/release helpers during lowering
            const bool isLvalue = as<const VarExpr>(expr) || as<const ArrayExpr>(expr) ||
                                  as<const MemberAccessExpr>(expr);
            if (isLvalue) {
                lowerer_.requireStrRetainMaybe();
                lowerer_.requireStrReleaseMaybe();
            }
            return;
        }
        lowerer_.requestHelper(strFeatureForNumeric(lowerer_.classifyNumericType(expr)));
    }

    /// @brief Map numeric categories to string conversion helper requirements.
    ///
    /// @param type Numeric classification determined by TypeRules.
    /// @return Runtime helper that implements the corresponding conversion.
    static Lowerer::RuntimeFeature strFeatureForNumeric(TypeRules::NumericType type) {
        using Feature = Lowerer::RuntimeFeature;
        static constexpr std::array<Feature, 4> kMap{
            Feature::StrFromI16,
            Feature::StrFromI32,
            Feature::StrFromSingle,
            Feature::StrFromDouble,
        };
        auto idx = static_cast<std::size_t>(type);
        return idx < kMap.size() ? kMap[idx] : Feature::StrFromDouble;
    }

    /// @brief Convert semantic analyzer type to AST type.
    ///
    /// @param semaType Semantic analyzer type enum.
    /// @return Corresponding AST type, or std::nullopt if not convertible.
    std::optional<Type> convertSemanticType(SemanticAnalyzer::Type semaType) {
        using SemaType = SemanticAnalyzer::Type;
        switch (semaType) {
            case SemaType::Int:
                return Type::I64;
            case SemaType::Float:
                return Type::F64;
            case SemaType::String:
                return Type::Str;
            case SemaType::Bool:
                return Type::Bool;
            default:
                return std::nullopt;
        }
    }

    /// @brief Infer variable type from semantic analyzer, then suffix, then fallback.
    ///
    /// @param name Variable name to query.
    /// @return Best-effort type derived from semantic analysis or naming convention.
    Type inferVariableType(const std::string &name) {
        // BUG-001 FIX: Query semantic analyzer via facade for value-based type inference
        if (auto inferredType = query_.lookupInferredType(name))
            return *inferredType;
        // Fall back to suffix-based inference
        return inferAstTypeFromName(name);
    }

    /// @brief Ensure a symbol has a known AST type, defaulting to @p fallback.
    ///
    /// @param name Symbol to update.
    /// @param fallback Type inferred from naming conventions when unresolved.
    void ensureSymbolType(const std::string &name, Type fallback) {
        if (!query_.hasExplicitType(name))
            lowerer_.setSymbolType(name, fallback);
    }

    /// @brief Record runtime needs when LET assigns to a variable target.
    ///
    /// @param var Target variable expression.
    /// @param value Optional value expression driving helper requirements.
    void handleLetVarTarget(const VarExpr &var, const Expr *value) {
        if (var.name.empty())
            return;
        bool targetIsObject = false;
        if (value) {
            std::string className;
            if (const auto *alloc = as<const NewExpr>(*value))
                className = alloc->className;
            else
                className = lowerer_.resolveObjectClass(*value);
            if (!className.empty()) {
                lowerer_.setSymbolObjectType(var.name, className);
                targetIsObject = true;
            }
        }
        if (!targetIsObject) {
            // Use facade for object check
            targetIsObject = query_.isSymbolObject(var.name);
        }
        if (targetIsObject) {
            using Feature = il::runtime::RuntimeFeature;
            lowerer_.requestRuntimeFeature(Feature::ObjRetainMaybe);
            lowerer_.requestRuntimeFeature(Feature::ObjReleaseChk0);
            lowerer_.requestRuntimeFeature(Feature::ObjFree);
        }
        ensureSymbolType(var.name, inferVariableType(var.name));
        // Use facade for array check
        if (query_.isSymbolArray(var.name)) {
            if (!query_.getObjectArrayElementClass(var.name).empty()) {
                lowerer_.requireArrayObjRelease();
            } else if (auto symType = query_.getSymbolType(var.name);
                       symType && *symType == Type::Str) {
                lowerer_.requireArrayStrRelease();
            } else {
                lowerer_.requireArrayI64Retain();
                lowerer_.requireArrayI64Release();
            }
            return;
        }
        // Use facade for type lookup
        auto symType = query_.getSymbolType(var.name);
        if ((symType.value_or(inferVariableType(var.name))) == Type::Str) {
            lowerer_.requireStrRetainMaybe();
            lowerer_.requireStrReleaseMaybe();
        }
    }

    /// @brief Record runtime needs when LET assigns into an array element.
    ///
    /// @param arr Array expression describing the target element.
    void handleLetArrayTarget(const ArrayExpr &arr) {
        if (arr.name.empty())
            return;
        ensureSymbolType(arr.name, inferVariableType(arr.name));
        tracker_.trackArray(arr.name);

        // Determine array element type and require appropriate runtime functions
        auto elemType = query_.getArrayElementType(arr.name);
        if (!query_.getObjectArrayElementClass(arr.name).empty()) {
            lowerer_.requireArrayObjLen();
            lowerer_.requireArrayObjPut();
        } else if (elemType && *elemType == Type::Str) {
            // String array
            lowerer_.requireArrayStrLen();
            lowerer_.requireArrayStrPut();
            lowerer_.requireStrRetainMaybe();
        } else {
            // Integer/numeric array
            lowerer_.requireArrayI64Len();
            lowerer_.requireArrayI64Set();
        }
        lowerer_.requireArrayOobPanic();
    }

    /// @brief Records ownership helpers for assignment to a member field.
    /// @param access Member target whose resolved field type is inspected.
    void handleLetMemberTarget(const MemberAccessExpr &access) {
        if (!access.base)
            return;

        std::string className = lowerer_.resolveObjectClass(*access.base);
        if (className.empty())
            return;

        auto layoutIt = lowerer_.classLayouts_.find(className);
        if (layoutIt == lowerer_.classLayouts_.end())
            return;

        const auto *field = layoutIt->second.findField(access.member);
        if (!field)
            return;

        if (field->type == Type::Str) {
            lowerer_.requireStrRetainMaybe();
            lowerer_.requireStrReleaseMaybe();
        }
    }

    /// @brief Track when the scanner descends into the lvalue side of LET.
    ///
    /// @param stmt LET statement currently being visited.
    /// @param child Child expression about to be visited.
    void beforeChild(const LetStmt &stmt, const Expr &child) {
        if (stmt.target && stmt.target.get() == &child)
            ++lvalueDepth_;
    }

    /// @brief Restore lvalue tracking after visiting a LET target.
    ///
    /// @param stmt LET statement currently being visited.
    /// @param child Child expression that was visited.
    void afterChild(const LetStmt &stmt, const Expr &child) {
        if (stmt.target && stmt.target.get() == &child)
            --lvalueDepth_;
    }

    /// @brief Ensure LINE INPUT# target variables receive string types.
    ///
    /// @param stmt LINE INPUT# statement under analysis.
    /// @param child Expression about to be visited.
    void beforeChild(const LineInputChStmt &stmt, const Expr &child) {
        if (!(stmt.targetVar && stmt.targetVar.get() == &child))
            return;
        if (auto *var = as<const VarExpr>(*stmt.targetVar.get()); var && !var->name.empty())
            lowerer_.setSymbolType(var->name, Type::Str);
    }

    /// Borrowed lowerer receiving symbol and runtime requirement updates.
    Lowerer &lowerer_;
    /// Procedure-aware facade used to record referenced scalars and arrays.
    ProcedureSymbolTracker tracker_;
    /// Read-only symbol query facade used for type and ownership decisions.
    BasicSymbolQuery query_;
    /// INPUT destination names retained between before/after hooks.
    std::vector<std::string> inputVarNames_{};
    /// Positive while traversing assignment targets, suppressing rvalue needs.
    int lvalueDepth_{0};
};

} // namespace detail

/// @brief Analyse a single statement to record runtime helper requirements.
///
/// @param lowerer Lowerer receiving runtime requirement updates.
/// @param stmt Statement to analyse.
void scanStmtRuntimeNeeds(Lowerer &lowerer, const Stmt &stmt) {
    detail::RuntimeNeedsScanner(lowerer).evaluateStmt(stmt);
}

/// @brief Analyse an entire BASIC program to record runtime helper requirements.
///
/// @param lowerer Lowerer receiving runtime requirement updates.
/// @param prog Program containing declarations and main body statements.
void scanProgramRuntimeNeeds(Lowerer &lowerer, const Program &prog) {
    detail::RuntimeNeedsScanner(lowerer).evaluateProgram(prog);
}

} // namespace il::frontends::basic::lower
