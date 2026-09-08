//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/ra/GlobalAllocator.cpp
// Purpose: Implements the AArch64 function-wide register allocator: interval
//          assignment, shared spill-slot layout, and the operand rewrite
//          with reloads, spill stores, and parallel-copy lowering.
// Key invariants:
//   - See GlobalAllocator.hpp. Every position query goes through the
//     per-register occupancy range lists (fixed ranges plus the intervals
//     assigned to the register), so "free at this instruction" has one
//     definition for temps, cycle scratch, and assignment.
// Ownership/Lifetime:
//   - Borrows the function; all state is per-run.
// Links: src/codegen/aarch64/ra/GlobalAllocator.hpp,
//        src/codegen/aarch64/ra/LiveIntervals.hpp,
//        src/codegen/common/ra/ParallelCopy.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/ra/GlobalAllocator.hpp"

#include "codegen/aarch64/ra/OpcodeClassify.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"
#include "codegen/aarch64/ra/RegClassify.hpp"
#include "codegen/common/ra/ParallelCopy.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

/// @file
/// @brief Implements GlobalAllocator and allocateGlobal().

namespace zanna::codegen::aarch64::ra {

namespace {

/// @brief "No register" marker in the assignment table.
constexpr PhysReg kNone = PhysReg::SP;

/// @brief Ordinal of a physical register in the 64-entry tables.
[[nodiscard]] std::size_t ord(PhysReg reg) noexcept {
    return static_cast<std::size_t>(reg) & 63u;
}

/// @brief Register class of a physical register.
[[nodiscard]] RegClass classOf(PhysReg reg) noexcept {
    return isFPR(reg) ? RegClass::FPR : RegClass::GPR;
}

/// @brief Reserved scratch registers of a class, in preference order.
[[nodiscard]] const std::vector<PhysReg> &reservedScratchFor(RegClass cls) {
    static const std::vector<PhysReg> gpr{kScratchGPR, kScratchGPR2, kScratchGPR3};
    static const std::vector<PhysReg> fpr{kScratchFPR, kScratchFPR2};
    return cls == RegClass::FPR ? fpr : gpr;
}

/// @brief `mov`/`fmov` between two physical registers of one class.
[[nodiscard]] MInstr makeMove(RegClass cls, PhysReg dst, PhysReg src) {
    return MInstr{cls == RegClass::FPR ? MOpcode::FMovRR : MOpcode::MovRR,
                  {MOperand::regOp(dst), MOperand::regOp(src)}};
}

/// @brief Frame-relative load into @p dst.
[[nodiscard]] MInstr makeLoad(RegClass cls, PhysReg dst, int offset) {
    return MInstr{cls == RegClass::FPR ? MOpcode::LdrFprFpImm : MOpcode::LdrRegFpImm,
                  {MOperand::regOp(dst), MOperand::immOp(offset)}};
}

/// @brief Frame-relative store of @p src.
[[nodiscard]] MInstr makeStore(RegClass cls, PhysReg src, int offset) {
    return MInstr{cls == RegClass::FPR ? MOpcode::StrFprFpImm : MOpcode::StrRegFpImm,
                  {MOperand::regOp(src), MOperand::immOp(offset)}};
}

/// @brief Whether @p mi is a same-register `mov`/`fmov`.
[[nodiscard]] bool isIdentityMove(const MInstr &mi) noexcept {
    if ((mi.opc != MOpcode::MovRR && mi.opc != MOpcode::FMovRR) || mi.ops.size() != 2)
        return false;
    const auto &a = mi.ops[0];
    const auto &b = mi.ops[1];
    return a.kind == MOperand::Kind::Reg && b.kind == MOperand::Kind::Reg && a.reg.isPhys &&
           b.reg.isPhys && a.reg.cls == b.reg.cls && a.reg.idOrPhys == b.reg.idOrPhys;
}

} // namespace

// -----------------------------------------------------------------------------
// Construction and driver
// -----------------------------------------------------------------------------

/// @copydoc GlobalAllocator::GlobalAllocator
GlobalAllocator::GlobalAllocator(MFunction &fn, const TargetInfo &ti) : fn_(fn), ti_(ti), fb_(fn) {
    buildPools();
}

/// @copydoc GlobalAllocator::run
GlobalAllocationStats GlobalAllocator::run() {
    intervals_.build(fn_, ti_);
    const std::size_t n = intervals_.vregs().size();
    stats_ = {};
    stats_.vregs = n;
    assigned_.assign(n, kNone);
    slotOffset_.assign(n, 0);
    for (std::size_t o = 0; o < 64; ++o) {
        occupied_[o] = intervals_.fixed(static_cast<PhysReg>(o));
        assignedTo_[o].clear();
    }

    assign();
    assignSlots();
    rewrite();
    finish();
    return stats_;
}

/// @copydoc GlobalAllocator::assignedRegister
PhysReg GlobalAllocator::assignedRegister(uint16_t vreg) const noexcept {
    const std::size_t idx = intervals_.indexOf(vreg);
    return idx == SIZE_MAX ? kNone : assigned_[idx];
}

/// @copydoc GlobalAllocator::spillOffset
int GlobalAllocator::spillOffset(uint16_t vreg) const noexcept {
    const std::size_t idx = intervals_.indexOf(vreg);
    return idx == SIZE_MAX ? 0 : slotOffset_[idx];
}

/// @brief Build the class pool orders (caller-saved first, then callee-saved).
void GlobalAllocator::buildPools() {
    for (PhysReg r : ti_.callerSavedGPR) {
        if (isAllocatableGPR(r)) {
            gprOrder_.push_back(r);
            allocatable_[ord(r)] = true;
        }
    }
    for (PhysReg r : ti_.calleeSavedGPR) {
        if (isAllocatableGPR(r)) {
            gprOrder_.push_back(r);
            allocatable_[ord(r)] = true;
            calleeSaved_[ord(r)] = true;
        }
    }
    for (PhysReg r : ti_.callerSavedFPR) {
        if (r == kScratchFPR || r == kScratchFPR2)
            continue;
        fprOrder_.push_back(r);
        allocatable_[ord(r)] = true;
    }
    for (PhysReg r : ti_.calleeSavedFPR) {
        fprOrder_.push_back(r);
        allocatable_[ord(r)] = true;
        calleeSaved_[ord(r)] = true;
    }
}

/// @copydoc GlobalAllocator::orderFor
const std::vector<PhysReg> &GlobalAllocator::orderFor(RegClass cls) const noexcept {
    return cls == RegClass::FPR ? fprOrder_ : gprOrder_;
}

/// @copydoc GlobalAllocator::noteUse
void GlobalAllocator::noteUse(PhysReg reg) {
    if (calleeSaved_[ord(reg)])
        savedUsed_[ord(reg)] = true;
    else if (isGPR(reg) && std::find(ti_.calleeSavedGPR.begin(), ti_.calleeSavedGPR.end(), reg) !=
                               ti_.calleeSavedGPR.end())
        savedUsed_[ord(reg)] = true;
    else if (isFPR(reg) && std::find(ti_.calleeSavedFPR.begin(), ti_.calleeSavedFPR.end(), reg) !=
                               ti_.calleeSavedFPR.end())
        savedUsed_[ord(reg)] = true;
}

// -----------------------------------------------------------------------------
// Assignment
// -----------------------------------------------------------------------------

/// @brief Whole-interval linear scan in (start, id) order.
void GlobalAllocator::assign() {
    const auto &vregs = intervals_.vregs();
    std::vector<std::size_t> order;
    order.reserve(vregs.size());
    for (std::size_t i = 0; i < vregs.size(); ++i) {
        if (!vregs[i].live.empty())
            order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const Pos sa = vregs[a].live.start();
        const Pos sb = vregs[b].live.start();
        if (sa != sb)
            return sa < sb;
        return vregs[a].id < vregs[b].id;
    });
    for (std::size_t idx : order)
        assignOne(idx);
}

