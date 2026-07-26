//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements a conservative loop unrolling pass that fully unrolls small
// constant-bound loops. The pass identifies loops with a single latch, single
// exit, and a recognizable trip count pattern, then replicates the loop body
// with proper SSA value threading.
//
// The implementation focuses on correctness over aggressive optimization,
// limiting unrolling to loops that meet strict structural requirements.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements conservative full unrolling for statically counted IL loops.
 *
 * @details The implementation validates a supported induction comparison and
 *          checked constant step, simulates a bounded exact trip count, rejects
 *          trapping or effectful body instructions, and clones each iteration
 *          with fresh SSA identities. Final loop-carried values are threaded
 *          directly to the exit before the original loop blocks are removed.
 */

#include "il/transform/LoopUnroll.hpp"

#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"
#include "il/transform/analysis/Liveness.hpp"
#include "il/transform/analysis/LoopInfo.hpp"

#include "il/analysis/Dominators.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"
#include "il/verify/VerifierTable.hpp"

#include <algorithm>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

using namespace il::core;

namespace il::transform {

namespace {
/// @brief Add signed integers while explicitly rejecting overflow.
/// @param lhs Left addend.
/// @param rhs Right addend.
/// @return Sum when representable as `long long`, otherwise `std::nullopt`.
std::optional<long long> checkedAdd(long long lhs, long long rhs) {
    if ((rhs > 0 && lhs > std::numeric_limits<long long>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<long long>::min() - rhs)) {
        return std::nullopt;
    }
    return lhs + rhs;
}

/// @brief Determine whether duplicating an instruction preserves observable behavior.
/// @param instr Non-terminator loop instruction to classify.
/// @return `true` only for pure, non-trapping, non-memory instructions without CFG data.
bool isSafeToCloneForFullUnroll(const Instr &instr) {
    if (zanna::il::isTerminator(instr))
        return false;
    if (const auto props = il::verify::lookup(instr.op); props && props->canTrap)
        return false;
    const auto &info = getOpcodeInfo(instr.op);
    if (info.hasSideEffects)
        return false;
    if (hasMemoryRead(instr.op) || hasMemoryWrite(instr.op))
        return false;
    if (instr.op == Opcode::Alloca || instr.op == Opcode::Call || instr.op == Opcode::CallIndirect)
        return false;
    if (!instr.labels.empty() || !instr.brArgs.empty())
        return false;
    return true;
}

/// @brief Test whether a temporary is defined by a parameter or instruction in a loop.
/// @param function Function containing the loop blocks.
/// @param loop Membership description limiting the scan.
/// @param tempId Temporary id whose definition is sought.
/// @return `true` when @p tempId originates within @p loop.
bool tempDefinedInLoop(const Function &function, const Loop &loop, unsigned tempId) {
    for (const auto &block : function.blocks) {
        if (!loop.contains(block.label))
            continue;
        for (const auto &param : block.params)
            if (param.id == tempId)
                return true;
        for (const auto &instr : block.instructions)
            if (instr.result && *instr.result == tempId)
                return true;
    }
    return false;
}

/// @brief Information about a simple counted loop.
struct CountedLoop {
    /// @brief Initial value of the induction variable.
    long long initValue = 0;
    /// @brief Constant comparison bound for the induction variable.
    long long endValue = 0;
    /// @brief Signed step applied after each iteration.
    long long step = 1;
    /// @brief Index of the induction variable in the header parameters.
    size_t ivParamIndex = 0;
    /// @brief Exact number of body iterations proved by bounded simulation.
    unsigned tripCount = 0;
};

/// @brief Find the unique predecessor outside a loop.
/// @param function Function containing the loop; retained for the common helper interface.
/// @param loop Loop whose header predecessors are filtered.
/// @param header Header block indexed by @p cfg.
/// @param cfg CFG snapshot containing predecessor relationships.
/// @param blockMap Label lookup used to recover mutable block pointers.
/// @return Unique external predecessor, or null if absent or ambiguous.
BasicBlock *findPreheader(Function &function,
                          const Loop &loop,
                          BasicBlock &header,
                          const CFGInfo &cfg,
                          const std::unordered_map<std::string, BasicBlock *> &blockMap) {
    auto predsIt = cfg.predecessors.find(&header);
    if (predsIt == cfg.predecessors.end())
        return nullptr;

    BasicBlock *preheader = nullptr;
    for (const auto *pred : predsIt->second) {
        if (!pred || loop.contains(pred->label))
            continue;
        auto it = blockMap.find(pred->label);
        if (it == blockMap.end())
            continue;
        if (preheader && preheader != it->second)
            return nullptr; // Multiple preheaders
        preheader = it->second;
    }
    return preheader;
}

/// @brief Get the index of a label in a terminator's labels vector.
/// @param term Terminator whose successor slots are searched.
/// @param target Label to locate.
/// @return Matching successor index, or `std::nullopt`.
std::optional<size_t> labelIndex(const Instr &term, const std::string &target) {
    for (size_t i = 0; i < term.labels.size(); ++i)
        if (term.labels[i] == target)
            return i;
    return std::nullopt;
}

/// @brief Analyze a loop to determine whether it has a statically bounded trip count.
/// @param function Function containing the candidate loop.
/// @param loop Loop structure requiring one latch and one header exit.
/// @param header Header expected to compare an induction parameter with a constant.
/// @param latch Latch expected to update that parameter by a constant step.
/// @param preheader Entry predecessor expected to supply a constant initial value.
/// @param blockMap Function block lookup retained for structural context.
/// @return Counted-loop description, or `std::nullopt` for an unsupported or
///         nonterminating pattern.
/// @details The helper simulates at most 1,000 induction steps with checked
///          addition, supporting signed relational and equality comparisons.
std::optional<CountedLoop> analyzeCountedLoop(
    Function &function,
    const Loop &loop,
    BasicBlock &header,
    BasicBlock &latch,
    BasicBlock *preheader,
    const std::unordered_map<std::string, BasicBlock *> &blockMap) {
    // Require single latch
    if (loop.latchLabels.size() != 1)
        return std::nullopt;

    // Require single exit
    if (loop.exits.size() != 1)
        return std::nullopt;

    // Header must have a conditional branch
    if (header.instructions.empty())
        return std::nullopt;
    const Instr &headerTerm = header.instructions.back();
    if (headerTerm.op != Opcode::CBr)
        return std::nullopt;
    if (headerTerm.labels.size() != 2)
        return std::nullopt;

    // Identify which branch goes to exit vs stays in loop
    const std::string &exitTarget = loop.exits[0].to;
    size_t exitBranchIdx = 0;
    if (headerTerm.labels[0] == exitTarget)
        exitBranchIdx = 0;
    else if (headerTerm.labels[1] == exitTarget)
        exitBranchIdx = 1;
    else
        return std::nullopt; // Exit not from header

    // The condition must be a comparison involving a header param
    if (headerTerm.operands.empty())
        return std::nullopt;
    const Value &condVal = headerTerm.operands[0];
    if (condVal.kind != Value::Kind::Temp)
        return std::nullopt;

    // Find the comparison instruction
    const Instr *cmpInstr = nullptr;
    for (const auto &instr : header.instructions) {
        if (instr.result && *instr.result == condVal.id) {
            cmpInstr = &instr;
            break;
        }
    }
    if (!cmpInstr)
        return std::nullopt;

    // Must be a signed comparison
    Opcode cmpOp = cmpInstr->op;
    if (cmpOp != Opcode::SCmpLT && cmpOp != Opcode::SCmpLE && cmpOp != Opcode::SCmpGT &&
        cmpOp != Opcode::SCmpGE && cmpOp != Opcode::ICmpEq && cmpOp != Opcode::ICmpNe)
        return std::nullopt;

    if (cmpInstr->operands.size() != 2)
        return std::nullopt;

    // One operand should be a header param, the other a constant
    const Value &lhs = cmpInstr->operands[0];
    const Value &rhs = cmpInstr->operands[1];

    size_t ivParamIndex = 0;
    long long boundValue = 0;
    bool ivIsLhs = false;

    // Check if LHS is a header param and RHS is constant
    if (lhs.kind == Value::Kind::Temp && rhs.kind == Value::Kind::ConstInt) {
        bool found = false;
        for (size_t i = 0; i < header.params.size(); ++i) {
            if (header.params[i].id == lhs.id) {
                ivParamIndex = i;
                found = true;
                break;
            }
        }
        if (!found)
            return std::nullopt;
        boundValue = rhs.i64;
        ivIsLhs = true;
    } else if (rhs.kind == Value::Kind::Temp && lhs.kind == Value::Kind::ConstInt) {
        bool found = false;
        for (size_t i = 0; i < header.params.size(); ++i) {
            if (header.params[i].id == rhs.id) {
                ivParamIndex = i;
                found = true;
                break;
            }
        }
        if (!found)
            return std::nullopt;
        boundValue = lhs.i64;
        ivIsLhs = false;
    } else {
        return std::nullopt;
    }

    // Get initial value from preheader's branch args
    if (!preheader || preheader->instructions.empty())
        return std::nullopt;
    const Instr &phTerm = preheader->instructions.back();
    auto toHeaderIdx = labelIndex(phTerm, header.label);
    if (!toHeaderIdx || *toHeaderIdx >= phTerm.brArgs.size())
        return std::nullopt;
    const auto &initArgs = phTerm.brArgs[*toHeaderIdx];
    if (ivParamIndex >= initArgs.size())
        return std::nullopt;
    const Value &initVal = initArgs[ivParamIndex];
    if (initVal.kind != Value::Kind::ConstInt)
        return std::nullopt;
    long long initValue = initVal.i64;

    // Find the step in the latch
    if (latch.instructions.empty())
        return std::nullopt;
    const Instr &latchTerm = latch.instructions.back();
    auto toHeaderFromLatch = labelIndex(latchTerm, header.label);
    if (!toHeaderFromLatch || *toHeaderFromLatch >= latchTerm.brArgs.size())
        return std::nullopt;
    const auto &latchArgs = latchTerm.brArgs[*toHeaderFromLatch];
    if (ivParamIndex >= latchArgs.size())
        return std::nullopt;
    const Value &nextVal = latchArgs[ivParamIndex];
    if (nextVal.kind != Value::Kind::Temp)
        return std::nullopt;

    // Find instruction that produces nextVal
    const Instr *stepInstr = nullptr;
    for (const auto &instr : latch.instructions) {
        if (instr.result && *instr.result == nextVal.id) {
            stepInstr = &instr;
            break;
        }
    }
    // Also check header for step instruction
    if (!stepInstr) {
        for (const auto &instr : header.instructions) {
            if (instr.result && *instr.result == nextVal.id) {
                stepInstr = &instr;
                break;
            }
        }
    }
    if (!stepInstr)
        return std::nullopt;

    // Step instruction should be add/sub with the IV param
    long long step = 0;
    if (stepInstr->op == Opcode::Add || stepInstr->op == Opcode::IAddOvf) {
        if (stepInstr->operands.size() != 2)
            return std::nullopt;
        const Value &a = stepInstr->operands[0];
        const Value &b = stepInstr->operands[1];

        // Find which operand is the IV
        unsigned ivId = header.params[ivParamIndex].id;
        bool foundIv = false;

        // Check header->latch to find latch param that receives IV
        auto toLatchIdx = labelIndex(header.instructions.back(), latch.label);
        if (toLatchIdx && *toLatchIdx < header.instructions.back().brArgs.size()) {
            const auto &argsToLatch = header.instructions.back().brArgs[*toLatchIdx];
            for (size_t i = 0; i < argsToLatch.size() && i < latch.params.size(); ++i) {
                if (argsToLatch[i].kind == Value::Kind::Temp && argsToLatch[i].id == ivId) {
                    ivId = latch.params[i].id;
                    break;
                }
            }
        }

        if (a.kind == Value::Kind::Temp && a.id == ivId && b.kind == Value::Kind::ConstInt) {
            step = b.i64;
            foundIv = true;
        } else if (b.kind == Value::Kind::Temp && b.id == ivId && a.kind == Value::Kind::ConstInt) {
            step = a.i64;
            foundIv = true;
        }
        if (!foundIv)
            return std::nullopt;
    } else if (stepInstr->op == Opcode::Sub || stepInstr->op == Opcode::ISubOvf) {
        if (stepInstr->operands.size() != 2)
            return std::nullopt;
        const Value &a = stepInstr->operands[0];
        const Value &b = stepInstr->operands[1];

        unsigned ivId = header.params[ivParamIndex].id;
        auto toLatchIdx = labelIndex(header.instructions.back(), latch.label);
        if (toLatchIdx && *toLatchIdx < header.instructions.back().brArgs.size()) {
            const auto &argsToLatch = header.instructions.back().brArgs[*toLatchIdx];
            for (size_t i = 0; i < argsToLatch.size() && i < latch.params.size(); ++i) {
                if (argsToLatch[i].kind == Value::Kind::Temp && argsToLatch[i].id == ivId) {
                    ivId = latch.params[i].id;
                    break;
                }
            }
        }

        if (a.kind == Value::Kind::Temp && a.id == ivId && b.kind == Value::Kind::ConstInt) {
            if (b.i64 == std::numeric_limits<long long>::min())
                return std::nullopt;
            step = -b.i64;
        } else {
            return std::nullopt;
        }
    } else {
        return std::nullopt;
    }

    if (step == 0)
        return std::nullopt;

    // Compute trip count based on comparison and exit branch
    // exitBranchIdx tells us which branch exits: 0 = true branch exits, 1 = false branch exits
    // The loop continues when the condition is true (if exitBranchIdx == 1) or false (if
    // exitBranchIdx == 0)
    bool loopWhileTrue = (exitBranchIdx == 1);

    unsigned tripCount = 0;

    // Simulate the loop to count iterations (conservative approach)
    long long iv = initValue;
    const unsigned maxIterations = 1000; // Safety limit
    for (unsigned iter = 0; iter < maxIterations; ++iter) {
        bool condResult = false;
        long long left = ivIsLhs ? iv : boundValue;
        long long right = ivIsLhs ? boundValue : iv;

        switch (cmpOp) {
            case Opcode::SCmpLT:
                condResult = (left < right);
                break;
            case Opcode::SCmpLE:
                condResult = (left <= right);
                break;
            case Opcode::SCmpGT:
                condResult = (left > right);
                break;
            case Opcode::SCmpGE:
                condResult = (left >= right);
                break;
            case Opcode::ICmpEq:
                condResult = (left == right);
                break;
            case Opcode::ICmpNe:
                condResult = (left != right);
                break;
            default:
                return std::nullopt;
        }

        // Loop continues if (condResult == loopWhileTrue)
        if (condResult != loopWhileTrue) {
            tripCount = iter;
            break;
        }

        auto nextIv = checkedAdd(iv, step);
        if (!nextIv)
            return std::nullopt;
        iv = *nextIv;

        if (iter == maxIterations - 1)
            return std::nullopt; // Too many iterations
    }

    if (tripCount == 0)
        return std::nullopt;

    CountedLoop result;
    result.initValue = initValue;
    result.endValue = boundValue;
    result.step = step;
    result.ivParamIndex = ivParamIndex;
    result.tripCount = tripCount;
    return result;
}

/// @brief Count instructions in all blocks belonging to a loop.
/// @param loop Loop whose labels define the counted block set.
/// @param function Function containing the loop; retained for the common helper interface.
/// @param blockMap Lookup from labels to current block storage.
/// @return Sum of instruction counts for labels found in @p blockMap.
size_t countLoopInstructions(const Loop &loop,
                             Function &function,
                             const std::unordered_map<std::string, BasicBlock *> &blockMap) {
    size_t count = 0;
    for (const auto &label : loop.blockLabels) {
        auto it = blockMap.find(label);
        if (it != blockMap.end())
            count += it->second->instructions.size();
    }
    return count;
}

/// @brief Fully unroll a simple loop into its preheader.
/// @param function Function receiving cloned instructions and block erasures.
/// @param loop Loop whose blocks are removed after expansion.
/// @param header Header providing the condition, body, and exit arguments.
/// @param latch Latch providing body instructions and next-iteration values.
/// @param preheader Entry block into which cloned instructions are inserted.
/// @param counted Induction description and exact trip count.
/// @param blockMap Current label lookup used during validation.
/// @return `true` after completing the rewrite, otherwise `false`.
/// @details Only one- or two-block loops containing clonable instructions are
///          supported. Each iteration receives fresh SSA results; the final
///          values are substituted into the exit edge before original loop
///          blocks are erased.
bool fullyUnrollLoop(Function &function,
                     const Loop &loop,
                     BasicBlock &header,
                     BasicBlock &latch,
                     BasicBlock *preheader,
                     const CountedLoop &counted,
                     std::unordered_map<std::string, BasicBlock *> &blockMap) {
    // For full unrolling, we:
    // 1. Clone the loop body (tripCount) times into the preheader
    // 2. Thread values from each iteration to the next
    // 3. Redirect preheader to the exit block with final values
    // 4. Remove the original loop blocks

    // This is a simplified implementation for single-block loops
    // (header and latch are the same block or latch immediately follows header)

    if (loop.blockLabels.size() > 2)
        return false; // Only handle simple loops
    if (loop.exits.empty() || header.instructions.empty() || latch.instructions.empty() ||
        !preheader || preheader->instructions.empty())
        return false;

    // Get the exit target and its arguments
    const std::string &exitLabel = loop.exits[0].to;
    auto exitBlockIt = blockMap.find(exitLabel);
    if (exitBlockIt == blockMap.end())
        return false;

    const Instr &headerTerm = header.instructions.back();
    if (headerTerm.op != Opcode::CBr || headerTerm.labels.size() != 2)
        return false;

    // Find which branch index goes to exit
    size_t exitBranchIdx = 0;
    if (headerTerm.labels[0] == exitLabel)
        exitBranchIdx = 0;
    else if (headerTerm.labels[1] == exitLabel)
        exitBranchIdx = 1;
    else
        return false;

    // Get exit branch args (what values to pass when exiting)
    std::vector<Value> exitArgs;
    if (!headerTerm.brArgs.empty() && exitBranchIdx < headerTerm.brArgs.size())
        exitArgs = headerTerm.brArgs[exitBranchIdx];

    // Get the body instructions (everything except terminator)
    std::vector<Instr> bodyInstrs;
    for (size_t i = 0; i < header.instructions.size() - 1; ++i) {
        if (!isSafeToCloneForFullUnroll(header.instructions[i]))
            return false;
        bodyInstrs.push_back(header.instructions[i]);
    }

    // Include latch body if different from header
    if (latch.label != header.label) {
        for (size_t i = 0; i < latch.instructions.size() - 1; ++i) {
            if (!isSafeToCloneForFullUnroll(latch.instructions[i]))
                return false;
            bodyInstrs.push_back(latch.instructions[i]);
        }
    }

    // Current values for header params (start with initial values from preheader)
    const Instr &phTerm = preheader->instructions.back();
    auto toHeaderIdx = labelIndex(phTerm, header.label);
    if (!toHeaderIdx || *toHeaderIdx >= phTerm.brArgs.size())
        return false;

    std::vector<Value> currentValues = phTerm.brArgs[*toHeaderIdx];
    if (currentValues.size() != header.params.size())
        return false;

    // Collect cloned instructions and insert once before the preheader terminator.
    std::vector<Instr> clonedInstrs;
    clonedInstrs.reserve(bodyInstrs.size() * counted.tripCount);
    std::vector<std::pair<unsigned, std::string>> clonedValueNames;
    clonedValueNames.reserve(bodyInstrs.size() * counted.tripCount);

    unsigned nextId = zanna::il::nextTempId(function);

    // Build value map: original temp id -> current value
    std::unordered_map<unsigned, Value> valueMap;

    // Unroll the loop
    for (unsigned iter = 0; iter < counted.tripCount; ++iter) {
        // Map header params to current iteration values
        valueMap.clear();
        for (size_t i = 0; i < header.params.size(); ++i)
            valueMap[header.params[i].id] = currentValues[i];

        // If latch is separate, also map latch params
        if (latch.label != header.label) {
            // Find args from header to latch
            auto toLatchIdx = labelIndex(header.instructions.back(), latch.label);
            if (!toLatchIdx || *toLatchIdx >= header.instructions.back().brArgs.size())
                return false;
            const auto &argsToLatch = headerTerm.brArgs[*toLatchIdx];
            if (argsToLatch.size() != latch.params.size())
                return false;
            for (size_t i = 0; i < latch.params.size(); ++i) {
                Value mappedVal = argsToLatch[i];
                if (mappedVal.kind == Value::Kind::Temp) {
                    auto it = valueMap.find(mappedVal.id);
                    if (it != valueMap.end())
                        mappedVal = it->second;
                }
                valueMap[latch.params[i].id] = mappedVal;
            }
        }

        // Clone body instructions for this iteration.
        for (const Instr &orig : bodyInstrs) {
            Instr cloned = orig;

            // Remap operands
            for (Value &op : cloned.operands) {
                if (op.kind == Value::Kind::Temp) {
                    auto it = valueMap.find(op.id);
                    if (it != valueMap.end())
                        op = it->second;
                }
            }

            // Allocate new result ID if needed
            if (cloned.result) {
                unsigned oldId = *cloned.result;
                unsigned newId = nextId++;
                cloned.result = newId;
                std::string oldName;
                if (oldId < function.valueNames.size())
                    oldName = function.valueNames[oldId];
                clonedValueNames.emplace_back(newId, std::move(oldName));
                valueMap[oldId] = Value::temp(newId);
            }

            clonedInstrs.push_back(std::move(cloned));
        }

        // Compute next iteration values
        // Get values that would be passed back to header
        const Instr &latchTerm = latch.instructions.back();
        auto toHeaderFromLatch = labelIndex(latchTerm, header.label);
        if (!toHeaderFromLatch || *toHeaderFromLatch >= latchTerm.brArgs.size())
            return false;
        const auto &nextArgs = latchTerm.brArgs[*toHeaderFromLatch];
        if (nextArgs.size() != currentValues.size())
            return false;
        for (size_t i = 0; i < nextArgs.size(); ++i) {
            Value nextVal = nextArgs[i];
            if (nextVal.kind == Value::Kind::Temp) {
                auto it = valueMap.find(nextVal.id);
                if (it != valueMap.end())
                    nextVal = it->second;
            }
            currentValues[i] = nextVal;
        }
    }

    // Map final exit args from the post-unroll header parameter values.
    // The previous iteration-local map is stale after currentValues advances.
    valueMap.clear();
    for (size_t i = 0; i < header.params.size(); ++i)
        valueMap[header.params[i].id] = currentValues[i];

    for (Value &arg : exitArgs) {
        if (arg.kind == Value::Kind::Temp) {
            auto it = valueMap.find(arg.id);
            if (it != valueMap.end())
                arg = it->second;
            else if (tempDefinedInLoop(function, loop, arg.id))
                return false;
        }
    }

    if (!clonedValueNames.empty()) {
        const unsigned lastId = clonedValueNames.back().first;
        if (function.valueNames.size() <= lastId)
            function.valueNames.resize(static_cast<std::size_t>(lastId) + 1);
        for (auto &[id, name] : clonedValueNames)
            function.valueNames[id] = std::move(name);
    }

    const size_t insertIdx = preheader->instructions.size() - 1;
    preheader->instructions.insert(preheader->instructions.begin() + static_cast<long>(insertIdx),
                                   std::make_move_iterator(clonedInstrs.begin()),
                                   std::make_move_iterator(clonedInstrs.end()));

    // Update preheader terminator to branch to exit
    Instr &newTerm = preheader->instructions.back();
    newTerm.op = Opcode::Br;
    newTerm.operands.clear();
    newTerm.setBranchTargets({exitLabel}, {exitArgs});

    // Remove original loop blocks from function
    std::unordered_set<std::string> loopBlockLabels(loop.blockLabels.begin(),
                                                    loop.blockLabels.end());
    /// @brief Test whether a block belonged to the now-expanded loop.
    /// @param b Candidate function block.
    /// @return True when @p b should be removed after full expansion.
    function.blocks.erase(
        std::remove_if(function.blocks.begin(),
                       function.blocks.end(),
                       [&](const BasicBlock &b) { return loopBlockLabels.count(b.label) > 0; }),
        function.blocks.end());

    return true;
}

} // namespace

/// @copydoc LoopUnroll::id()
std::string_view LoopUnroll::id() const {
    return "loop-unroll";
}

/// @copydoc LoopUnroll::run()
PreservedAnalyses LoopUnroll::run(Function &function, AnalysisManager &analysis) {
    bool changed = false;
    for (;;) {
        LoopInfo loopInfo = computeLoopInfo(analysis.module(), function);
        CFGInfo cfg = buildCFG(analysis.module(), function);

        std::unordered_map<std::string, BasicBlock *> blockMap;
        blockMap.reserve(function.blocks.size());
        for (auto &blk : function.blocks)
            blockMap.emplace(blk.label, &blk);

        bool changedThisIteration = false;

        // Process loops from innermost to outermost (reverse order in LoopInfo).
        // Restart after each mutation so CFGInfo and LoopInfo never outlive a block erase.
        for (const Loop &loop : loopInfo.loops()) {
            // Skip nested loops (only unroll innermost)
            if (!loop.childHeaders.empty())
                continue;

            auto headerIt = blockMap.find(loop.headerLabel);
            if (headerIt == blockMap.end())
                continue;
            BasicBlock *header = headerIt->second;

            if (loop.latchLabels.size() != 1)
                continue;

            auto latchIt = blockMap.find(loop.latchLabels[0]);
            if (latchIt == blockMap.end())
                continue;
            BasicBlock *latch = latchIt->second;

            BasicBlock *preheader = findPreheader(function, loop, *header, cfg, blockMap);
            if (!preheader)
                continue;

            // Check loop size
            size_t loopSize = countLoopInstructions(loop, function, blockMap);
            if (loopSize > config_.maxLoopSize)
                continue;

            // Analyze for counted loop pattern
            auto counted = analyzeCountedLoop(function, loop, *header, *latch, preheader, blockMap);
            if (!counted)
                continue;

            // Check trip count threshold for full unrolling
            if (counted->tripCount > config_.fullUnrollThreshold)
                continue;

            if (fullyUnrollLoop(function, loop, *header, *latch, preheader, *counted, blockMap)) {
                changed = true;
                changedThisIteration = true;
                break;
            }
        }

        if (!changedThisIteration)
            break;
    }

    if (!changed)
        return PreservedAnalyses::all();

    // Invalidate most analyses since we modified CFG structure
    PreservedAnalyses preserved;
    preserved.preserveAllModules();
    return preserved;
}

/// @copydoc registerLoopUnrollPass()
void registerLoopUnrollPass(PassRegistry &registry) {
    // Sequential: rewrites loop CFG and recomputes whole-module loop/CFG snapshots.
    /// @brief Construct a default-configured loop unroller for the sequential pipeline.
    /// @return Newly owned `LoopUnroll` pass.
    registry.registerFunctionPass(
        "loop-unroll", []() { return std::make_unique<LoopUnroll>(); }, false);
}

} // namespace il::transform
