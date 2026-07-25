//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Provides the default instruction verification strategies for the IL verifier.
/// @details Supplies specialised handlers for control-flow instructions alongside
///          a catch-all strategy that delegates to the general instruction checker.

#include "il/verify/InstructionStrategies.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/verify/BranchVerifier.hpp"
#include "il/verify/FunctionVerifier.hpp"
#include "il/verify/InstructionChecker.hpp"
#include "il/verify/TypeInference.hpp"
#include "il/verify/VerifyCtx.hpp"

#include <memory>
#include <unordered_map>

using namespace il::core;

namespace il::verify {
using il::support::Expected;

/// @brief Verify a non-control-flow instruction using the default checker.
/// @param ctx Verification context describing the current instruction.
/// @return Success on validity; otherwise a diagnostic error.
Expected<void> verifyInstruction_E(const VerifyCtx &ctx);

namespace {

/// @brief Strategy that handles control-flow instructions with dedicated checks.
///
/// @details Control-flow opcodes need bespoke validation to check successor
///          arguments and condition semantics.  This strategy dispatches to the
///          appropriate helper functions and ignores maps that are irrelevant
///          for these opcodes.
class ControlFlowStrategy final : public FunctionVerifier::InstructionStrategy {
  public:
    /// @brief Identify whether the strategy should verify the given instruction.
    /// @param instr Instruction under inspection.
    /// @return @c true when @p instr is a control-flow opcode.
    bool matches(const Instr &instr) const override {
        return instr.op == Opcode::Br || instr.op == Opcode::CBr || instr.op == Opcode::SwitchI32 ||
               instr.op == Opcode::Ret;
    }

    /// @brief Run control-flow specific verification logic.
    /// @details Dispatches to the appropriate helper based on the opcode while
    /// ignoring maps that are irrelevant for the handled instructions.
    /// @param fn Function owning the instruction.
    /// @param bb Basic block containing the instruction.
    /// @param instr Instruction to verify.
    /// @param blockMap Mapping from labels to block definitions.
    /// @param externs Map of extern declarations (unused).
    /// @param funcs Map of function declarations (unused).
    /// @param globals Map of global declarations (unused).
    /// @param types Type inference context for the current function.
    /// @param sink Diagnostic sink used for reporting (unused here).
    /// @return Success when the instruction satisfies control-flow invariants.
    Expected<void> verify(const Function &fn,
                          const BasicBlock &bb,
                          const Instr &instr,
                          const BlockMap &blockMap,
                          const std::unordered_map<std::string, const Extern *> &externs,
                          const std::unordered_map<std::string, const Function *> &funcs,
                          const std::unordered_map<std::string, const Global *> &globals,
                          TypeInference &types,
                          DiagSink &sink) const override {
        (void)externs;
        (void)funcs;
        (void)globals;
        (void)sink;
        switch (instr.op) {
            case Opcode::Br:
                return verifyBr_E(fn, bb, instr, blockMap, types);
            case Opcode::CBr:
                return verifyCBr_E(fn, bb, instr, blockMap, types);
            case Opcode::SwitchI32:
                return verifySwitchI32_E(fn, bb, instr, blockMap, types);
            case Opcode::Ret:
                return verifyRet_E(fn, bb, instr, types);
            default:
                break;
        }
        return {};
    }
};

/// @brief Strategy that delegates generic instruction checking to the common verifier.
///
/// @details Acts as the fallback for all opcodes not claimed by specialised
///          strategies.  Verification is forwarded to the shared instruction
///          checker that enforces operand/result typing rules.
class DefaultInstructionStrategy final : public FunctionVerifier::InstructionStrategy {
  public:
    /// @brief Always claim responsibility for verification when no other strategy applies.
    /// @details The instruction parameter is intentionally ignored.
    /// @return Always returns @c true.
    bool matches(const Instr &) const override {
        return true;
    }

    /// @brief Verify an instruction using the default checker pipeline.
    /// @details Binds the instruction into a @c VerifyCtx and invokes the shared
    /// instruction checker that handles type and operand validation.
    /// @param fn Function owning the instruction.
    /// @param bb Basic block containing the instruction.
    /// @param instr Instruction to verify.
    /// @param blockMap Mapping from labels to block definitions (unused).
    /// @param externs Map of extern declarations.
    /// @param funcs Map of function declarations.
    /// @param globals Map of global declarations used by memory checkers.
    /// @param types Type inference context for the current function.
    /// @param sink Diagnostic sink used for warnings and reporting.
    /// @return Success when the instruction is valid; otherwise a diagnostic error.
    Expected<void> verify(const Function &fn,
                          const BasicBlock &bb,
                          const Instr &instr,
                          const BlockMap &blockMap,
                          const std::unordered_map<std::string, const Extern *> &externs,
                          const std::unordered_map<std::string, const Function *> &funcs,
                          const std::unordered_map<std::string, const Global *> &globals,
                          TypeInference &types,
                          DiagSink &sink) const override {
        (void)blockMap;
        VerifyCtx ctx{sink, types, externs, funcs, globals, fn, bb, instr};
        return verifyInstruction_E(ctx);
    }
};

} // namespace

/// @brief Construct the default set of instruction verification strategies.
///
/// @details The resulting vector orders strategies from most specific to most
///          general so that control-flow opcodes are handled before the
///          catch-all strategy claims the remainder.
///
/// @return Vector containing control-flow and generic verification strategies.
std::vector<std::unique_ptr<FunctionVerifier::InstructionStrategy>>
makeDefaultInstructionStrategies() {
    std::vector<std::unique_ptr<FunctionVerifier::InstructionStrategy>> strategies;
    strategies.push_back(std::make_unique<ControlFlowStrategy>());
    strategies.push_back(std::make_unique<DefaultInstructionStrategy>());
    return strategies;
}

} // namespace il::verify
