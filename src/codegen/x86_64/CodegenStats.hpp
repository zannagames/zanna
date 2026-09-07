//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/CodegenStats.hpp
// Purpose: Machine-readable code-shape counters for final x86-64 MIR, the
//          mirror of the AArch64 counters so scripts/codegen_stats.sh reads
//          one format for both backends.
// Key invariants:
//   - A frame access is any instruction with an RBP-relative memory operand
//     other than LEA and other than the prologue/epilogue moves through the
//     callee-saved save area; store opcodes count as frame stores, everything
//     else as frame loads.
//   - `offsetPrefixes` is always 0: x86-64 encodes every displacement inline.
//   - The report line format is stable: `[codegen-stats] arch=x64 fn=<name>`
//     followed by `key=value` pairs.
// Ownership/Lifetime:
//   - Stateless free functions.
// Links: src/codegen/aarch64/CodegenStats.hpp,
//        src/codegen/x86_64/passes/CodegenStatsPass.hpp, scripts/codegen_stats.sh
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/x86_64/FrameLowering.hpp"
#include "codegen/x86_64/MachineIR.hpp"

#include <cstddef>
#include <string>

/// @file
/// @brief Declares the x86-64 codegen statistics counters.

namespace zanna::codegen::x64 {

/// @brief Code-shape counters for one function or a whole module.
struct CodegenStats {
    std::size_t functions{0};      ///< Functions folded into this record.
    std::size_t blocks{0};         ///< Basic blocks.
    std::size_t instructions{0};   ///< Instructions.
    std::size_t calls{0};          ///< `CALL`.
    std::size_t branches{0};       ///< `JMP`, `JCC`, `JUMPTABLE`, `RET`.
    std::size_t moves{0};          ///< `MOVrr` / `MOVSDrr`.
    std::size_t loads{0};          ///< Memory loads and `POP`.
    std::size_t stores{0};         ///< Memory stores and `PUSH`.
    std::size_t frameLoads{0};     ///< Instructions reading an RBP-relative slot.
    std::size_t frameStores{0};    ///< Stores to an RBP-relative slot.
    std::size_t offsetPrefixes{0}; ///< Always 0 on x86-64.
    std::size_t spillSlots{0};     ///< Spill area in 8-byte slots.
    std::size_t frameBytes{0};     ///< `FrameInfo::frameSize`.
    std::size_t calleeSaved{0};    ///< Saved callee-saved registers.

    /// @brief Fold @p other into this record.
    void add(const CodegenStats &other) noexcept;
};

/// @brief Whether `ZANNA_CODEGEN_STATS` requests a report.
[[nodiscard]] bool codegenStatsEnabled() noexcept;

/// @brief Count @p fn with its frame summary @p frame.
[[nodiscard]] CodegenStats computeCodegenStats(const MFunction &fn, const FrameInfo &frame);

/// @brief Render one report line for @p stats labelled @p name.
[[nodiscard]] std::string formatCodegenStats(const CodegenStats &stats, const std::string &name);

} // namespace zanna::codegen::x64
