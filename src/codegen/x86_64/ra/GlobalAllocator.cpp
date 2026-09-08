//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/GlobalAllocator.cpp
// Purpose: Implements the x86-64 function-wide allocator: pools, the shared
//          assignment and slot layout, the operand rewrite with reloads and
//          spill stores (register operands and memory address registers),
//          temporaries with a save/restore fallback, and PX_COPY lowering
//          through the shared sequentializer.
// Key invariants:
//   - Every temporary is chosen against the occupancy of the final
//     assignment at the instruction's read and write positions, so it never
//     clobbers a value that is live there; when nothing is free the
//     occupant of a pool register is saved to a fresh slot and restored.
//   - Reserved R10/R11 are used only within one instruction and only when
//     the instruction does not touch them (jump tables, division sequences).
//   - Identity moves produced by the rewrite are dropped.
// Ownership/Lifetime:
//   - Mutates the function in place; the result is returned by value.
// Links: src/codegen/x86_64/ra/GlobalAllocator.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/ra/GlobalAllocator.hpp"

#include "codegen/common/ra/ParallelCopy.hpp"
#include "codegen/x86_64/OperandRoles.hpp"
#include "codegen/x86_64/ra/SpillSlots.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

/// @file
/// @brief Implements the x86-64 function-wide register allocator.

