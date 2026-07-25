//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements verification helpers for runtime calls and trap-related
// instructions.  The routines validate argument counts, operand types, and
// result expectations against both opcode metadata and runtime signature rules
// to ensure modules adhere to the IL specification before execution.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Runtime-specific instruction verification utilities.
/// @details Provides helpers that validate runtime array operations, indirect
///          calls, and trap instructions.  Diagnostics are routed through the
///          shared @ref VerifyCtx infrastructure so all failures include
///          location and opcode context.

#include "il/verify/InstructionCheckerShared.hpp"

#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/runtime/HelperEffects.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/verify/TypeInference.hpp"

#include <limits>
#include <sstream>
#include <string_view>

namespace il::verify::checker {

using il::core::Extern;
using il::core::Function;
using il::core::kindToString;
using il::core::Type;
using il::support::Expected;

namespace {

/// @brief Direct runtime array helper recognized for specialized ABI checks.
enum class RuntimeArrayCallee {
    None,
    // i32 arrays
    NewI32,
    LenI32,
    GetI32,
    SetI32,
    ResizeI32,
    RetainI32,
    ReleaseI32,
    // i64 arrays
    NewI64,
    LenI64,
    GetI64,
    SetI64,
    ResizeI64,
    RetainI64,
    ReleaseI64,
    // f64 arrays
    NewF64,
    LenF64,
    GetF64,
    SetF64,
    ResizeF64,
    RetainF64,
    ReleaseF64,
    // string arrays
    NewStr,
    LenStr,
    GetStr,
    SetStr,
    ReleaseStr,
    // object arrays (void* elements)
    NewObj,
    LenObj,
    GetObj,
    PutObj,
    ResizeObj,
    ReleaseObj,
};

/// @brief Effect guarantees resolved for a call target.
struct CalleeEffectMetadata {
    /// @brief Whether the guarantees come from authoritative metadata.
    bool known = false;

    /// @brief Whether the callee is free of observable side effects.
    bool pure = false;

    /// @brief Whether the callee avoids externally visible writes.
    bool readonly = false;

