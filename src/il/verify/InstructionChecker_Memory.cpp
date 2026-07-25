//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the verifier helpers that validate memory-related instructions
// such as alloca, load, store, and constant pointer forms.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Memory instruction verification helpers.
/// @details Provides functions that inspect @ref VerifyCtx and ensure memory
///          operations obey type and range rules, emitting diagnostics when they
///          do not.

#include "il/verify/InstructionCheckerShared.hpp"

#include "il/core/Function.hpp"
#include "il/core/Global.hpp"
#include "il/core/Instr.hpp"
#include "il/verify/DiagSink.hpp"
#include "il/verify/TypeInference.hpp"

#include <cstdint>
#include <limits>
#include <optional>

namespace il::verify::checker {

using il::core::Type;
using il::core::Value;
using il::support::Diag;
using il::support::Expected;
using il::support::Severity;

namespace {

/// @brief Emit a non-fatal diagnostic associated with the current instruction.
/// @param ctx Verification context providing diagnostic sink and location.
/// @param message Warning text to append to the diagnostic.
void emitWarning(const VerifyCtx &ctx, std::string_view message) {
    ctx.diags.report(Diag{Severity::Warning, formatDiag(ctx, message), ctx.instr.loc, {}});
}

/// @brief Extract a signed constant offset or allocation size.
/// @param value Value to inspect.
/// @return Integer payload for a constant, or no value otherwise.
std::optional<long long> constInt(const Value &value) {
    if (value.kind != Value::Kind::ConstInt)
        return std::nullopt;
    return value.i64;
}

/// @brief Test whether signed addition would overflow `long long`.
/// @param lhs Left addend.
/// @param rhs Right addend.
/// @return `true` when the mathematical sum is not representable.
bool addOverflows(long long lhs, long long rhs) {
    return (rhs > 0 && lhs > std::numeric_limits<long long>::max() - rhs) ||
           (rhs < 0 && lhs < std::numeric_limits<long long>::min() - rhs);
}

/// @brief Reject direct global-address literals where a computed pointer is required.
/// @param ctx Current instruction context used for diagnostics.
/// @param ptr Candidate pointer operand.
/// @param operation Operation name included in any diagnostic.
/// @return Success unless @p ptr is a direct `GlobalAddr`.
Expected<void> rejectDirectGlobalAddress(const VerifyCtx &ctx,
                                         const Value &ptr,
                                         std::string_view operation) {
    if (ptr.kind == Value::Kind::GlobalAddr)
        return fail(ctx,
                    std::string(operation) +
                        " requires a pointer value; use gaddr or addr_of for globals");
    return {};
}

/// @brief Find the instruction defining a temporary within a function.
/// @param fn Function scanned in block and instruction order.
/// @param temp Temporary identifier to locate.
/// @return Pointer to the first matching result definition, or null.
const il::core::Instr *findDef(const il::core::Function &fn, unsigned temp) {
    for (const auto &block : fn.blocks)
        for (const auto &instr : block.instructions)
            if (instr.result && *instr.result == temp)
                return &instr;
    return nullptr;
}

/// @brief Statically known allocation size and pointer offset within it.
struct StackBounds {
    /// @brief Root `alloca` size in bytes.
    long long size = 0;