namespace zanna::codegen::x64::ra {

namespace {

constexpr PhysReg kNone = PhysReg::RSP;

[[nodiscard]] std::size_t ord(PhysReg reg) noexcept {
    return static_cast<std::size_t>(reg) % kPhysRegCount;
}

[[nodiscard]] RegClass classOf(PhysReg reg) noexcept {
    return isXMM(reg) ? RegClass::XMM : RegClass::GPR;
}

[[nodiscard]] unsigned clsTag(RegClass cls) noexcept {
    return static_cast<unsigned>(cls);
}

/// @brief Registers the general-purpose pool never contains: the frame and
///        stack pointers, and the backend scratch pair every multi-instruction
///        lowering sequence (division, jump tables, call targets) may use.
[[nodiscard]] bool isReservedGPR(PhysReg reg) noexcept {
    return reg == PhysReg::RSP || reg == PhysReg::RBP || reg == PhysReg::R10 || reg == PhysReg::R11;
}

/// @brief Emergency temporaries of a class, tried after the pool.
[[nodiscard]] const std::vector<PhysReg> &reservedScratchFor(RegClass cls) {
    static const std::vector<PhysReg> gpr{PhysReg::R10, PhysReg::R11};
    static const std::vector<PhysReg> xmm{};
    return cls == RegClass::XMM ? xmm : gpr;
}

/// @brief `mov`/`movsd` between two physical registers of one class.
[[nodiscard]] MInstr makeMove(RegClass cls, PhysReg dst, PhysReg src) {
    return MInstr::make(cls == RegClass::GPR ? MOpcode::MOVrr : MOpcode::MOVSDrr,
                        {makePhysRegOperand(cls, static_cast<uint16_t>(dst)),
                         makePhysRegOperand(cls, static_cast<uint16_t>(src))});
}

/// @brief Whether @p mi is a same-register `mov`/`movsd`.
[[nodiscard]] bool isIdentityMove(const MInstr &mi) noexcept {
    if ((mi.opcode != MOpcode::MOVrr && mi.opcode != MOpcode::MOVSDrr) || mi.operands.size() != 2)
        return false;
    const auto *a = std::get_if<OpReg>(&mi.operands[0]);
    const auto *b = std::get_if<OpReg>(&mi.operands[1]);
    return a && b && a->isPhys && b->isPhys && a->cls == b->cls && a->idOrPhys == b->idOrPhys;
}

/// @brief Every physical register named by @p mi's operands (register
///        operands and memory address registers).
[[nodiscard]] PhysRegMask operandPhysRegs(const MInstr &mi) noexcept {
    PhysRegMask mask = 0;
    for (const Operand &op : mi.operands) {
        if (const auto *reg = std::get_if<OpReg>(&op)) {
            if (reg->isPhys)
                mask |= physRegBit(static_cast<PhysReg>(reg->idOrPhys));
        } else if (const auto *mem = std::get_if<OpMem>(&op)) {
            if (mem->base.isPhys)
                mask |= physRegBit(static_cast<PhysReg>(mem->base.idOrPhys));
            if (mem->hasIndex && mem->index.isPhys)
                mask |= physRegBit(static_cast<PhysReg>(mem->index.idOrPhys));
        }
    }
    return mask;
}

} // namespace

// -----------------------------------------------------------------------------
// Construction and driver
// -----------------------------------------------------------------------------

/// @copydoc GlobalAllocator::GlobalAllocator
GlobalAllocator::GlobalAllocator(MFunction &fn, const TargetInfo &ti) : fn_(fn), ti_(ti) {
    buildPools();
}

/// @copydoc GlobalAllocator::run
GlobalAllocationStats GlobalAllocator::run() {
    intervals_.build(fn_, ti_);
    const std::size_t n = intervals_.vregs().size();
    stats_ = {};
    result_ = {};
    stats_.vregs = n;
    assigned_.assign(n, kNone);
    slotIndex_.assign(n, -1);
    nextSlot_ = {};

    assign();
    rewrite();

    stats_.spillSlotsGPR = nextSlot_[clsTag(RegClass::GPR)];
    stats_.spillSlotsXMM = nextSlot_[clsTag(RegClass::XMM)];
    return stats_;
}

/// @copydoc GlobalAllocator::assignedRegister
PhysReg GlobalAllocator::assignedRegister(uint16_t vreg) const noexcept {
    const std::size_t idx = intervals_.indexOf(vreg);
    return idx == SIZE_MAX ? kNone : assigned_[idx];
}

/// @copydoc GlobalAllocator::spillSlot
int GlobalAllocator::spillSlot(uint16_t vreg) const noexcept {
    const std::size_t idx = intervals_.indexOf(vreg);
    return idx == SIZE_MAX ? -1 : slotIndex_[idx];
}

/// @brief Build the class pool orders (caller-saved first, then callee-saved).
void GlobalAllocator::buildPools() {
    for (PhysReg r : ti_.callerSavedGPR) {
        if (isReservedGPR(r))
            continue;
        gprOrder_.push_back(r);
        allocatable_[ord(r)] = true;
    }
    for (PhysReg r : ti_.calleeSavedGPR) {
        if (isReservedGPR(r))
            continue;
        gprOrder_.push_back(r);
        allocatable_[ord(r)] = true;
        calleeSaved_[ord(r)] = true;
    }
    for (PhysReg r : ti_.callerSavedFPR) {
        xmmOrder_.push_back(r);
        allocatable_[ord(r)] = true;
    }
    for (PhysReg r : ti_.calleeSavedFPR) {
        xmmOrder_.push_back(r);
        allocatable_[ord(r)] = true;
        calleeSaved_[ord(r)] = true;
    }
}

/// @copydoc GlobalAllocator::orderFor
const std::vector<PhysReg> &GlobalAllocator::orderFor(RegClass cls) const noexcept {
    return cls == RegClass::XMM ? xmmOrder_ : gprOrder_;
}

// -----------------------------------------------------------------------------
// Assignment and spill slots (shared core)
// -----------------------------------------------------------------------------

/// @brief Run the shared whole-interval linear scan and lay out the spill
///        slots per class: hottest spills first, first-fit sharing among
///        non-intersecting intervals.
void GlobalAllocator::assign() {
    using zanna::codegen::ra::IntervalAssigner;
    using zanna::codegen::ra::IntervalInfo;
    using zanna::codegen::ra::kNoReg;
    using zanna::codegen::ra::RegisterFile;

    const auto &vregs = intervals_.vregs();
    std::vector<IntervalInfo> infos;
    infos.reserve(vregs.size());
    for (const VRegInterval &iv : vregs) {
        IntervalInfo info;
        info.id = iv.id;
        info.cls = clsTag(iv.cls);
        info.live = iv.live;
        info.weight = iv.weight;
        info.crossesCall = iv.crossesCall;
        info.crossesEhPush = iv.crossesEhPush;
        info.hintPhys = iv.hasPhysHint ? static_cast<unsigned>(ord(iv.hintPhys)) : kNoReg;
        info.hintIds.assign(iv.hintVRegs.begin(), iv.hintVRegs.end());
        infos.push_back(std::move(info));
    }

    RegisterFile regs;
    regs.orderByClass.resize(2);
    for (PhysReg r : gprOrder_)
        regs.orderByClass[clsTag(RegClass::GPR)].push_back(static_cast<unsigned>(ord(r)));
    for (PhysReg r : xmmOrder_)
        regs.orderByClass[clsTag(RegClass::XMM)].push_back(static_cast<unsigned>(ord(r)));
    regs.allocatable.assign(kPhysRegCount, 0);
    regs.calleeSaved.assign(kPhysRegCount, 0);
    regs.classOf.assign(kPhysRegCount, clsTag(RegClass::GPR));
    for (std::size_t o = 0; o < kPhysRegCount; ++o) {
        regs.allocatable[o] = allocatable_[o] ? 1 : 0;
        regs.calleeSaved[o] = calleeSaved_[o] ? 1 : 0;
        regs.classOf[o] = clsTag(classOf(static_cast<PhysReg>(o)));
    }
    std::vector<RangeList> fixed(kPhysRegCount);
    for (std::size_t o = 0; o < kPhysRegCount; ++o)
        fixed[o] = intervals_.fixed(static_cast<PhysReg>(o));

    IntervalAssigner assigner(infos, std::move(regs), std::move(fixed));
    assigner.run();

    for (std::size_t i = 0; i < infos.size(); ++i) {
        const unsigned r = assigner.assigned(i);
        assigned_[i] = r == kNoReg ? kNone : static_cast<PhysReg>(r);
        if (r != kNoReg)
            result_.vregToPhys[vregs[i].id] = assigned_[i];
    }
    stats_.spilled = assigner.spilledCount();
    for (std::size_t o = 0; o < kPhysRegCount; ++o)
        occupied_[o] = assigner.occupied(static_cast<unsigned>(o));

    // Slots: one placeholder namespace per class; groups come back hottest
    // first, so the hottest spills get the lowest indices.
    for (RegClass cls : {RegClass::GPR, RegClass::XMM}) {
        const auto groups = assigner.shareSlots(clsTag(cls));
        for (const auto &group : groups) {
            const int slot = freshSlot(cls);
            for (std::size_t idx : group)
                slotIndex_[idx] = slot;
        }
    }
}

/// @copydoc GlobalAllocator::freshSlot
int GlobalAllocator::freshSlot(RegClass cls) {
    return static_cast<int>(nextSlot_[clsTag(cls)]++);
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
                                  PhysRegMask blocked,
                                  std::vector<PhysReg> &taken,
                                  Wrap *wrap) {
    /// Whether @p r may not serve as a temporary here.
    const auto excluded = [&](PhysReg r) {
        return (blocked & physRegBit(r)) != 0 ||
               std::find(taken.begin(), taken.end(), r) != taken.end();
    };
    for (PhysReg r : orderFor(cls)) {
        if (excluded(r) || !freeAt(r, read, write))
            continue;
        taken.push_back(r);
        return r;
    }
    for (PhysReg r : reservedScratchFor(cls)) {
        if (excluded(r) || !freeAt(r, read, write))
            continue;
        taken.push_back(r);
        return r;
    }
    if (wrap != nullptr) {
        // Every register of the class is occupied here: save one that the
        // instruction does not touch, use it, and restore it afterwards.
        for (PhysReg r : orderFor(cls)) {
            if (excluded(r))
                continue;
            const int slot = freshSlot(cls);
            wrap->before.push_back(makeSpillStore(cls, slot, r));
            wrap->after.push_back(makeSpillLoad(cls, r, slot));
            taken.push_back(r);
            ++stats_.spillArounds;
            return r;
        }
    }
    throw std::runtime_error(
        "x86 register allocator: no temporary register for a spilled operand in function '" +
        fn_.name + "' (every " + std::string(cls == RegClass::XMM ? "XMM" : "GPR") +
        " register is touched by the instruction or occupied across it)");
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
    out.reserve(bb.instructions.size() + 8);

    /// Index of a virtual register's interval, or a lowering-bug diagnostic.
    const auto indexOf = [&](uint16_t id) {
        const std::size_t idx = intervals_.indexOf(id);
        if (idx == SIZE_MAX)
            throw std::runtime_error("x86 register allocator: unknown virtual register v" +
                                     std::to_string(id) + " in function '" + fn_.name + "'");
        return idx;
    };

    for (std::size_t ii = 0; ii < bb.instructions.size(); ++ii) {
        MInstr mi = std::move(bb.instructions[ii]);
        if (mi.opcode == MOpcode::PX_COPY) {
            lowerParallelCopy(bi, ii, mi, out);
            continue;
        }
        const Pos rp = pos.readPos(bi, ii);
        const Pos wp = pos.writePos(bi, ii);

        // Substitute assigned virtual registers first and collect the
        // spilled ones with their merged roles (a value may be read and
        // written by one instruction; a memory address register is a read).
        std::vector<Reload> reloads;
        const auto noteSpilled = [&](uint16_t id, RegClass cls, bool isUse, bool isDef) {
            const std::size_t idx = indexOf(id);
            auto it = std::find_if(
                reloads.begin(), reloads.end(), [&](const Reload &r) { return r.vreg == id; });
            if (it == reloads.end())
                reloads.push_back(Reload{id, cls, kNone, slotIndex_[idx], isUse, isDef});
            else {
                it->isUse = it->isUse || isUse;
                it->isDef = it->isDef || isDef;
            }
        };
        for (std::size_t k = 0; k < mi.operands.size(); ++k) {
            Operand &op = mi.operands[k];
            if (auto *reg = std::get_if<OpReg>(&op)) {
                if (reg->isPhys)
                    continue;
                const std::size_t idx = indexOf(reg->idOrPhys);
                if (assigned_[idx] != kNone) {
                    *reg = makePhysReg(reg->cls, static_cast<uint16_t>(assigned_[idx]));
                    continue;
                }
                const auto [isUse, isDef] = operandRoles(mi, k);
                noteSpilled(reg->idOrPhys, reg->cls, isUse, isDef);
            } else if (auto *mem = std::get_if<OpMem>(&op)) {
                if (!mem->base.isPhys) {
                    const std::size_t idx = indexOf(mem->base.idOrPhys);
                    if (assigned_[idx] != kNone)
                        mem->base =
                            makePhysReg(RegClass::GPR, static_cast<uint16_t>(assigned_[idx]));
                    else
                        noteSpilled(mem->base.idOrPhys, RegClass::GPR, true, false);
                }
                if (mem->hasIndex && !mem->index.isPhys) {
                    const std::size_t idx = indexOf(mem->index.idOrPhys);
                    if (assigned_[idx] != kNone)
                        mem->index =
                            makePhysReg(RegClass::GPR, static_cast<uint16_t>(assigned_[idx]));
                    else
                        noteSpilled(mem->index.idOrPhys, RegClass::GPR, true, false);
                }
            }
        }

        if (reloads.empty()) {
            if (!isIdentityMove(mi))
                out.push_back(std::move(mi));
            continue;
        }

        // Registers the instruction touches: its (now physical) operands,
        // everything it reads (argument registers of a call included), and
        // its implicit definitions. A call's caller-saved clobbers are not
        // blocked: they happen after the reads a temporary serves.
        const InstrEffects fx = effectsOf(mi, ti_);
        const PhysRegMask blocked = operandPhysRegs(mi) | fx.uses | implicitDefMask(mi.opcode) |
                                    physRegBit(PhysReg::RSP) | physRegBit(PhysReg::RBP);
        Wrap wrap;
        Wrap *wrapPtr = fx.isTerminator ? nullptr : &wrap;

        std::vector<PhysReg> taken;
        std::vector<PhysReg> readOnlyTemps;
        for (Reload &r : reloads) {
            if (!r.isUse)
                continue;
            r.tmp = pickTemp(r.cls, rp, r.isDef ? wp : rp, blocked, taken, wrapPtr);
            if (!r.isDef)
                readOnlyTemps.push_back(r.tmp);
        }
        for (Reload &r : reloads) {
            if (r.isUse)
                continue;
            // A read-only temporary can be written here only if nothing
            // assigned occupies it at the write position.
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
                r.tmp = pickTemp(r.cls, wp, wp, blocked, taken, wrapPtr);
            }
        }

        /// Temporary chosen for spilled virtual register @p id.
        const auto tempFor = [&](uint16_t id) {
            return std::find_if(reloads.begin(),
                                reloads.end(),
                                [&](const Reload &r) { return r.vreg == id; })
                ->tmp;
        };
        for (Operand &op : mi.operands) {
            if (auto *reg = std::get_if<OpReg>(&op)) {
                if (!reg->isPhys)
                    *reg = makePhysReg(reg->cls, static_cast<uint16_t>(tempFor(reg->idOrPhys)));
            } else if (auto *mem = std::get_if<OpMem>(&op)) {
                if (!mem->base.isPhys)
                    mem->base = makePhysReg(RegClass::GPR,
                                            static_cast<uint16_t>(tempFor(mem->base.idOrPhys)));
                if (mem->hasIndex && !mem->index.isPhys)
                    mem->index = makePhysReg(RegClass::GPR,
                                             static_cast<uint16_t>(tempFor(mem->index.idOrPhys)));
            }
        }

        for (MInstr &save : wrap.before)
            out.push_back(std::move(save));
        for (const Reload &r : reloads) {
            if (r.isUse) {
                out.push_back(makeSpillLoad(r.cls, r.tmp, r.slot));
                out.back().loc = mi.loc;
                ++stats_.reloads;
            }
        }
        if (!isIdentityMove(mi))
            out.push_back(std::move(mi));
        for (const Reload &r : reloads) {
            if (r.isDef) {
                out.push_back(makeSpillStore(r.cls, r.slot, r.tmp));
                ++stats_.spillStores;
            }
        }
        for (auto it = wrap.after.rbegin(); it != wrap.after.rend(); ++it)
            out.push_back(std::move(*it));
    }

