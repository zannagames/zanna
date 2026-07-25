//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the SiblingRecursion pass. Detects functions with double
// self-recursion where both call results are combined with an associative
// operator (iadd.ovf / add) and returned. Converts the second recursive call
// into a loop iteration with an accumulator, halving total calls.
//
// Example transformation (fibonacci):
//
// BEFORE:
//   recurse(%n2:i64):
//     %nm1 = isub.ovf %n2, 1
//     %r1  = call @fib(%nm1)
//     %nm2 = isub.ovf %n2, 2
//     %r2  = call @fib(%nm2)
//     %sum = iadd.ovf %r1, %r2
//     ret %sum
//
// AFTER:
//   recurse(%n2:i64, %acc:i64):        ; accumulator added
//     %nm1  = isub.ovf %n2, 1
//     %r1   = call @fib(%nm1)
//     %acc2 = iadd.ovf %acc, %r1       ; accumulate first result
//     %nm2  = isub.ovf %n2, 2
//     %cmp  = scmp_le %nm2, 1          ; base case check
//     cbr %cmp, done(%nm2, %acc2), recurse(%nm2, %acc2)
//
//   done(%nbase:i64, %acc_done:i64):   ; new exit block
//     %result = iadd.ovf %nbase, %acc_done
//     ret %result
//
//===----------------------------------------------------------------------===//

#include "il/transform/SiblingRecursion.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Value.hpp"
#include "il/utils/Utils.hpp"

#include <string>
#include <vector>

using namespace il::core;

namespace il::transform {
namespace {

/// @brief Find the maximum temporary or parameter id used in a function.
/// @param fn Function whose ABI params, block params, and results are scanned.
/// @return Largest observed id, or zero for a function without ids.
unsigned findMaxTempId(const Function &fn) {
    unsigned maxId = 0;
    for (const auto &p : fn.params)
        maxId = std::max(maxId, p.id);
    for (const auto &bb : fn.blocks) {
        for (const auto &p : bb.params)
            maxId = std::max(maxId, p.id);
        for (const auto &instr : bb.instructions)
            if (instr.result)
                maxId = std::max(maxId, *instr.result);
    }
    return maxId;
}

/// @brief Check whether an opcode is a supported associative integer add.
/// @param op Opcode to classify.
/// @return `true` for checked or verifier-demoted integer addition.
bool isAssocAdd(Opcode op) {
    return op == Opcode::IAddOvf || op == Opcode::Add;
}

/// @brief Check whether an opcode can express the recognized signed base case.
/// @param op Opcode to classify.
/// @return `true` for signed less/greater relational comparisons.
bool isSignedCmp(Opcode op) {
    return op == Opcode::SCmpLE || op == Opcode::SCmpLT || op == Opcode::SCmpGE ||
           op == Opcode::SCmpGT;
}

/// @brief Restore a checked opcode before moving arithmetic onto a loop-carried path.
/// @param op Original arithmetic opcode.
/// @return Checked add/subtract/multiply counterpart, or @p op unchanged.
Opcode checkedOpcodeForLoopCarriedArithmetic(Opcode op) {
    switch (op) {
        case Opcode::Add:
            return Opcode::IAddOvf;
        case Opcode::Sub:
            return Opcode::ISubOvf;
        case Opcode::Mul:
            return Opcode::IMulOvf;
        default:
            return op;
    }
}

/// @brief Copy an instruction while restoring verifier-stable checked arithmetic.
/// @param instr Instruction copied from the recursive block.
/// @return Updated instruction suitable for the transformed loop.
Instr makeLoopCarriedVerifierStable(Instr instr) {
    instr.op = checkedOpcodeForLoopCarriedArithmetic(instr.op);
    return instr;
}

/// Matched pattern information for the sibling recursion transformation.
struct SiblingPattern {
    /// Index of the recursive block in `Function::blocks`.
    size_t blockIdx{0}; // Index of the recurse block in fn.blocks
    /// Instruction index of the first self call.
    size_t call1Idx{0}; // Instruction index of first self-call
    /// Instruction index of the second self call.
    size_t call2Idx{0}; // Instruction index of second self-call
    /// Instruction index of the add combining both results.
    size_t addIdx{0};   // Instruction index of the combining add
    /// Original checked or plain addition opcode.
    Opcode addOp{Opcode::Count}; // The add opcode (IAddOvf or Add)

