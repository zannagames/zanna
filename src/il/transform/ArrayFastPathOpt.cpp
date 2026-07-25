//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/ArrayFastPathOpt.cpp
// Purpose: Rewrite checked numeric array helpers to fast helpers when an IL
//          bounds proof dominates the access.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements bounds-proof-driven numeric array fast-path selection.
/// @details The pass recognizes lowered out-of-bounds guards and explicit
///          `idx.chk` instructions, tracks facts until memory or ownership
///          effects invalidate them, and rewrites only dominated accesses.

#include "il/transform/ArrayFastPathOpt.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"
#include "il/transform/CallEffects.hpp"

#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace il::core;

namespace il::transform {
namespace {

/// @brief Records that one array/index pair has passed a bounds check.
struct BoundsFact {
    /// Array payload value covered by the proof.
    Value array;
    /// Index value covered by the proof.
    Value index;
};

/// @brief Test whether a callee returns a supported numeric array length.
/// @param callee Direct runtime helper name.
/// @return True for I32, I64, or F64 array length helpers.
[[nodiscard]] bool isNumericArrayLen(std::string_view callee) {
    return callee == "rt_arr_i32_len" || callee == "rt_arr_i64_len" || callee == "rt_arr_f64_len";
}

/// @brief Map a checked numeric array accessor to its unchecked equivalent.
/// @param callee Direct runtime helper name.
/// @return Fast helper name, or an empty view when no rewrite exists.
[[nodiscard]] std::string_view fastArrayHelper(std::string_view callee) {
    if (callee == "rt_arr_i32_get")
        return "rt_arr_i32_get_fast";
    if (callee == "rt_arr_i32_set")
        return "rt_arr_i32_set_fast";
    if (callee == "rt_arr_i64_get")
        return "rt_arr_i64_get_fast";
    if (callee == "rt_arr_i64_set")
        return "rt_arr_i64_set_fast";
    if (callee == "rt_arr_f64_get")
        return "rt_arr_f64_get_fast";
    if (callee == "rt_arr_f64_set")
        return "rt_arr_f64_set_fast";
    return {};
}

/// @brief Test whether a helper preserves established array bounds facts.
/// @param callee Direct runtime helper name.
/// @return True for length queries and non-resizing numeric accessors.
[[nodiscard]] bool isBoundsPreservingArrayHelper(std::string_view callee) {
    return isNumericArrayLen(callee) || callee == "rt_arr_i32_get" ||
           callee == "rt_arr_i32_get_fast" || callee == "rt_arr_i32_set" ||
           callee == "rt_arr_i32_set_fast" || callee == "rt_arr_i64_get" ||
           callee == "rt_arr_i64_get_fast" || callee == "rt_arr_i64_set" ||
           callee == "rt_arr_i64_set_fast" || callee == "rt_arr_f64_get" ||
           callee == "rt_arr_f64_get_fast" || callee == "rt_arr_f64_set" ||
           callee == "rt_arr_f64_set_fast";
}

/// @brief Determine whether an instruction invalidates active bounds facts.
/// @details Stores always invalidate. Calls invalidate unless they are known
///          numeric accessors or their metadata proves memory reordering safe
///          with no ownership effects.
/// @param instr Instruction to classify.
/// @return True when subsequent array accesses require a new proof.
[[nodiscard]] bool mayInvalidateBoundsFacts(const Instr &instr) {
    if (instr.op == Opcode::Store)
        return true;

    if (instr.op != Opcode::Call)
        return false;

    if (isBoundsPreservingArrayHelper(instr.callee))
        return false;

    const CallEffects effects = classifyCallEffects(instr);
    return !effects.canReorderWithMemory() || effects.hasOwnershipEffects();
}

/// @brief Resolve a temporary value to its defining instruction.
/// @param defs Function-wide temporary-definition map.
/// @param value Candidate temporary.
/// @return Borrowed defining instruction, or null for constants and unknown IDs.
[[nodiscard]] const Instr *findDef(const std::unordered_map<unsigned, const Instr *> &defs,
                                   const Value &value) {
    if (value.kind != Value::Kind::Temp)
        return nullptr;
    auto it = defs.find(value.id);
    return it == defs.end() ? nullptr : it->second;
}

/// @brief Peel boolean widen/narrow instructions from a condition value.
/// @param defs Function-wide temporary-definition map.
/// @param value Condition value to normalize.
/// @return First value not defined by `zext1` or `trunc1`.
[[nodiscard]] Value stripBoolWidening(const std::unordered_map<unsigned, const Instr *> &defs,
                                      Value value) {
    while (value.kind == Value::Kind::Temp) {
        const Instr *def = findDef(defs, value);
        if (!def || (def->op != Opcode::Zext1 && def->op != Opcode::Trunc1) ||
            def->operands.empty()) {
            break;
        }
        value = def->operands.front();
    }
    return value;
}

/// @brief Match the `index < 0` half of a lowered bounds guard.
/// @param instr Candidate signed comparison.
/// @param index Receives the guarded index on success.
/// @return True when @p instr has the expected lower-bound shape.
[[nodiscard]] bool matchLowerBoundCheck(const Instr &instr, Value &index) {
    if (instr.operands.size() != 2)
        return false;
    if (instr.op == Opcode::SCmpLT && instr.operands[1].kind == Value::Kind::ConstInt &&
        instr.operands[1].i64 == 0) {
        index = instr.operands[0];
        return true;
    }
    return false;
}

/// @brief Match the `index >= array.length` half of a lowered bounds guard.
/// @param instr Candidate comparison.
/// @param lenArray Mapping from length temporaries to source arrays.
/// @param array Receives the guarded array on success.
/// @param index Receives the guarded index on success.
/// @return True when the upper-bound shape and length definition match.
[[nodiscard]] bool matchUpperBoundCheck(const Instr &instr,
                                        const std::unordered_map<unsigned, Value> &lenArray,
                                        Value &array,
                                        Value &index) {
    if (instr.operands.size() != 2)
        return false;
    if (instr.op != Opcode::SCmpGE && instr.op != Opcode::UCmpGE)
        return false;
    if (instr.operands[1].kind != Value::Kind::Temp)
        return false;

    auto it = lenArray.find(instr.operands[1].id);
    if (it == lenArray.end())
        return false;

    array = it->second;
    index = instr.operands[0];
    return true;
}

/// @brief Recover an array/index proof from a lowered out-of-bounds condition.
/// @details Accepts optional boolean widening and comparison-to-zero wrappers
///          around the disjunction of lower- and upper-bound failures.
/// @param defs Function-wide temporary-definition map.
/// @param lenArray Mapping from length temporaries to source arrays.
/// @param condition Conditional-branch value to inspect.
/// @return Matched fact when both comparisons guard the same index.
[[nodiscard]] std::optional<BoundsFact> matchOobCondition(
    const std::unordered_map<unsigned, const Instr *> &defs,
    const std::unordered_map<unsigned, Value> &lenArray,
    Value condition) {
    condition = stripBoolWidening(defs, condition);
    const Instr *root = findDef(defs, condition);
    if (!root)
        return std::nullopt;

    Value disjunction = condition;
    if (root->op == Opcode::ICmpNe && root->operands.size() == 2 &&
        root->operands[1].kind == Value::Kind::ConstInt && root->operands[1].i64 == 0) {
        disjunction = stripBoolWidening(defs, root->operands[0]);
        root = findDef(defs, disjunction);
    }

    if (!root || root->op != Opcode::Or || root->operands.size() != 2)
        return std::nullopt;

    Value lowerIndex;
    Value upperArray;
    Value upperIndex;
    bool sawLower = false;
    bool sawUpper = false;

    for (const Value &rawOperand : root->operands) {
        const Value operand = stripBoolWidening(defs, rawOperand);
        const Instr *cmp = findDef(defs, operand);
        if (!cmp)
            continue;
        Value candidateIndex;
        if (matchLowerBoundCheck(*cmp, candidateIndex)) {
            lowerIndex = candidateIndex;
            sawLower = true;
            continue;
        }
        Value candidateArray;
        if (matchUpperBoundCheck(*cmp, lenArray, candidateArray, candidateIndex)) {
            upperArray = candidateArray;
            upperIndex = candidateIndex;
            sawUpper = true;
        }
    }

    if (!sawLower || !sawUpper || !valueEquals(lowerIndex, upperIndex))
        return std::nullopt;

    return BoundsFact{upperArray, upperIndex};
}

/// @brief Test whether a fact set contains an equivalent array/index pair.
/// @param facts Active bounds facts.
/// @param array Array value to match.
/// @param index Index value to match.
/// @return True when both values equal a stored fact.
[[nodiscard]] bool hasFact(const std::vector<BoundsFact> &facts,
                           const Value &array,
                           const Value &index) {
    for (const auto &fact : facts)
        if (valueEquals(fact.array, array) && valueEquals(fact.index, index))
            return true;
    return false;
}

/// @brief Add a bounds fact unless an equivalent fact is already present.
/// @param facts Fact set updated in place.
/// @param fact New array/index proof.
void addFact(std::vector<BoundsFact> &facts, BoundsFact fact) {
    if (!hasFact(facts, fact.array, fact.index))
        facts.push_back(std::move(fact));
}

} // namespace

/// @brief Return the pass registry identifier.
/// @return Stable `array-fastpath` identifier.
std::string_view ArrayFastPathOpt::id() const {
    return "array-fastpath";
}

/// @brief Rewrite dominated checked numeric array accesses to fast helpers.
/// @param function Function analyzed and updated in place.
/// @param analysis Unused analysis manager; this pass derives local facts.
/// @return All analyses when unchanged, otherwise preservation of module, CFG,
///         dominator, and loop structure.
PreservedAnalyses ArrayFastPathOpt::run(Function &function, AnalysisManager & /*analysis*/) {
    std::unordered_map<unsigned, const Instr *> defs;
    std::unordered_map<unsigned, Value> lenArray;
    std::unordered_map<std::string, unsigned> predCounts;

    for (const auto &block : function.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.result)
                defs[*instr.result] = &instr;
            if (instr.op == Opcode::Call && instr.result && instr.operands.size() == 1 &&
                isNumericArrayLen(instr.callee)) {
                lenArray[*instr.result] = instr.operands.front();
            }
        }
        if (!block.instructions.empty()) {
            const Instr &term = block.instructions.back();
            for (const auto &label : term.labels)
                ++predCounts[label];
        }
    }

    std::unordered_map<std::string, std::vector<BoundsFact>> entryFacts;
    for (const auto &block : function.blocks) {
        if (block.instructions.empty())
            continue;
        const Instr &term = block.instructions.back();
        if (term.op != Opcode::CBr || term.operands.empty() || term.labels.size() != 2)
            continue;
        auto fact = matchOobCondition(defs, lenArray, term.operands.front());
        if (!fact)
            continue;

        const std::string &okLabel = term.labels[1];
        if (predCounts[okLabel] == 1)
            addFact(entryFacts[okLabel], *fact);
    }

    bool changed = false;
    for (auto &block : function.blocks) {
        std::vector<BoundsFact> activeFacts;
        auto entryIt = entryFacts.find(block.label);
        if (entryIt != entryFacts.end())
            activeFacts = entryIt->second;

        for (auto &instr : block.instructions) {
            if (instr.op == Opcode::IdxChk && instr.operands.size() == 3 &&
                instr.operands[1].kind == Value::Kind::ConstInt && instr.operands[1].i64 == 0 &&
                instr.operands[2].kind == Value::Kind::Temp) {
                auto lenIt = lenArray.find(instr.operands[2].id);
                if (lenIt != lenArray.end())
                    addFact(activeFacts, BoundsFact{lenIt->second, instr.operands[0]});
                continue;
            }

            if (instr.op == Opcode::Call) {
                const std::string_view fast = fastArrayHelper(instr.callee);
                if (!fast.empty() && instr.operands.size() >= 2 &&
                    hasFact(activeFacts, instr.operands[0], instr.operands[1])) {
                    instr.setDirectCallee(std::string(fast));
                    changed = true;
                }
            }

            if (mayInvalidateBoundsFacts(instr))
                activeFacts.clear();
        }
    }

    if (!changed)
        return PreservedAnalyses::all();

    PreservedAnalyses preserved;
    preserved.preserveAllModules();
    preserved.preserveCFG();
    preserved.preserveDominators();
    preserved.preserveLoopInfo();
    return preserved;
}

/// @brief Register the array fast-path function-pass factory.
/// @param registry Pass registry updated with the `array-fastpath` entry.
void registerArrayFastPathOptPass(PassRegistry &registry) {
    /// Construct a fresh pass instance for each pipeline request.
    registry.registerFunctionPass(
        "array-fastpath", []() { return std::make_unique<ArrayFastPathOpt>(); }, true);
}

} // namespace il::transform