    bb.instructions = std::move(out);
}

/// @brief Lower one PX_COPY at (@p bi, @p ii) into moves, loads, and stores.
void GlobalAllocator::lowerParallelCopy(std::size_t bi,
                                        std::size_t ii,
                                        const MInstr &mi,
                                        std::vector<MInstr> &out) {
    using zanna::codegen::ra::CopyLoc;
    using zanna::codegen::ra::ParallelCopyTask;

    const auto &pos = intervals_.positions();
    const Pos rp = pos.readPos(bi, ii);
    const Pos wp = pos.writePos(bi, ii);

    if ((mi.operands.size() % 2U) != 0U)
        throw std::runtime_error("x86 PX_COPY lowering: operand count must be even");

    /// Location of a register operand of the copy.
    const auto locate = [&](const Operand &op) -> CopyLoc {
        const auto *reg = std::get_if<OpReg>(&op);
        if (reg == nullptr)
            throw std::runtime_error("x86 PX_COPY lowering: expected register operand pairs");
        const unsigned cls = clsTag(reg->cls);
        if (reg->isPhys)
            return CopyLoc::regLoc(cls, reg->idOrPhys);
        const std::size_t idx = intervals_.indexOf(reg->idOrPhys);
        if (idx == SIZE_MAX)
            throw std::runtime_error("x86 PX_COPY lowering: unknown virtual register v" +
                                     std::to_string(reg->idOrPhys));
        if (assigned_[idx] != kNone)
            return CopyLoc::regLoc(cls, static_cast<unsigned>(assigned_[idx]));
        return CopyLoc::memLoc(cls, slotIndex_[idx]);
    };

    std::vector<ParallelCopyTask> tasks;
    PhysRegMask blocked = physRegBit(PhysReg::RSP) | physRegBit(PhysReg::RBP);
    for (std::size_t k = 0; k + 1 < mi.operands.size(); k += 2) {
        const CopyLoc dst = locate(mi.operands[k]);
        const CopyLoc src = locate(mi.operands[k + 1]);
        if (dst.isReg())
            blocked |= PhysRegMask{1} << dst.reg;
        if (src.isReg())
            blocked |= PhysRegMask{1} << src.reg;
        tasks.push_back(ParallelCopyTask{dst, src});
    }

    /// Emitter for the shared sequentializer.
    struct Emitter {
        GlobalAllocator &owner;
        std::vector<MInstr> &body;
        Wrap &wrap;
        Pos rp;
        Pos wp;
        PhysRegMask blocked;
        std::vector<PhysReg> handedOut;

        [[nodiscard]] static RegClass cls(const CopyLoc &loc) noexcept {
            return static_cast<RegClass>(loc.cls);
        }

        void move(const CopyLoc &dst, const CopyLoc &src) {
            const RegClass c = cls(dst);
            if (dst.isReg() && src.isReg()) {
                body.push_back(
                    makeMove(c, static_cast<PhysReg>(dst.reg), static_cast<PhysReg>(src.reg)));
            } else if (dst.isReg()) {
                body.push_back(makeSpillLoad(c, static_cast<PhysReg>(dst.reg), src.slot));
            } else if (src.isReg()) {
                body.push_back(makeSpillStore(c, dst.slot, static_cast<PhysReg>(src.reg)));
            } else {
                const CopyLoc tmp = memTemp(dst.cls);
                body.push_back(makeSpillLoad(c, static_cast<PhysReg>(tmp.reg), src.slot));
                body.push_back(makeSpillStore(c, dst.slot, static_cast<PhysReg>(tmp.reg)));
                releaseScratch(tmp);
                ++owner.stats_.edgeMoves;
            }
            ++owner.stats_.edgeMoves;
        }

        /// A register free across the copy, or a fresh slot when none is.
        CopyLoc cycleScratch(unsigned clsTagValue) {
            const RegClass c = static_cast<RegClass>(clsTagValue);
            if (const PhysReg r = freeRegister(c); r != kNone)
                return CopyLoc::regLoc(clsTagValue, static_cast<unsigned>(r));
            return CopyLoc::memLoc(clsTagValue, owner.freshSlot(c));
        }

        /// A register for a memory-to-memory copy: free across the copy, the
        /// reserved scratch, or a pool register saved around the sequence.
        CopyLoc memTemp(unsigned clsTagValue) {
            const RegClass c = static_cast<RegClass>(clsTagValue);
            if (const PhysReg r = freeRegister(c); r != kNone)
                return CopyLoc::regLoc(clsTagValue, static_cast<unsigned>(r));
            std::vector<PhysReg> taken = handedOut;
            const PhysReg r = owner.pickTemp(c, rp, wp, blocked, taken, &wrap);
            handedOut.push_back(r);
            return CopyLoc::regLoc(clsTagValue, static_cast<unsigned>(r));
        }

        void releaseScratch(const CopyLoc &loc) {
            if (!loc.isReg())
                return;
            const PhysReg r = static_cast<PhysReg>(loc.reg);
            handedOut.erase(std::remove(handedOut.begin(), handedOut.end(), r), handedOut.end());
        }

        /// First pool or reserved register of @p c free at the copy and not
        /// touched by it, or kNone.
        [[nodiscard]] PhysReg freeRegister(RegClass c) {
            const auto candidate = [&](PhysReg r) {
                if ((blocked & physRegBit(r)) != 0)
                    return false;
                if (std::find(handedOut.begin(), handedOut.end(), r) != handedOut.end())
                    return false;
                return owner.freeAt(r, rp, wp);
            };
            for (PhysReg r : owner.orderFor(c)) {
                if (candidate(r)) {
                    handedOut.push_back(r);
                    return r;
                }
            }
            for (PhysReg r : reservedScratchFor(c)) {
                if (candidate(r)) {
                    handedOut.push_back(r);
                    return r;
                }
            }
            return kNone;
        }
    };

    std::vector<MInstr> body;
    Wrap wrap;
    Emitter emitter{*this, body, wrap, rp, wp, blocked, {}};
    (void)zanna::codegen::ra::sequentializeParallelCopy(std::move(tasks), emitter);

    for (MInstr &save : wrap.before)
        out.push_back(std::move(save));
    for (MInstr &instr : body) {
        instr.loc = mi.loc;
        out.push_back(std::move(instr));
    }
    for (auto it = wrap.after.rbegin(); it != wrap.after.rend(); ++it)
        out.push_back(std::move(*it));
}

} // namespace zanna::codegen::x64::ra

namespace zanna::codegen::x64 {

/// @copydoc allocate
AllocationResult allocate(MFunction &func, const TargetInfo &target) {
    ra::GlobalAllocator allocator(func, target);
    const ra::GlobalAllocationStats stats = allocator.run();
    AllocationResult result;
    result.spillSlotsGPR = static_cast<int>(stats.spillSlotsGPR);
    result.spillSlotsXMM = static_cast<int>(stats.spillSlotsXMM);
    for (const auto &iv : allocator.intervals().vregs()) {
        const PhysReg reg = allocator.assignedRegister(iv.id);
        if (reg != PhysReg::RSP)
            result.vregToPhys[iv.id] = reg;
    }
    return result;
}

} // namespace zanna::codegen::x64
