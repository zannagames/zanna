//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ra/CfgExtract.hpp
// Purpose: Shared MIR control-flow-graph extraction for backend register
//          allocators. Walks each block's instructions through a
//          backend-supplied classifier and produces per-block successor index
//          lists, handling conditional branches, unconditional branches,
//          returns, no-return calls, and implicit fallthrough uniformly.
// Key invariants:
//   - Every conditional branch in a block contributes a successor edge, not
//     just the one nearest the terminator (multi-branch blocks are legal MIR
//     for some lowerings, e.g. switch compare cascades).
//   - A block that ends without an unconditional terminator, or whose last
//     terminator is conditional, falls through to the next block in layout
//     order when one exists.
//   - Successor lists are sorted and deduplicated for deterministic dataflow.
// Ownership/Lifetime:
//   - Header-only function template; no state.
// Links: codegen/common/ra/DataflowLiveness.hpp,
//        codegen/x86_64/ra/Liveness.cpp,
//        codegen/aarch64/ra/Liveness.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file CfgExtract.hpp
 * @brief Defines backend-neutral MIR control-flow successor extraction.
 */

#pragma once

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::ra {

/// @brief Control-flow classification of a single MIR instruction.
/// @details Returned by the backend-supplied classifier callback. The
///          @c target pointer (when non-null) names a basic-block label that
///          becomes a successor edge; labels that do not resolve to a block
///          (e.g. external symbols) are ignored by the extractor.
struct BranchDesc {
    /// @brief Control-flow effect of one classified MIR instruction.
    enum class Kind {
        None,     ///< Not a control-flow instruction; scanning continues.
        Cond,     ///< Conditional branch: adds @c target, keeps scanning,
                  ///< and requests fallthrough if it ends the block.
        Uncond,   ///< Unconditional branch: adds @c target and ends the scan.
        Multi,    ///< Multi-way transfer (jump table): adds every entry of
                  ///< @c multiTargets and ends the scan.
        Return,   ///< Function return: no successors, ends the scan.
        NoReturn, ///< Trap / no-return call: no successors, ends the scan.
    };

    ///< Classified control-flow effect.
    Kind kind{Kind::None};
    const std::string *target{nullptr}; ///< Branch target label, if any.
    std::vector<const std::string *> multiTargets{}; ///< Targets for Kind::Multi.
};

/// @brief Extract per-block successor index lists from MIR blocks.
///
/// @details Walks every instruction of every block through @p classify and
///          builds the successor relation consumed by
///          @ref solveBackwardDataflow. Conditional branches contribute their
///          targets and keep the scan going (so a compare cascade with several
///          conditional branches yields every edge); the first unconditional
///          branch / return / no-return instruction ends the block's scan. A
///          block whose scan ends without an explicit block-ending terminator
///          falls through to the next block in layout order.
///
/// @tparam BlockRange Container of blocks (indexable, sized).
/// @tparam InstrsOf   Callable: const Block& -> const instruction container&.
/// @tparam Classify   Callable: const Instr& -> BranchDesc.
/// @param blocks     Function blocks in layout order.
/// @param blockIndex Map from block label to block index.
/// @param instrsOf   Accessor returning a block's instruction list.
/// @param classify   Per-instruction control-flow classifier.
/// @return Per-block sorted, deduplicated successor index lists.
template <typename BlockRange, typename InstrsOf, typename Classify>
std::vector<std::vector<std::size_t>> extractSuccessors(
    const BlockRange &blocks,
    const std::unordered_map<std::string, std::size_t> &blockIndex,
    InstrsOf &&instrsOf,
    Classify &&classify) {
    const std::size_t n = blocks.size();
    std::vector<std::vector<std::size_t>> succs(n);

    for (std::size_t bi = 0; bi < n; ++bi) {
        auto &out = succs[bi];
        bool endedExplicitly = false;

        for (const auto &instr : instrsOf(blocks[bi])) {
            const BranchDesc desc = classify(instr);
            switch (desc.kind) {
                case BranchDesc::Kind::None:
                case BranchDesc::Kind::Cond:
                    if (desc.kind == BranchDesc::Kind::Cond && desc.target != nullptr) {
                        auto it = blockIndex.find(*desc.target);
                        if (it != blockIndex.end())
                            out.push_back(it->second);
                    }
                    continue;
                case BranchDesc::Kind::Uncond:
                    if (desc.target != nullptr) {
                        auto it = blockIndex.find(*desc.target);
                        if (it != blockIndex.end())
                            out.push_back(it->second);
                    }
                    endedExplicitly = true;
                    break;
                case BranchDesc::Kind::Multi:
                    for (const std::string *target : desc.multiTargets) {
                        if (target == nullptr)
                            continue;
                        auto it = blockIndex.find(*target);
                        if (it != blockIndex.end())
                            out.push_back(it->second);
                    }
                    endedExplicitly = true;
                    break;
                case BranchDesc::Kind::Return:
                case BranchDesc::Kind::NoReturn:
                    endedExplicitly = true;
                    break;
            }
            if (endedExplicitly)
                break;
        }

        // Fallthrough: a block that never ended in an unconditional transfer
        // (no terminator at all, or a trailing conditional branch) continues
        // into the next block in layout order.
        if (!endedExplicitly && bi + 1 < n)
            out.push_back(bi + 1);

        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }

    return succs;
}

} // namespace zanna::codegen::ra
