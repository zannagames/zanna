//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/IfConvert.cpp
// Purpose: Fold small branch diamonds and triangles into `select` so backends
//          emit conditional moves (csel/cmov) instead of branches.
// Key invariants:
//   - Arm blocks are speculated only when every instruction is pure,
//     non-trapping, and produces a result (at most kMaxSpeculatedPerArm).
//   - Arm blocks must have the converting block as their only predecessor,
//     take no block parameters, and end in an unconditional branch to a
//     common join block.
// Ownership/Lifetime:
//   - Rewrites the function in place; erased arm blocks are removed from the
//     function's block list.
// Links: docs/adr/0063-il-select-and-if-conversion.md
//
//===----------------------------------------------------------------------===//

#include "il/transform/IfConvert.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

using namespace il::core;

namespace il::transform {
namespace {

/// @brief Upper bound on instructions speculated out of one arm.
constexpr std::size_t kMaxSpeculatedPerArm = 3;

/// @brief Return true when @p instr may be executed unconditionally.
/// @details Requires a pure result-producing instruction that cannot trap:
///          the opcode table's side-effect flag rejects stores, calls, and
///          every checked/overflow form, and the explicit list rejects the
///          remaining faulting or state-touching operations.
[[nodiscard]] bool isSpeculatable(const Instr &instr) {
    if (!instr.result)
        return false;
    if (getOpcodeInfo(instr.op).hasSideEffects)
        return false;
    switch (instr.op) {
        case Opcode::Load: // may fault on a bad pointer
        case Opcode::SDiv: // division traps
        case Opcode::UDiv:
        case Opcode::SRem:
        case Opcode::URem:
        case Opcode::Alloca: // stack-state mutation
        case Opcode::Call:
        case Opcode::CallIndirect:
            return false;
        // Demoted plain arithmetic: these spec-rejected opcodes are accepted
        // only when the verifier re-proves no overflow from operand ranges,
        // and the guarding branch is often what bounds the operand. Hoisting
        // one past its guard makes verified IL unverifiable (crackman
        // pulseColor/chompAngle regression). Bitwise ops stay speculatable —
        // they carry no proof obligation.
        case Opcode::Add:
        case Opcode::Sub:
        case Opcode::Mul:
            return false;
        default:
            return true;
    }
}

/// @brief Return true when @p kind is safe to route through a `select`.
[[nodiscard]] bool isSelectableParamKind(Type::Kind kind) {
    switch (kind) {
        case Type::Kind::I1:
        case Type::Kind::I64:
        case Type::Kind::F64:
        case Type::Kind::Ptr:
        case Type::Kind::Str:
            return true;
        default:
            return false;
    }
}

/// @brief Description of a conversion candidate's arm block.
struct ArmInfo {
    BasicBlock *block{nullptr};       ///< The arm block (null for a triangle's direct edge).
    const std::vector<Value> *args{}; ///< Arguments passed to the join block.
};

/// @brief Return the block's terminator, or null when the block is malformed.
[[nodiscard]] Instr *terminatorOf(BasicBlock &block) {
    if (block.instructions.empty())
        return nullptr;
    return &block.instructions.back();
}

/// @brief Return the local instruction defining @p id, or null if absent.
[[nodiscard]] const Instr *findLocalDef(const BasicBlock &block, unsigned id) {
    for (const auto &instr : block.instructions) {
        if (instr.result && *instr.result == id)
            return &instr;
    }
    return nullptr;
}

/// @brief Return true when a compare can refine integer ranges on cbr edges.
[[nodiscard]] bool compareCarriesRangeFact(const Instr &instr) {
    switch (instr.op) {
        case Opcode::SCmpLT:
        case Opcode::SCmpLE:
        case Opcode::SCmpGT:
        case Opcode::SCmpGE:
        case Opcode::ICmpEq:
            break;
        default:
            return false;
    }

    if (instr.operands.size() != 2)
        return false;
    const bool lhsTemp = instr.operands[0].kind == Value::Kind::Temp;
    const bool rhsTemp = instr.operands[1].kind == Value::Kind::Temp;
    const bool lhsConst = instr.operands[0].kind == Value::Kind::ConstInt;
    const bool rhsConst = instr.operands[1].kind == Value::Kind::ConstInt;
    return (lhsTemp && rhsConst) || (lhsConst && rhsTemp);
}

/// @brief Return true when the conditional branch carries edge range facts.
[[nodiscard]] bool cbrCarriesRangeFacts(const BasicBlock &head, const Instr &term) {
    if (term.operands.empty() || term.operands[0].kind != Value::Kind::Temp)
        return false;
    const Instr *condDef = findLocalDef(head, term.operands[0].id);
    return condDef != nullptr && compareCarriesRangeFact(*condDef);
}

/// @brief Return true when two branch-argument vectors are not identical.
[[nodiscard]] bool argsDiffer(const std::vector<Value> &lhs, const std::vector<Value> &rhs) {
    if (lhs.size() != rhs.size())
        return true;
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        if (!valueEquals(lhs[i], rhs[i]))
            return true;
    }
    return false;
}

/// @brief Check whether @p block qualifies as a speculatable arm to @p join.
[[nodiscard]] bool armQualifies(BasicBlock &block, const std::string &join) {
    if (!block.params.empty())
        return false;
    Instr *term = terminatorOf(block);
    if (term == nullptr || term->op != Opcode::Br || term->labels.size() != 1 ||
        term->labels[0] != join)
        return false;
    if (block.instructions.size() > kMaxSpeculatedPerArm + 1)
        return false;
    for (std::size_t i = 0; i + 1 < block.instructions.size(); ++i) {
        if (!isSpeculatable(block.instructions[i]))
            return false;
    }
    return true;
}

} // namespace