/// @brief Give interval @p idx a register, spilling it or evicting lighter
///        occupants when every candidate is taken.
void GlobalAllocator::assignOne(std::size_t idx) {
    const auto &vregs = intervals_.vregs();
    const VRegInterval &iv = vregs[idx];

    if (iv.crossesEhPush) {
        ++stats_.spilled;
        return; // EH-1: memory-homed across setjmp.
    }

    // Candidate order: hints, then the class pool with callee-saved registers
    // first when the interval is live across a call.
    std::vector<PhysReg> candidates;
    candidates.reserve(orderFor(iv.cls).size() + 4);
    /// Append a candidate once.
    const auto push = [&](PhysReg r) {
        if (!allocatable_[ord(r)] || classOf(r) != iv.cls)
            return;
        if (std::find(candidates.begin(), candidates.end(), r) == candidates.end())
            candidates.push_back(r);
    };
    if (iv.hasPhysHint())
        push(iv.hintPhys);
    for (uint16_t h : iv.hintVRegs) {
        const std::size_t hi = intervals_.indexOf(h);
        if (hi != SIZE_MAX && assigned_[hi] != kNone)
            push(assigned_[hi]);
    }
    const auto &pool = orderFor(iv.cls);
    if (iv.crossesCall) {
        for (PhysReg r : pool)
            if (calleeSaved_[ord(r)])
                push(r);
        for (PhysReg r : pool)
            if (!calleeSaved_[ord(r)])
                push(r);
    } else {
        for (PhysReg r : pool)
            push(r);
    }

    for (PhysReg r : candidates) {
        if (!occupied_[ord(r)].intersects(iv.live)) {
            place(idx, r);
            return;
        }
    }

    // Every candidate conflicts: find the register whose conflicting
    // occupants are lightest; a fixed conflict makes a register unusable.
    PhysReg best = kNone;
    double bestWeight = std::numeric_limits<double>::infinity();
    std::vector<std::size_t> bestConflicts;
    for (PhysReg r : pool) {
        if (intervals_.fixed(r).intersects(iv.live))
            continue;
        double weight = 0.0;
        std::vector<std::size_t> conflicts;
        for (std::size_t j : assignedTo_[ord(r)]) {
            const VRegInterval &other = vregs[j];
            if (other.live.end() < iv.live.start() || iv.live.end() < other.live.start())
                continue;
            if (other.live.intersects(iv.live)) {
                weight += other.weight;
                conflicts.push_back(j);
            }
        }
        if (weight < bestWeight) {
            bestWeight = weight;
            best = r;
            bestConflicts = std::move(conflicts);
        }
    }

    if (best == kNone || bestWeight >= iv.weight) {
        ++stats_.spilled;
        return;
    }
    for (std::size_t j : bestConflicts)
        unassign(j);
    place(idx, best);
}

