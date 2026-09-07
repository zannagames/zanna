//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/MirCfg.cpp
// Purpose: Implements the shared AArch64 MIR control-flow graph snapshot and
//          the post-RA block-exit liveness seed.
// Key invariants:
//   - Edge extraction delegates to ra::classifyControlFlow through the
//     backend-neutral extractor; no opcode is classified here.
//   - backEdges()/naturalLoop() use dominance, never layout order, to decide
//     what is a loop.
//   - blockExitLive() decides "leaves the function" with the same classifier.
// Ownership/Lifetime:
//   - Value semantics; no shared state.
// Links: src/codegen/aarch64/MirCfg.hpp, src/codegen/aarch64/ra/Liveness.cpp,
//        src/codegen/common/ra/CfgExtract.hpp,
//        src/codegen/common/ra/GlobalPinning.hpp (computeLoopDepths)
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/MirCfg.hpp"

#include "codegen/aarch64/ra/Liveness.hpp"
#include "codegen/common/ra/CfgExtract.hpp"
#include "codegen/common/ra/DataflowLiveness.hpp"
#include "codegen/common/ra/GlobalPinning.hpp"

#include <algorithm>

/// @file
/// @brief Implements MirCfg and blockExitLive() for AArch64 MIR.

namespace zanna::codegen::aarch64 {

/// @copydoc NaturalLoop::contains
bool NaturalLoop::contains(std::size_t bi) const noexcept {
    return std::binary_search(blocks.begin(), blocks.end(), bi);
}

/// @copydoc MirCfg::MirCfg
MirCfg::MirCfg(const MFunction &fn) {
    for (std::size_t bi = 0; bi < fn.blocks.size(); ++bi)
        blockIndex_.emplace(fn.blocks[bi].name, bi);

    succs_ = zanna::codegen::ra::extractSuccessors(
        fn.blocks,
        blockIndex_,
        /// Expose the instruction vector expected by the generic extractor.
        [](const MBasicBlock &bb) -> const std::vector<MInstr> & { return bb.instrs; },
        /// Translate an AArch64 instruction into the generic branch descriptor.
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

/// @copydoc MirCfg::exitsDirectlyTo
bool MirCfg::exitsDirectlyTo(const MFunction &fn, std::size_t bi, std::size_t to) const {
    if (!hasEdge(bi, to))
        return false;
    if (fallsThrough(bi))
        return bi + 1 == to;
    const auto &instrs = fn.blocks[bi].instrs;
    if (instrs.empty())
        return false;
    const MInstr &last = instrs.back();
    return last.opc == MOpcode::Br && !last.ops.empty() &&
           last.ops[0].kind == MOperand::Kind::Label && last.ops[0].label == fn.blocks[to].name;
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

/// @copydoc carriedExitRegSet
PhysRegSet carriedExitRegSet(const MBasicBlock &block) noexcept {
    PhysRegSet regs;
    for (uint16_t phys : block.carriedExitRegs)
        regs.add(static_cast<PhysReg>(phys));
    return regs;
}

/// @copydoc blockExitLive
PhysRegSet blockExitLive(const MFunction &fn, std::size_t bi, const TargetInfo &target) {
    using Desc = zanna::codegen::ra::BranchDesc;

    const MBasicBlock &block = fn.blocks[bi];
    PhysRegSet live = carriedExitRegSet(block);
    live.add(PhysReg::SP);
    live.add(PhysReg::X29);
    live.add(PhysReg::X30);

    // Does control leave the function here: through Ret, or by falling off
    // the end of the last block (which the verifier reports, but must not be
    // mis-optimised on the way there)?
    bool returns = bi + 1 >= fn.blocks.size();
    for (const MInstr &mi : block.instrs) {
        const Desc desc = ra::classifyControlFlow(mi);
        if (desc.kind == Desc::Kind::Return) {
            returns = true;
            break;
        }
        if (desc.kind == Desc::Kind::Uncond || desc.kind == Desc::Kind::Multi ||
            desc.kind == Desc::Kind::NoReturn) {
            returns = false;
            break;
        }
    }

    if (returns) {
        // Only the result registers reach the caller; the epilogue restores
        // every callee-saved register from its save slot, so a value left in
        // one here is dead.
        live.add(target.intReturnReg);
        live.add(target.f64ReturnReg);
        return live;
    }

    // Every other exit stays inside the function: the callee-saved registers
    // may hold pinned frame slots or values the allocator keeps across blocks.
    for (PhysReg reg : target.calleeSavedGPR)
        live.add(reg);
    for (PhysReg reg : target.calleeSavedFPR)
        live.add(reg);
    return live;
}

} // namespace zanna::codegen::aarch64
