//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/MirCfg.cpp
// Purpose: Implements the shared x86-64 MIR control-flow graph snapshot.
// Key invariants:
//   - Edge extraction delegates to ra::classifyControlFlow through the
//     backend-neutral extractor; no opcode is classified here.
//   - backEdges()/naturalLoop() use dominance, never layout order, to decide
//     what is a loop.
// Ownership/Lifetime:
//   - Value semantics; no shared state.
// Links: src/codegen/x86_64/MirCfg.hpp, src/codegen/x86_64/ra/Liveness.cpp,
//        src/codegen/common/ra/CfgExtract.hpp,
//        src/codegen/common/ra/GlobalPinning.hpp (computeLoopDepths)
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/MirCfg.hpp"

#include "codegen/common/ra/CfgExtract.hpp"
#include "codegen/common/ra/DataflowLiveness.hpp"
#include "codegen/common/ra/GlobalPinning.hpp"
#include "codegen/x86_64/ra/Liveness.hpp"

#include <algorithm>

/// @file
/// @brief Implements MirCfg for x86-64 MIR.

namespace zanna::codegen::x64 {

/// @copydoc NaturalLoop::contains
bool NaturalLoop::contains(std::size_t bi) const noexcept {
    return std::binary_search(blocks.begin(), blocks.end(), bi);
}

/// @copydoc MirCfg::MirCfg
MirCfg::MirCfg(const MFunction &fn) {
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi)
        blockIndex_.emplace(fn.blocks[bi].label, bi);

    succs_ = zanna::codegen::ra::extractSuccessors(
        fn.blocks,
        blockIndex_,
        /// Expose the instruction vector expected by the generic extractor.
        [](const MBasicBlock &bb) -> const std::vector<MInstr> & { return bb.instructions; },
        /// Translate an x86-64 instruction into the generic branch descriptor.
        [](const MInstr &mi) { return ra::classifyControlFlow(mi); },
        &fallsThrough_);
    preds_ = zanna::codegen::ra::buildPredecessors(succs_);
}

/// @copydoc MirCfg::indexOf
std::optional<std::size_t> MirCfg::indexOf(const std::string &label) const {
    const auto it = blockIndex_.find(label);
    if (it == blockIndex_.end())
        return std::nullopt;
    return it->second;
}

/// @copydoc MirCfg::hasEdge
bool MirCfg::hasEdge(std::size_t from, std::size_t to) const {
    if (from >= succs_.size())
        return false;
    return std::binary_search(succs_[from].begin(), succs_[from].end(), to);
}

/// @copydoc MirCfg::anyFallthrough
bool MirCfg::anyFallthrough() const noexcept {
    return std::any_of(
        fallsThrough_.begin(), fallsThrough_.end(), [](unsigned char flag) { return flag != 0; });
}

/// @copydoc MirCfg::dominators
const zanna::codegen::ra::DominatorSets &MirCfg::dominators() const {
    if (!dominators_)
        dominators_ = zanna::codegen::ra::computeDominators(succs_.size(), preds_);
    return *dominators_;
}

/// @copydoc MirCfg::backEdges
std::vector<BackEdge> MirCfg::backEdges() const {
    std::vector<BackEdge> edges;
    const auto &dom = dominators();
    for (std::size_t latch = 0; latch < succs_.size(); ++latch) {
        for (std::size_t header : succs_[latch]) {
            if (dom.dominates(header, latch))
                edges.push_back(BackEdge{latch, header});
        }
    }
    return edges;
}

/// @copydoc MirCfg::naturalLoop
NaturalLoop MirCfg::naturalLoop(const BackEdge &edge) const {
    NaturalLoop loop;
    loop.edge = edge;
    const std::size_t n = preds_.size();
    if (edge.header >= n || edge.latch >= n)
        return loop;

    std::vector<unsigned char> inLoop(n, 0);
    inLoop[edge.header] = 1;
    std::vector<std::size_t> work;
    if (!inLoop[edge.latch]) {
        inLoop[edge.latch] = 1;
        work.push_back(edge.latch);
    }
    while (!work.empty()) {
        const std::size_t node = work.back();
        work.pop_back();
        for (std::size_t pred : preds_[node]) {
            if (!inLoop[pred]) {
                inLoop[pred] = 1;
                work.push_back(pred);
            }
        }
    }
    for (std::size_t bi = 0; bi < n; ++bi) {
        if (inLoop[bi])
            loop.blocks.push_back(bi);
    }
    return loop;
}

/// @copydoc MirCfg::loopDepths
std::vector<unsigned> MirCfg::loopDepths() const {
    return zanna::codegen::ra::computeLoopDepths(succs_);
}

} // namespace zanna::codegen::x64
