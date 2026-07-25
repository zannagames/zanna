//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/BuiltinRegistry.hpp
// Purpose: Central registry mapping BASIC built-in function names to their
//          semantic-analysis and IL-lowering hooks (SIN, LEFT$, CINT, etc.).
// Key invariants: Descriptor table order matches BuiltinCallExpr::Builtin for
//                 O(1) lookup; the separate handler registry is mutable.
// Ownership/Lifetime: Descriptor metadata has static lifetime. Rule vectors and
//                     lookup/handler maps own any dynamic storage they allocate.
// Links: src/frontends/basic/BuiltinRegistry.cpp,
//        src/frontends/basic/builtin_registry.inc
//
//===----------------------------------------------------------------------===//

/**
 * @file BuiltinRegistry.hpp
 * @brief Declares BASIC builtin metadata, semantic rules, and lowering rules.
 *
 * The dense static tables share one BuiltinCallExpr::Builtin index space.
 * A separate process-wide handler map supports exact-name dynamic overrides.
 */

#pragma once

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/SemanticAnalyzer.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"
#include "zanna/il/Module.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace il::frontends::basic {
namespace lower {
class BuiltinLowerContext;
} // namespace lower

/// @brief Declarative scan rule describing runtime requirements for a builtin.
/// @details Scanning first visits arguments selected by @ref traversal, derives
///          a result type through @ref result, then evaluates @ref features in
///          order against argument presence and inferred types.
struct BuiltinScanRule {
    /// @brief How the builtin's result type should be computed.
    struct ResultSpec {
        /// @brief Result strategy used during scanning.
        enum class Kind {
            Fixed,   ///< Always yields the @ref type supplied below.
            FromArg, ///< Mirrors the type inferred for a specific argument.
        } kind{Kind::Fixed};

        Lowerer::ExprType type{Lowerer::ExprType::I64}; ///< Fixed type or fallback.
        std::size_t argIndex{0};                        ///< Argument index for FromArg.
    } result{};

    /// @brief How scanning should traverse the builtin's argument list.
    enum class ArgTraversal {
        All,      ///< Visit every provided argument in order.
        Explicit, ///< Visit only the indexes listed in @ref explicitArgs.
    } traversal{ArgTraversal::All};

    /// @brief Specific argument indexes to traverse when @ref traversal is Explicit.
    std::vector<std::size_t> explicitArgs{};

    /// @brief Runtime feature toggles evaluated after scanning arguments.
    struct Feature {
        /// @brief Which Lowerer helper should be invoked for the feature.
        enum class Action {
            Request, ///< Call Lowerer::requestHelper.
            Track,   ///< Call Lowerer::trackRuntime.
        } action{Action::Request};

        /// @brief Conditional guard controlling whether the feature fires.
        enum class Condition {
            Always,         ///< Unconditionally perform the action.
            IfArgPresent,   ///< Fire when the argument at @ref argIndex exists.
            IfArgMissing,   ///< Fire when the argument at @ref argIndex is absent.
            IfArgTypeIs,    ///< Fire when argument type matches @ref type.
            IfArgTypeIsNot, ///< Fire when argument type differs from @ref type.
        } condition{Condition::Always};

        il::runtime::RuntimeFeature feature{
            il::runtime::RuntimeFeature::Count};        ///< Runtime feature to toggle.
        std::size_t argIndex{0};                        ///< Argument inspected by the condition.
        Lowerer::ExprType type{Lowerer::ExprType::I64}; ///< Type operand for type-guarded rules.
    };

    std::vector<Feature> features{}; ///< Ordered feature actions evaluated after argument scans.
};

/// @brief Fetch the declarative scan rule for a builtin.
/// @param b Dense builtin enumerator.
/// @return Reference to static-lifetime scan metadata.
/// @pre @p b is a valid BuiltinCallExpr::Builtin enumerator.
const BuiltinScanRule &getBuiltinScanRule(BuiltinCallExpr::Builtin b);

/// @brief Declarative lowering rule describing runtime invocation for a builtin.
/// @details Lowering derives a result type, chooses the first matching variant,
///          materializes its ordered arguments and defaults, applies transforms,
///          performs feature actions, and emits the selected operation.
struct BuiltinLoweringRule {
    /// @brief Result strategy mirroring @ref BuiltinScanRule::ResultSpec.
    struct ResultSpec {
        /// @brief How the result type is determined at lowering time.
        enum class Kind {
            Fixed,   ///< Always use the supplied @ref type.
            FromArg, ///< Mirror the type of an argument index.
        } kind{Kind::Fixed};

        Lowerer::ExprType type{Lowerer::ExprType::I64}; ///< Result type when fixed.
        std::size_t argIndex{0};                        ///< Referenced argument index.
    };

