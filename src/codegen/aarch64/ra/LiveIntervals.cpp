//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/LiveIntervals.cpp
// Purpose: Builds the function-wide interval model (block numbering, virtual
//          register ranges with holes, fixed physical ranges, weights, hints)
//          consumed by the AArch64 function-wide register allocator.
// Key invariants:
//   - Ranges are produced by one backward walk per block seeded from the CFG
//     liveness solution; sets are consumed in sorted order so the result does
//     not depend on hash iteration order.
//   - Fixed ranges use the same backward walk over effectsOf(), seeded from
//     the solved physical liveness, so an explicitly written physical
//     register stays occupied until its last read, across blocks if needed.
// Ownership/Lifetime:
//   - Value-owned containers; see LiveIntervals.hpp.
// Links: src/codegen/aarch64/ra/LiveIntervals.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/ra/LiveIntervals.hpp"

#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/MirCfg.hpp"
#include "codegen/aarch64/PhysLiveness.hpp"
#include "codegen/aarch64/ra/Liveness.hpp"
#include "codegen/aarch64/ra/OpcodeClassify.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"
#include "codegen/aarch64/ra/RegClassify.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

/// @file
/// @brief Implements LiveIntervals::build().

namespace zanna::codegen::aarch64::ra {

// -----------------------------------------------------------------------------
// LiveIntervals
// -----------------------------------------------------------------------------

/// @copydoc LiveIntervals::build
void LiveIntervals::build(const MFunction &fn, const TargetInfo &target) {
    positions_ = {};
    vregs_.clear();
    indexById_.clear();
    for (auto &f : fixed_)
        f = RangeList{};
    callPositions_.clear();
    ehPushPositions_.clear();

    numberBlocks(fn);
    buildVRegIntervals(fn);
    buildFixedIntervals(fn, target);
    collectHints(fn);
}

/// @copydoc LiveIntervals::find
const VRegInterval *LiveIntervals::find(uint16_t id) const noexcept {
    const std::size_t idx = indexOf(id);
    return idx == SIZE_MAX ? nullptr : &vregs_[idx];
}

/// @copydoc LiveIntervals::indexOf
std::size_t LiveIntervals::indexOf(uint16_t id) const noexcept {
    if (id >= indexById_.size())
        return SIZE_MAX;
    return indexById_[id];
}

/// @copydoc LiveIntervals::fixed
const RangeList &LiveIntervals::fixed(PhysReg reg) const noexcept {
    return fixed_[static_cast<std::size_t>(reg) & 63u];
}

/// @brief Number the blocks in reverse post-order and lay out positions.
void LiveIntervals::numberBlocks(const MFunction &fn) {
    const std::size_t n = fn.blocks.size();
    const MirCfg cfg(fn);

    // Iterative DFS post-order from the entry over sorted successors.
    std::vector<unsigned char> seen(n, 0);
    std::vector<std::size_t> post;
    post.reserve(n);
    if (n > 0) {
        std::vector<std::pair<std::size_t, std::size_t>> stack; // (block, next succ index)
        stack.emplace_back(0, 0);
        seen[0] = 1;
        while (!stack.empty()) {
            auto &[b, next] = stack.back();
            const auto &succs = cfg.succs(b);
            if (next < succs.size()) {
                const std::size_t s = succs[next++];
                if (!seen[s]) {
                    seen[s] = 1;
                    stack.emplace_back(s, 0);
                }
            } else {
                post.push_back(b);
                stack.pop_back();
            }
        }
    }
    positions_.rpo.assign(post.rbegin(), post.rend());
    for (std::size_t b = 0; b < n; ++b) {
        if (!seen[b])
            positions_.rpo.push_back(b);
    }

    positions_.blockBase.assign(n, 0);
    positions_.blockExit.assign(n, 0);
    Pos cursor = 0;
    for (std::size_t b : positions_.rpo) {
        positions_.blockBase[b] = cursor;
        cursor += static_cast<Pos>(2 * fn.blocks[b].instrs.size());
        positions_.blockExit[b] = cursor + 1;
        cursor += 2;
    }
    positions_.total = cursor;
    positions_.loopDepth = cfg.loopDepths();
}

/// @brief Build every virtual-register interval from the CFG liveness solution.
void LiveIntervals::buildVRegIntervals(const MFunction &fn) {
    LivenessAnalysis liveness;
    liveness.run(fn);

    // Discover ids and classes first so intervals are indexed by id.
    uint16_t maxId = 0;
    std::unordered_map<uint16_t, RegClass> classOf;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            for (const auto &op : mi.ops) {
                if (op.kind != MOperand::Kind::Reg || op.reg.isPhys)
                    continue;
                classOf.emplace(op.reg.idOrPhys, op.reg.cls);
                maxId = std::max(maxId, op.reg.idOrPhys);
            }
        }
    }
    std::vector<uint16_t> ids;
    ids.reserve(classOf.size());
    for (const auto &kv : classOf)
        ids.push_back(kv.first);
    std::sort(ids.begin(), ids.end());

    indexById_.assign(static_cast<std::size_t>(maxId) + 1, SIZE_MAX);
    vregs_.reserve(ids.size());
    for (uint16_t id : ids) {
        indexById_[id] = vregs_.size();
        VRegInterval iv;
        iv.id = id;
        iv.cls = classOf[id];
        vregs_.push_back(std::move(iv));
    }

    /// Sorted copy of a live-out set (deterministic range construction).
    const auto sortedSet = [](const std::unordered_set<uint16_t> &set) {
        std::vector<uint16_t> out(set.begin(), set.end());
        std::sort(out.begin(), out.end());
        return out;
    };

    for (std::size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto &instrs = fn.blocks[b].instrs;
        const double depthWeight = std::pow(10.0, std::min(positions_.loopDepth[b], 6u));

        // Open ranges: vreg -> end position of the range being built.
        std::unordered_map<uint16_t, Pos> open;
        for (uint16_t id : sortedSet(liveness.liveOutGPR(b)))
            open[id] = positions_.blockExit[b];
        for (uint16_t id : sortedSet(liveness.liveOutFPR(b)))
            open[id] = positions_.blockExit[b];

        for (std::size_t i = instrs.size(); i-- > 0;) {
            const MInstr &mi = instrs[i];
            const Pos rp = positions_.readPos(b, i);
            const Pos wp = positions_.writePos(b, i);

            // Writes first (they happen after this instruction's reads).
            for (std::size_t k = 0; k < mi.ops.size(); ++k) {
                const auto &op = mi.ops[k];
                if (op.kind != MOperand::Kind::Reg || op.reg.isPhys)
                    continue;
                const auto [isUse, isDef] = operandRoles(mi, k);
                (void)isUse;
                if (!isDef)
                    continue;
                VRegInterval &iv = vregs_[indexById_[op.reg.idOrPhys]];
                auto it = open.find(op.reg.idOrPhys);
                if (it != open.end()) {
                    iv.live.add(wp, it->second);
                    open.erase(it);
                } else {
                    iv.live.add(wp, wp); // dead definition still occupies its register
                }
                iv.defs.push_back(wp);
                iv.weight += depthWeight;
            }
            for (std::size_t k = 0; k < mi.ops.size(); ++k) {
                const auto &op = mi.ops[k];
                if (op.kind != MOperand::Kind::Reg || op.reg.isPhys)
                    continue;
                const auto [isUse, isDef] = operandRoles(mi, k);
                (void)isDef;
                if (!isUse)
                    continue;
                VRegInterval &iv = vregs_[indexById_[op.reg.idOrPhys]];
                if (open.find(op.reg.idOrPhys) == open.end())
                    open[op.reg.idOrPhys] = rp;
                if (iv.uses.empty() || iv.uses.back() != rp) {
                    iv.uses.push_back(rp);
                    iv.weight += depthWeight;
                }
            }
        }

        // Whatever is still open is live-in to the block.
        std::vector<uint16_t> liveIn;
        liveIn.reserve(open.size());
        for (const auto &kv : open)
            liveIn.push_back(kv.first);
        std::sort(liveIn.begin(), liveIn.end());
        for (uint16_t id : liveIn)
            vregs_[indexById_[id]].live.add(positions_.blockBase[b], open[id]);
    }

    for (VRegInterval &iv : vregs_) {
        std::sort(iv.uses.begin(), iv.uses.end());
        iv.uses.erase(std::unique(iv.uses.begin(), iv.uses.end()), iv.uses.end());
        std::sort(iv.defs.begin(), iv.defs.end());
    }
}