/// @brief Record the assignment of interval @p idx to @p reg.
void GlobalAllocator::place(std::size_t idx, PhysReg reg) {
    assigned_[idx] = reg;
    occupied_[ord(reg)].addAll(intervals_.vregs()[idx].live);
    assignedTo_[ord(reg)].push_back(idx);
}

/// @brief Evict interval @p idx (it is spilled from now on) and rebuild its
///        register's occupancy from the remaining occupants.
void GlobalAllocator::unassign(std::size_t idx) {
    const PhysReg reg = assigned_[idx];
    assigned_[idx] = kNone;
    ++stats_.spilled;
    auto &list = assignedTo_[ord(reg)];
    list.erase(std::remove(list.begin(), list.end(), idx), list.end());
    RangeList occ = intervals_.fixed(reg);
    for (std::size_t j : list)
        occ.addAll(intervals_.vregs()[j].live);
    occupied_[ord(reg)] = std::move(occ);
}

// -----------------------------------------------------------------------------
// Spill slots
// -----------------------------------------------------------------------------

/// @brief First-fit shared slots for the spilled intervals, hottest first.
void GlobalAllocator::assignSlots() {
    const auto &vregs = intervals_.vregs();
    std::vector<std::size_t> spilled;
    for (std::size_t i = 0; i < vregs.size(); ++i) {
        if (assigned_[i] == kNone && !vregs[i].live.empty())
            spilled.push_back(i);
    }
    std::sort(spilled.begin(), spilled.end(), [&](std::size_t a, std::size_t b) {
        if (vregs[a].weight != vregs[b].weight)
            return vregs[a].weight > vregs[b].weight;
        return vregs[a].id < vregs[b].id;
    });

    struct Slot {
        RangeList occ;
        std::vector<std::size_t> occupants;
    };

    std::vector<Slot> slots;
    for (std::size_t idx : spilled) {
        bool placed = false;
        for (Slot &s : slots) {
            if (!s.occ.intersects(vregs[idx].live)) {
                s.occ.addAll(vregs[idx].live);
                s.occupants.push_back(idx);
                placed = true;
                break;
            }
        }
        if (!placed) {
            Slot s;
            s.occ = vregs[idx].live;
            s.occupants.push_back(idx);
            slots.push_back(std::move(s));
        }
    }

    for (const Slot &s : slots) {
        std::vector<uint32_t> keys;
        keys.reserve(s.occupants.size());
        for (std::size_t idx : s.occupants)
            keys.push_back(vregs[idx].id);
        const int off = fb_.addSharedSpill(keys);
        for (std::size_t idx : s.occupants)
            slotOffset_[idx] = off;
    }
    stats_.spillSlots = slots.size();
}

