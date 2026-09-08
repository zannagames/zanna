//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/PassManager.hpp
// Purpose: AArch64 pass manager types — delegates to common PassManager template.
// Key invariants: Passes run sequentially, short-circuiting on failure while preserving
//                 prior pass results. Each pass receives the shared AArch64Module state.
// Ownership/Lifetime: PassManager owns registered passes via unique_ptr and operates on
//                     a caller-owned AArch64Module instance passed by reference.
// Links: docs/internals/codemap.md, src/codegen/aarch64/
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/RodataPool.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/common/Diagnostics.hpp"
#include "codegen/common/PassManager.hpp"
#include "codegen/common/objfile/CodeSection.hpp"
#include "il/core/Module.hpp"

#include <optional>
#include <string>
#include <vector>

/**
 * @file
 * @brief Defines shared AArch64 pipeline state and common pass-manager aliases.
 *
 * Pass implementations progressively replace fields in `AArch64Module`.
 * Input module and target pointers are non-owning; all MIR, textual, section,
 * and debug products are owned by the state object.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Owns mutable artifacts threaded through the AArch64 pass sequence.
 *
 * Lowering produces MIR/global pools, legalization and optimization mutate MIR,
 * and one or both emitters populate assembly or object-section products.
 *
 * @invariant Non-null `ilMod` and `ti` pointers outlive every pass that reads them.
 * @invariant `binaryTextSections` remains in MIR function order.
 */
struct AArch64Module {
    const il::core::Module *ilMod = nullptr; ///< Non-owning pointer to the IL module.
    const TargetInfo *ti = nullptr;          ///< Non-owning pointer to the target info.
    std::string debugSourcePath{};     ///< Source path used for DWARF line table file entries.
    bool emitDebugLines = false;       ///< Emit DWARF .debug_line entries when true.
    bool coalesceTextSections = false; ///< Emit one text section instead of per-function sections.
    std::vector<MFunction> mir;        ///< MIR functions, populated by LoweringPass.
    RodataPool rodataPool;             ///< Rodata pool, populated by LoweringPass.
    std::string assembly;              ///< Final assembly text, populated by EmitPass.

    // Binary emission output (populated by BinaryEmitPass).
    std::optional<objfile::CodeSection> binaryText;       ///< Machine code bytes + relocations.
    std::optional<objfile::CodeSection> binaryRodata;     ///< Read-only data section.
    std::optional<objfile::CodeSection> binaryData;       ///< Writable initialized data section.
    std::vector<objfile::CodeSection> binaryTextSections; ///< Per-function text sections.
    std::vector<uint8_t> debugLineData;                   ///< Pre-encoded DWARF .debug_line bytes.
};

// Backward-compatible aliases — consumers use these names unchanged.
using Diagnostics = zanna::codegen::common::Diagnostics;
using Pass = zanna::codegen::common::Pass<AArch64Module>;
using PassManager = zanna::codegen::common::PassManager<AArch64Module>;

} // namespace zanna::codegen::aarch64::passes
