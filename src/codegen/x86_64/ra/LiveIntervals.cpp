//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/LiveIntervals.cpp
// Purpose: Builds the function-wide interval model (block numbering, virtual
//          register range lists with holes, fixed physical ranges, weights,
//          hints) for the x86-64 function-wide allocator.
// Key invariants:
//   - Virtual ranges come from one backward walk per block seeded with the
//     CFG liveness solution: a read opens a range, a write closes it (a
//     dead definition still occupies its write position).
//   - Fixed ranges come from effectsOf plus the physical liveness solution:
//     an explicit or implicit write occupies its register until the last
//     read that consumes it; a physical live-in occupies the register from
//     the block start.
//   - Every list is built from sorted inputs so the result is deterministic.
// Ownership/Lifetime:
//   - Value-owned containers; valid until build() is called again.
// Links: src/codegen/x86_64/ra/LiveIntervals.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/ra/LiveIntervals.hpp"

#include "codegen/x86_64/MirCfg.hpp"
#include "codegen/x86_64/OperandRoles.hpp"
#include "codegen/x86_64/PhysLiveness.hpp"
#include "codegen/x86_64/ra/Liveness.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>

/// @file
/// @brief Implements LiveIntervals::build() for x86-64.

namespace zanna::codegen::x64::ra {

namespace {

/// @brief Virtual register occurrence in one instruction with its roles.
struct VRegOcc {
    uint16_t id;
    RegClass cls;
    bool isUse;
    bool isDef;
};

/// @brief Collect every virtual register @p mi touches (register operands
///        with their roles, memory address registers as reads).
void collectVRegs(const MInstr &mi, std::vector<VRegOcc> &out) {
    out.clear();
    for (std::size_t k = 0; k < mi.operands.size(); ++k) {
        const Operand &op = mi.operands[k];
        if (const auto *reg = std::get_if<OpReg>(&op)) {
            if (reg->isPhys)
                continue;
            const auto [isUse, isDef] = operandRoles(mi, k);
            out.push_back(VRegOcc{reg->idOrPhys, reg->cls, isUse, isDef});
            continue;
        }
        if (const auto *mem = std::get_if<OpMem>(&op)) {
            if (!mem->base.isPhys)
                out.push_back(VRegOcc{mem->base.idOrPhys, mem->base.cls, true, false});
            if (mem->hasIndex && !mem->index.isPhys)
                out.push_back(VRegOcc{mem->index.idOrPhys, mem->index.cls, true, false});
        }
    }
}

/// @brief Whether @p mi is a call to the native EH frame push or the setjmp
///        that follows it (a longjmp returns through both).
[[nodiscard]] bool isEhPushCall(const MInstr &mi) {
    if (mi.opcode != MOpcode::CALL || mi.operands.empty())
        return false;
    const auto *label = std::get_if<OpLabel>(&mi.operands[0]);
    if (label == nullptr)
        return false;
    std::string name = label->name;
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(name))
        name = std::string(*mapped);
    return name == "rt_native_eh_push" || name == "setjmp" || name == "_setjmp";
}

} // namespace

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
    return fixed_[static_cast<std::size_t>(reg) % kPhysRegCount];
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
        cursor += static_cast<Pos>(2 * fn.blocks[b].instructions.size());
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
    std::vector<VRegOcc> occs;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instructions) {
            collectVRegs(mi, occs);
            for (const VRegOcc &o : occs) {
                classOf.emplace(o.id, o.cls);
                maxId = std::max(maxId, o.id);
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
        const auto &instrs = fn.blocks[b].instructions;
        const double depthWeight = std::pow(10.0, std::min(positions_.loopDepth[b], 6u));

        // Open ranges: vreg -> end position of the range being built.
        std::unordered_map<uint16_t, Pos> open;
        for (uint16_t id : sortedSet(liveness.liveOut(b)))
            open[id] = positions_.blockExit[b];

        for (std::size_t i = instrs.size(); i-- > 0;) {
            const MInstr &mi = instrs[i];
            const Pos rp = positions_.readPos(b, i);
            const Pos wp = positions_.writePos(b, i);
            collectVRegs(mi, occs);

            // Writes first (they happen after this instruction's reads).
            for (const VRegOcc &o : occs) {
                if (!o.isDef)
                    continue;
                VRegInterval &iv = vregs_[indexById_[o.id]];
                auto it = open.find(o.id);
                if (it != open.end()) {
                    iv.live.add(wp, it->second);
                    open.erase(it);
                } else {
                    iv.live.add(wp, wp); // dead definition still occupies its register
                }
                iv.defs.push_back(wp);
                iv.weight += depthWeight;
            }
            for (const VRegOcc &o : occs) {
                if (!o.isUse)
                    continue;
                VRegInterval &iv = vregs_[indexById_[o.id]];
                if (open.find(o.id) == open.end())
                    open[o.id] = rp;
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

    /// Iterate the register ordinals of a mask.
    const auto forEachReg = [](PhysRegMask mask, auto &&fnc) {
        for (unsigned bit = 0; bit < kPhysRegCount; ++bit) {
            if ((mask & (PhysRegMask{1} << bit)) != 0)
                fnc(static_cast<std::size_t>(bit));
        }
    };

    for (std::size_t b = 0; b < fn.blocks.size(); ++b) {
        const auto &instrs = fn.blocks[b].instructions;
        std::array<Pos, kPhysRegCount> openEnd{};
        std::array<unsigned char, kPhysRegCount> isOpen{};
        forEachReg(pl.liveOut[b], [&](std::size_t o) {
            isOpen[o] = 1;
            openEnd[o] = positions_.blockExit[b];
        });

        for (std::size_t i = instrs.size(); i-- > 0;) {
            const MInstr &mi = instrs[i];
            const Pos rp = positions_.readPos(b, i);
            const Pos wp = positions_.writePos(b, i);
            const InstrEffects fx = effectsOf(mi, target);

            if (fx.isCall) {
                callPositions_.push_back(wp);
                if (isEhPushCall(mi))
                    ehPushPositions_.push_back(wp);
            }

            forEachReg(fx.defs, [&](std::size_t o) {
                if (isOpen[o]) {
                    fixed_[o].add(wp, openEnd[o]);
                    isOpen[o] = 0;
                } else {
                    fixed_[o].add(wp, wp);
                }
            });
            forEachReg(fx.uses, [&](std::size_t o) {
                if (!isOpen[o]) {
                    isOpen[o] = 1;
                    openEnd[o] = rp;
                }
            });
        }

        // Physical live-ins (ABI inputs in the entry block) are occupied from
        // the block start to their last read.
        for (std::size_t o = 0; o < kPhysRegCount; ++o) {
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
    /// Record a physical hint for a virtual register (first one wins).
    const auto hintPhys = [&](uint16_t id, PhysReg reg) {
        VRegInterval &iv = vregs_[indexById_[id]];
        if (!iv.hasPhysHint) {
            iv.hasPhysHint = true;
            iv.hintPhys = reg;
        }
    };
    /// Record the affinity of one (dst, src) copy pair.
    const auto pair = [&](const OpReg &dst, const OpReg &src) {
        if (dst.cls != src.cls)
            return;
        if (!dst.isPhys && !src.isPhys)
            link(dst.idOrPhys, src.idOrPhys);
        else if (!dst.isPhys && src.isPhys)
            hintPhys(dst.idOrPhys, static_cast<PhysReg>(src.idOrPhys));
        else if (dst.isPhys && !src.isPhys)
            hintPhys(src.idOrPhys, static_cast<PhysReg>(dst.idOrPhys));
    };

    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instructions) {
            if (mi.opcode == MOpcode::PX_COPY) {
                for (std::size_t k = 0; k + 1 < mi.operands.size(); k += 2) {
                    const auto *dst = std::get_if<OpReg>(&mi.operands[k]);
                    const auto *src = std::get_if<OpReg>(&mi.operands[k + 1]);
                    if (dst && src)
                        pair(*dst, *src);
                }
                continue;
            }
            if ((mi.opcode != MOpcode::MOVrr && mi.opcode != MOpcode::MOVSDrr) ||
                mi.operands.size() != 2)
                continue;
            const auto *dst = std::get_if<OpReg>(&mi.operands[0]);
            const auto *src = std::get_if<OpReg>(&mi.operands[1]);
            if (dst && src)
                pair(*dst, *src);
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
        os << "%v" << iv.id << ':' << (iv.cls == RegClass::XMM ? "xmm" : "gpr") << ' '
           << iv.live.toString() << " w=" << iv.weight << (iv.crossesCall ? " call" : "")
           << (iv.crossesEhPush ? " eh" : "");
        if (iv.hasPhysHint)
            os << " hint=" << regName(iv.hintPhys);
        os << "\n";
    }
    for (std::size_t o = 0; o < kPhysRegCount; ++o) {
        if (fixed_[o].empty())
            continue;
        os << "fixed " << regName(static_cast<PhysReg>(o)) << ' ' << fixed_[o].toString() << "\n";
    }
    return os.str();
}

} // namespace zanna::codegen::x64::ra
