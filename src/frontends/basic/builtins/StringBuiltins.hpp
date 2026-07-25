//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: frontends/basic/builtins/StringBuiltins.hpp
// Purpose: Declares registry and lowering helpers for BASIC string built-ins.
// Key invariants: Registry lookups return immutable metadata describing the
//                 builtin's name, arity bounds, and lowering entry point.
// Ownership/Lifetime: Functions operate on the lowering context supplied by the
//                     caller; LowerCtx borrows the Lowerer and call AST node.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/Lowerer.hpp"
#include "frontends/basic/ast/ExprNodes.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "zanna/il/Module.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// @brief Declares the legacy callback-based BASIC string builtin lowerers.
/// @details The registry maps canonical source spellings to arity constraints
///          and lowering callbacks. LowerCtx lazily lowers call arguments and
///          centralizes coercion, runtime-feature tracking, and runtime-call
///          emission for those callbacks.

namespace il::frontends::basic::builtins {
using il::core::Type;
using il::core::Value;

class LowerCtx;

/// @brief Alias used for passing contiguous argument views.
template <typename T> using ArrayRef = std::span<T>;

/// @brief Alias for lightweight string views.
using StringRef = std::string_view;

/// @brief Function signature used by builtin lowering handlers.
/// @details The argument span exposes the context's materialized IL values;
///          handlers may instead request lazy arguments directly from LowerCtx.
/// @return IL value produced for the builtin call.
using LoweringFn = Value (*)(LowerCtx &, ArrayRef<Value>);

/// @brief Specification record describing a registered builtin.
struct BuiltinSpec {
    std::string name;       ///< Canonical BASIC spelling of the builtin.
    int minArity{0};        ///< Minimum number of accepted arguments.
    int maxArity{0};        ///< Maximum number of accepted arguments.
    LoweringFn fn{nullptr}; ///< Lowering entry point for the builtin.
};

/// @brief Lookup metadata for a string builtin by BASIC spelling.
/// @param name BASIC source spelling (case sensitive).
/// @return Pointer to the builtin spec when found; nullptr otherwise.
const BuiltinSpec *findBuiltin(StringRef name);

/// @brief Helper context exposing lowering utilities to builtin handlers.
class LowerCtx {
  public:
    /// @brief Construct a lowering context.
    /// @param lowerer Owning lowering driver.
    /// @param call Builtin call being lowered.
    LowerCtx(Lowerer &lowerer, const BuiltinCallExpr &call);

    /// @brief Retrieve the lowering driver.
    /// @return Borrowed lowering driver supplied at construction.
    Lowerer &lowerer() const noexcept;

    /// @brief Access the builtin call AST node.
    /// @return Borrowed immutable call node supplied at construction.
    const BuiltinCallExpr &call() const noexcept;

    /// @brief Total number of argument slots recorded on the call.
    /// @return Number of entries in BuiltinCallExpr::args, including omitted
    ///         optional argument slots.
    std::size_t argCount() const noexcept;

    /// @brief Check whether argument @p idx is present.
    /// @param idx Zero-based argument slot.
    /// @return True when the slot exists and contains an expression.
    bool hasArg(std::size_t idx) const noexcept;

    /// @brief Source location for argument @p idx or the call site when absent.
    /// @param idx Zero-based argument slot.
    /// @return Argument location when available; otherwise the call location.
    il::support::SourceLoc argLoc(std::size_t idx) const noexcept;

    /// @brief Access the lowered argument at @p idx.
    /// @param idx Zero-based argument slot to lower on demand.
    /// @return Mutable reference to the cached lowered r-value.
    Lowerer::RVal &arg(std::size_t idx);

    /// @brief Access the lowered argument value at @p idx.
    /// @param idx Zero-based argument slot to lower on demand.
    /// @return Mutable reference to the cached IL value.
    Value &argValue(std::size_t idx);

    /// @brief Immutable view over the materialized argument values.
    /// @return View backed by this context's argument-value cache.
    ArrayRef<Value> values() noexcept;

    /// @brief Update the result type that the handler will produce.
    void setResultType(Type ty) noexcept;

    /// @brief Final result type populated by the handler.
    /// @return Handler-selected result type, or I64 before a handler changes it.
    Type resultType() const noexcept;