/// @copydoc IfConvert::id()
std::string_view IfConvert::id() const {
    return "if-conv";
}

/// @copydoc IfConvert::run()
PreservedAnalyses IfConvert::run(Function &function, AnalysisManager & /*analysis*/) {
    if (std::getenv("ZANNA_NO_IF_CONVERT") != nullptr)
        return PreservedAnalyses::all();

    bool changed = false;

    // Restart the scan after every conversion: block indices and predecessor
    // counts change when arm blocks are erased.
    bool converted = true;
    while (converted) {
        converted = false;

        std::unordered_map<std::string, std::size_t> blockIndex;
        std::unordered_map<std::string, unsigned> predCount;
        for (std::size_t i = 0; i < function.blocks.size(); ++i) {
            blockIndex[function.blocks[i].label] = i;
        }
        for (const auto &block : function.blocks) {
            if (block.instructions.empty())
                continue;
            for (const auto &label : block.instructions.back().labels)
                ++predCount[label];
        }

        for (std::size_t hi = 0; hi < function.blocks.size() && !converted; ++hi) {
            BasicBlock &head = function.blocks[hi];
            Instr *term = terminatorOf(head);
            if (term == nullptr || term->op != Opcode::CBr || term->labels.size() != 2 ||
                term->operands.empty())
                continue;

            const std::string &trueLabel = term->labels[0];
            const std::string &falseLabel = term->labels[1];

            // Collapsed diamond: both edges reach the same block and differ
            // only in their arguments — the canonical form SimplifyCFG leaves
            // behind. Replace each differing argument with a select and fold
            // the branch into an unconditional one.
            if (trueLabel == falseLabel) {
                if (term->brArgs.size() < 2)
                    continue;
                auto targetIt = blockIndex.find(trueLabel);
                if (targetIt == blockIndex.end())
                    continue;
                BasicBlock &target = function.blocks[targetIt->second];
                const std::vector<Value> &tArgs = term->brArgs[0];
                const std::vector<Value> &fArgs = term->brArgs[1];
                if (tArgs.size() != target.params.size() || fArgs.size() != target.params.size())
                    continue;
                // Compare-driven collapsed branches encode range facts on
                // their individual edges, e.g. clamp shapes such as
                // `x < 0 ? 0 : x`. CheckOpt may already have demoted checked
                // arithmetic downstream using those block-param facts. Until
                // range analysis can recover the same facts from `select`,
                // preserve the branch form instead of erasing the proof.
                if (argsDiffer(tArgs, fArgs) && cbrCarriesRangeFacts(head, *term))
                    continue;
                bool argTypesOk = true;
                for (std::size_t i = 0; i < target.params.size(); ++i) {
                    const bool differs = !valueEquals(tArgs[i], fArgs[i]);
                    if (differs && !isSelectableParamKind(target.params[i].type.kind)) {
                        argTypesOk = false;
                        break;
                    }
                }
                if (!argTypesOk)
                    continue;

                const Value cond = term->operands[0];
                unsigned nextId = zanna::il::nextTempId(function);
                std::vector<Instr> selects;
                std::vector<Value> joinArgs;
                joinArgs.reserve(target.params.size());
                for (std::size_t i = 0; i < target.params.size(); ++i) {
                    const bool same = valueEquals(tArgs[i], fArgs[i]);
                    if (same) {
                        joinArgs.push_back(tArgs[i]);
                        continue;
                    }
                    Instr sel;
                    sel.op = Opcode::Select;
                    sel.type = target.params[i].type;
                    sel.result = nextId++;
                    sel.operands = {cond, tArgs[i], fArgs[i]};
                    sel.loc = term->loc;
                    selects.push_back(std::move(sel));
                    joinArgs.push_back(Value::temp(nextId - 1));
                }

                Instr br;
                br.op = Opcode::Br;
                br.labels = {trueLabel};
                br.brArgs = {std::move(joinArgs)};
                br.loc = term->loc;

                head.instructions.pop_back();
                for (auto &sel : selects)
                    head.instructions.push_back(std::move(sel));
                head.instructions.push_back(std::move(br));

                changed = true;
                converted = true;
                continue;
            }
            auto trueIt = blockIndex.find(trueLabel);
            auto falseIt = blockIndex.find(falseLabel);
            if (trueIt == blockIndex.end() || falseIt == blockIndex.end())
                continue;
            // The hoisting patterns below require paramless arm blocks
            // (armQualifies), and verified IL only passes branch arguments to
            // blocks with matching parameters — so head-to-arm edges are
            // guaranteed argument-free here.

            BasicBlock &trueBlock = function.blocks[trueIt->second];
            BasicBlock &falseBlock = function.blocks[falseIt->second];

            ArmInfo trueArm{};
            ArmInfo falseArm{};
            std::string joinLabel;

            /// Return the argument vector for one conditional edge, treating omission as empty.
            const auto edgeArgs = [&](std::size_t edge) -> const std::vector<Value> * {
                static const std::vector<Value> kEmpty;
                if (edge < term->brArgs.size())
                    return &term->brArgs[edge];
                return &kEmpty;
            };

            if (predCount[trueLabel] == 1 && predCount[falseLabel] == 1 &&
                terminatorOf(trueBlock) != nullptr && terminatorOf(falseBlock) != nullptr &&
                terminatorOf(trueBlock)->op == Opcode::Br &&
                terminatorOf(falseBlock)->op == Opcode::Br &&
                terminatorOf(trueBlock)->labels.size() == 1 &&
                terminatorOf(falseBlock)->labels.size() == 1 &&
                terminatorOf(trueBlock)->labels[0] == terminatorOf(falseBlock)->labels[0]) {
                // Diamond: head -> {T, F} -> join.
                joinLabel = terminatorOf(trueBlock)->labels[0];
                if (!armQualifies(trueBlock, joinLabel) || !armQualifies(falseBlock, joinLabel))
                    continue;
                trueArm = {&trueBlock,
                           terminatorOf(trueBlock)->brArgs.empty()
                               ? nullptr
                               : &terminatorOf(trueBlock)->brArgs[0]};
                falseArm = {&falseBlock,
                            terminatorOf(falseBlock)->brArgs.empty()
                                ? nullptr
                                : &terminatorOf(falseBlock)->brArgs[0]};
            } else if (predCount[trueLabel] == 1 && armQualifies(trueBlock, falseLabel)) {
                // Triangle: head -> T -> join, head -false-> join.
                joinLabel = falseLabel;
                trueArm = {&trueBlock,
                           terminatorOf(trueBlock)->brArgs.empty()
                               ? nullptr
                               : &terminatorOf(trueBlock)->brArgs[0]};
                falseArm = {nullptr, edgeArgs(1)};
            } else if (predCount[falseLabel] == 1 && armQualifies(falseBlock, trueLabel)) {
                // Triangle: head -> F -> join, head -true-> join.
                joinLabel = trueLabel;
                falseArm = {&falseBlock,
                            terminatorOf(falseBlock)->brArgs.empty()
                                ? nullptr
                                : &terminatorOf(falseBlock)->brArgs[0]};
                trueArm = {nullptr, edgeArgs(0)};
            } else {
                continue;
            }

            auto joinIt = blockIndex.find(joinLabel);
            if (joinIt == blockIndex.end() || joinLabel == head.label)
                continue;
            BasicBlock &join = function.blocks[joinIt->second];

            static const std::vector<Value> kNoArgs;
            const std::vector<Value> &trueArgs = trueArm.args ? *trueArm.args : kNoArgs;
            const std::vector<Value> &falseArgs = falseArm.args ? *falseArm.args : kNoArgs;
            if (trueArgs.size() != join.params.size() || falseArgs.size() != join.params.size())
                continue;

            bool typesOk = true;
            for (const auto &param : join.params) {
                if (!isSelectableParamKind(param.type.kind)) {
                    typesOk = false;
                    break;
                }
            }
            if (!typesOk)
                continue;

            // --- Commit the conversion ---
            const Value cond = term->operands[0];
            unsigned nextId = zanna::il::nextTempId(function);

            std::vector<Instr> hoisted;
            /// Append the non-terminator instructions from a converted arm in execution order.
            const auto hoistArm = [&](BasicBlock *arm) {
                if (arm == nullptr)
                    return;
                for (std::size_t i = 0; i + 1 < arm->instructions.size(); ++i)
                    hoisted.push_back(arm->instructions[i]);
            };
            hoistArm(trueArm.block);
            hoistArm(falseArm.block);

            std::vector<Value> joinArgs;
            joinArgs.reserve(join.params.size());
            for (std::size_t i = 0; i < join.params.size(); ++i) {
                if (valueEquals(trueArgs[i], falseArgs[i])) {
                    joinArgs.push_back(trueArgs[i]);
                    continue;
                }
                Instr sel;
                sel.op = Opcode::Select;
                sel.type = join.params[i].type;
                sel.result = nextId++;
                sel.operands = {cond, trueArgs[i], falseArgs[i]};
                sel.loc = term->loc;
                hoisted.push_back(std::move(sel));
                joinArgs.push_back(Value::temp(nextId - 1));
            }

            Instr br;
            br.op = Opcode::Br;
            br.labels = {joinLabel};
            br.brArgs = {std::move(joinArgs)};
            br.loc = term->loc;

            head.instructions.pop_back();
            for (auto &instr : hoisted)
                head.instructions.push_back(std::move(instr));
            head.instructions.push_back(std::move(br));

            // Erase arm blocks (highest index first so the other stays valid).
            std::vector<std::size_t> toErase;
            if (trueArm.block != nullptr)
                toErase.push_back(trueIt->second);
            if (falseArm.block != nullptr)
                toErase.push_back(falseIt->second);
            std::sort(toErase.begin(), toErase.end(), std::greater<std::size_t>());
            for (std::size_t idx : toErase)
                function.blocks.erase(function.blocks.begin() + static_cast<std::ptrdiff_t>(idx));

            changed = true;
            converted = true;
        }
    }

    if (!changed)
        return PreservedAnalyses::all();
    return PreservedAnalyses::none();
}

/// @copydoc registerIfConvertPass()
void registerIfConvertPass(PassRegistry &registry) {
    registry.registerFunctionPass("if-conv", []() { return std::make_unique<IfConvert>(); }, true);
}

} // namespace il::transform