    /// @brief Signed constant offset from the root allocation.
    long long offset = 0;
};

/// @brief Resolve an alloca-derived pointer to static size and offset bounds.
/// @details Follows result definitions through constant-offset GEP chains,
///          stopping after 32 links or whenever size, offset, definition, or
///          addition cannot be proven safely.
/// @param fn Function containing pointer definitions.
/// @param ptr Pointer value to resolve.
/// @param depth Current recursion depth.
/// @return Static bounds when rooted in a constant nonnegative alloca.
std::optional<StackBounds> stackBoundsForPointer(const il::core::Function &fn,
                                                 const Value &ptr,
                                                 unsigned depth = 0) {
    if (depth > 32 || ptr.kind != Value::Kind::Temp)
        return std::nullopt;
    const il::core::Instr *def = findDef(fn, ptr.id);
    if (!def)
        return std::nullopt;

    if (def->op == il::core::Opcode::Alloca && !def->operands.empty()) {
        auto size = constInt(def->operands.front());
        if (!size || *size < 0)
            return std::nullopt;
        return StackBounds{*size, 0};
    }

    if (def->op != il::core::Opcode::GEP || def->operands.size() < 2)
        return std::nullopt;
    auto base = stackBoundsForPointer(fn, def->operands[0], depth + 1);
    auto offset = constInt(def->operands[1]);
    if (!base || !offset)
        return std::nullopt;
    if (addOverflows(base->offset, *offset))
        return std::nullopt;
    base->offset += *offset;
    return base;
}

/// @brief Check a statically resolvable stack load or store against its allocation.
/// @details Unknown provenance is accepted for later/runtime handling. Known
///          provenance rejects negative offsets, pointers beyond the one-past
///          endpoint, and accesses whose storage size crosses the allocation.
/// @param ctx Verification context providing the function and diagnostics.
/// @param ptr Pointer operand used by the access.
/// @param accessKind Type kind loaded or stored.
/// @param operation Operation name included in diagnostics.
/// @return Success when unknown or in bounds; otherwise an error.
Expected<void> checkStackAccessBounds(const VerifyCtx &ctx,
                                      const Value &ptr,
                                      Type::Kind accessKind,
                                      std::string_view operation) {
    const auto size = il::core::storageSizeBytes(accessKind);
    if (!size)
        return fail(ctx, std::string(operation) + " type has no storage size");

    auto bounds = stackBoundsForPointer(ctx.fn, ptr);
    if (!bounds)
        return {};

    if (bounds->offset < 0 || bounds->offset > bounds->size)
        return fail(ctx, std::string(operation) + " outside alloca");
    if (*size > bounds->size - bounds->offset)
        return fail(ctx, std::string(operation) + " exceeds alloca bounds");
    return {};
}

} // namespace

/// @brief Verify the semantics of the @c alloca instruction.
/// @details Ensures the size operand exists, is i64-typed, and warns when the
///          requested allocation is suspiciously large.  Records the result as a
///          pointer type when validation succeeds.
/// @param ctx Verification context containing the allocation.
/// @return Success for a valid size, or an error for missing, mistyped, or
///         negative sizes.
Expected<void> checkAlloca(const VerifyCtx &ctx) {
    if (ctx.instr.operands.empty())
        return fail(ctx, "missing size operand");

    if (ctx.types.valueType(ctx.instr.operands[0]).kind != Type::Kind::I64)
        return fail(ctx, "size must be i64");

    if (ctx.instr.operands[0].kind == Value::Kind::ConstInt) {
        const long long size = ctx.instr.operands[0].i64;
        if (size < 0)
            return fail(ctx, "negative alloca size");
        if (size > (1LL << 20))
            emitWarning(ctx, "huge alloca");
    }

    ctx.types.recordResult(ctx.instr, Type(Type::Kind::Ptr));
    return {};
}

/// @brief Verify the @c gep instruction.
/// @details Requires a pointer base and i64 byte offset, rejects direct global
///          address literals, checks constant offsets against statically known
///          alloca bounds, and records a pointer result.
/// @param ctx Verification context containing the GEP.
/// @return Success when operand types and any provable bounds are valid.
Expected<void> checkGEP(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() < 2)
        return fail(ctx, "invalid operand count");
    if (auto result = rejectDirectGlobalAddress(ctx, ctx.instr.operands[0], "gep"); !result)
        return result;

    bool baseMissing = false;
    if (ctx.types.valueType(ctx.instr.operands[0], &baseMissing).kind != Type::Kind::Ptr) {
        if (baseMissing)
            return fail(ctx, "base pointer type is unknown");
        return fail(ctx, "base pointer must be ptr");
    }

    bool offsetMissing = false;
    if (ctx.types.valueType(ctx.instr.operands[1], &offsetMissing).kind != Type::Kind::I64) {
        if (offsetMissing)
            return fail(ctx, "offset type is unknown");
        return fail(ctx, "offset must be i64");
    }

    if (const auto offset = constInt(ctx.instr.operands[1])) {
        if (auto bounds = stackBoundsForPointer(ctx.fn, ctx.instr.operands[0])) {
            if (addOverflows(bounds->offset, *offset))
                return fail(ctx, "gep offset overflow");
            const long long absolute = bounds->offset + *offset;
            if (absolute < 0 || absolute > bounds->size)
                return fail(ctx, "gep offset outside alloca");
        }
    }

    ctx.types.recordResult(ctx.instr, Type(Type::Kind::Ptr));
    return {};
}

/// @brief Verify the @c load instruction.
/// @details Requires a pointer operand and records the result as the annotated
///          instruction type, reporting diagnostics when the pointer type does
///          not match expectations.
/// @param ctx Verification context containing the load.
/// @return Success when pointer type and any provable stack bounds are valid.
Expected<void> checkLoad(const VerifyCtx &ctx) {
    if (ctx.instr.operands.empty())
        return fail(ctx, "missing operand");
    if (auto result = rejectDirectGlobalAddress(ctx, ctx.instr.operands[0], "load"); !result)
        return result;

    if (ctx.types.valueType(ctx.instr.operands[0]).kind != Type::Kind::Ptr)
        return fail(ctx, "pointer type mismatch");

    if (auto result =
            checkStackAccessBounds(ctx, ctx.instr.operands[0], ctx.instr.type.kind, "load");
        !result)
        return result;

    ctx.types.recordResult(ctx.instr, ctx.instr.type);
    return {};
}

