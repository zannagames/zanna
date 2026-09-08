//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/LoweringPass.cpp
// Purpose: IL→MIR lowering for the AArch64 modular pipeline. Builds the
//          RodataPool from string globals, lowers each IL function in parallel,
//          sanitizes block labels, and remaps AdrPage/AddPageOff rodata refs.
// Key invariants:
//   - LowerILToMIR instances are created per-function and are thread-safe.
//   - Block labels gain a function-name suffix when the module has >1 function
//     to prevent assembler label collisions across function bodies.
//   - All block labels are prefixed with "L" for Mach-O local-label semantics.
// Ownership/Lifetime:
//   - Stateless pass; mutates AArch64Module::mir and ::rodataPool.
// Links: src/codegen/aarch64/passes/LoweringPass.hpp,
//        src/codegen/aarch64/LowerILToMIR.hpp,
//        src/codegen/aarch64/passes/LegalizePass.cpp (downstream)
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/LoweringPass.hpp"

#include "codegen/aarch64/LowerILToMIR.hpp"
#include "codegen/common/LabelUtil.hpp"
#include "codegen/common/Parallelism.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

/**
 * @file
 * @brief Implements parallel IL-to-MIR lowering and module label normalization.
 *
 * Each function receives an independent lowerer and result slot. After
 * lowering, internal block targets are sanitized/uniquified and literal global
 * references are rewritten to deterministic rodata-pool labels.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Replaces prior codegen products with freshly lowered AArch64 MIR.
 * @param[in,out] module Module containing IL and target metadata.
 * @param[in,out] diags Sink receiving the first lowering error.
 * @return `true` when all indexed output slots are populated successfully.
 */
bool LoweringPass::run(AArch64Module &module, Diagnostics &diags) {
    if (!module.ilMod || !module.ti) {
        diags.error("LoweringPass: ilMod and ti must be non-null");
        return false;
    }

    module.mir.clear();
    module.assembly.clear();
    module.rodataPool = RodataPool{};
    module.binaryText.reset();
    module.binaryRodata.reset();
    module.binaryTextSections.clear();
    module.debugLineData.clear();

    const il::core::Module &ilMod = *module.ilMod;
    const TargetInfo &ti = *module.ti;

    // Build the rodata pool from all string globals in the module.
    module.rodataPool.buildFromModule(ilMod);
    const auto &n2l = module.rodataPool.nameToLabel();
    std::unordered_map<std::string, std::size_t> stringLiteralByteLengths;
    stringLiteralByteLengths.reserve(ilMod.globals.size());
    for (const auto &g : ilMod.globals) {
        if (g.type.kind == il::core::Type::Kind::Str)
            stringLiteralByteLengths.emplace(g.name, g.init.size());
    }

    std::unordered_map<std::string, std::size_t> knownVarArgNamedArgCounts;
    for (const auto &fn : ilMod.functions) {
        if (fn.isVarArg)
            knownVarArgNamedArgCounts.emplace(fn.name, fn.params.size());
    }
    const bool uniquify = (ilMod.functions.size() > 1);

    std::vector<MFunction> lowered(ilMod.functions.size());
    std::string firstError;
    std::mutex errorMutex;

    /**
     * @brief Lowers and normalizes one function into its deterministic output slot.
     * @param index Index shared by `ilMod.functions` and `lowered`.
     */
    auto lowerOne = [&](std::size_t index) {
        try {
            const auto &fn = ilMod.functions[index];
            LowerILToMIR lowerer{ti, &stringLiteralByteLengths};
            lowerer.setKnownVarArgCallees(knownVarArgNamedArgCounts);
            MFunction mir = lowerer.lowerFunction(fn);

            // --- Label sanitization: hyphens → underscores, optional suffix ----
            using zanna::codegen::common::sanitizeLabel;
            std::unordered_map<std::string, std::string> bbMap;
            bbMap.reserve(mir.blocks.size());

            const std::string suffix = uniquify ? (std::string("_") + fn.name) : std::string{};

            for (std::size_t bi = 0; bi < mir.blocks.size(); ++bi) {
                auto &bb = mir.blocks[bi];
                const std::string old = bb.name;
                // All block labels use the Mach-O assembler-local "L" prefix so
                // they stay out of the symbol table and are compatible with
                // .subsections_via_symbols.  The function's external symbol
                // (_main, _f, etc.) is emitted separately by AsmEmitter::
                // emitFunctionHeader, so block labels are purely internal.
                const std::string neu = "L" + sanitizeLabel(old, suffix);
                bbMap.emplace(old, neu);
                bb.name = neu;
            }

            // Remap branch target labels to their sanitized names.
            for (auto &bb : mir.blocks) {
                for (auto &mi : bb.instrs) {
                    /**
                     * @brief Rewrites a raw block label when it appears in the local map.
                     * @param[in,out] lbl Label operand text.
                     */
                    auto remapBB = [&](std::string &lbl) {
                        auto it = bbMap.find(lbl);
                        if (it != bbMap.end())
                            lbl = it->second;
                    };

                    switch (mi.opc) {
                        case MOpcode::Br:
                            if (!mi.ops.empty() && mi.ops[0].kind == MOperand::Kind::Label)
                                remapBB(mi.ops[0].label);
                            break;
                        case MOpcode::BCond:
                        case MOpcode::Cbz:
                        case MOpcode::Cbnz:
                        case MOpcode::Tbz:
                        case MOpcode::Tbnz:
                            if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Label)
                                remapBB(mi.ops[1].label);
                            break;
                        case MOpcode::JumpTable:
                            // Case labels start at operand 2.
                            for (std::size_t li = 2; li < mi.ops.size(); ++li) {
                                if (mi.ops[li].kind == MOperand::Kind::Label)
                                    remapBB(mi.ops[li].label);
                            }
                            break;
                        default:
                            break;
                    }

                    // --- Rodata label remapping: IL global names → pool labels --
                    switch (mi.opc) {
                        case MOpcode::AdrPage:
                            if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Label) {
                                auto it = n2l.find(mi.ops[1].label);
                                if (it != n2l.end())
                                    mi.ops[1].label = it->second;
                            }
                            break;
                        case MOpcode::AddPageOff:
                            if (mi.ops.size() >= 3 && mi.ops[2].kind == MOperand::Kind::Label) {
                                auto it = n2l.find(mi.ops[2].label);
                                if (it != n2l.end())
                                    mi.ops[2].label = it->second;
                            }
                            break;
                        default:
                            break;
                    }
                }
            }

            lowered[index] = std::move(mir);
        } catch (const std::exception &ex) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (firstError.empty())
                firstError = ex.what();
        }
    };

    const std::size_t functionCount = ilMod.functions.size();
    const std::size_t workerCount = common::codegenWorkerCount(functionCount);
    if (workerCount <= 1) {
        for (std::size_t i = 0; i < functionCount; ++i)
            lowerOne(i);
    } else {
        std::atomic_size_t nextIndex{0};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            /// @brief Claims lowering indices atomically until all functions are assigned.
            workers.emplace_back([&]() {
                for (;;) {
                    const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (index >= functionCount)
                        break;
                    lowerOne(index);
                }
            });
        }
        for (auto &worker : workers)
            worker.join();
    }

    if (!firstError.empty()) {
        diags.error(std::string("AArch64 lowering failed: ") + firstError);
        return false;
    }

    module.mir = std::move(lowered);
    return true;
}

} // namespace zanna::codegen::aarch64::passes