    /// @brief Runtime helper invocation tracking used during lowering.
    struct Feature {
        /// @brief Which runtime tracking API to invoke.
        enum class Action {
            Request, ///< Call @ref Lowerer::requestHelper.
            Track,   ///< Call @ref Lowerer::trackRuntime.
        } action{Action::Request};

        il::runtime::RuntimeFeature feature{
            il::runtime::RuntimeFeature::Count}; ///< Runtime feature identifier.
    };

    /// @brief Transformation applied to an argument prior to emission.
    struct ArgTransform {
        /// @brief Specific adjustment performed on the argument value.
        enum class Kind {
            EnsureI64,  ///< Invoke @ref Lowerer::ensureI64.
            EnsureF64,  ///< Invoke @ref Lowerer::ensureF64.
            EnsureI32,  ///< Invoke @ref Lowerer::ensureI64 followed by a narrow to i32.
            CoerceI64,  ///< Invoke @ref Lowerer::coerceToI64.
            CoerceF64,  ///< Invoke @ref Lowerer::coerceToF64.
            CoerceBool, ///< Invoke @ref Lowerer::coerceToBool.
            AddConst,   ///< Emit an add with an integer constant.
        } kind{Kind::EnsureI64};

        std::int64_t immediate{0}; ///< Constant used by AddConst.
    };

    /// @brief Argument description for runtime emission.
    struct Argument {
        std::size_t index{0};                   ///< Index into BuiltinCallExpr::args.
        std::vector<ArgTransform> transforms{}; ///< Transformations to apply before use.

        /// @brief Typed literal used when the indexed source argument is absent.
        struct DefaultValue {
            Lowerer::ExprType type{Lowerer::ExprType::I64}; ///< Expression type of the default.
            double f64{0.0};                                ///< Floating default payload.
            std::int64_t i64{0};                            ///< Integer default payload.
            std::string_view str{};                         ///< String default payload.
        };

        std::optional<DefaultValue> defaultValue{}; ///< Optional default when argument absent.
    };

    /// @brief Variant describing how to materialize the builtin call.
    struct Variant {
        /// @brief Selection predicate for the variant.
        enum class Condition {
            Always,        ///< Applies unconditionally.
            IfArgPresent,  ///< Applies when argument exists.
            IfArgMissing,  ///< Applies when argument is absent.
            IfArgTypeIs,   ///< Applies when argument type matches.
            IfArgTypeIsNot ///< Applies when argument type differs.
        } condition{Condition::Always};

        std::size_t conditionArg{0}; ///< Argument inspected by the condition.
        Lowerer::ExprType conditionType{
            Lowerer::ExprType::I64};             ///< Type operand for type-based predicates.
        std::optional<std::size_t> callLocArg{}; ///< Optional argument providing emission location.

        /// @brief Lowering strategy for the variant.
        enum class Kind {
            CallRuntime, ///< Call a runtime helper.
            EmitUnary,   ///< Emit a unary opcode.
            Custom,      ///< Reserved for bespoke handlers.
        } kind{Kind::CallRuntime};

        const char *runtime{nullptr}; ///< Runtime symbol name when calling helpers.
        il::core::Opcode opcode{
            il::core::Opcode::IAddOvf};    ///< Unary opcode when @ref kind == EmitUnary.
        std::vector<Argument> arguments{}; ///< Ordered argument specifications.
        std::vector<Feature> features{};   ///< Feature tracking applied for this variant.
    };

    ResultSpec result{};             ///< Result determination rule.
    std::vector<Variant> variants{}; ///< Ordered set of variants; first match is used.
};

/// @brief Metadata for a BASIC built-in function.
struct BuiltinInfo {
    const char *name;                          ///< BASIC source spelling.
    SemanticAnalyzer::BuiltinAnalyzer analyze; ///< Optional semantic handler.
};

/// @brief Argument count constraints for a builtin.
struct BuiltinArity {
    std::size_t minArgs; ///< Minimum required arguments.
    std::size_t maxArgs; ///< Maximum allowed arguments.
};

/// @brief Lookup builtin info by enum.
/// @param b Dense builtin enumerator.
/// @return Reference to static-lifetime name and analyzer metadata.
/// @pre @p b is a valid BuiltinCallExpr::Builtin enumerator.
const BuiltinInfo &getBuiltinInfo(BuiltinCallExpr::Builtin b);

/// @brief Retrieve argument arity constraints for a builtin.
/// @param b Builtin enumerator to query.
/// @return Inclusive minimum/maximum counts, or `{0, 0}` for an out-of-range value.
BuiltinArity getBuiltinArity(BuiltinCallExpr::Builtin b);