    // Entry/predecessor block base case info
    /// Signed comparison opcode used by the predecessor's base-case branch.
    Opcode cmpOp{Opcode::Count}; // Base case comparison opcode (e.g., SCmpLE)
    /// Threshold operand compared with the recursive argument.
    Value cmpThreshold;  // Base case threshold value (e.g., 1)
    /// Whether the true predecessor edge represents the base case.
    bool baseCaseIsTrue{false}; // True if base case fires on the TRUE branch of CBr
};

/// Attempt to match the sibling recursion pattern in a function.
///
/// Detection criteria:
///   1. Function has exactly one i64 parameter (single-arg recursion).
///   2. The recursive block already carries that recursive state as a block
///      parameter in SSA form. This pass does not rewrite frontend-lowered
///      stack-slot recursion into loop-carried block parameters.
///   3. Some block has exactly 2 self-recursive calls.
///   4. Both call results are combined with an associative add (IAddOvf/Add).
///   5. The combined result is immediately returned.
///   6. Instructions between calls don't use the first call's result.
///   7. A predecessor block has a signed comparison + CBr to the recurse block.
/// @param fn Candidate single-argument recursive function.
/// @return Instruction indices and base-case metadata, or `std::nullopt`.
std::optional<SiblingPattern> matchPattern(const Function &fn) {
    // For now, require single-argument functions.
    if (fn.params.size() != 1)
        return std::nullopt;
    if (fn.params[0].type.kind != Type::Kind::I64)
        return std::nullopt;

    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        const auto &bb = fn.blocks[bi];

        // The transformation mutates predecessor edges to thread both the
        // recursive argument and the accumulator through block parameters. If
        // the block does not already carry the recursive argument in SSA form,
        // the pass cannot safely rewrite stack-slot based frontend lowering.
        if (bb.params.size() != fn.params.size())
            continue;
        if (bb.params[0].type.kind != Type::Kind::I64)
            continue;

        // Find self-recursive calls.
        std::vector<size_t> selfCallIndices;
        for (size_t i = 0; i < bb.instructions.size(); ++i) {
            const auto &instr = bb.instructions[i];
            if (instr.op == Opcode::Call && instr.callee == fn.name)
                selfCallIndices.push_back(i);
        }

        if (selfCallIndices.size() != 2)
            continue;

        const size_t call1Idx = selfCallIndices[0];
        const size_t call2Idx = selfCallIndices[1];
        const auto &call1 = bb.instructions[call1Idx];
        const auto &call2 = bb.instructions[call2Idx];

        // Both calls must produce results with same arity as function params.
        if (!call1.result || !call2.result)
            continue;
        if (call1.operands.size() != fn.params.size() || call2.operands.size() != fn.params.size())
            continue;

        const unsigned r1 = *call1.result;
        const unsigned r2 = *call2.result;

        // Safety: instructions between calls must not use first call result.
        bool r1UsedBetweenCalls = false;
        for (size_t i = call1Idx + 1; i < call2Idx; ++i) {
            for (const auto &op : bb.instructions[i].operands) {
                if (op.kind == Value::Kind::Temp && op.id == r1) {
                    r1UsedBetweenCalls = true;
                    break;
                }
            }
            if (r1UsedBetweenCalls)
                break;
        }
        if (r1UsedBetweenCalls)
            continue;

        // Find the add combining both results (after second call).
        size_t addIdx = SIZE_MAX;
        for (size_t i = call2Idx + 1; i < bb.instructions.size(); ++i) {
            const auto &instr = bb.instructions[i];
            if (isAssocAdd(instr.op) && instr.operands.size() == 2) {
                bool hasR1 = false, hasR2 = false;
                for (const auto &op : instr.operands) {
                    if (op.kind == Value::Kind::Temp && op.id == r1)
                        hasR1 = true;
                    if (op.kind == Value::Kind::Temp && op.id == r2)
                        hasR2 = true;
                }
                if (hasR1 && hasR2) {
                    addIdx = i;
                    break;
                }
            }
        }
        if (addIdx == SIZE_MAX)
            continue;

        const auto &addInstr = bb.instructions[addIdx];
        if (!addInstr.result)
            continue;
        const unsigned sumId = *addInstr.result;

        // The add result must be immediately returned.
        size_t retIdx = SIZE_MAX;
        for (size_t i = addIdx + 1; i < bb.instructions.size(); ++i) {
            if (bb.instructions[i].op == Opcode::Ret) {
                const auto &retOp = bb.instructions[i];
                if (!retOp.operands.empty() && retOp.operands[0].kind == Value::Kind::Temp &&
                    retOp.operands[0].id == sumId) {
                    retIdx = i;
                }
                break;
            }
        }
        if (retIdx == SIZE_MAX)
            continue;

        // Find a predecessor with a signed comparison + CBr to this block.
        SiblingPattern pat;
        pat.blockIdx = bi;
        pat.call1Idx = call1Idx;
        pat.call2Idx = call2Idx;
        pat.addIdx = addIdx;
        pat.addOp = addInstr.op;

        bool foundEntry = false;
        for (size_t ei = 0; ei < fn.blocks.size(); ++ei) {
            if (ei == bi)
                continue;
            const auto &entryBB = fn.blocks[ei];
            if (!zanna::il::isTerminated(entryBB))
                continue;

            const auto &term = entryBB.instructions.back();
            if (term.op != Opcode::CBr || term.labels.size() != 2)
                continue;

            // Check if one branch target is our recurse block.
            int recurseTargetIdx = -1;
            for (size_t li = 0; li < term.labels.size(); ++li) {
                if (term.labels[li] == bb.label)
                    recurseTargetIdx = static_cast<int>(li);
            }
            if (recurseTargetIdx < 0)
                continue;

            // Find the comparison instruction feeding the CBr.
            if (term.operands.empty() || term.operands[0].kind != Value::Kind::Temp)
                continue;
            const unsigned cmpId = term.operands[0].id;

            for (const auto &instr : entryBB.instructions) {
                if (instr.result && *instr.result == cmpId && isSignedCmp(instr.op)) {
                    if (instr.operands.size() < 2)
                        break;

                    pat.cmpOp = instr.op;
                    pat.cmpThreshold = instr.operands[1];

                    // CBr: labels[0] = true target, labels[1] = false target.
                    // baseCaseIsTrue: true when the FALSE branch goes to recurse
                    // (meaning the TRUE branch is the base case).
                    pat.baseCaseIsTrue = (recurseTargetIdx == 1);
                    foundEntry = true;
                    break;
                }
            }

            if (foundEntry)
                break;
        }

        if (!foundEntry)
            continue;

        return pat;
    }