/// @brief Build the fixed range lists of the physical registers and the call positions.
void LiveIntervals::buildFixedIntervals(const MFunction &fn, const TargetInfo &target) {
    const PhysLiveness pl = computePhysLiveness(fn, target);

    /// Iterate the registers of a PhysRegSet in ordinal order.
    const auto forEachReg = [](const PhysRegSet &set, auto &&fnc) {
        for (unsigned bit = 0; bit < 64; ++bit) {
            if ((set.bits & (uint64_t{1} << bit)) == 0)
                continue;
            const PhysReg reg =
                bit < 32 ? static_cast<PhysReg>(bit)
                         : static_cast<PhysReg>(static_cast<unsigned>(PhysReg::V0) + (bit - 32));
            fnc(reg);
        }
    };

    for (std::size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto &instrs = fn.blocks[b].instrs;
        std::array<Pos, 64> openEnd{};
        std::array<unsigned char, 64> isOpen{};
        forEachReg(pl.liveOut[b], [&](PhysReg reg) {
            const auto o = static_cast<std::size_t>(reg) & 63u;
            isOpen[o] = 1;
            openEnd[o] = positions_.blockExit[b];
        });

        for (std::size_t i = instrs.size(); i-- > 0;) {
            const MInstr &mi = instrs[i];
            const Pos rp = positions_.readPos(b, i);
            const Pos wp = positions_.writePos(b, i);
            const InstrEffects fx = effectsOf(mi, target);

            if (isCall(mi.opc)) {
                callPositions_.push_back(wp);
                if (!mi.ops.empty() && mi.ops[0].kind == MOperand::Kind::Label) {
                    std::string label = mi.ops[0].label;
                    if (auto mapped = il::runtime::mapCanonicalRuntimeName(label))
                        label = std::string(*mapped);
                    // The native EH frame push and the setjmp that follows it
                    // are the two calls a longjmp returns through; a value
                    // live across either is memory-homed (EH-1).
                    if (label == "rt_native_eh_push" || label == "setjmp" || label == "_setjmp")
                        ehPushPositions_.push_back(wp);
                }
            }

            forEachReg(fx.defs, [&](PhysReg reg) {
                const auto o = static_cast<std::size_t>(reg) & 63u;
                if (isOpen[o]) {
                    fixed_[o].add(wp, openEnd[o]);
                    isOpen[o] = 0;
                } else {
                    fixed_[o].add(wp, wp);
                }
            });
            forEachReg(fx.uses, [&](PhysReg reg) {
                const auto o = static_cast<std::size_t>(reg) & 63u;
                if (!isOpen[o]) {
                    isOpen[o] = 1;
                    openEnd[o] = rp;
                }
            });
        }

        // Physical live-ins (ABI inputs in the entry block) are occupied from
        // the block start to their last read.
        for (std::size_t o = 0; o < 64; ++o) {
            if (isOpen[o])
                fixed_[o].add(positions_.blockBase[b], openEnd[o]);
        }
    }

    std::sort(callPositions_.begin(), callPositions_.end());
    std::sort(ehPushPositions_.begin(), ehPushPositions_.end());

    for (VRegInterval &iv : vregs_) {
        for (Pos p : callPositions_) {
            if (iv.live.contains(p)) {
                iv.crossesCall = true;
                break;
            }
        }
        for (Pos p : ehPushPositions_) {
            if (iv.live.contains(p)) {
                iv.crossesEhPush = true;
                break;
            }
        }
    }
}