    /// @brief Ensure argument @p idx is materialized as a 64-bit integer.
    /// @param idx Zero-based argument slot.
    /// @param loc Source location attributed to any emitted conversion.
    /// @return Mutable reference to the converted cached r-value.
    Lowerer::RVal &ensureI64(std::size_t idx, il::support::SourceLoc loc);

    /// @brief Ensure argument @p idx is materialized as a 64-bit float.
    /// @param idx Zero-based argument slot.
    /// @param loc Source location attributed to any emitted conversion.
    /// @return Mutable reference to the converted cached r-value.
    Lowerer::RVal &ensureF64(std::size_t idx, il::support::SourceLoc loc);

    /// @brief Coerce argument @p idx to a 64-bit integer.
    /// @param idx Zero-based argument slot.
    /// @param loc Source location attributed to any emitted conversion.
    /// @return Mutable reference to the best-effort converted cached r-value.
    Lowerer::RVal &coerceToI64(std::size_t idx, il::support::SourceLoc loc);

    /// @brief Coerce argument @p idx to a 64-bit float.
    /// @param idx Zero-based argument slot.
    /// @param loc Source location attributed to any emitted conversion.
    /// @return Mutable reference to the best-effort converted cached r-value.
    Lowerer::RVal &coerceToF64(std::size_t idx, il::support::SourceLoc loc);

    /// @brief Add integer constant @p immediate to argument @p idx.
    /// @param idx Zero-based argument slot.
    /// @param immediate Signed constant added with checked-overflow semantics.
    /// @param loc Source location attributed to the emitted addition.
    /// @return Mutable reference to the updated cached r-value.
    Lowerer::RVal &addConst(std::size_t idx, std::int64_t immediate, il::support::SourceLoc loc);

    /// @brief Narrow integer argument @p idx to @p target with runtime checks.
    /// @param idx Zero-based argument slot.
    /// @param target Destination integer type.
    /// @param loc Source location attributed to the emitted conversion.
    /// @return Mutable reference to the narrowed cached r-value.
    Lowerer::RVal &narrowInt(std::size_t idx, Type target, il::support::SourceLoc loc);

    /// @brief Classify the numeric type of the provided expression.
    /// @param expr Expression whose BASIC numeric category is required.
    /// @return Numeric category inferred by the owning lowerer.
    TypeRules::NumericType classifyNumericType(const Expr &expr);

    /// @brief Request a runtime helper feature.
    /// @param feature Runtime capability that must be linked.
    void requestHelper(il::runtime::RuntimeFeature feature);

    /// @brief Track a runtime helper feature usage.
    /// @param feature Runtime capability used by emitted IL.
    void trackRuntime(il::runtime::RuntimeFeature feature);

    /// @brief Emit a runtime call returning a value.
    /// @param ty Declared result type of the runtime helper.
    /// @param runtime Null-terminated runtime function name.
    /// @param args Ordered IL arguments passed to the helper.
    /// @param loc Source location assigned to the call.
    /// @return IL value representing the helper result.
    Value emitCallRet(Type ty,
                      const char *runtime,
                      const std::vector<Value> &args,
                      il::support::SourceLoc loc);

  private:
    /// @brief Lazily lower argument @p idx, caching the result for reuse.
    /// @param idx Zero-based argument slot.
    /// @return Mutable reference to the cached lowered r-value.
    Lowerer::RVal &ensureLowered(std::size_t idx);
    /// @brief Refresh @ref argValues_ entry @p idx from the cached lowered RVal.
    /// @param idx Zero-based cache entry to synchronize.
    void syncValue(std::size_t idx) noexcept;

    Lowerer &lowerer_;            ///< Borrowed lowering driver.
    const BuiltinCallExpr &call_; ///< Borrowed builtin call being lowered.
    std::vector<std::optional<Lowerer::RVal>> loweredArgs_; ///< Per-arg lazily lowered values.
    std::vector<Value> argValues_; ///< Materialized IL values mirroring loweredArgs_.
    std::vector<il::support::SourceLoc> argLocs_; ///< Per-argument source locations.
    std::vector<bool> hasArg_;                    ///< Presence flag per argument slot.
    Type resultType_{Type(Type::Kind::I64)};      ///< Handler-selected result type (default i64).
};

} // namespace il::frontends::basic::builtins