    return std::nullopt;
}

} // anonymous namespace

/// @copydoc SiblingRecursion::id()
std::string_view SiblingRecursion::id() const {
    return "sibling-recursion";
}

/// @copydoc SiblingRecursion::run()
PreservedAnalyses SiblingRecursion::run(Function &fn, AnalysisManager &) {
    auto patOpt = matchPattern(fn);
    if (!patOpt)
        return PreservedAnalyses::all();

    const auto &pat = *patOpt;
    const std::string recurseLabel = fn.blocks[pat.blockIdx].label;
    const std::string doneLabel = "done_" + recurseLabel;

    // --- Allocate new temp IDs ---
    unsigned nextId = findMaxTempId(fn) + 1;
    const unsigned accParamId = nextId++;
    const unsigned accNewId = nextId++;
    const unsigned cmpLoopId = nextId++;
    const unsigned doneResultId = nextId++;

    // Extend valueNames to cover new IDs.
    while (fn.valueNames.size() <= nextId)
        fn.valueNames.emplace_back();
    fn.valueNames[accParamId] = "acc";
    fn.valueNames[accNewId] = "acc2";
    fn.valueNames[cmpLoopId] = "cmp_loop";
    fn.valueNames[doneResultId] = "result";

    // --- Step 1: Update all predecessor edges to pass initial accumulator (0) ---
    // This must happen BEFORE modifying the recurse block.
    for (size_t bi = 0; bi < fn.blocks.size(); ++bi) {
        if (bi == pat.blockIdx)
            continue;
        for (auto &instr : fn.blocks[bi].instructions) {
            if (instr.op != Opcode::Br && instr.op != Opcode::CBr && instr.op != Opcode::SwitchI32)
                continue;

            for (size_t li = 0; li < instr.labels.size(); ++li) {
                if (instr.labels[li] == recurseLabel)
                    instr.brArgs[li].push_back(Value::constInt(0));
            }
        }
    }

    // --- Step 2: Collect data from the recurse block ---
    const Value secondCallArg = fn.blocks[pat.blockIdx].instructions[pat.call2Idx].operands[0];
    const unsigned r1Id = *fn.blocks[pat.blockIdx].instructions[pat.call1Idx].result;

    // --- Step 3: Build new instructions for the loop block ---
    std::vector<Instr> newInstrs;

    // Keep instructions [0, call1Idx] (up to and including first call).
    for (size_t i = 0; i <= pat.call1Idx; ++i)
        newInstrs.push_back(makeLoopCarriedVerifierStable(fn.blocks[pat.blockIdx].instructions[i]));

    // Keep instructions between calls (e.g., second arg computation).
    for (size_t i = pat.call1Idx + 1; i < pat.call2Idx; ++i)
        newInstrs.push_back(makeLoopCarriedVerifierStable(fn.blocks[pat.blockIdx].instructions[i]));

    // New: accumulate — %accNew = addOp %acc, %r1
    {
        Instr accInstr;
        accInstr.result = accNewId;
        accInstr.op = checkedOpcodeForLoopCarriedArithmetic(pat.addOp);
        accInstr.type = Type(Type::Kind::I64);
        accInstr.operands.push_back(Value::temp(accParamId));
        accInstr.operands.push_back(Value::temp(r1Id));
        newInstrs.push_back(std::move(accInstr));
    }

    // New: base case check — %cmpLoop = cmpOp secondCallArg, threshold
    {
        Instr cmpInstr;
        cmpInstr.result = cmpLoopId;
        cmpInstr.op = pat.cmpOp;
        cmpInstr.type = Type(Type::Kind::I1);
        cmpInstr.operands.push_back(secondCallArg);
        cmpInstr.operands.push_back(pat.cmpThreshold);
        newInstrs.push_back(std::move(cmpInstr));
    }

    // New: CBr — branch to done or loop back.
    // The done block uses cross-block temp references (no block params) to avoid
    // redundant stores — both targets would receive identical values, but separate
    // block params cause the codegen to emit duplicate stores.
    // Mirrors the entry block's branch polarity:
    //   baseCaseIsTrue  → true=done, false=loop
    //   !baseCaseIsTrue → true=loop, false=done
    {
        Instr cbrInstr;
        cbrInstr.op = Opcode::CBr;
        cbrInstr.type = Type(Type::Kind::Void);
        cbrInstr.operands.push_back(Value::temp(cmpLoopId));

        std::vector<Value> loopArgs = {secondCallArg, Value::temp(accNewId)};
        std::vector<Value> doneArgs; // No args — done block uses temps directly.

        if (pat.baseCaseIsTrue) {
            cbrInstr.addBranchTarget(doneLabel, std::move(doneArgs));
            cbrInstr.addBranchTarget(recurseLabel, std::move(loopArgs));
        } else {
            cbrInstr.addBranchTarget(recurseLabel, std::move(loopArgs));
            cbrInstr.addBranchTarget(doneLabel, std::move(doneArgs));
        }

        newInstrs.push_back(std::move(cbrInstr));
    }

    // --- Step 4: Replace recurse block instructions and add accumulator param ---
    fn.blocks[pat.blockIdx].instructions = std::move(newInstrs);
    fn.blocks[pat.blockIdx].terminated = true;

    Param accParam;
    accParam.name = "acc";
    accParam.type = Type(Type::Kind::I64);
    accParam.id = accParamId;
    fn.blocks[pat.blockIdx].params.push_back(accParam);

    // --- Step 5: Create the "done" exit block ---
    // The done block has NO block params — it references temps from the dominating
    // recurse block directly (secondCallArg for the base case value, accNewId for
    // the accumulated sum). This avoids codegen allocating separate frame slots
    // for identical values, eliminating redundant stores per loop iteration.
    BasicBlock doneBlock;
    doneBlock.label = doneLabel;

    // %result = addOp secondCallArg, %accNew
    {
        Instr addInstr;
        addInstr.result = doneResultId;
        addInstr.op = checkedOpcodeForLoopCarriedArithmetic(pat.addOp);
        addInstr.type = Type(Type::Kind::I64);
        addInstr.operands.push_back(secondCallArg);
        addInstr.operands.push_back(Value::temp(accNewId));
        doneBlock.instructions.push_back(std::move(addInstr));
    }

    // ret %result
    {
        Instr retInstr;
        retInstr.op = Opcode::Ret;
        retInstr.type = Type(Type::Kind::Void);
        retInstr.operands.push_back(Value::temp(doneResultId));
        doneBlock.instructions.push_back(std::move(retInstr));
    }
    doneBlock.terminated = true;

    fn.blocks.push_back(std::move(doneBlock));

    return PreservedAnalyses::none();
}

/// @brief Register sibling recursion pass.
/// @param registry Registry that receives the pass factory.
void registerSiblingRecursionPass(PassRegistry &registry) {
    /// Construct a stateless sibling-recursion optimizer for one function.
    registry.registerFunctionPass(
        "sibling-recursion", []() { return std::make_unique<SiblingRecursion>(); }, true);
}

} // namespace il::transform
