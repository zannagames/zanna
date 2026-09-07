//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/PeepholePass.cpp
// Purpose: Peephole optimisation pass for the AArch64 modular pipeline.
//          Runs the optimizer on each post-RA MIR function, optionally
//          collecting statistics via ZANNA_CODEGEN_STATS, and validates that
//          no virtual registers remain and branches target known blocks.
// Key invariants:
//   - Must run after RegAllocPass (all regs must be physical).
//   - Validation errors abort the pass (returns false).
//   - Statistics output goes to the Diagnostics warning channel, not stdout.
// Ownership/Lifetime:
//   - Stateless pass; mutates AArch64Module::mir in place.
// Links: src/codegen/aarch64/passes/PeepholePass.hpp,
//        src/codegen/aarch64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/PeepholePass.hpp"

#include "codegen/aarch64/Noreturn.hpp"
#include "codegen/aarch64/Peephole.hpp"

#include <cstddef>
#include <cstdlib>
#include <sstream>
#include <string>
#include <unordered_set>

/**
 * @file
 * @brief Implements peephole orchestration, cleanup, MIR validation, and statistics.
 */

namespace zanna::codegen::aarch64::passes {
namespace {

/// @brief Tests whether an opcode carries an internal block target.
/// @param opcode Opcode to classify.
/// @return `true` for supported direct and conditional branch families.
[[nodiscard]] bool isBranchTargetOpcode(MOpcode opcode) noexcept {
    return opcode == MOpcode::Br || opcode == MOpcode::BCond || opcode == MOpcode::Cbz ||
           opcode == MOpcode::Cbnz || opcode == MOpcode::Tbz || opcode == MOpcode::Tbnz;
}

/// @brief Tests the `ZANNA_CODEGEN_STATS` environment switch.
/// @return `true` for a non-empty value whose first character is not zero.
[[nodiscard]] bool codegenStatsEnabled() noexcept {
    if (const char *value = std::getenv("ZANNA_CODEGEN_STATS"))
        return value[0] != '\0' && value[0] != '0';
    return false;
}

/// @brief Tests whether an opcode ends normal block flow.
/// @param opcode Opcode to classify.
/// @return `true` for unconditional `Br` or `Ret`.
[[nodiscard]] bool isHardTerminator(MOpcode opcode) noexcept {
    return opcode == MOpcode::Br || opcode == MOpcode::Ret;
}

/// @brief Removes instruction tails after the first known no-return call per block.
/// @param[in,out] fn Function to prune.
/// @return Number of erased instructions.
std::size_t pruneAfterNoReturnCalls(MFunction &fn) {
    std::size_t removed = 0;
    for (auto &block : fn.blocks) {
        for (std::size_t ii = 0; ii < block.instrs.size(); ++ii) {
            if (!isNoReturnCall(block.instrs[ii]))
                continue;
            const std::size_t keep = ii + 1;
            removed += block.instrs.size() - keep;
            block.instrs.erase(block.instrs.begin() + static_cast<std::ptrdiff_t>(keep),
                               block.instrs.end());
            break;
        }
    }
    return removed;
}

/**
 * @brief Validates physical registers, terminator placement, and branch targets.
 * @param fn Optimized function to inspect.
 * @param[in,out] diags Sink receiving the first violation.
 * @return `true` when all checked MIR invariants hold.
 */
[[nodiscard]] bool validateFunction(const MFunction &fn, Diagnostics &diags) {
    std::unordered_set<std::string> labels;
    for (const auto &block : fn.blocks)
        labels.insert(block.name);

    for (const auto &block : fn.blocks) {
        bool seenTerminator = false;
        for (std::size_t ii = 0; ii < block.instrs.size(); ++ii) {
            const auto &instr = block.instrs[ii];

            if (seenTerminator) {
                std::ostringstream msg;
                msg << "aarch64 peephole: non-terminator after terminator in function '" << fn.name
                    << "', block '" << block.name << "'";
                diags.error(msg.str());
                return false;
            }
            if (isHardTerminator(instr.opc) || isNoReturnCall(instr))
                seenTerminator = true;

            for (const auto &op : instr.ops) {
                if (op.kind == MOperand::Kind::Reg && !op.reg.isPhys) {
                    std::ostringstream msg;
                    msg << "aarch64 peephole: virtual register remains in function '" << fn.name
                        << "', block '" << block.name << "'";
                    diags.error(msg.str());
                    return false;
                }
            }

            if (!isBranchTargetOpcode(instr.opc))
                continue;
            if (instr.opc == MOpcode::Br) {
                if (instr.ops.empty() || instr.ops[0].kind != MOperand::Kind::Label)
                    continue;
                if (labels.count(instr.ops[0].label) != 0)
                    continue;
            } else {
                if (instr.ops.size() < 2 || instr.ops[1].kind != MOperand::Kind::Label)
                    continue;
                if (labels.count(instr.ops[1].label) != 0)
                    continue;
            }

            std::ostringstream msg;
            msg << "aarch64 peephole: branch to missing block in function '" << fn.name
                << "', block '" << block.name << "'";
            diags.error(msg.str());
            return false;
        }
    }
    return true;
}

} // namespace

/**
 * @brief Runs the selected optimizer, no-return cleanup, metadata pruning, and validation.
 * @param[in,out] module Module whose functions are processed.
 * @param[in,out] diags Diagnostic/statistics sink.
 * @return `false` on missing target metadata or the first validation failure.
 */
bool PeepholePass::run(AArch64Module &module, Diagnostics &diags) {
    if (module.ti == nullptr) {
        diags.error("aarch64 peephole: target info is required");
        return false;
    }

    int total = 0;
    for (auto &fn : module.mir) {
        auto peepholeStats = mode_ == Mode::Full
                                 ? runPeephole(fn, module.ti, !module.edgeCopyLowering)
                                 : runPostSchedulePeephole(fn, module.ti);
        total += peepholeStats.total();
        total += static_cast<int>(pruneAfterNoReturnCalls(fn));
        pruneUnusedCalleeSaved(fn);
        if (!validateFunction(fn, diags))
            return false;
    }

    // The MIR shape counters live in CodegenStatsPass, which runs on the
    // final MIR at every optimization level.
    if (codegenStatsEnabled())
        diags.warning("aarch64 peephole: " + std::to_string(total) + " transformations");

    return true;
}

} // namespace zanna::codegen::aarch64::passes