    /// @brief Whether the callee cannot throw or trap.
    bool nothrow = false;
};

/// @brief Resolve effect guarantees for a direct callee.
/// @details Prefers runtime metadata, then function attributes, explicitly
///          attributed externs, and helper-effect classification.
/// @param externSig Resolved extern declaration, if any.
/// @param fnSig Resolved function declaration, if any.
/// @param runtimeSig Canonical runtime signature, if any.
/// @param calleeName Symbol used for helper classification.
/// @return Effect metadata with `known` false when no authoritative source exists.
CalleeEffectMetadata queryCalleeEffects(const Extern *externSig,
                                        const Function *fnSig,
                                        const il::runtime::RuntimeSignature *runtimeSig,
                                        std::string_view calleeName) {
    if (runtimeSig)
        return CalleeEffectMetadata{
            true, runtimeSig->pure, runtimeSig->readonly, runtimeSig->nothrow};
    if (fnSig)
        return CalleeEffectMetadata{
            true, fnSig->attrs().pure, fnSig->attrs().readonly, fnSig->attrs().nothrow};
    if (externSig &&
        (externSig->attrs().pure || externSig->attrs().readonly || externSig->attrs().nothrow)) {
        return CalleeEffectMetadata{
            true, externSig->attrs().pure, externSig->attrs().readonly, externSig->attrs().nothrow};
    }
    if (const auto helper = il::runtime::classifyHelperEffects(calleeName); helper.known)
        return CalleeEffectMetadata{true, helper.pure, helper.readonly, helper.nothrow};
    return {};
}

/// @brief Map a runtime helper name to its array-handling category.
/// @details The runtime exposes a fixed set of array helpers with predictable
///          names.  This function compares @p callee against the supported
///          strings and returns the corresponding enumerator so subsequent
///          verification can apply helper-specific rules.  Unknown names fall
///          back to @ref RuntimeArrayCallee::None so other verifiers may handle
///          the call.
/// @param callee Runtime helper name extracted from the instruction.
/// @return Enumerated helper classification.
RuntimeArrayCallee classifyRuntimeArrayCallee(std::string_view callee) {
    // i32 array helpers
    if (callee == "rt_arr_i32_new")
        return RuntimeArrayCallee::NewI32;
    if (callee == "rt_arr_i32_len")
        return RuntimeArrayCallee::LenI32;
    if (callee == "rt_arr_i32_get")
        return RuntimeArrayCallee::GetI32;
    if (callee == "rt_arr_i32_set")
        return RuntimeArrayCallee::SetI32;
    if (callee == "rt_arr_i32_resize")
        return RuntimeArrayCallee::ResizeI32;
    if (callee == "rt_arr_i32_retain")
        return RuntimeArrayCallee::RetainI32;
    if (callee == "rt_arr_i32_release")
        return RuntimeArrayCallee::ReleaseI32;

    // i64 array helpers
    if (callee == "rt_arr_i64_new")
        return RuntimeArrayCallee::NewI64;
    if (callee == "rt_arr_i64_len")
        return RuntimeArrayCallee::LenI64;
    if (callee == "rt_arr_i64_get")
        return RuntimeArrayCallee::GetI64;
    if (callee == "rt_arr_i64_set")
        return RuntimeArrayCallee::SetI64;
    if (callee == "rt_arr_i64_resize")
        return RuntimeArrayCallee::ResizeI64;
    if (callee == "rt_arr_i64_retain")
        return RuntimeArrayCallee::RetainI64;
    if (callee == "rt_arr_i64_release")
        return RuntimeArrayCallee::ReleaseI64;

    // f64 array helpers
    if (callee == "rt_arr_f64_new")
        return RuntimeArrayCallee::NewF64;
    if (callee == "rt_arr_f64_len")
        return RuntimeArrayCallee::LenF64;
    if (callee == "rt_arr_f64_get")
        return RuntimeArrayCallee::GetF64;
    if (callee == "rt_arr_f64_set")
        return RuntimeArrayCallee::SetF64;
    if (callee == "rt_arr_f64_resize")
        return RuntimeArrayCallee::ResizeF64;
    if (callee == "rt_arr_f64_retain")
        return RuntimeArrayCallee::RetainF64;
    if (callee == "rt_arr_f64_release")
        return RuntimeArrayCallee::ReleaseF64;

    // string array helpers
    if (callee == "rt_arr_str_alloc")
        return RuntimeArrayCallee::NewStr;
    if (callee == "rt_arr_str_len")
        return RuntimeArrayCallee::LenStr;
    if (callee == "rt_arr_str_get")
        return RuntimeArrayCallee::GetStr;
    if (callee == "rt_arr_str_put")
        return RuntimeArrayCallee::SetStr;
    if (callee == "rt_arr_str_release")
        return RuntimeArrayCallee::ReleaseStr;
    // object array helpers
    if (callee == "rt_arr_obj_new")
        return RuntimeArrayCallee::NewObj;
    if (callee == "rt_arr_obj_len")
        return RuntimeArrayCallee::LenObj;
    if (callee == "rt_arr_obj_get")
        return RuntimeArrayCallee::GetObj;
    if (callee == "rt_arr_obj_put")
        return RuntimeArrayCallee::PutObj;
    if (callee == "rt_arr_obj_resize")
        return RuntimeArrayCallee::ResizeObj;
    if (callee == "rt_arr_obj_release")
        return RuntimeArrayCallee::ReleaseObj;
    return RuntimeArrayCallee::None;
}

/// @brief Test whether two IL type kinds match exactly.
/// @param actual Inferred argument or result kind.
/// @param expected Signature-required kind.
/// @return `true` when the kinds are equal.
bool typeKindsCompatible(Type::Kind actual, Type::Kind expected) {
    return actual == expected;
}

/// @brief Check one fixed runtime argument against its signature kind.
/// @details In addition to exact matches, runtime parameters marked as object
///          pointers accept string handles.
/// @param actual Inferred operand kind.
/// @param expected Canonical runtime parameter kind.
/// @param runtimeSig Runtime metadata containing the object-parameter mask.
/// @param index Zero-based parameter index.
/// @return `true` when exact or permitted by object-handle compatibility.
bool runtimeArgTypeCompatible(Type::Kind actual,
                              Type::Kind expected,
                              const il::runtime::RuntimeSignature *runtimeSig,
                              size_t index) {
    if (typeKindsCompatible(actual, expected))
        return true;
    if (!runtimeSig || index >= 64)
        return false;
    const bool objectParam = (runtimeSig->objectParamMask & (std::uint64_t{1} << index)) != 0;
    return objectParam && expected == Type::Kind::Ptr && actual == Type::Kind::Str;
}

/// @brief Classify IL kinds permitted in variadic call positions.
/// @param kind Inferred kind of a variadic operand.
/// @return `true` for scalar, pointer, and string ABI values.
bool isSupportedVarArgType(Type::Kind kind) {
    switch (kind) {
        case Type::Kind::I1:
        case Type::Kind::I16:
        case Type::Kind::I32:
        case Type::Kind::I64:
        case Type::Kind::F64:
        case Type::Kind::Ptr:
        case Type::Kind::Str:
            return true;
        default:
            return false;
    }
}

/// @brief Test a string-view prefix without allocation.
/// @param text Complete text.
/// @param prefix Candidate prefix.
/// @return `true` when @p text begins with @p prefix.
bool startsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

/// @brief Test whether a string view contains a substring.
/// @param text Text to search.
/// @param needle Substring to locate.
/// @return `true` when @p needle occurs in @p text.
bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

/// @brief Determine whether discarding a runtime return would leak ownership.
/// @details All string returns require binding. Pointer returns require binding
///          for known allocation, construction, and resize helper families.
/// @param callee Runtime helper symbol.
/// @param retType Canonical return type.
/// @return `true` when the call must produce a bound SSA result.
bool runtimeReturnMustBeBound(std::string_view callee, Type retType) {
    if (retType.kind == Type::Kind::Str)
        return true;
    if (retType.kind != Type::Kind::Ptr)
        return false;
    if (callee == "rt_alloc")
        return true;
    if (startsWith(callee, "rt_arr_") &&
        (contains(callee, "_new") || contains(callee, "_alloc") || contains(callee, "_resize")))
        return true;
    if (startsWith(callee, "rt_obj_new"))
        return true;
    if (startsWith(callee, "rt_str_") &&
        (contains(callee, "_alloc") || callee == "rt_str_empty" || callee == "rt_str_left" ||
         callee == "rt_str_right" || callee == "rt_str_mid" || callee == "rt_str_mid_len" ||
         callee == "rt_str_ltrim" || callee == "rt_str_rtrim" || callee == "rt_str_trim" ||
         callee == "rt_str_ucase" || callee == "rt_str_lcase" || callee == "rt_str_chr"))
        return true;
    return false;
}

/// @brief Bundles the operand/result validation primitives used by
///        checkRuntimeArrayCall so the per-callee switch reads as a table of
///        intent rather than inline diagnostic boilerplate.
struct RuntimeArrayCallChecker {
    /// @brief Verification context shared by all primitive checks.
    const VerifyCtx &ctx;