/// @brief Record register hints from moves and parallel copies.
void LiveIntervals::collectHints(const MFunction &fn) {
    /// Record a virtual-virtual affinity in both directions.
    const auto link = [&](uint16_t a, uint16_t b) {
        if (a == b)
            return;
        VRegInterval &ia = vregs_[indexById_[a]];
        VRegInterval &ib = vregs_[indexById_[b]];
        if (std::find(ia.hintVRegs.begin(), ia.hintVRegs.end(), b) == ia.hintVRegs.end())
            ia.hintVRegs.push_back(b);
        if (std::find(ib.hintVRegs.begin(), ib.hintVRegs.end(), a) == ib.hintVRegs.end())
            ib.hintVRegs.push_back(a);
    };

    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            if (mi.opc == MOpcode::ParallelCopy) {
                for (std::size_t k = 0; k + 1 < mi.ops.size(); k += 2) {
                    const auto &dst = mi.ops[k];
                    const auto &src = mi.ops[k + 1];
                    if (dst.kind == MOperand::Kind::Reg && src.kind == MOperand::Kind::Reg &&
                        !dst.reg.isPhys && !src.reg.isPhys && dst.reg.cls == src.reg.cls)
                        link(dst.reg.idOrPhys, src.reg.idOrPhys);
                }
                continue;
            }
            if ((mi.opc != MOpcode::MovRR && mi.opc != MOpcode::FMovRR) || mi.ops.size() != 2)
                continue;
            const auto &dst = mi.ops[0];
            const auto &src = mi.ops[1];
            if (dst.kind != MOperand::Kind::Reg || src.kind != MOperand::Kind::Reg)
                continue;
            if (dst.reg.cls != src.reg.cls)
                continue;
            if (!dst.reg.isPhys && !src.reg.isPhys) {
                link(dst.reg.idOrPhys, src.reg.idOrPhys);
            } else if (!dst.reg.isPhys && src.reg.isPhys) {
                VRegInterval &iv = vregs_[indexById_[dst.reg.idOrPhys]];
                if (!iv.hasPhysHint())
                    iv.hintPhys = static_cast<PhysReg>(src.reg.idOrPhys);
            } else if (dst.reg.isPhys && !src.reg.isPhys) {
                VRegInterval &iv = vregs_[indexById_[src.reg.idOrPhys]];
                if (!iv.hasPhysHint())
                    iv.hintPhys = static_cast<PhysReg>(dst.reg.idOrPhys);
            }
        }
    }
}

/// @copydoc LiveIntervals::dump
std::string LiveIntervals::dump() const {
    std::ostringstream os;
    os << "rpo:";
    for (std::size_t b : positions_.rpo)
        os << ' ' << b << '@' << positions_.blockBase[b] << ".." << positions_.blockExit[b];
    os << "\n";
    for (const VRegInterval &iv : vregs_) {
        os << "%v" << iv.id << ':' << (iv.cls == RegClass::FPR ? "fpr" : "gpr") << ' '
           << iv.live.toString() << " w=" << iv.weight << (iv.crossesCall ? " call" : "")
           << (iv.crossesEhPush ? " eh" : "");
        if (iv.hasPhysHint())
            os << " hint=" << regName(iv.hintPhys);
        os << "\n";
    }
    for (std::size_t o = 0; o < 64; ++o) {
        if (fixed_[o].empty())
            continue;
        const PhysReg reg =
            o < 32 ? static_cast<PhysReg>(o)
                   : static_cast<PhysReg>(static_cast<unsigned>(PhysReg::V0) + (o - 32));
        os << "fixed " << regName(reg) << ' ' << fixed_[o].toString() << "\n";
    }
    return os.str();
}

} // namespace zanna::codegen::aarch64::ra