// -----------------------------------------------------------------------------
// Rewrite
// -----------------------------------------------------------------------------

/// @copydoc GlobalAllocator::freeAt
bool GlobalAllocator::freeAt(PhysReg reg, Pos read, Pos write) const noexcept {
    const RangeList &occ = occupied_[ord(reg)];
    return !occ.contains(read) && !occ.contains(write);
}

/// @copydoc GlobalAllocator::pickTemp
PhysReg GlobalAllocator::pickTemp(RegClass cls,
                                  Pos read,
                                  Pos write,
                                  const std::vector<PhysReg> &blocked,
                                  std::vector<PhysReg> &taken) {
    /// Whether @p r may not serve as a temporary here.
    const auto excluded = [&](PhysReg r) {
        return std::find(blocked.begin(), blocked.end(), r) != blocked.end() ||
               std::find(taken.begin(), taken.end(), r) != taken.end();
    };
    for (PhysReg r : orderFor(cls)) {
        if (excluded(r) || !freeAt(r, read, write))
            continue;
        taken.push_back(r);
        noteUse(r);
        return r;
    }
    for (PhysReg r : reservedScratchFor(cls)) {
        if (excluded(r))
            continue;
        taken.push_back(r);
        return r;
    }
    throw std::runtime_error(
        "AArch64 register allocator: reserved emergency scratch exhausted for a spilled-operand "
        "reload (" +
        std::to_string(reservedScratchFor(cls).size()) +
        " scratch reg(s)); the instruction "
        "presents more simultaneous spilled register operands than the class reserves");
}

/// @brief Rewrite every block.
void GlobalAllocator::rewrite() {
    for (std::size_t bi = 0; bi < fn_.blocks.size(); ++bi)
        rewriteBlock(bi);
}