    /// @brief Require an exact operand count, else emit a descriptive error.
    /// @param expected Required number of call operands.
    /// @return Success when the count matches; otherwise a callee-qualified error.
    Expected<void> requireArgCount(size_t expected) const {
        if (ctx.instr.operands.size() == expected)
            return {};
        std::ostringstream ss;
        ss << "expected " << expected << " argument";
        if (expected != 1)
            ss << 's';
        ss << " to @" << ctx.instr.callee;
        return fail(ctx, ss.str());
    }

    /// @brief Require operand @p index to have type @p expected.
    /// @param index Operand index, assumed present after count validation.
    /// @param expected Required inferred type kind.
    /// @param role Human-readable operand role included in diagnostics.
    /// @return Success when known and matching; otherwise an error.
    Expected<void> requireOperandType(size_t index,
                                      Type::Kind expected,
                                      std::string_view role) const {
        bool missing = false;
        const Type actual = ctx.types.valueType(ctx.instr.operands[index], &missing);
        if (missing) {
            std::ostringstream ss;
            ss << "@" << ctx.instr.callee << ' ' << role << " operand has unknown type";
            return fail(ctx, ss.str());
        }
        if (actual.kind != expected) {
            std::ostringstream ss;
            ss << "@" << ctx.instr.callee << ' ' << role << " operand must be "
               << kindToString(expected);
            return fail(ctx, ss.str());
        }
        return Expected<void>{};
    }

