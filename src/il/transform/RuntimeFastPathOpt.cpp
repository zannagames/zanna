//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/RuntimeFastPathOpt.cpp
// Purpose: Rewrite generic object RC helpers to known-object helpers when the
//          value provenance proves a heap object rather than a string handle.
// Key invariants:
//   - Provenance originates only from live runtime signature metadata.
//   - Same-block definitions must precede uses; cross-block definitions dominate.
// Ownership/Lifetime: Rewrites borrowed function instructions in place.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements dominance-checked runtime ownership helper specialization.
 *
 * @details Factory calls whose registered signatures guarantee object results
 *          are indexed by SSA ID and source position. A generic ownership call
 *          is rewritten only when the factory precedes it in the same block or
 *          its block dominates the use, preserving provenance validity on
 *          every control-flow path.
 */

#include "il/transform/RuntimeFastPathOpt.hpp"

#include "il/analysis/Dominators.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/runtime/RuntimeSignatures.hpp"
#include "il/transform/AnalysisIDs.hpp"
#include "il/transform/AnalysisManager.hpp"

#include <memory>
#include <unordered_map>
#include <string_view>

using namespace il::core;

namespace il::transform {
namespace {

/// @brief Query whether a runtime helper guarantees an object result.
/// @param callee Direct callee name to resolve in the runtime signature registry.
/// @return `true` when the live signature exists and marks its result as a known object.
[[nodiscard]] bool isKnownObjectFactory(std::string_view callee) {
    const auto *signature = il::runtime::findRuntimeSignature(callee);
    return signature && signature->returnsKnownObject;
}

} // namespace

/// @copydoc RuntimeFastPathOpt::id()
std::string_view RuntimeFastPathOpt::id() const {
    return "runtime-fastpath";
}

/// @copydoc RuntimeFastPathOpt::run()
PreservedAnalyses RuntimeFastPathOpt::run(Function &function, AnalysisManager &analysis) {
    /// @brief Location of a factory result used for dominance checks.
    struct ObjectDefinition {
        /// @brief Block containing the defining call.
        BasicBlock *block{nullptr};
        /// @brief Instruction position used to order same-block definitions and uses.
        std::size_t instructionIndex{0};
    };

    const auto &dominators = analysis.getFunctionResult<zanna::analysis::DomTree>(
        kAnalysisDominators, function);
    std::unordered_map<unsigned, ObjectDefinition> knownObjects;
    bool changed = false;

    for (auto &block : function.blocks) {
        for (std::size_t instructionIndex = 0; instructionIndex < block.instructions.size();
             ++instructionIndex) {
            const auto &instr = block.instructions[instructionIndex];
            if (instr.op == Opcode::Call && instr.result && isKnownObjectFactory(instr.callee))
                knownObjects.emplace(
                    *instr.result, ObjectDefinition{&block, instructionIndex});
        }
    }

    for (auto &block : function.blocks) {
        for (std::size_t instructionIndex = 0; instructionIndex < block.instructions.size();
             ++instructionIndex) {
            auto &instr = block.instructions[instructionIndex];
            if (instr.op != Opcode::Call || instr.operands.empty() ||
                instr.operands.front().kind != Value::Kind::Temp) {
                continue;
            }
            const auto definition = knownObjects.find(instr.operands.front().id);
            if (definition == knownObjects.end())
                continue;

            const bool available = definition->second.block == &block
                                       ? definition->second.instructionIndex < instructionIndex
                                       : dominators.dominates(definition->second.block, &block);
            if (!available)
                continue;

            if (instr.callee == "rt_obj_retain_maybe") {
                instr.setDirectCallee("rt_obj_retain_known");
                changed = true;
            } else if (instr.callee == "rt_obj_release_check0") {
                instr.setDirectCallee("rt_obj_release_known_check0");
                changed = true;
            }
        }
    }

    if (!changed)
        return PreservedAnalyses::all();

    PreservedAnalyses preserved;
    preserved.preserveCFG();
    preserved.preserveDominators();
    preserved.preserveLoopInfo();
    preserved.preserveLiveness();
    return preserved;
}

/// @copydoc registerRuntimeFastPathOptPass()
void registerRuntimeFastPathOptPass(PassRegistry &registry) {
    /// @brief Construct a stateless optimizer for one independently assigned function.
    /// @return Newly owned `RuntimeFastPathOpt` pass.
    registry.registerFunctionPass(
        "runtime-fastpath", []() { return std::make_unique<RuntimeFastPathOpt>(); }, true);
}

} // namespace il::transform