/// @brief Find builtin enum by BASIC name.
/// @details Attempts an exact canonical lookup, then an ASCII-oriented uppercase
///          lookup, and finally accepts `CHR` as an alias for `CHR$`.
/// @param name Source spelling, with or without canonical case.
/// @return Matching enumerator or `std::nullopt`.
std::optional<BuiltinCallExpr::Builtin> lookupBuiltin(std::string_view name);

/// @brief Fetch the lowering rule for a builtin.
/// @param b Dense builtin enumerator.
/// @return Reference to static-lifetime lowering metadata.
/// @pre @p b is a valid BuiltinCallExpr::Builtin enumerator.
const BuiltinLoweringRule &getBuiltinLoweringRule(BuiltinCallExpr::Builtin b);

/// @brief Function signature used by builtin lowering handlers.
/// @details Handlers borrow the lowering context and return one Lowerer rvalue.
using BuiltinHandler = Lowerer::RVal (*)(lower::BuiltinLowerContext &);

/// @brief Register a lowering handler for the builtin spelled @p name.
/// @details A non-null handler inserts or replaces the exact key. A null handler
///          removes the exact key.
/// @param name Case-sensitive registry key copied into process-lifetime storage.
/// @param fn Handler to install, or null to erase @p name.
/// @warning Concurrent mutation or lookup is not synchronized.
void register_builtin(std::string_view name, BuiltinHandler fn);

/// @brief Locate the lowering handler associated with @p name.
/// @param name Case-sensitive key to query without copying.
/// @return Registered handler or null when absent.
/// @warning Concurrent mutation or lookup is not synchronized.
BuiltinHandler find_builtin(std::string_view name);

/// @brief Lightweight result kind derived from builtin descriptor metadata.
/// @details Only returns a concrete kind for builtins with fixed result
///          categories; builtins whose result depends on arguments yield Unknown.
enum class BuiltinResultKind : std::uint8_t {
    Unknown = 0, ///< Result type depends on arguments or is not determinable.
    Int,         ///< Always produces an integer result.
    Float,       ///< Always produces a floating-point result.
    String,      ///< Always produces a string result.
};

/// \brief Fetch a fixed result kind for @p b when available.
/// \details Consults the builtin descriptor table. When the descriptor marks
///          multiple possible categories (e.g., depends on args), returns Unknown.
/// \param b Builtin enumerator to inspect.
/// \return Sole descriptor result category, or Unknown for invalid, empty, or
///         multi-category descriptors.
BuiltinResultKind getBuiltinFixedResult(BuiltinCallExpr::Builtin b);

// Semantic signature view (registry-driven) ----------------------------------

/// @brief Bitmask describing which BASIC type categories are accepted for an argument.
enum class BuiltinArgTypeMask : std::uint8_t {
    None = 0,                          ///< No type accepted (sentinel).
    Int = 1U << 0U,                    ///< Integer types (INTEGER, LONG).
    Float = 1U << 1U,                  ///< Floating-point types (SINGLE, DOUBLE).
    String = 1U << 2U,                 ///< String type.
    Bool = 1U << 3U,                   ///< Boolean type.
    Number = Int | Float,              ///< Any numeric type.
    Any = Int | Float | String | Bool, ///< Any type.
};

/// @brief Per-argument specification in a semantic signature view.
/// @details Describes whether an argument is optional and which type categories
///          are accepted by the builtin for that argument position.
struct SemanticArgSpecView {
    bool optional{false};                                ///< True when the argument may be omitted.
    BuiltinArgTypeMask allowed{BuiltinArgTypeMask::Any}; ///< Accepted type categories.
};

/// @brief Registry-backed semantic signature describing arity and argument types.
/// @details Provides a lightweight view over the builtin's argument constraints
///          for use during semantic analysis. The args pointer references static data.
struct SemanticSignatureView {
    std::size_t minArgs{0};                   ///< Minimum required argument count.
    std::size_t maxArgs{0};                   ///< Maximum allowed argument count.
    const SemanticArgSpecView *args{nullptr}; ///< Per-argument specifications (static storage).
    std::size_t argCount{0};                  ///< Number of entries in the args array.
    BuiltinResultKind result{BuiltinResultKind::Unknown}; ///< Fixed result kind, if determinable.
};

/// \brief Return a registry-backed semantic signature when available.
/// \details For builtins that have per-argument typing metadata encoded, the
///          returned view describes allowed argument categories and arity.
///          Currently ARGC, ARGGET, and COMMAND are covered.
/// \param b Builtin enumerator to query.
/// \return Pointer to static-lifetime signature data, or nullptr for builtins
///         not covered by this minimal view.
const SemanticSignatureView *getBuiltinSemanticSignature(BuiltinCallExpr::Builtin b);

} // namespace il::frontends::basic
