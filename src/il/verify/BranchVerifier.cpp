//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/verify/BranchVerifier.cpp
// Purpose: Implement verification helpers that enforce branch argument and
//          return-value correctness across IL functions.
// Key invariants: Branch instructions must forward arguments that align with
//                 their destination block parameters; return instructions must
//                 respect the containing function signature.
// Ownership/Lifetime: Operates entirely on caller-owned IL structures and
//                     emits diagnostics without caching long-lived state.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements branch-edge payload and return-value verification.
/// @details The helpers validate terminator-local structure and inferred value
///          compatibility. They consult a caller-built block map but defer
///          diagnostics for unresolved labels to the dedicated control-flow
///          checks.

#include "il/verify/BranchVerifier.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Type.hpp"
#include "il/verify/DiagFormat.hpp"
#include "il/verify/InstructionCheckUtils.hpp"
#include "il/verify/TypeInference.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace il::core;

namespace il::verify {
using il::support::Expected;
using il::support::makeError;

namespace {

/// @brief Test whether a value can satisfy an expected IL type.
/// @details Exact inferred kinds are accepted. Integer constants may also
///          satisfy a different integer kind when their value is representable;
///          boolean constants remain restricted to i1 and non-boolean integers
///          are never implicitly accepted as i1.
/// @param value Source value whose constant metadata may permit compatibility.
/// @param actualType Type inferred for @p value.
/// @param expectedType Type required by the consuming position.
/// @return `true` when the value may be consumed as @p expectedType.
bool valueCompatibleWithType(const Value &value, const Type &actualType, const Type &expectedType) {
    if (actualType.kind == expectedType.kind)
        return true;
    if (value.kind == Value::Kind::ConstInt) {
        if (value.isBool)
            return expectedType.kind == Type::Kind::I1;
        if (expectedType.kind == Type::Kind::I1)
            return false;
        return detail::fitsInIntegerKind(value.i64, expectedType.kind);
    }
    return false;
}

/// @brief Validate the argument bundle transferred along a branch edge.
///
/// @details Confirms that the argument count matches the destination block's
///          parameter list and that each operand's inferred type lines up with
///          the corresponding parameter type. When a mismatch is detected the
///          helper emits a diagnostic anchored to @p instr so callers can
///          surface a precise error to users.
///
/// @param fn Function containing the branch instruction.
/// @param bb Basic block holding @p instr.
/// @param instr Branch-like instruction whose arguments are validated.
/// @param target Destination block receiving the branch arguments.
/// @param args Optional branch argument vector aligned with @p target params.
/// @param label Label string associated with the evaluated branch edge.
/// @param types Inference context used to obtain operand types for comparison.
/// @return Success when counts and types agree, otherwise the first diagnostic.
Expected<void> verifyBranchArgs(const Function &fn,
                                const BasicBlock &bb,
                                const Instr &instr,
                                const BasicBlock &target,
                                const std::vector<Value> *args,
                                std::string_view label,
                                TypeInference &types) {
    size_t argCount = args ? args->size() : 0;
    if (argCount != target.params.size())
        return Expected<void>{makeError(
            instr.loc,
            formatInstrDiag(
                fn, bb, instr, "branch arg count mismatch for label " + std::string(label)))};

    for (size_t i = 0; i < argCount; ++i) {
        bool missing = false;
        auto argType = types.valueType((*args)[i], &missing);
        if (missing) {
            return Expected<void>{makeError(
                instr.loc,
                formatInstrDiag(
                    fn, bb, instr, "unknown branch arg for label " + std::string(label)))};
        }
        if (argType.kind == Type::Kind::Void) {
            return Expected<void>{makeError(
                instr.loc,
                formatInstrDiag(fn, bb, instr, "void branch arg for label " + std::string(label)))};
        }
        if (!valueCompatibleWithType((*args)[i], argType, target.params[i].type)) {
            return Expected<void>{
                makeError(instr.loc,
                          formatInstrDiag(
                              fn, bb, instr, "arg type mismatch for label " + std::string(label)))};
        }
    }
    return {};
}

} // namespace

/// @brief Ensure an unconditional branch references a valid target and payload.
///
/// @details Checks structural properties of the `br` instruction—no operands
///          beyond explicit branch arguments and exactly one label—and then
///          invokes @ref verifyBranchArgs to confirm per-parameter compatibility
///          with the resolved target block. Diagnostics are emitted through the
///          `Expected` channel when any constraint is violated.
///
/// @param fn Function currently being verified.
/// @param bb Block containing the unconditional branch.
/// @param instr The branch instruction to verify.
/// @param blockMap Lookup table mapping block labels to their definitions.
/// @param types Type inference cache supplying operand type information.
/// @return Success when local structure and any resolved edge are valid;
///         otherwise the first diagnostic.
Expected<void> verifyBr_E(const Function &fn,
                          const BasicBlock &bb,
                          const Instr &instr,
                          const BlockMap &blockMap,
                          TypeInference &types) {
    bool argsOk = instr.operands.empty() && instr.labels.size() == 1;
    if (!argsOk)
        return Expected<void>{
            makeError(instr.loc, formatInstrDiag(fn, bb, instr, "branch mismatch"))};

    if (auto it = blockMap.find(instr.labels[0]); it != blockMap.end()) {
        const BasicBlock &target = *it->second;
        const std::vector<Value> *argsVec = !instr.brArgs.empty() ? &instr.brArgs[0] : nullptr;
        if (auto result = verifyBranchArgs(fn, bb, instr, target, argsVec, instr.labels[0], types);
            !result)
            return result;
    }

    return {};
}

/// @brief Validate conditional branch structure, condition type, and edge payloads.
///
/// @details Requires a single i1 condition operand plus exactly two successor
///          labels, then checks each edge's branch arguments using
///          @ref verifyBranchArgs. Unresolved labels are skipped for the
///          dedicated target-existence checker; malformed structure, unknown
///          conditions, and type mismatches produce a diagnostic tied to
///          @p instr.
///
/// @param fn Function currently being verified.
/// @param bb Block containing the conditional branch.
/// @param instr Conditional branch instruction under validation.
/// @param blockMap Mapping from block labels to their resolved blocks.
/// @param types Type inference cache used for operand type queries.
/// @return Success when local structure and each resolved edge are valid;
///         otherwise the first diagnostic.
Expected<void> verifyCBr_E(const Function &fn,
                           const BasicBlock &bb,
                           const Instr &instr,
                           const BlockMap &blockMap,
                           TypeInference &types) {
    bool missingCond = false;
    const Type condType = instr.operands.size() == 1
                              ? types.valueType(instr.operands[0], &missingCond)
                              : Type(Type::Kind::Void);
    if (missingCond) {
        return Expected<void>{
            makeError(instr.loc, formatInstrDiag(fn, bb, instr, "unknown branch condition"))};
    }
    bool condOk =
        instr.operands.size() == 1 && instr.labels.size() == 2 && condType.kind == Type::Kind::I1;
    if (condOk) {
        for (size_t t = 0; t < 2; ++t) {
            auto it = blockMap.find(instr.labels[t]);
            if (it == blockMap.end())
                continue;
            const BasicBlock &target = *it->second;
            const std::vector<Value> *argsVec =
                instr.brArgs.size() > t ? &instr.brArgs[t] : nullptr;
            if (auto result =
                    verifyBranchArgs(fn, bb, instr, target, argsVec, instr.labels[t], types);
                !result)
                return result;
        }
        return {};
    }

    return Expected<void>{
        makeError(instr.loc, formatInstrDiag(fn, bb, instr, "conditional branch mismatch"))};
}

/// @brief Check switch.i32 instructions for well-formed structure and edge payloads.
///
/// @details Validates that a scrutinee operand exists and is an i32, that the
///          default label and branch argument bundles align, and that each case
///          literal is a unique 32-bit constant.  For every successor,
///          @ref verifyBranchArgs is used to ensure the transferred operands
///          match the destination parameter types.
///
/// @param fn Function currently being verified.
/// @param bb Block containing the switch instruction.
/// @param instr switch.i32 instruction whose structure is examined.
/// @param blockMap Lookup for resolving target blocks referenced by labels.
/// @param types Type inference context providing operand type data.
/// @return Success when the switch and each resolved edge are valid; otherwise
///         the first structural, constant, or type diagnostic.
Expected<void> verifySwitchI32_E(const Function &fn,
                                 const BasicBlock &bb,
                                 const Instr &instr,
                                 const BlockMap &blockMap,
                                 TypeInference &types) {
    if (instr.operands.empty()) {
        return Expected<void>{
            makeError(instr.loc, formatInstrDiag(fn, bb, instr, "switch.i32 missing scrutinee"))};
    }

    bool missingScrutinee = false;
    const Type actualScrutineeType = types.valueType(switchScrutinee(instr), &missingScrutinee);
    if (!valueCompatibleWithType(
            switchScrutinee(instr), actualScrutineeType, Type(Type::Kind::I32))) {
        if (missingScrutinee) {
            return Expected<void>{makeError(
                instr.loc, formatInstrDiag(fn, bb, instr, "unknown switch.i32 scrutinee"))};
        }
        return Expected<void>{makeError(
            instr.loc, formatInstrDiag(fn, bb, instr, "switch.i32 scrutinee must be i32"))};
    }

    if (instr.labels.empty()) {
        return Expected<void>{
            makeError(instr.loc, formatInstrDiag(fn, bb, instr, "switch.i32 missing default"))};
    }

    if (instr.brArgs.size() != instr.labels.size()) {
        return Expected<void>{makeError(
            instr.loc,
            formatInstrDiag(fn, bb, instr, "switch.i32 branch argument vector count mismatch"))};
    }

    const std::vector<Value> *defaultArgs = &instr.brArgs.front();

    const size_t caseCount = switchCaseCount(instr);
    if (instr.operands.size() != caseCount + 1) {
        return Expected<void>{makeError(
            instr.loc, formatInstrDiag(fn, bb, instr, "switch.i32 operands mismatch cases"))};
    }

    if (auto it = blockMap.find(switchDefaultLabel(instr)); it != blockMap.end()) {
        if (auto result = verifyBranchArgs(
                fn, bb, instr, *it->second, defaultArgs, switchDefaultLabel(instr), types);
            !result)
            return result;
    }

    std::unordered_set<int32_t> seen;
    seen.reserve(caseCount);

    for (size_t idx = 0; idx < caseCount; ++idx) {
        const Value &caseValue = switchCaseValue(instr, idx);
        if (caseValue.kind != Value::Kind::ConstInt) {
            return Expected<void>{makeError(
                instr.loc, formatInstrDiag(fn, bb, instr, "switch.i32 case must be const i32"))};
        }
        if (caseValue.i64 < std::numeric_limits<int32_t>::min() ||
            caseValue.i64 > std::numeric_limits<int32_t>::max()) {
            return Expected<void>{makeError(
                instr.loc, formatInstrDiag(fn, bb, instr, "switch.i32 case out of i32 range"))};
        }

        const int32_t key = static_cast<int32_t>(caseValue.i64);
        if (!seen.insert(key).second) {
            return Expected<void>{
                makeError(instr.loc, formatInstrDiag(fn, bb, instr, "duplicate switch.i32 case"))};
        }

        const std::string &label = switchCaseLabel(instr, idx);
        auto it = blockMap.find(label);
        if (it == blockMap.end())
            continue;
        const std::vector<Value> *caseArgs = &instr.brArgs[idx + 1];
        if (auto result = verifyBranchArgs(fn, bb, instr, *it->second, caseArgs, label, types);
            !result)
            return result;
    }

    return {};
}

/// @brief Validate that a `ret` instruction respects the function signature.
///
/// @details Ensures void functions do not return a value and non-void
///          functions return exactly one operand of the declared type.  Any
///          discrepancy produces a diagnostic referencing the offending
///          instruction.
///
/// @param fn Function currently being verified.
/// @param bb Block containing the return instruction.
/// @param instr Return instruction to check.
/// @param types Type inference cache used to resolve operand types.
/// @return Success or a diagnostic describing the mismatch.
Expected<void> verifyRet_E(const Function &fn,
                           const BasicBlock &bb,
                           const Instr &instr,
                           TypeInference &types) {
    if (fn.retType.kind == Type::Kind::Void) {
        if (!instr.operands.empty())
            return Expected<void>{
                makeError(instr.loc, formatInstrDiag(fn, bb, instr, "ret void with value"))};
        return {};
    }

    if (instr.operands.size() != 1)
        return Expected<void>{
            makeError(instr.loc, formatInstrDiag(fn, bb, instr, "ret value type mismatch"))};

    Type actualType = types.valueType(instr.operands[0]);
    if (!valueCompatibleWithType(instr.operands[0], actualType, fn.retType)) {
        std::string message = "ret value type mismatch: expected " + fn.retType.toString() +
                              " but got " + actualType.toString();
        return Expected<void>{makeError(instr.loc, formatInstrDiag(fn, bb, instr, message))};
    }

    return {};
}

} // namespace il::verify