    /// @brief Require the instruction to produce a result of type @p expected.
    /// @param expected Required result kind.
    /// @return Success after recording the type, or an error for missing or
    ///         conflicting result metadata.
    Expected<void> requireResultType(Type::Kind expected) const {
        if (!ctx.instr.result) {
            std::ostringstream ss;
            ss << "@" << ctx.instr.callee << " must produce " << kindToString(expected)
               << " result";
            return fail(ctx, ss.str());
        }
        // Record the result type so IL parsed from text gets proper type inference.
        ctx.types.recordResult(ctx.instr, Type(expected));
        if (ctx.instr.type.kind != Type::Kind::Void && ctx.instr.type.kind != expected) {
            std::ostringstream ss;
            ss << "@" << ctx.instr.callee << " result must be " << kindToString(expected);
            return fail(ctx, ss.str());
        }
        return Expected<void>{};
    }

    /// @brief Require the instruction to be side-effect only (no result).
    /// @return Success for an unbound void call; otherwise an error.
    Expected<void> requireNoResult() const {
        if (ctx.instr.result) {
            std::ostringstream ss;
            ss << "@" << ctx.instr.callee << " must not produce a result";
            return fail(ctx, ss.str());
        }
        if (ctx.instr.type.kind != Type::Kind::Void) {
            std::ostringstream ss;
            ss << "@" << ctx.instr.callee << " result type must be void";
            return fail(ctx, ss.str());
        }
        return Expected<void>{};
    }
};

/// @brief Validate calls to recognized runtime array helpers.
/// @details Applies helper-specific operand counts, operand roles and types,
///          and result binding contracts for allocation, indexing, mutation,
///          resize, retain, and release operations. Unknown callees are left to
///          general call verification.
/// @param ctx Verification context describing the direct call.
/// @return Success when unknown or ABI-compatible; otherwise an error.
Expected<void> checkRuntimeArrayCall(const VerifyCtx &ctx) {
    const RuntimeArrayCallee calleeKind = classifyRuntimeArrayCallee(ctx.instr.callee);
    if (calleeKind == RuntimeArrayCallee::None)
        return {};

    RuntimeArrayCallChecker checker{ctx};
    /// @brief Forward an exact-count requirement to the checker.
    /// @param expected Required operand count.
    /// @return Success or the checker's diagnostic.
    const auto requireArgCount = [&](size_t expected) { return checker.requireArgCount(expected); };

    /// @brief Forward a typed operand requirement to the checker.
    /// @param index Operand index.
    /// @param expected Required type kind.
    /// @param role Diagnostic role name.
    /// @return Success or the checker's diagnostic.
    const auto requireOperandType = [&](size_t index, Type::Kind expected, std::string_view role) {
        return checker.requireOperandType(index, expected, role);
    };

    /// @brief Forward a result-kind requirement to the checker.
    /// @param expected Required result kind.
    /// @return Success or the checker's diagnostic.
    const auto requireResultType = [&](Type::Kind expected) {
        return checker.requireResultType(expected);
    };

    /// @brief Forward the side-effect-only result requirement to the checker.
    /// @return Success or the checker's diagnostic.
    const auto requireNoResult = [&]() { return checker.requireNoResult(); };

    switch (calleeKind) {
        case RuntimeArrayCallee::NewI32: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::LenI32: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireResultType(Type::Kind::I64);
        }
        case RuntimeArrayCallee::GetI32: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            return requireResultType(Type::Kind::I64);
        }
        case RuntimeArrayCallee::SetI32: {
            if (auto result = requireArgCount(3); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            if (auto result = requireOperandType(2, Type::Kind::I64, "value"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::ResizeI32: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::RetainI32: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::ReleaseI32: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::NewI64: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::LenI64: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireResultType(Type::Kind::I64);
        }
        case RuntimeArrayCallee::GetI64: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            return requireResultType(Type::Kind::I64);
        }
        case RuntimeArrayCallee::SetI64: {
            if (auto result = requireArgCount(3); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            if (auto result = requireOperandType(2, Type::Kind::I64, "value"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::ResizeI64: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::RetainI64: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::ReleaseI64: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::NewF64: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::LenF64: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireResultType(Type::Kind::I64);
        }
        case RuntimeArrayCallee::GetF64: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            return requireResultType(Type::Kind::F64);
        }
        case RuntimeArrayCallee::SetF64: {
            if (auto result = requireArgCount(3); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            if (auto result = requireOperandType(2, Type::Kind::F64, "value"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::ResizeF64: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::RetainF64: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::ReleaseF64: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireNoResult();
        }
        // String array helpers with string-typed results/operands as needed
        case RuntimeArrayCallee::NewStr: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::LenStr: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireResultType(Type::Kind::I64);
        }
        case RuntimeArrayCallee::GetStr: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            return requireResultType(Type::Kind::Str);
        }
        case RuntimeArrayCallee::SetStr: {
            if (auto result = requireArgCount(3); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            // ABI: third param is the string value (str) passed by value.
            if (auto result = requireOperandType(2, Type::Kind::Str, "value"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::ReleaseStr: {
            // String array release takes (ptr handle, i64 length) so runtime can
            // release contained elements before freeing the array payload.
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "length"); !result)
                return result;
            return requireNoResult();
        }
        // object arrays mirror i32 shapes, but get/put use ptr for value
        case RuntimeArrayCallee::NewObj: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::LenObj: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireResultType(Type::Kind::I64);
        }
        case RuntimeArrayCallee::GetObj: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::PutObj: {
            if (auto result = requireArgCount(3); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "index"); !result)
                return result;
            if (auto result = requireOperandType(2, Type::Kind::Ptr, "value"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::ResizeObj: {
            if (auto result = requireArgCount(2); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            if (auto result = requireOperandType(1, Type::Kind::I64, "length"); !result)
                return result;
            return requireResultType(Type::Kind::Ptr);
        }
        case RuntimeArrayCallee::ReleaseObj: {
            if (auto result = requireArgCount(1); !result)
                return result;
            if (auto result = requireOperandType(0, Type::Kind::Ptr, "handle"); !result)
                return result;
            return requireNoResult();
        }
        case RuntimeArrayCallee::None:
            break;
    }

    return {};
}

} // namespace

/// @brief Verify direct and indirect calls to functions, externs, and runtime helpers.
/// @details Resolves named callees against known functions and externs, checks
///          argument counts and operand types against the resolved signature,
///          validates return type compatibility, and records the result type
///          when present. Pointer-based indirect calls require embedded
///          signature metadata; global-address indirect calls are cross-checked
///          against the resolved symbol. Runtime calls also validate claimed
///          effects and owned-return binding requirements.
/// @param ctx Verification context containing call operands and signature maps.
/// @return Success when the call matches the signature; otherwise an error.
Expected<void> checkCall(const VerifyCtx &ctx) {
    // Handle direct calls with special runtime array helpers; skip for indirect.
    if (ctx.instr.op == il::core::Opcode::Call) {
        if (auto result = checkRuntimeArrayCall(ctx); !result)
            return result;
    }

    // Resolve callee name and argument slice depending on opcode kind.
    std::string calleeName;
    size_t argStart = 0;
    if (ctx.instr.op == il::core::Opcode::Call) {
        calleeName = ctx.instr.callee;
        argStart = 0;
    } else if (ctx.instr.op == il::core::Opcode::CallIndirect) {
        if (ctx.instr.operands.empty())
            return fail(ctx, "call.indirect missing callee operand");
        const auto &calleeVal = ctx.instr.operands[0];
        if (calleeVal.kind == il::core::Value::Kind::GlobalAddr) {
            calleeName = calleeVal.str;
            argStart = 1;
        } else {
            const auto calleeType = ctx.types.valueType(calleeVal).kind;
            if (calleeType != il::core::Type::Kind::Ptr) {
                return fail(ctx,
                            std::string("call.indirect callee must be ptr but got ") +
                                kindToString(calleeType));
            }
            if (ctx.instr.hasIndirectSignature) {
                const size_t provided =
                    ctx.instr.operands.size() > 0 ? ctx.instr.operands.size() - 1 : 0;
                const size_t fixed = ctx.instr.indirectParamTypes.size();
                const bool badCount =
                    ctx.instr.indirectIsVarArg ? provided < fixed : provided != fixed;
                if (badCount) {
                    std::ostringstream ss;
                    ss << "call.indirect arg count mismatch: signature expects ";
                    if (ctx.instr.indirectIsVarArg)
                        ss << "at least ";
                    ss << fixed << " argument" << (fixed == 1 ? "" : "s") << " but got "
                       << provided;
                    return fail(ctx, ss.str());
                }
                for (size_t i = 0; i < fixed; ++i) {
                    const auto expected = ctx.instr.indirectParamTypes[i].kind;
                    const auto actual = ctx.types.valueType(ctx.instr.operands[1 + i]).kind;
                    if (!typeKindsCompatible(actual, expected)) {
                        std::ostringstream ss;
                        ss << "call.indirect arg type mismatch: signature parameter " << i
                           << " expects " << kindToString(expected) << " but got "
                           << kindToString(actual);
                        return fail(ctx, ss.str());
                    }
                }
                for (size_t i = fixed; i < provided; ++i) {
                    const auto actual = ctx.types.valueType(ctx.instr.operands[1 + i]).kind;
                    if (!isSupportedVarArgType(actual)) {
                        std::ostringstream ss;
                        ss << "call.indirect vararg type mismatch: argument " << i
                           << " has unsupported type " << kindToString(actual);
                        return fail(ctx, ss.str());
                    }
                }
                if (ctx.instr.indirectRetType.kind == Type::Kind::Void) {
                    if (ctx.instr.result)
                        return fail(ctx, "call.indirect to void signature must not have a result");
                } else {
                    if (!ctx.instr.result)
                        return fail(ctx, "call.indirect to non-void signature requires a result");
                    if (!typeKindsCompatible(ctx.instr.type.kind, ctx.instr.indirectRetType.kind)) {
                        std::ostringstream ss;
                        ss << "call.indirect return type mismatch: signature returns "
                           << kindToString(ctx.instr.indirectRetType.kind)
                           << " but instruction declares " << kindToString(ctx.instr.type.kind);
                        return fail(ctx, ss.str());
                    }
                    ctx.types.recordResult(ctx.instr, ctx.instr.indirectRetType);
                }
                return {};
            }

            // Legacy pointer-based indirect call without signature metadata.
            return fail(ctx, "call.indirect through pointer requires an explicit signature");
        }
    } else {
        // Not a call; defer to default checker.
        return {};
    }

    const Extern *externSig = nullptr;
    const Function *fnSig = nullptr;

    if (auto it = ctx.externs.find(calleeName); it != ctx.externs.end())
        externSig = it->second;
    else if (auto itFn = ctx.functions.find(calleeName); itFn != ctx.functions.end())
        fnSig = itFn->second;

    // Runtime helper metadata is authoritative for helper attributes and is
    // used for signatures when no explicit extern/function declaration exists.
    const il::runtime::RuntimeSignature *runtimeSig = il::runtime::findRuntimeSignature(calleeName);
    if (!externSig && !fnSig) {
        if (!runtimeSig) {
            return fail(ctx, std::string("unknown callee @") + calleeName);
        }
    }

    if (ctx.instr.op == il::core::Opcode::Call) {
        const CalleeEffectMetadata effects =
            queryCalleeEffects(externSig, fnSig, runtimeSig, calleeName);

        if (!effects.known && (ctx.instr.CallAttr.pure || ctx.instr.CallAttr.readonly ||
                               ctx.instr.CallAttr.nothrow)) {
            return fail(ctx, "call attributes require callee effect metadata");
        }

        if (effects.known) {
            if (ctx.instr.CallAttr.pure && !effects.pure)
                return fail(ctx, "call pure attribute contradicts callee metadata");
            if (ctx.instr.CallAttr.readonly && !(effects.readonly || effects.pure))
                return fail(ctx, "call readonly attribute contradicts callee metadata");
            if (ctx.instr.CallAttr.nothrow && !effects.nothrow)
                return fail(ctx, "call nothrow attribute contradicts callee metadata");
        }
    }

    // Determine parameter count and validate argument count.
    size_t paramCount = 0;
    bool isVarArg = false;
    if (externSig)
        paramCount = externSig->params.size();
    else if (fnSig) {
        paramCount = fnSig->params.size();
        isVarArg = fnSig->isVarArg;
    } else if (runtimeSig) {
        paramCount = runtimeSig->paramTypes.size();
        isVarArg = runtimeSig->isVarArg;
    }

    if (ctx.instr.operands.size() < argStart)
        return fail(ctx, "call operand slice starts past operand list");
    const size_t providedArgs = ctx.instr.operands.size() - argStart;
    const bool badArgCount = isVarArg ? (providedArgs < paramCount) : (providedArgs != paramCount);
    if (badArgCount) {
        std::ostringstream ss;
        ss << "call arg count mismatch: @" << calleeName << " expects ";
        if (isVarArg)
            ss << "at least ";
        ss << paramCount << " argument" << (paramCount == 1 ? "" : "s") << " but got "
           << providedArgs;
        return fail(ctx, ss.str());
    }

    // Validate argument types.
    for (size_t i = 0; i < paramCount; ++i) {
        Type expected;
        if (externSig)
            expected = externSig->params[i];
        else if (fnSig)
            expected = fnSig->params[i].type;
        else if (runtimeSig)
            expected = runtimeSig->paramTypes[i];

        const auto actualKind = ctx.types.valueType(ctx.instr.operands[argStart + i]).kind;
        if (!runtimeArgTypeCompatible(actualKind, expected.kind, runtimeSig, i)) {
            std::ostringstream ss;
            ss << "call arg type mismatch: @" << calleeName << " parameter " << i << " expects "
               << kindToString(expected.kind) << " but got " << kindToString(actualKind);
            return fail(ctx, ss.str());
        }
    }
    for (size_t i = paramCount; i < providedArgs; ++i) {
        const auto actualKind = ctx.types.valueType(ctx.instr.operands[argStart + i]).kind;
        if (!isSupportedVarArgType(actualKind)) {
            std::ostringstream ss;
            ss << "call vararg type mismatch: @" << calleeName << " argument " << i
               << " has unsupported type " << kindToString(actualKind);
            return fail(ctx, ss.str());
        }
    }

    // Determine expected return type.
    Type retType;
    if (externSig)
        retType = externSig->retType;
    else if (fnSig)
        retType = fnSig->retType;
    else if (runtimeSig)
        retType = runtimeSig->retType;

    if (ctx.instr.op == il::core::Opcode::CallIndirect && ctx.instr.hasIndirectSignature) {
        if (!typeKindsCompatible(ctx.instr.indirectRetType.kind, retType.kind)) {
            std::ostringstream ss;
            ss << "call.indirect signature return mismatch: @" << calleeName << " returns "
               << kindToString(retType.kind) << " but signature declares "
               << kindToString(ctx.instr.indirectRetType.kind);
            return fail(ctx, ss.str());
        }
        if (ctx.instr.indirectIsVarArg != isVarArg) {
            return fail(ctx, "call.indirect signature variadic flag mismatch");
        }
        if (ctx.instr.indirectParamTypes.size() != paramCount) {
            std::ostringstream ss;
            ss << "call.indirect signature arg count mismatch: @" << calleeName << " expects "
               << paramCount << " parameter" << (paramCount == 1 ? "" : "s")
               << " but signature declares " << ctx.instr.indirectParamTypes.size();
            return fail(ctx, ss.str());
        }
        for (size_t i = 0; i < paramCount; ++i) {
            Type expected;
            if (externSig)
                expected = externSig->params[i];
            else if (fnSig)
                expected = fnSig->params[i].type;
            else if (runtimeSig)
                expected = runtimeSig->paramTypes[i];
            if (!typeKindsCompatible(ctx.instr.indirectParamTypes[i].kind, expected.kind)) {
                std::ostringstream ss;
                ss << "call.indirect signature parameter " << i << " mismatch: @" << calleeName
                   << " expects " << kindToString(expected.kind) << " but signature declares "
                   << kindToString(ctx.instr.indirectParamTypes[i].kind);
                return fail(ctx, ss.str());
            }
        }
    }

    // Validate return type consistency.
    if (retType.kind == Type::Kind::Void) {
        // Void-returning call should not have a result.
        if (ctx.instr.result) {
            std::ostringstream ss;
            ss << "call to void @" << calleeName << " must not have a result";
            return fail(ctx, ss.str());
        }
    } else {
        // Non-void call should have a result.
        if (!ctx.instr.result) {
            if (runtimeSig && runtimeReturnMustBeBound(calleeName, retType)) {
                std::ostringstream ss;
                ss << "discarding owned return from @" << calleeName
                   << " requires binding the result";
                return fail(ctx, ss.str());
            }
        } else {
            // Validate that the declared instruction type matches the return type.
            if (ctx.instr.type.kind != Type::Kind::Void &&
                !typeKindsCompatible(ctx.instr.type.kind, retType.kind)) {
                std::ostringstream ss;
                ss << "call return type mismatch: @" << calleeName << " returns "
                   << kindToString(retType.kind) << " but instruction declares "
                   << kindToString(ctx.instr.type.kind);
                return fail(ctx, ss.str());
            }
            // Record the result type for type inference.
            ctx.types.recordResult(ctx.instr, retType);
        }
    }

    return {};
}

/// @brief Verify the @c trap.kind intrinsic.
/// @details Accepts an optional @c error operand naming the record to inspect
///          and records the result type as @c i64 so subsequent instructions can
///          reason about the produced trap code.
/// @param ctx Verification context for the @c trap.kind instruction.
/// @return Success when the instruction shape is valid; otherwise an error.
Expected<void> checkTrapKind(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() > 1)
        return fail(ctx, "trap.kind takes at most one operand");
    if (!ctx.instr.operands.empty()) {
        bool missing = false;
        const Type operandType = ctx.types.valueType(ctx.instr.operands.front(), &missing);
        if (missing)
            return fail(ctx, "trap.kind operand type is unknown");
        if (operandType.kind != Type::Kind::Error)
            return fail(ctx, "trap.kind operand must be error");
    }

    ctx.types.recordResult(ctx.instr, Type(Type::Kind::I64));
    return {};
}

/// @brief Verify the @c trap.err intrinsic.
/// @details Checks that two operands are provided, validates their inferred
///          types (@c i32 error code and @c str message), and records the
///          resulting @c error type in the type lattice.
/// @param ctx Verification context for the @c trap.err instruction.
/// @return Success when operand counts and types are correct.
Expected<void> checkTrapErr(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() != 2)
        return fail(ctx, "trap.err expects code and text operands");

    const auto codeType = ctx.types.valueType(ctx.instr.operands[0]).kind;
    if (codeType != Type::Kind::I32)
        return fail(ctx, "trap.err code must be i32");

    const auto textType = ctx.types.valueType(ctx.instr.operands[1]).kind;
    if (textType != Type::Kind::Str)
        return fail(ctx, "trap.err text must be str");

    ctx.types.recordResult(ctx.instr, Type(Type::Kind::Error));
    return {};
}

/// @brief Verify the @c trap.from_err intrinsic.
/// @details Ensures a single @c i32 operand is provided either as a temporary or
///          as an in-range integer constant, and checks that the instruction's
///          declared type is also @c i32.  This guards the runtime error bridge
///          against invalid conversions.
/// @param ctx Verification context for the @c trap.from_err instruction.
/// @return Success when operand and result types satisfy the intrinsic
///         contract.
Expected<void> checkTrapFromErr(const VerifyCtx &ctx) {
    if (ctx.instr.operands.size() != 1)
        return fail(ctx, "invalid operand count");

    if (ctx.instr.type.kind != Type::Kind::I32)
        return fail(ctx, "trap.from_err expects i32 type");

    const auto &operand = ctx.instr.operands.front();
    if (operand.kind == il::core::Value::Kind::Temp) {
        if (ctx.types.valueType(operand).kind != Type::Kind::I32)
            return fail(ctx, "trap.from_err operand must be i32");
    } else if (operand.kind == il::core::Value::Kind::ConstInt) {
        if (operand.i64 < std::numeric_limits<int32_t>::min() ||
            operand.i64 > std::numeric_limits<int32_t>::max())
            return fail(ctx, "trap.from_err constant out of range");
    } else {
        return fail(ctx, "trap.from_err operand must be i32");
    }

    return {};
}

} // namespace il::verify::checker
