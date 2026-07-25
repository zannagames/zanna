//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/LegalizePass.cpp
// Purpose: Pre-RA AArch64 MIR legalization. Expands overflow pseudos, inserts
//          runtime context init for @main, and refreshes the isLeaf flag.
// Key invariants:
//   - lowerOverflowOps() must run before register allocation (pseudos are
//     lowered to virtual-register sequences that RA then allocates).
//   - All calls, including calls in trap blocks, contribute to isLeaf so frame
//     and unwind decisions match the emitted instruction stream.
// Ownership/Lifetime:
//   - Stateless pass; mutates AArch64Module::mir in place.
// Links: src/codegen/aarch64/passes/LegalizePass.hpp,
//        src/codegen/aarch64/LowerOvf.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/LegalizePass.hpp"

#include "codegen/aarch64/LowerOvf.hpp"

#include <exception>
#include <string>

/**
 * @file
 * @brief Implements checked-arithmetic expansion and runtime-entry legalization.
 */

namespace zanna::codegen::aarch64::passes {
namespace {

/**
 * @brief Tests whether an instruction is a direct or indirect call.
 * @param instr MIR instruction to classify.
 * @return `true` for `Bl` or `Blr`.
 */
[[nodiscard]] bool isCall(const MInstr &instr) noexcept {
    return instr.opc == MOpcode::Bl || instr.opc == MOpcode::Blr;
}

/**
 * @brief Tests whether an instruction directly calls an exact label.
 * @param instr MIR instruction to inspect.
 * @param name Non-null target spelling.
 * @return `true` for a labeled `Bl` whose target equals @p name.
 */
[[nodiscard]] bool isCallTo(const MInstr &instr, const char *name) noexcept {
    return instr.opc == MOpcode::Bl && !instr.ops.empty() &&
           instr.ops[0].kind == MOperand::Kind::Label && instr.ops[0].label == name;
}

/**
 * @brief Prepends legacy runtime-context setup to a recognized main entry point.
 * @param[in,out] fn Function to modify when named `main` or `@main`.
 * @post Existing canonical leading setup calls are not duplicated.
 */
void insertMainRuntimeContextInit(MFunction &fn) {
    if ((fn.name != "main" && fn.name != "@main") || fn.blocks.empty())
        return;

    auto &entryInstrs = fn.blocks.front().instrs;
    if (entryInstrs.size() >= 2 && isCallTo(entryInstrs[0], "rt_legacy_context") &&
        isCallTo(entryInstrs[1], "rt_set_current_context")) {
        return;
    }

    entryInstrs.insert(entryInstrs.begin(),
                       {MInstr{MOpcode::Bl, {MOperand::labelOp("rt_legacy_context")}},
                        MInstr{MOpcode::Bl, {MOperand::labelOp("rt_set_current_context")}}});
}

/**
 * @brief Recomputes leaf status from all direct and indirect calls.
 * @param[in,out] fn Function whose `isLeaf` flag is replaced.
 * @post `fn.isLeaf` is true exactly when no block contains `Bl` or `Blr`.
 */
void refreshLeafFlag(MFunction &fn) noexcept {
    fn.isLeaf = true;
    for (const auto &bb : fn.blocks) {
        for (const auto &instr : bb.instrs) {
            if (isCall(instr)) {
                fn.isLeaf = false;
                return;
            }
        }
    }
}

} // namespace

/**
 * @brief Legalizes overflow operations, main setup, and leaf metadata.
 * @param[in,out] module Module whose MIR is rewritten.
 * @param[in,out] diags Sink receiving validation or expansion failures.
 * @return `true` on complete legalization.
 */
bool LegalizePass::run(AArch64Module &module, Diagnostics &diags) {
    if (!module.ti) {
        diags.error("LegalizePass: ti must be non-null");
        return false;
    }

    try {
        for (auto &fn : module.mir) {
            lowerOverflowOps(fn);
            insertMainRuntimeContextInit(fn);
            refreshLeafFlag(fn);
        }
    } catch (const std::exception &ex) {
        diags.error(std::string("AArch64 legalization failed: ") + ex.what());
        return false;
    }

    return true;
}

} // namespace zanna::codegen::aarch64::passes
