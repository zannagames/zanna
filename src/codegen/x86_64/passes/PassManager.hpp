//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/PassManager.hpp
// Purpose: x86_64 pass manager types — delegates to common PassManager template.
// Key invariants:
//   - Passes run sequentially, short-circuiting on failure while preserving
//     prior pass results.
//   - Each pass receives the shared Module state.
// Ownership/Lifetime:
//   - PassManager owns registered passes via unique_ptr and operates on a
//     caller-owned Module instance passed by reference.
// Links: src/codegen/common/PassManager.hpp,
//        src/codegen/x86_64/CodegenPipeline.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/Diagnostics.hpp"
#include "codegen/common/PassManager.hpp"
#include "codegen/common/objfile/CodeSection.hpp"
#include "codegen/x86_64/Backend.hpp"
#include "il/core/Module.hpp"

#include <optional>
#include <vector>

/// @file
/// @brief Defines x86-64 pipeline state and target-specialized pass aliases.

namespace zanna::codegen::x64::passes {

/// @brief Mutable state threaded through the x86-64 code-generation passes.
/// @details Owns the canonical input and every progressively derived artifact.
///          Passes mutate this object in place and use the stage flags to reject
///          out-of-order execution. @c mir and @c frames are parallel vectors;
///          textual and binary emission are alternative terminal products.
/// @invariant When MIR is valid, @c mir.size() equals @c frames.size().
/// @invariant A non-null @c target points to a process-lifetime descriptor.
struct Module {
    /// @brief Canonical source IL retained for global and source metadata.
    il::core::Module il;                ///< Original IL module loaded from disk.
    /// @brief Backend adapter module produced by @ref LoweringPass.
    std::optional<ILModule> lowered;    ///< Adapter module produced by lowering.
    /// @brief Borrowed ABI descriptor used by legalization and later passes.
    const TargetInfo *target = nullptr; ///< Target descriptor for lowering/allocation.
    /// @brief Backend options shared by target selection and terminal emission.
    CodegenOptions options{};           ///< Backend configuration for later passes.
    /// @brief Module-wide strings and floating-point literals collected during legalization.
    AsmEmitter::RoDataPool roData{};    ///< Module-level literal pool built during MIR lowering.
    /// @brief Legalized machine functions in source-function order.
    std::vector<MFunction> mir{};       ///< Per-function MIR after legalization.
    /// @brief Frame summaries parallel to @c mir.
    std::vector<FrameInfo> frames{};    ///< Per-function frame summaries aligned with mir.
    /// @brief Whether legalization installed complete MIR and frame state.
    bool legalised = false;             ///< Flag toggled once legalisation completes.
    /// @brief Whether all MIR virtual registers have been allocated.
    bool registersAllocated = false;    ///< Flag toggled once register allocation runs.
    /// @brief Owned terminal textual-assembly result, when @ref EmitPass ran.
    std::optional<CodegenResult> codegenResult; ///< Backend assembly emission artefacts.

    /// @brief Optional merged code section used by debug-line emission.
    std::optional<objfile::CodeSection> binaryText;   ///< Merged text, present for debug output.
    /// @brief Module read-only-data section from @ref BinaryEmitPass.
    std::optional<objfile::CodeSection> binaryRodata; ///< Read-only data section.
    /// @brief Module initialized writable-data section from scalar globals.
    std::optional<objfile::CodeSection> binaryData;   ///< Writable initialized data (.data).
    /// @brief Independently dead-strippable text section for each MIR function.
    std::vector<objfile::CodeSection> binaryTextSections; ///< Per-function text sections.
    /// @brief Pre-encoded DWARF v5 line program, empty when unavailable.
    std::vector<uint8_t> debugLineData;                   ///< Pre-encoded DWARF .debug_line bytes.
};

/// @brief Target-local spelling of the shared diagnostic collector.
using Diagnostics = zanna::codegen::common::Diagnostics;
/// @brief Abstract pass interface specialized for x86-64 @ref Module state.
using Pass = zanna::codegen::common::Pass<Module>;
/// @brief Sequential owning pass manager specialized for x86-64 @ref Module state.
using PassManager = zanna::codegen::common::PassManager<Module>;

} // namespace zanna::codegen::x64::passes
