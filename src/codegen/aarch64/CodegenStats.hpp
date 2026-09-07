//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/CodegenStats.hpp
// Purpose: Machine-readable code-shape counters for final AArch64 MIR: the
//          measurement behind the register-allocation work (frame traffic,
//          offset-materialization prefixes, spill slots, frame bytes).
// Key invariants:
//   - Counters describe the MIR they are given; run after ExpandPseudos they
//     describe what the emitters print.
//   - A frame access is any load/store through x29 (FpImm forms, pair forms,
//     PhiStore) or through a scratch register that the immediately preceding
//     `MovRI xS,#off; AddRRR xS,x29,xS` prefix pointed into the frame.
//   - The report line format is stable: `[codegen-stats] arch=arm64 fn=<name>`
//     followed by `key=value` pairs; scripts/codegen_stats.sh parses it.
// Ownership/Lifetime:
//   - Stateless free functions.
// Links: src/codegen/aarch64/passes/CodegenStatsPass.hpp,
//        scripts/codegen_stats.sh, docs/internals/backend.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"

#include <cstddef>
#include <string>

/// @file
/// @brief Declares the AArch64 codegen statistics counters.

namespace zanna::codegen::aarch64 {

/// @brief Code-shape counters for one function or a whole module.
struct CodegenStats {
    std::size_t functions{0};      ///< Functions folded into this record.
    std::size_t blocks{0};         ///< Basic blocks.
    std::size_t instructions{0};   ///< Instructions.
    std::size_t calls{0};          ///< `Bl` / `Blr`.
    std::size_t branches{0};       ///< Branches, jump tables and returns.
    std::size_t moves{0};          ///< `MovRR` / `FMovRR`.
    std::size_t loads{0};          ///< Every load form.
    std::size_t stores{0};         ///< Every store form.
    std::size_t frameLoads{0};     ///< Loads that read the frame.
    std::size_t frameStores{0};    ///< Stores that write the frame.
    std::size_t offsetPrefixes{0}; ///< `MovRI xS,#off; AddRRR xS,x29,xS` pairs.
    std::size_t spillSlots{0};     ///< Distinct spill-slot offsets.
    std::size_t frameBytes{0};     ///< `frame.totalBytes`.
    std::size_t calleeSaved{0};    ///< Saved callee-saved registers.

    /// @brief Fold @p other into this record.
    void add(const CodegenStats &other) noexcept;
};

/// @brief Whether `ZANNA_CODEGEN_STATS` requests a report.
/// @return `true` for a non-empty value whose first character is not `0`.
[[nodiscard]] bool codegenStatsEnabled() noexcept;

/// @brief Count @p fn.
[[nodiscard]] CodegenStats computeCodegenStats(const MFunction &fn);

/// @brief Render one report line for @p stats labelled @p name.
[[nodiscard]] std::string formatCodegenStats(const CodegenStats &stats, const std::string &name);

} // namespace zanna::codegen::aarch64
