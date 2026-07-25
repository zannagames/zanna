//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/transform/Devirtualize.cpp
// Purpose: Fold call.indirect through constant function addresses to call.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements direct-call recovery from constant function addresses.

#include "il/transform/Devirtualize.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"

#include <memory>
#include <string>
#include <unordered_map>

using namespace il::core;

namespace il::transform {
namespace {

/// @brief Extract the symbol spelling from a global-address value.
/// @param value Candidate callee value.
/// @return Owned symbol name, or an empty string for other value kinds.
[[nodiscard]] std::string globalNameFromValue(const Value &value) {
    if (value.kind == Value::Kind::GlobalAddr)
        return value.str;
    return {};
}

} // namespace

/// @brief Return the pass registry identifier.
/// @return Stable `devirt` identifier.
std::string_view Devirtualize::id() const {
    return "devirt";
}

/// @brief Convert statically resolved indirect calls into direct calls.
/// @details Tracks `gaddr` results and accepts inline global-address operands.
///          Rewrites remove the function-pointer operand and obsolete indirect
///          signature metadata.
/// @param function Function updated in place.
/// @param analysis Unused analysis manager.
/// @return All analyses when unchanged; otherwise preserves module and CFG
///         structural analyses.
PreservedAnalyses Devirtualize::run(Function &function, AnalysisManager & /*analysis*/) {
    std::unordered_map<unsigned, std::string> globalByTemp;
    bool changed = false;

    for (auto &block : function.blocks) {
        for (auto &instr : block.instructions) {
            if (instr.op == Opcode::GAddr && instr.result && instr.operands.size() == 1) {
                std::string name = globalNameFromValue(instr.operands.front());
                if (!name.empty())
                    globalByTemp[*instr.result] = std::move(name);
                continue;
            }

            if (instr.op != Opcode::CallIndirect || instr.operands.empty())
                continue;

            std::string callee = globalNameFromValue(instr.operands.front());
            if (callee.empty() && instr.operands.front().kind == Value::Kind::Temp) {
                auto it = globalByTemp.find(instr.operands.front().id);
                if (it != globalByTemp.end())
                    callee = it->second;
            }

            if (callee.empty())
                continue;

            instr.op = Opcode::Call;
            instr.setDirectCallee(std::move(callee));
            instr.operands.erase(instr.operands.begin());
            instr.clearIndirectSignature();
            changed = true;
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

/// @brief Register the parallel-safe devirtualization pass factory.
/// @param registry Pass registry updated with `devirt`.
void registerDevirtualizePass(PassRegistry &registry) {
    /// Construct a fresh pass for each pipeline request.
    registry.registerFunctionPass(
        "devirt", []() { return std::make_unique<Devirtualize>(); }, true);
}

} // namespace il::transform