/// @brief Rewrite block @p bi: operands, reloads, spill stores, parallel copies.
void GlobalAllocator::rewriteBlock(std::size_t bi) {
    MBasicBlock &bb = fn_.blocks[bi];
    const auto &pos = intervals_.positions();
    std::vector<MInstr> out;
    out.reserve(bb.instrs.size() + 8);

    for (std::size_t ii = 0; ii < bb.instrs.size(); ++ii) {
        MInstr mi = bb.instrs[ii];
        if (mi.opc == MOpcode::ParallelCopy) {
            lowerParallelCopy(bi, ii, mi, out);
            continue;
        }
        const Pos rp = pos.readPos(bi, ii);
        const Pos wp = pos.writePos(bi, ii);

        // Explicit physical operands are never temporaries here.
        std::vector<PhysReg> blocked;
        for (const auto &op : mi.ops) {
            if (op.kind == MOperand::Kind::Reg && op.reg.isPhys)
                blocked.push_back(static_cast<PhysReg>(op.reg.idOrPhys));
        }

        // Spilled operands: one temporary per virtual register per instruction.
        // Roles are merged first (a vreg may be read and written by one
        // instruction), then temporaries are chosen: a value that is read
        // needs a register free at the read position (and at the write
        // position when it is also written); a value that is only written
        // needs one free at the write position and may reuse a read-only
        // temporary, because every source is read before the destination is
        // written.
        struct Reload {
            uint16_t vreg;
            RegClass cls;
            PhysReg tmp;
            int offset;
            bool isUse;
            bool isDef;
        };

        std::vector<Reload> reloads;
        for (std::size_t k = 0; k < mi.ops.size(); ++k) {
            const auto &op = mi.ops[k];
            if (op.kind != MOperand::Kind::Reg || op.reg.isPhys)
                continue;
            const std::size_t idx = intervals_.indexOf(op.reg.idOrPhys);
            if (idx == SIZE_MAX)
                throw std::runtime_error("AArch64 global register allocation: unknown vreg");
            if (assigned_[idx] != kNone)
                continue;
            const auto [isUse, isDef] = operandRoles(mi, k);
            auto it = std::find_if(reloads.begin(), reloads.end(), [&](const Reload &r) {
                return r.vreg == op.reg.idOrPhys;
            });
            if (it == reloads.end()) {
                reloads.push_back(
                    Reload{op.reg.idOrPhys, op.reg.cls, kNone, slotOffset_[idx], isUse, isDef});
            } else {
                it->isUse = it->isUse || isUse;
                it->isDef = it->isDef || isDef;
            }
        }
        std::vector<PhysReg> taken;
        std::vector<PhysReg> readOnlyTemps;
        for (Reload &r : reloads) {
            if (!r.isUse)
                continue;
            r.tmp = pickTemp(r.cls, rp, r.isDef ? wp : rp, blocked, taken);
            if (!r.isDef)
                readOnlyTemps.push_back(r.tmp);
        }
        for (Reload &r : reloads) {
            if (r.isUse)
                continue;
            // A read-only temporary can be written here only if nothing
            // assigned occupies it at the write position (an assigned value
            // this instruction defines could).
            PhysReg reuse = kNone;
            for (PhysReg t : readOnlyTemps) {
                if (classOf(t) == r.cls && freeAt(t, wp, wp)) {
                    reuse = t;
                    break;
                }
            }
            if (reuse != kNone) {
                readOnlyTemps.erase(std::find(readOnlyTemps.begin(), readOnlyTemps.end(), reuse));
                r.tmp = reuse;
            } else {
                r.tmp = pickTemp(r.cls, wp, wp, blocked, taken);
            }
        }

        for (std::size_t k = 0; k < mi.ops.size(); ++k) {
            auto &op = mi.ops[k];
            if (op.kind != MOperand::Kind::Reg)
                continue;
            if (op.reg.isPhys) {
                noteUse(static_cast<PhysReg>(op.reg.idOrPhys));
                continue;
            }
            const std::size_t idx = intervals_.indexOf(op.reg.idOrPhys);
            if (assigned_[idx] != kNone) {
                const PhysReg reg = assigned_[idx];
                noteUse(reg);
                op = MOperand::regOp(reg);
                continue;
            }
            auto it = std::find_if(reloads.begin(), reloads.end(), [&](const Reload &r) {
                return r.vreg == op.reg.idOrPhys;
            });
            op = MOperand::regOp(it->tmp);
        }

        for (const Reload &r : reloads) {
            if (r.isUse) {
                out.push_back(makeLoad(r.cls, r.tmp, r.offset));
                ++stats_.reloads;
            }
        }
        if (!isIdentityMove(mi))
            out.push_back(std::move(mi));
        for (const Reload &r : reloads) {
            if (r.isDef) {
                out.push_back(makeStore(r.cls, r.tmp, r.offset));
                ++stats_.spillStores;
            }
        }
    }

    bb.instrs = std::move(out);
}