/// @brief Verify the @c store instruction.
/// @details Validates pointer operand type, checks boolean stores for legal
///          literal values, and enforces integer literal bounds based on the
///          target type.
/// @param ctx Verification context containing the store.
/// @return Success when pointer, stack bounds, and literal range are valid.
Expected<void> checkStore(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() < 2)
        return fail(ctx, "invalid operand count");
    if (auto result = rejectDirectGlobalAddress(ctx, ctx.instr.operands[0], "store"); !result)
        return result;

    bool pointerMissing = false;
    const Type pointerType = ctx.types.valueType(ctx.instr.operands[0], &pointerMissing);
    if (pointerMissing)
        return fail(ctx, "pointer operand type is unknown");

    if (pointerType.kind != Type::Kind::Ptr)
        return fail(ctx, "pointer type mismatch");

    if (auto result =
            checkStackAccessBounds(ctx, ctx.instr.operands[0], ctx.instr.type.kind, "store");
        !result)
        return result;

    const bool isBoolConst = ctx.instr.type.kind == Type::Kind::I1 &&
                             ctx.instr.operands[1].kind == Value::Kind::ConstInt;
    if (isBoolConst) {
        const long long value = ctx.instr.operands[1].i64;
        if (value != 0 && value != 1)
            return fail(ctx, "boolean store expects 0 or 1");
    } else if (ctx.instr.operands[1].kind == Value::Kind::ConstInt &&
               (ctx.instr.type.kind == Type::Kind::I16 || ctx.instr.type.kind == Type::Kind::I32)) {
        const long long value = ctx.instr.operands[1].i64;
        const long long min = ctx.instr.type.kind == Type::Kind::I16
                                  ? std::numeric_limits<int16_t>::min()
                                  : std::numeric_limits<int32_t>::min();
        const long long max = ctx.instr.type.kind == Type::Kind::I16
                                  ? std::numeric_limits<int16_t>::max()
                                  : std::numeric_limits<int32_t>::max();
        if (value < min || value > max)
            return fail(ctx, "value out of range for store type");
    }

    return {};
}

/// @brief Verify the @c addr.of instruction.
/// @details Requires a single global-address operand and records the result as a
///          pointer type. The named global must exist and have string type.
/// @param ctx Verification context containing `addr.of`.
/// @return Success for a known string global; otherwise an error.
Expected<void> checkAddrOf(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() != 1 || ctx.instr.operands[0].kind != Value::Kind::GlobalAddr)
        return fail(ctx, "operand must be global");

    const std::string &name = ctx.instr.operands[0].str;
    auto it = ctx.globals.find(name);
    if (it == ctx.globals.end())
        return fail(ctx, "unknown global @" + name);
    if (it->second->type.kind != Type::Kind::Str)
        return fail(ctx, "addr.of operand must name a string global");

    ctx.types.recordResult(ctx.instr, Type(Type::Kind::Ptr));
    return {};
}

/// @brief Verify the @c const.str instruction.
/// @details Confirms the operand references a known string global and records
///          the result as a string type.
/// @param ctx Verification context containing `const.str`.
/// @return Success for a known string global; otherwise an error.
Expected<void> checkConstStr(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() != 1 || ctx.instr.operands[0].kind != Value::Kind::GlobalAddr)
        return fail(ctx, "unknown string global");

    const std::string &name = ctx.instr.operands[0].str;
    auto it = ctx.globals.find(name);
    if (it == ctx.globals.end())
        return fail(ctx, "unknown string global @" + name);
    if (it->second->type.kind != Type::Kind::Str)
        return fail(ctx, "const.str operand must name a string global");

    ctx.types.recordResult(ctx.instr, Type(Type::Kind::Str));
    return {};
}

/// @brief Verify the @c gaddr instruction.
/// @details Requires a declared non-string global with addressable storage.
/// @param ctx Verification context containing `gaddr`.
/// @return Success for a known supported scalar global; otherwise an error.
Expected<void> checkGAddr(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() != 1 || ctx.instr.operands[0].kind != Value::Kind::GlobalAddr)
        return fail(ctx, "gaddr operand must be global");

    const std::string &name = ctx.instr.operands[0].str;
    auto it = ctx.globals.find(name);
    if (it == ctx.globals.end())
        return fail(ctx, "unknown global @" + name);

    const Type::Kind kind = it->second->type.kind;
    if (kind == Type::Kind::Str)
        return fail(ctx, "gaddr requires a scalar storage global, not a string constant");
    if (kind == Type::Kind::Void || kind == Type::Kind::Error || kind == Type::Kind::ResumeTok)
        return fail(ctx, "gaddr requires a scalar storage global");

    ctx.types.recordResult(ctx.instr, Type(Type::Kind::Ptr));
    return {};
}

/// @brief Verify the @c const.null instruction.
/// @details Accepts only pointer-like result annotations and records the result
///          for downstream passes.
/// @param ctx Verification context containing `const.null`.
/// @return Success for ptr, str, error, or resumetok; otherwise an error.
Expected<void> checkConstNull(const VerifyCtx &ctx) {
    Type resultType = ctx.instr.type;
    switch (resultType.kind) {
        case Type::Kind::Ptr:
        case Type::Kind::Str:
        case Type::Kind::Error:
        case Type::Kind::ResumeTok:
            break;
        default:
            return fail(ctx, "const.null result type must be ptr, str, error, or resumetok");
    }

    ctx.types.recordResult(ctx.instr, resultType);
    return {};
}

} // namespace il::verify::checker