/// @brief Lower one ParallelCopy at (@p bi, @p ii) into moves, loads, and stores.
void GlobalAllocator::lowerParallelCopy(std::size_t bi,
                                        std::size_t ii,
                                        const MInstr &mi,
                                        std::vector<MInstr> &out) {
    using zanna::codegen::ra::CopyLoc;
    using zanna::codegen::ra::ParallelCopyTask;

    const auto &pos = intervals_.positions();
    const Pos rp = pos.readPos(bi, ii);
    const Pos wp = pos.writePos(bi, ii);

    /// Location of a virtual register operand of the copy.
    const auto locate = [&](const MOperand &op) -> CopyLoc {
        if (op.kind != MOperand::Kind::Reg)
            throw std::runtime_error("AArch64 global register allocation: parallel copy operand "
                                     "is not a register");
        const unsigned cls = static_cast<unsigned>(op.reg.cls);
        if (op.reg.isPhys) {
            noteUse(static_cast<PhysReg>(op.reg.idOrPhys));
            return CopyLoc::regLoc(cls, op.reg.idOrPhys);
        }
        const std::size_t idx = intervals_.indexOf(op.reg.idOrPhys);
        if (idx == SIZE_MAX)
            throw std::runtime_error("AArch64 global register allocation: unknown vreg");
        if (assigned_[idx] != kNone) {
            noteUse(assigned_[idx]);
            return CopyLoc::regLoc(cls, static_cast<unsigned>(assigned_[idx]));
        }
        return CopyLoc::memLoc(cls, slotOffset_[idx]);
    };

    std::vector<ParallelCopyTask> tasks;
    for (std::size_t k = 0; k + 1 < mi.ops.size(); k += 2)
        tasks.push_back(ParallelCopyTask{locate(mi.ops[k]), locate(mi.ops[k + 1])});

    /// Emitter for the shared sequentializer.
    struct Emitter {
        GlobalAllocator &owner;
        std::vector<MInstr> &out;
        Pos rp;
        Pos wp;
        std::vector<PhysReg> handedOut;

        [[nodiscard]] static RegClass cls(const CopyLoc &loc) noexcept {
            return static_cast<RegClass>(loc.cls);
        }

        void move(const CopyLoc &dst, const CopyLoc &src) {
            const RegClass c = cls(dst);
            if (dst.isReg() && src.isReg()) {
                out.push_back(
                    makeMove(c, static_cast<PhysReg>(dst.reg), static_cast<PhysReg>(src.reg)));
            } else if (dst.isReg()) {
                out.push_back(makeLoad(c, static_cast<PhysReg>(dst.reg), src.slot));
            } else if (src.isReg()) {
                out.push_back(makeStore(c, static_cast<PhysReg>(src.reg), dst.slot));
            } else {
                const CopyLoc tmp = memTemp(dst.cls);
                out.push_back(makeLoad(c, static_cast<PhysReg>(tmp.reg), src.slot));
                out.push_back(makeStore(c, static_cast<PhysReg>(tmp.reg), dst.slot));
                ++owner.stats_.edgeMoves;
            }
            ++owner.stats_.edgeMoves;
        }

        CopyLoc cycleScratch(unsigned clsTag) {
            const RegClass c = static_cast<RegClass>(clsTag);
            for (PhysReg r : owner.orderFor(c)) {
                if (std::find(handedOut.begin(), handedOut.end(), r) != handedOut.end())
                    continue;
                if (!owner.freeAt(r, rp, wp))
                    continue;
                handedOut.push_back(r);
                owner.noteUse(r);
                return CopyLoc::regLoc(clsTag, static_cast<unsigned>(r));
            }
            // No free register at this edge: break the cycle through memory.
            const int off = owner.fb_.ensureSpill(owner.nextTempSlotKey_++);
            ++owner.stats_.spillSlots;
            return CopyLoc::memLoc(clsTag, off);
        }

        CopyLoc memTemp(unsigned clsTag) {
            const PhysReg r =
                static_cast<RegClass>(clsTag) == RegClass::FPR ? kScratchFPR2 : kScratchGPR3;
            return CopyLoc::regLoc(clsTag, static_cast<unsigned>(r));
        }

        void releaseScratch(const CopyLoc &loc) {
            if (!loc.isReg())
                return;
            const PhysReg r = static_cast<PhysReg>(loc.reg);
            handedOut.erase(std::remove(handedOut.begin(), handedOut.end(), r), handedOut.end());
        }
    };

    Emitter emitter{*this, out, rp, wp, {}};
    (void)zanna::codegen::ra::sequentializeParallelCopy(std::move(tasks), emitter);
}

// -----------------------------------------------------------------------------
// Finish
// -----------------------------------------------------------------------------

/// @brief Finalize the frame and publish the callee-saved registers in target order.
void GlobalAllocator::finish() {
    fb_.finalize();
    fn_.savedGPRs.clear();
    fn_.savedFPRs.clear();
    for (PhysReg r : ti_.calleeSavedGPR) {
        if (savedUsed_[ord(r)])
            fn_.savedGPRs.push_back(r);
    }
    for (PhysReg r : ti_.calleeSavedFPR) {
        if (savedUsed_[ord(r)])
            fn_.savedFPRs.push_back(r);
    }
}

/// @copydoc allocateGlobal
AllocationResult allocateGlobal(MFunction &fn, const TargetInfo &ti) {
    GlobalAllocator allocator(fn, ti);
    const GlobalAllocationStats stats = allocator.run();
    AllocationResult result;
    result.gprSpillSlots = static_cast<int>(stats.spillSlots);
    return result;
}

} // namespace zanna::codegen::aarch64::ra
