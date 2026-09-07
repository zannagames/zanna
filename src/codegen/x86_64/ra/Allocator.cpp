//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/Allocator.cpp
// Purpose: Implement the linear-scan allocation phase which assigns physical
//          registers, inserts spill code, and lowers PX_COPY bundles for the
//          x86-64 backend.
// Key invariants:
//   - Register pools are deterministically populated from the target ABI.
//   - Allocation proceeds in block order and carries values only across safe edges.
// Ownership/Lifetime:
//   - Mutates MIR blocks in place; returns AllocationResult to the caller.
// Links: src/codegen/x86_64/ra/Allocator.hpp,
//        src/codegen/x86_64/ra/Coalescer.hpp,
//        src/codegen/x86_64/OperandRoles.hpp
//
//===----------------------------------------------------------------------===//

#include "Allocator.hpp"

#include "Coalescer.hpp"
#include "codegen/common/ra/GlobalPinning.hpp"
#include "codegen/x86_64/OperandRoles.hpp"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

/// @file
/// @brief Implements the x86-64 linear-scan register allocator.
/// @details The allocator walks Machine IR blocks in order, leasing physical
///          registers from ABI-configured pools, spilling values when pressure
///          grows, and invoking the coalescer to expand PX_COPY pseudos.  The
///          implementation maintains per-class pools and active lists so live
///          ranges can be reconstituted on demand.

namespace zanna::codegen::x64::ra {

namespace {

/// @brief Deterministically ordered free-register container.
using RegPool = std::deque<PhysReg>;

/// @brief Visitor-composition helper for @c std::visit.
/// @tparam Ts Callable types combined into one overload set.
template <typename... Ts> struct Overload : Ts... {
    using Ts::operator()...;
};

template <typename... Ts> Overload(Ts...) -> Overload<Ts...>;

/// @brief Identify general-purpose registers that must never be allocated.
/// @details The stack/frame pointers are reserved by the calling convention.
///          R10/R11 are backend scratch registers used by multi-instruction
///          lowering sequences; keeping them out of the general pool prevents a
///          later vreg reload from overwriting scratch state before it is read.
/// @param reg Candidate register.
/// @return @c true when @p reg is reserved.
[[nodiscard]] bool isReservedGPR(PhysReg reg) noexcept {
    return reg == PhysReg::RSP || reg == PhysReg::RBP || reg == PhysReg::R10 || reg == PhysReg::R11;
}

/// @brief Wrap a physical register into a Machine IR operand.
/// @details Converts the strongly typed @ref PhysReg enumeration into the raw
///          identifier used by Machine IR instructions so helper routines can
///          build @c MOV-like instructions without repeating casts.
/// @param cls Register class the operand belongs to.
/// @param reg Physical register identifier.
/// @return Machine operand referencing @p reg.
[[nodiscard]] Operand makePhysOperand(RegClass cls, PhysReg reg) {
    return makePhysRegOperand(cls, static_cast<uint16_t>(reg));
}

/// @brief Description of a physical register clobbered by an instruction.
struct PhysClobber {
    PhysReg reg{PhysReg::RAX};   ///< Clobbered physical register.
    RegClass cls{RegClass::GPR}; ///< Register class (GPR or XMM).
};

/// @brief Append @p reg to @p clobbers, computing its class and de-duplicating.
/// @details Allocator code needs a set-like view of every physical clobber
///          implied by an instruction so it can spill conflicting live vregs.
///          We use a vector (compact, cache-friendly) and dedupe linearly —
///          the typical clobber count per instruction is < 4.
/// @param clobbers Compact set-like vector extended in place.
/// @param reg Physical register to add when not already represented.
void addPhysClobber(std::vector<PhysClobber> &clobbers, PhysReg reg) {
    const RegClass cls = isXMM(reg) ? RegClass::XMM : RegClass::GPR;
    /// @brief Tests whether a physical clobber is already recorded.
    /// @param item Candidate clobber entry.
    /// @return `true` when the register and class match `reg`.
    const auto duplicate = std::any_of(clobbers.begin(), clobbers.end(), [&](const auto &item) {
        return item.reg == reg && item.cls == cls;
    });
    if (!duplicate) {
        clobbers.push_back(PhysClobber{reg, cls});
    }
}

/// @brief Return a compact register-class name for allocator diagnostics.
/// @param cls Register class to render.
/// @return Process-lifetime literal @c "XMM" or @c "GPR".
[[nodiscard]] const char *regClassName(RegClass cls) noexcept {
    return cls == RegClass::XMM ? "XMM" : "GPR";
}

/// @brief Compute the full set of physical registers an instruction overwrites.
/// @details Combines two sources: explicit def-position physical operands, and
///          implicit clobbers for opcodes that touch fixed registers (CQO,
///          IDIVrm, DIVrm, MULr, and IMULr). The result drives the allocator's
///          "spill any live vreg that lands here" logic.
/// @param instr Instruction whose explicit and implicit definitions are inspected.
/// @return De-duplicated physical-register/class clobber vector.
std::vector<PhysClobber> collectPhysicalClobbers(const MInstr &instr) {
    std::vector<PhysClobber> clobbers;
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (!isDef)
            continue;
        const auto *reg = std::get_if<OpReg>(&instr.operands[idx]);
        if (reg && reg->isPhys) {
            addPhysClobber(clobbers, static_cast<PhysReg>(reg->idOrPhys));
        }
    }

    // Implicit fixed-register writes (CQO → RDX; IDIV/DIV/MUL/IMUL → RAX:RDX)
    // come from the shared role model so the allocator, scheduler, ISel, and
    // every peephole agree on them.
    const PhysRegMask implicit = implicitDefMask(instr.opcode);
    for (unsigned bit = 0; bit < 64 && (implicit >> bit) != 0; ++bit) {
        if ((implicit & (PhysRegMask{1} << bit)) != 0)
            addPhysClobber(clobbers, static_cast<PhysReg>(bit));
    }
    return clobbers;
}

/// @brief Return the source vreg of a copy instruction (or @c UINT16_MAX).
/// @details Used when scanning for a MOVrr/MOVSDrr whose destination is being
///          allocated — if the source is a virtual register we can sometimes
///          coalesce by giving the destination the same physical register.
/// @param instr Candidate integer or scalar-double move.
/// @return Source vreg id, or @c UINT16_MAX when the instruction is not a
///         vreg-to-anything copy.
uint16_t passthroughSourceVReg(const MInstr &instr) {
    if (instr.opcode != MOpcode::MOVrr && instr.opcode != MOpcode::MOVSDrr)
        return std::numeric_limits<uint16_t>::max();
    if (instr.operands.size() < 2)
        return std::numeric_limits<uint16_t>::max();
    const auto *srcReg = std::get_if<OpReg>(&instr.operands[1]);
    if (!srcReg || srcReg->isPhys)
        return std::numeric_limits<uint16_t>::max();
    return srcReg->idOrPhys;
}

/// @brief Predicate: is @p instr a no-op physical move into @p physDest?
/// @details After allocation, a MOVrr whose source and destination
///          resolve to the same physical register is a no-op and can be
///          discarded. The check is conservative — both operands must be
///          the same physical register of the same class.
/// @param instr Candidate integer or scalar-double move.
/// @param physDest Expected physical destination clobber.
/// @return @c true when both physical operands are identical to @p physDest.
bool isIdentityPhysicalMove(const MInstr &instr, PhysReg physDest) {
    if (instr.opcode != MOpcode::MOVrr && instr.opcode != MOpcode::MOVSDrr)
        return false;
    if (instr.operands.size() < 2)
        return false;
    const auto *dst = std::get_if<OpReg>(&instr.operands[0]);
    const auto *src = std::get_if<OpReg>(&instr.operands[1]);
    return dst && src && dst->isPhys && src->isPhys &&
           static_cast<PhysReg>(dst->idOrPhys) == physDest && dst->cls == src->cls &&
           dst->idOrPhys == src->idOrPhys;
}

} // namespace

/// @brief Create an allocator for a machine function.
/// @details The constructor caches references to the function being rewritten,
///          target ABI metadata, and live intervals. It also precomputes
///          the register pools so @ref run can draw from ready-to-use vectors.
/// @param func Machine function to allocate.
/// @param target Target-specific register and ABI description.
/// @param intervals Live interval analysis results for @p func.
LinearScanAllocator::LinearScanAllocator(MFunction &func,
                                         const TargetInfo &target,
                                         const LiveIntervals &intervals)
    : func_(func), target_(target), intervals_(intervals) {
    buildPools();

    // Precompute caller-saved register bitsets for O(1) lookup during CALL handling.
    // This avoids O(n) linear search through vectors on every call instruction.
    for (PhysReg reg : target_.callerSavedGPR)
        callerSavedGPRBits_.set(static_cast<std::size_t>(reg));
    for (PhysReg reg : target_.callerSavedFPR)
        callerSavedFPRBits_.set(static_cast<std::size_t>(reg));
}

/// @brief Execute the allocation pipeline over the entire function.
/// @details Iterates blocks in layout order, rewriting each instruction to use
///          physical registers while invoking the coalescer to lower PX_COPY
///          pseudos. CFG liveness identifies safe register-carry edges and
///          cross-block spill homes; an optional global tier pins hot GPR
///          chains unless @c ZANNA_NO_GLOBAL_RA is set. The final spill-slot
///          counts are copied from the spiller before returning the result map.
/// @return Summary of virtual→physical mappings and spill requirements.
AllocationResult LinearScanAllocator::run() {
    // Compute CFG-aware liveness: builds control-flow graph from JMP/JCC
    // terminators and solves the standard backward dataflow equations to
    // produce per-block liveIn/liveOut sets. This replaces the conservative
    // "unconditional spill" hack that previously force-spilled ALL cross-block
    // vregs.
    liveness_.run(func_);

    // Classify vreg-free blocks (trap/abort stubs) once: carry decisions treat
    // edges into them as transparent because they never read a carried value.
    blockReadsNoVRegs_.assign(func_.blocks.size(), 1);
    for (std::size_t bi = 0; bi < func_.blocks.size(); ++bi) {
        for (const auto &instr : func_.blocks[bi].instructions) {
            bool readsVReg = false;
            for (const auto &operand : instr.operands) {
                if (const auto *reg = std::get_if<OpReg>(&operand); reg && !reg->isPhys) {
                    readsVReg = true;
                    break;
                }
                if (const auto *mem = std::get_if<OpMem>(&operand); mem && !mem->base.isPhys) {
                    readsVReg = true;
                    break;
                }
            }
            if (readsVReg) {
                blockReadsNoVRegs_[bi] = 0;
                break;
            }
        }
    }

    crossBlockSpillVRegs_.clear();
    pinnedGlobals_.clear();

    // Tier 1: pin the hottest cross-block vregs to callee-saved registers for
    // their entire lifetime so they never round-trip through spill slots at
    // block boundaries. Must run before the spill-home pre-pass below so
    // pinned vregs are excluded from it. ZANNA_NO_GLOBAL_RA=1 disables the
    // tier for triage.
    if (std::getenv("ZANNA_NO_GLOBAL_RA") == nullptr) {
        assignPinnedGlobals();
    }

    // Pre-pass: mark vregs that cross non-carryable CFG boundaries as needing
    // spill homes. Straight-line single-predecessor successors can carry values
    // in registers directly, but joins, backedges, and out-of-order successors
    // still need a memory home for correctness.
    for (std::size_t bi = 0; bi < func_.blocks.size(); ++bi) {
        if (canCarryIntoNextBlock(bi)) {
            continue;
        }
        for (uint16_t vreg : liveness_.liveOut(bi)) {
            if (pinnedGlobals_.count(vreg)) {
                continue;
            }
            crossBlockSpillVRegs_.insert(vreg);
            const auto *interval = intervals_.lookup(vreg);
            RegClass cls = interval ? interval->cls : RegClass::GPR;

            auto &state = stateFor(cls, vreg);
            if (!state.spill.needsSpill) {
                state.spill.needsSpill = true;
                spiller_.ensureSpillSlot(cls, state.spill);
            }
        }
    }

    Coalescer coalescer{*this, spiller_};
    for (std::size_t bi = 0; bi < func_.blocks.size(); ++bi) {
        currentBlockIdx_ = bi;
        processBlock(func_.blocks[bi], coalescer);
        releaseActiveForBlock(func_.blocks[bi], bi);
    }
    result_.spillSlotsGPR = spiller_.gprSlots();
    result_.spillSlotsXMM = spiller_.xmmSlots();
    return result_;
}

/// @copydoc LinearScanAllocator::assignPinnedGlobals
void LinearScanAllocator::assignPinnedGlobals() {
    const std::size_t blockCount = func_.blocks.size();
    if (blockCount == 0) {
        return;
    }

    // Pinnable pool: callee-saved GPRs, minus reserved registers. Callee-saved
    // registers survive calls by ABI and are never implicit clobber targets
    // (CQO/IDIV touch RAX/RDX; shifts touch RCX), so a pinned value is safe
    // everywhere without extra bookkeeping. SysV has no callee-saved XMM, so
    // floating-point values are not pinnable on this target.
    std::vector<PhysReg> pool;
    for (PhysReg reg : target_.calleeSavedGPR) {
        if (!isReservedGPR(reg)) {
            pool.push_back(reg);
        }
    }
    if (pool.empty()) {
        return;
    }

    // Candidates: the vregs the spill-home pre-pass would demote to memory —
    // GPR values live across a non-carryable boundary.
    std::unordered_set<uint16_t> candidateSet;
    for (std::size_t bi = 0; bi < blockCount; ++bi) {
        if (canCarryIntoNextBlock(bi)) {
            continue;
        }
        for (uint16_t vreg : liveness_.liveOut(bi)) {
            const auto *interval = intervals_.lookup(vreg);
            if (interval && interval->cls == RegClass::XMM) {
                continue;
            }
            candidateSet.insert(vreg);
        }
    }
    if (candidateSet.empty()) {
        return;
    }

    // Expand to the copy-closure: a GPR vreg connected to a candidate by a
    // register copy joins the set, so whole loop-carried chains coalesce onto
    // one pinned register and the connecting copies become identity moves.
    std::vector<std::pair<uint16_t, uint16_t>> allCopyPairs;
    for (const auto &block : func_.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode == MOpcode::PX_COPY) {
                for (std::size_t oi = 0; oi + 1 < instr.operands.size(); oi += 2) {
                    const auto *dst = std::get_if<OpReg>(&instr.operands[oi]);
                    const auto *src = std::get_if<OpReg>(&instr.operands[oi + 1]);
                    if (dst && src && !dst->isPhys && !src->isPhys && dst->cls == RegClass::GPR &&
                        src->cls == RegClass::GPR) {
                        allCopyPairs.emplace_back(dst->idOrPhys, src->idOrPhys);
                    }
                }
            } else if (instr.opcode == MOpcode::MOVrr && instr.operands.size() >= 2) {
                const auto *dst = std::get_if<OpReg>(&instr.operands[0]);
                const auto *src = std::get_if<OpReg>(&instr.operands[1]);
                if (dst && src && !dst->isPhys && !src->isPhys) {
                    allCopyPairs.emplace_back(dst->idOrPhys, src->idOrPhys);
                }
            }
        }
    }
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto &[dst, src] : allCopyPairs) {
            const bool hasDst = candidateSet.count(dst) != 0;
            const bool hasSrc = candidateSet.count(src) != 0;
            if (hasDst == hasSrc) {
                continue;
            }
            candidateSet.insert(hasDst ? src : dst);
            grew = true;
        }
    }

    // One scan collects, per candidate: per-block use counts (weight input)
    // and per-block first/last access half-positions (copy-coalescing input).
    std::unordered_map<uint16_t, std::vector<uint32_t>> useCounts;

    struct AccessSpan {
        uint32_t first{std::numeric_limits<uint32_t>::max()};
        uint32_t last{0};
    };

    std::unordered_map<uint16_t, std::unordered_map<std::size_t, AccessSpan>> accessSpans;
    for (std::size_t bi = 0; bi < blockCount; ++bi) {
        const auto &instructions = func_.blocks[bi].instructions;
        for (std::size_t ii = 0; ii < instructions.size(); ++ii) {
            const MInstr &instr = instructions[ii];
            for (std::size_t oi = 0; oi < instr.operands.size(); ++oi) {
                const auto *reg = std::get_if<OpReg>(&instr.operands[oi]);
                if (!reg || reg->isPhys || !candidateSet.count(reg->idOrPhys)) {
                    continue;
                }
                auto [it, inserted] =
                    useCounts.try_emplace(reg->idOrPhys, std::vector<uint32_t>(blockCount, 0));
                ++it->second[bi];

                const auto [isUse, isDef] = operandRoles(instr, oi);
                auto &span = accessSpans[reg->idOrPhys][bi];
                const uint32_t base = static_cast<uint32_t>(ii) * 2U;
                if (isUse) {
                    span.first = std::min(span.first, base);
                    span.last = std::max(span.last, base);
                }
                if (isDef) {
                    span.first = std::min(span.first, base + 1U);
                    span.last = std::max(span.last, base + 1U);
                }
            }
        }
    }

    // Loop depths from the liveness CFG drive the spill weights.
    std::vector<std::vector<std::size_t>> succs(blockCount);
    for (std::size_t bi = 0; bi < blockCount; ++bi) {
        succs[bi] = liveness_.successors(bi);
    }
    const std::vector<unsigned> loopDepth = zanna::codegen::ra::computeLoopDepths(succs);

    std::vector<zanna::codegen::ra::GlobalPinCandidate> candidates;
    candidates.reserve(candidateSet.size());
    for (uint16_t vreg : candidateSet) {
        zanna::codegen::ra::GlobalPinCandidate candidate;
        candidate.vreg = vreg;
        candidate.liveBlocks.assign(blockCount, 0);
        const auto countsIt = useCounts.find(vreg);
        const auto spansIt = accessSpans.find(vreg);
        for (std::size_t bi = 0; bi < blockCount; ++bi) {
            const bool liveIn = liveness_.liveIn(bi).count(vreg) != 0;
            const bool liveOut = liveness_.liveOut(bi).count(vreg) != 0;
            const bool accessed = spansIt != accessSpans.end() && spansIt->second.count(bi) != 0;
            if (!liveIn && !liveOut && !accessed) {
                continue;
            }
            candidate.liveBlocks[bi] = 1;

            zanna::codegen::ra::BlockSegment segment;
            if (liveIn) {
                segment.start = 0;
            } else if (accessed) {
                segment.start = spansIt->second.at(bi).first;
            }
            if (liveOut) {
                segment.end = std::numeric_limits<uint32_t>::max();
            } else if (accessed) {
                segment.end = spansIt->second.at(bi).last;
            }
            candidate.segments.emplace(bi, segment);

            double blockWeight = 1.0;
            for (unsigned d = 0; d < loopDepth[bi]; ++d) {
                blockWeight *= 10.0;
            }
            const uint32_t uses = countsIt != useCounts.end() ? countsIt->second[bi] : 0;
            candidate.weight += blockWeight * (1.0 + uses);
        }
        candidates.push_back(std::move(candidate));
    }

    // Merge copy-connected chains (loop param <-> loop temp) so each chain
    // occupies a single register and the connecting copies become identities.
    const auto alias = zanna::codegen::ra::coalescePinChains(candidates, allCopyPairs);

    auto assignment = zanna::codegen::ra::assignGlobalPins(std::move(candidates), pool, blockCount);
    if (assignment.pinned.empty()) {
        return;
    }

    /// @brief Installs one whole-function virtual-to-physical pin.
    /// @param vreg Virtual register to pin.
    /// @param phys Assigned callee-saved physical register.
    auto pinVreg = [&](uint16_t vreg, PhysReg phys) {
        pinnedGlobals_.emplace(vreg, phys);
        auto &state = stateFor(RegClass::GPR, vreg);
        state.hasPhys = true;
        state.phys = phys;
        state.pinnedGlobal = true;
        result_.vregToPhys[vreg] = phys;
    };
    for (const auto &[vreg, phys] : assignment.pinned) {
        pinVreg(vreg, phys);
    }
    for (const auto &[member, root] : alias) {
        auto rootPin = assignment.pinned.find(root);
        if (rootPin != assignment.pinned.end()) {
            pinVreg(member, rootPin->second);
        }
    }

    // Pinned registers belong to their candidates for the whole function;
    // remove them from the local free pool.
    /// @brief Tests whether a register is consumed by a global pin assignment.
    /// @param reg Free-pool register to inspect.
    /// @return `true` when the register occurs in `assignment.usedRegs`.
    freeGPR_.erase(std::remove_if(freeGPR_.begin(),
                                  freeGPR_.end(),
                                  [&](PhysReg reg) {
                                      return std::find(assignment.usedRegs.begin(),
                                                       assignment.usedRegs.end(),
                                                       reg) != assignment.usedRegs.end();
                                  }),
                   freeGPR_.end());
}

/// @copydoc LinearScanAllocator::canCarryIntoNextBlock
bool LinearScanAllocator::canCarryIntoNextBlock(std::size_t blockIdx) const {
    if (blockIdx + 1 >= func_.blocks.size()) {
        return false;
    }

    // The fallthrough successor must be the next block in layout order with
    // this block as its only predecessor. Additional successors are permitted
    // when they never read a virtual register (trap/abort blocks): control
    // that reaches them terminates without observing any carried value, so
    // registers can stay live across the edge to the fallthrough.
    const auto &succs = liveness_.successors(blockIdx);
    bool fallthroughFound = false;
    for (std::size_t succ : succs) {
        if (succ == blockIdx + 1) {
            fallthroughFound = true;
            continue;
        }
        if (succ >= func_.blocks.size() || !blockReadsNoVRegs_[succ]) {
            return false;
        }
    }
    if (!fallthroughFound) {
        return false;
    }

    const auto &preds = liveness_.predecessors(blockIdx + 1);
    return preds.size() == 1 && preds.front() == blockIdx;
}

/// @brief Populate the per-class register pools from target metadata.
/// @details Caller-saved and callee-saved registers are concatenated so the
///          allocator can draw from a single vector per class.  Reserved
///          GPRs (stack/frame pointers and backend scratch registers) are
///          filtered out to avoid accidental allocation.
void LinearScanAllocator::buildPools() {
    /// @brief Appends target-ordered registers to a free pool.
    /// @param pool Destination pool.
    /// @param regs Target metadata vector to append.
    auto appendRegs = [](RegPool &pool, const std::vector<PhysReg> &regs) {
        pool.insert(pool.end(), regs.begin(), regs.end());
    };

    appendRegs(freeGPR_, target_.callerSavedGPR);
    appendRegs(freeGPR_, target_.calleeSavedGPR);
    /// @brief Tests whether a GPR is reserved from allocator use.
    /// @param reg Register to inspect.
    /// @return `true` when the target reserves `reg`.
    freeGPR_.erase(std::remove_if(freeGPR_.begin(),
                                  freeGPR_.end(),
                                  [](PhysReg reg) { return isReservedGPR(reg); }),
                   freeGPR_.end());

    appendRegs(freeXMM_, target_.callerSavedFPR);
    appendRegs(freeXMM_, target_.calleeSavedFPR);
}

/// @brief Access the register pool matching a class.
/// @param cls Register class to query.
/// @return Mutable deque of available physical registers.
std::deque<PhysReg> &LinearScanAllocator::poolFor(RegClass cls) {
    return cls == RegClass::GPR ? freeGPR_ : freeXMM_;
}

/// @brief Access the active list for a given register class.
/// @param cls Register class to query.
/// @return Mutable set of virtual registers currently holding physical regs.
std::unordered_set<uint16_t> &LinearScanAllocator::activeFor(RegClass cls) {
    return cls == RegClass::GPR ? activeGPR_ : activeXMM_;
}

/// @brief Fetch or create the allocation record for a virtual register.
/// @details Stores the register class on first use and asserts that subsequent
///          queries agree on the class, catching mismatched operand encodings.
/// @param cls Register class inferred from the current operand.
/// @param id Virtual register identifier.
/// @return Mutable allocation state for @p id.
VirtualAllocation &LinearScanAllocator::stateFor(RegClass cls, uint16_t id) {
    auto [it, inserted] = states_.try_emplace(id);
    auto &state = it->second;
    if (inserted) {
        state.cls = cls;
        state.seen = true;
    } else {
        state.seen = true;
        if (state.cls != cls) {
            throw std::runtime_error("x86 register allocator: virtual register v" +
                                     std::to_string(id) + " reused as " + regClassName(cls) +
                                     " after being seen as " + regClassName(state.cls));
        }
    }
    return state;
}

/// @brief Look up (once) and cache the live interval for @p vreg on its state.
/// @details LiveIntervals is immutable during allocation, so the result of the
///          hash lookup is stable; caching it removes a map probe from the
///          per-instruction expiry scan over every active vreg.
/// @param vreg Virtual-register identifier to look up.
/// @param state Matching allocation state receiving the cached result.
/// @return Stable interval pointer, or @c nullptr when none exists.
const LiveInterval *LinearScanAllocator::cachedInterval(uint16_t vreg, VirtualAllocation &state) {
    if (!state.intervalCached) {
        state.interval = intervals_.lookup(vreg);
        state.intervalCached = true;
    }
    return state.interval;
}

/// @brief Record that a virtual register currently owns a physical register.
/// @details Active sets ensure the allocator can pick eviction victims and
///          release registers at block boundaries. Uses unordered_set for O(1)
///          insert instead of O(n) linear search.
/// @param cls Register class of the active value.
/// @param id Virtual register identifier.
void LinearScanAllocator::addActive(RegClass cls, uint16_t id) {
    activeFor(cls).insert(id);
}

/// @brief Remove a virtual register from the active set.
/// @details Called when a value goes dead or is explicitly spilled so future
///          spill victims do not consider the register. Uses unordered_set for
///          O(1) erase instead of O(n) remove-erase idiom.
/// @param cls Register class of the active value.
/// @param id Virtual register identifier to remove.
void LinearScanAllocator::removeActive(RegClass cls, uint16_t id) {
    activeFor(cls).erase(id);
}

/// @brief Lease a physical register from the free pool.
/// @details If the pool is empty the allocator triggers a spill to free one
///          register, appending spill code to @p prefix.  Once a register is
///          available, it is removed from the front of the pool to preserve a
///          deterministic allocation order.
/// @param cls Register class to allocate.
/// @param prefix Instruction list receiving any required spill code.
/// @return Physical register assigned to the caller.
/// @throws std::runtime_error If the pool is empty and no active value can be spilled.
PhysReg LinearScanAllocator::takeRegister(RegClass cls, std::vector<MInstr> &prefix) {
    auto &pool = poolFor(cls);
    if (pool.empty()) {
        if (!spillOne(cls, prefix)) {
            throw std::runtime_error(std::string("x86 register allocator: ") + regClassName(cls) +
                                     " register pool exhausted; all active values are pinned or "
                                     "unspillable for the current instruction");
        }
    }
    const PhysReg reg = pool.front();
    pool.pop_front(); // O(1) instead of O(n) erase(begin())
    return reg;
}

/// @brief Return a physical register to the free pool.
/// @details Used after temporary loads or at block exits to recycle registers
///          for future allocations.
/// @param phys Register being released.
/// @param cls Class of @p phys.
/// @throws std::runtime_error If @p phys is already free.
void LinearScanAllocator::releaseRegister(PhysReg phys, RegClass cls) {
    auto &pool = poolFor(cls);
    if (std::find(pool.begin(), pool.end(), phys) != pool.end()) {
        throw std::runtime_error("x86 register allocator: duplicate physical register release");
    }
    pool.push_back(phys);
}

/// @brief Spill one active virtual register to free a physical register.
/// @details Prefers non-cached, non-pinned values, then falls back to all
///          non-pinned active values. Within either tier it selects the
///          interval ending furthest away, breaking ties by virtual id.
///          Lifetime-based slot reuse is used when interval data is available.
/// @param cls Register class experiencing pressure.
/// @param prefix Instruction list capturing generated spill code.
/// @return True when spilling made a register available.
bool LinearScanAllocator::spillOne(RegClass cls, std::vector<MInstr> &prefix) {
    auto &active = activeFor(cls);
    if (active.empty()) {
        return false;
    }
    // Deterministic victim selection with two-pass Belady-style heuristic:
    // Pass 1: Prefer evicting non-cached vregs (those not loaded this block) to
    //         avoid thrashing the in-block register cache.
    // Pass 2: Fall back to all active vregs if all are cached.
    // Within each pass, pick the vreg whose live interval ends furthest from the
    // current instruction. Ties are broken by vreg ID for determinism.
    uint16_t victimId = 0;
    bool found = false;
    std::size_t furthestEnd = 0;

    // Pass 1: non-cached vregs only
    for (uint16_t vreg : active) {
        auto stateIt = states_.find(vreg);
        if (stateIt == states_.end() || !stateIt->second.hasPhys)
            continue;
        if (pinnedForInstr_.contains(vreg))
            continue;
        if (stateIt->second.cachedInBlock)
            continue; // Skip cached vregs in first pass
        const auto *interval = intervals_.lookup(vreg);
        const std::size_t end = interval ? interval->end : std::numeric_limits<std::size_t>::max();
        if (!found || end > furthestEnd || (end == furthestEnd && vreg > victimId)) {
            furthestEnd = end;
            victimId = vreg;
            found = true;
        }
    }

    // Pass 2: all vregs (fallback if all active are cached)
    if (!found) {
        for (uint16_t vreg : active) {
            auto stateIt = states_.find(vreg);
            if (stateIt == states_.end() || !stateIt->second.hasPhys)
                continue;
            if (pinnedForInstr_.contains(vreg))
                continue;
            const auto *interval = intervals_.lookup(vreg);
            const std::size_t end =
                interval ? interval->end : std::numeric_limits<std::size_t>::max();
            if (!found || end > furthestEnd || (end == furthestEnd && vreg > victimId)) {
                furthestEnd = end;
                victimId = vreg;
                found = true;
            }
        }
    }
    if (!found) {
        return false;
    }
    active.erase(victimId);
    auto it = states_.find(victimId);
    if (it == states_.end()) {
        return false;
    }
    auto &victim = it->second;
    if (!victim.hasPhys) {
        return false;
    }
    spillActiveValue(victimId, victim, prefix);
    return !poolFor(cls).empty();
}

/// @copydoc LinearScanAllocator::spillActiveValue
void LinearScanAllocator::spillActiveValue(uint16_t vreg,
                                           VirtualAllocation &state,
                                           std::vector<MInstr> &out) {
    const auto *interval = intervals_.lookup(vreg);
    if (interval && !crossBlockSpillVRegs_.contains(vreg)) {
        spiller_.spillValueWithReuse(state.cls,
                                     vreg,
                                     state,
                                     poolFor(state.cls),
                                     out,
                                     result_,
                                     interval->start,
                                     interval->end);
        return;
    }

    spiller_.spillValue(state.cls, vreg, state, poolFor(state.cls), out, result_);
}

/// @copydoc LinearScanAllocator::canUseScratchReload
bool LinearScanAllocator::canUseScratchReload(uint16_t vreg, const OperandRole &role) const {
    if (!role.isUse || role.isDef) {
        return false;
    }
    const auto *interval = intervals_.lookup(vreg);
    if (!interval || interval->end > currentInstrIdx_ + 1U) {
        return false;
    }
    if (currentBlockIdx_ < liveness_.numBlocks() &&
        liveness_.liveOut(currentBlockIdx_).contains(vreg)) {
        return false;
    }
    return true;
}

/// @copydoc LinearScanAllocator::pinInstructionVRegs
void LinearScanAllocator::pinInstructionVRegs(const MInstr &instr) {
    pinnedForInstr_.clear();
    for (const auto &operand : instr.operands) {
        if (const auto *reg = std::get_if<OpReg>(&operand)) {
            if (!reg->isPhys)
                pinnedForInstr_.insert(reg->idOrPhys);
            continue;
        }
        const auto *mem = std::get_if<OpMem>(&operand);
        if (!mem)
            continue;
        if (!mem->base.isPhys)
            pinnedForInstr_.insert(mem->base.idOrPhys);
        if (mem->hasIndex && !mem->index.isPhys)
            pinnedForInstr_.insert(mem->index.idOrPhys);
    }
}

/// @brief Release registers for vregs whose live intervals have ended.
/// @details At each instruction, we check all active vregs to see if their
///          interval ends at or before the current instruction. If so, the vreg
///          is no longer live and its physical register can be returned to the
///          free pool for reuse. This is essential for correct register reuse
///          within basic blocks.
void LinearScanAllocator::expireIntervals() {
    // Collect expired vregs (can't modify active set while iterating)
    std::vector<uint16_t> expiredGPR{};
    std::vector<uint16_t> expiredXMM{};

    for (auto vreg : activeGPR_) {
        auto it = states_.find(vreg);
        if (it == states_.end())
            continue;
        const auto *interval = cachedInterval(vreg, it->second);
        // Expire if interval ends at or before current instruction
        if (interval && interval->end <= currentInstrIdx_) {
            expiredGPR.push_back(vreg);
        }
    }

    for (auto vreg : activeXMM_) {
        auto it = states_.find(vreg);
        if (it == states_.end())
            continue;
        const auto *interval = cachedInterval(vreg, it->second);
        if (interval && interval->end <= currentInstrIdx_) {
            expiredXMM.push_back(vreg);
        }
    }

    // Now release the expired vregs
    for (auto vreg : expiredGPR) {
        auto it = states_.find(vreg);
        if (it != states_.end() && it->second.hasPhys) {
            releaseRegister(it->second.phys, RegClass::GPR);
            it->second.hasPhys = false;
            it->second.cachedInBlock = false;
        }
        removeActive(RegClass::GPR, vreg);
    }

    for (auto vreg : expiredXMM) {
        auto it = states_.find(vreg);
        if (it != states_.end() && it->second.hasPhys) {
            releaseRegister(it->second.phys, RegClass::XMM);
            it->second.hasPhys = false;
            it->second.cachedInBlock = false;
        }
        removeActive(RegClass::XMM, vreg);
    }
}

/// @copydoc LinearScanAllocator::isCallerSaved
bool LinearScanAllocator::isCallerSaved(PhysReg reg, RegClass cls) const noexcept {
    const auto &bits = cls == RegClass::GPR ? callerSavedGPRBits_ : callerSavedFPRBits_;
    return bits.test(static_cast<std::size_t>(reg));
}

/// @copydoc LinearScanAllocator::takeFreeCalleeSaved
std::pair<bool, PhysReg> LinearScanAllocator::takeFreeCalleeSaved(RegClass cls) {
    auto &pool = poolFor(cls);
    const auto &bits = cls == RegClass::GPR ? callerSavedGPRBits_ : callerSavedFPRBits_;
    for (auto it = pool.begin(); it != pool.end(); ++it) {
        if (!bits.test(static_cast<std::size_t>(*it))) {
            PhysReg reg = *it;
            pool.erase(it);
            return {true, reg};
        }
    }
    return {false, PhysReg::RAX};
}

/// @copydoc LinearScanAllocator::collectCallerSavedToSpill
std::vector<uint16_t> LinearScanAllocator::collectCallerSavedToSpill(RegClass cls) const {
    std::vector<uint16_t> out;
    const auto &active = cls == RegClass::GPR ? activeGPR_ : activeXMM_;
    for (auto vreg : active) {
        auto it = states_.find(vreg);
        if (it == states_.end() || !it->second.hasPhys)
            continue;
        if (!isCallerSaved(it->second.phys, cls))
            continue;
        // If we don't have interval info, conservatively spill to avoid data loss.
        const auto *interval = intervals_.lookup(vreg);
        const bool liveOut = liveness_.liveOut(currentBlockIdx_).count(vreg) != 0;
        if (!liveOut && interval && interval->end <= currentInstrIdx_ + 1)
            continue; // Value confirmed dead after the call.
        out.push_back(vreg);
    }
    return out;
}

/// @copydoc LinearScanAllocator::spillOrRehomeAcrossCall
void LinearScanAllocator::spillOrRehomeAcrossCall(RegClass cls,
                                                  const std::vector<uint16_t> &candidates,
                                                  std::vector<MInstr> &prefix) {
    // Phase 1: Try to move each value to a free callee-saved register so it
    // survives the CALL without a memory round-trip.
    std::vector<uint16_t> stillNeedSpill;
    for (auto vreg : candidates) {
        auto &state = states_[vreg];
        auto [found, csReg] = takeFreeCalleeSaved(cls);
        if (found) {
            prefix.push_back(makeMove(cls, csReg, state.phys));
            releaseRegister(state.phys, cls);
            state.phys = csReg;
            result_.vregToPhys[vreg] = csReg;
            // Value stays active with new physical register; cachedInBlock preserved.
        } else {
            stillNeedSpill.push_back(vreg);
        }
    }

    // Phase 2: Spill remaining values to memory.
    for (auto vreg : stillNeedSpill) {
        auto &state = states_[vreg];
        spillActiveValue(vreg, state, prefix);
        removeActive(cls, vreg);
    }
}

/// @brief Rewrite a block so each instruction uses allocated registers.
/// @details The method iterates the block, lowering PX_COPY pseudos via the
///          coalescer and handling other instructions by:
///          1. Classifying operand roles (use/def).
///          2. Ensuring operands have physical registers, emitting loads or
///             spills into prefix/suffix buffers as needed.
///          3. Releasing scratch registers after their final use.
///          The rewritten instruction sequence replaces the original block
///          contents in place.
/// @param block Machine basic block being processed.
/// @param coalescer Helper that lowers PX_COPY instructions.
void LinearScanAllocator::processBlock(MBasicBlock &block, Coalescer &coalescer) {
    std::vector<MInstr> rewritten{};
    rewritten.reserve(block.instructions.size());

    for (auto &instr : block.instructions) {
        // Expire vregs whose live intervals have ended before this instruction.
        // This ensures their physical registers are returned to the free pool for reuse.
        expireIntervals();
        pinInstructionVRegs(instr);

        if (instr.opcode == MOpcode::PX_COPY) {
            coalescer.lower(instr, rewritten);
            pinnedForInstr_.clear();
            ++currentInstrIdx_;
            continue;
        }

        // Before processing operands, check if this instruction writes to a physical
        // register. Fixed-register sequences such as division setup clobber RAX/RDX/R10
        // before normal virtual operands are rewritten, so any active value occupying
        // those registers must be spilled first.
        std::vector<MInstr> prefix{};
        const uint16_t srcVreg = passthroughSourceVReg(instr);
        for (const auto &clobber : collectPhysicalClobbers(instr)) {
            auto &activeSet = activeFor(clobber.cls);
            for (auto vreg : activeSet) {
                auto it = states_.find(vreg);
                if (it == states_.end() || !it->second.hasPhys || it->second.phys != clobber.reg) {
                    continue;
                }
                if (vreg == srcVreg || isIdentityPhysicalMove(instr, clobber.reg)) {
                    break;
                }

                auto &state = it->second;
                const auto *interval = intervals_.lookup(vreg);
                const bool valueNeeded = !interval || interval->end > currentInstrIdx_;
                if (valueNeeded) {
                    spillActiveValue(vreg, state, prefix);
                } else {
                    releaseRegister(state.phys, clobber.cls);
                    state.hasPhys = false;
                    state.cachedInBlock = false;
                }
                removeActive(clobber.cls, vreg);
                break;
            }

            if (isArgumentRegister(clobber.reg)) {
                reserveForCall(clobber.reg);
            }
        }
        std::vector<MInstr> suffix{};
        std::vector<ScratchRelease> scratch{};
        // The original block is replaced wholesale at the end of this loop, so
        // the instruction can be moved instead of deep-copied (operand vectors
        // and label strings make copies expensive at this frequency).
        MInstr current = std::move(instr);
        auto roles = classifyOperands(current);

        for (std::size_t idx = 0; idx < current.operands.size(); ++idx) {
            handleOperand(current.operands[idx], roles[idx], prefix, suffix, scratch);
        }

        // Handle CALL: values in caller-saved registers are clobbered.
        // Spill (or re-home into callee-saved registers when free) BEFORE the call.
        if (current.opcode == MOpcode::CALL) {
            // Snapshot active sets first — spillOrRehome mutates them.
            const auto gprCandidates = collectCallerSavedToSpill(RegClass::GPR);
            const auto xmmCandidates = collectCallerSavedToSpill(RegClass::XMM);
            spillOrRehomeAcrossCall(RegClass::GPR, gprCandidates, prefix);
            spillOrRehomeAcrossCall(RegClass::XMM, xmmCandidates, prefix);

            // Release argument registers reserved during call setup.
            releaseCallReserved();
        }

        // (CQO's implicit RDX clobber is handled by collectPhysicalClobbers
        // before operand rewriting; no second pass is needed here.)

        for (auto &pre : prefix) {
            rewritten.push_back(std::move(pre));
        }
        rewritten.push_back(std::move(current));
        for (auto &suf : suffix) {
            rewritten.push_back(std::move(suf));
        }
        for (const auto &rel : scratch) {
            releaseRegister(rel.phys, rel.cls);
        }

        pinnedForInstr_.clear();
        ++currentInstrIdx_;
    }

    pinnedForInstr_.clear();
    block.instructions = std::move(rewritten);
}

/// @brief Release or spill registers at block boundaries using CFG-aware liveOut.
/// @details Carries registers across a safe next-block edge. Otherwise,
///          live-out values are guaranteed spill homes and all local physical
///          holdings are released. Boundary stores are inserted before the
///          first terminator.
/// @param block The block that was just processed.
/// @param blockIdx Index of the block for liveOut lookup.
void LinearScanAllocator::releaseActiveForBlock(MBasicBlock &block, std::size_t blockIdx) {
    const auto &liveOutSet = liveness_.liveOut(blockIdx);
    const bool carryToNext = canCarryIntoNextBlock(blockIdx);

    // Helper to check if an instruction is a terminator
    /// @brief Classifies instructions before which boundary spills must be inserted.
    /// @param opc Machine opcode to inspect.
    /// @return @c true for jump, conditional jump, return, or trap.
    auto isTerminator = [](MOpcode opc) {
        return opc == MOpcode::JMP || opc == MOpcode::JCC || opc == MOpcode::RET ||
               opc == MOpcode::UD2;
    };

    // Find insertion point — before the terminator if present.
    std::size_t insertPos = block.instructions.size();
    for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
        if (isTerminator(block.instructions[idx].opcode)) {
            insertPos = idx;
            break;
        }
    }

    std::vector<MInstr> spills{};
    std::unordered_set<uint16_t> nextActiveGPR{};
    std::unordered_set<uint16_t> nextActiveXMM{};

    // Process GPR values at block boundaries.
    for (auto vreg : activeGPR_) {
        auto it = states_.find(vreg);
        if (it == states_.end() || !it->second.hasPhys)
            continue;

        auto &state = it->second;

        if (liveOutSet.count(vreg) && carryToNext) {
            if (state.spill.needsSpill) {
                state.cachedInBlock = true;
            }
            nextActiveGPR.insert(vreg);
            continue;
        }

        // Live across the boundary: the value must be in its spill slot. For
        // vregs already marked needsSpill the slot is necessarily current —
        // every def of such a vreg appends a suffix store — so emitting
        // another store here only duplicated the last one. The store remains
        // as a defensive path for vregs the cross-block pre-pass somehow
        // missed (it marks every liveOut vreg of non-carry blocks).
        if (liveOutSet.count(vreg) && !state.spill.needsSpill) {
            spiller_.ensureSpillSlot(RegClass::GPR, state.spill);
            state.spill.needsSpill = true;
            spills.push_back(spiller_.makeStore(RegClass::GPR, state.spill, state.phys));
        }

        releaseRegister(state.phys, RegClass::GPR);
        state.hasPhys = false;
        state.cachedInBlock = false;
    }
    activeGPR_.swap(nextActiveGPR);

    // Process XMM values — same approach as GPR.
    for (auto vreg : activeXMM_) {
        auto it = states_.find(vreg);
        if (it == states_.end() || !it->second.hasPhys)
            continue;

        auto &state = it->second;

        if (liveOutSet.count(vreg) && carryToNext) {
            if (state.spill.needsSpill) {
                state.cachedInBlock = true;
            }
            nextActiveXMM.insert(vreg);
            continue;
        }

        if (liveOutSet.count(vreg) && !state.spill.needsSpill) {
            spiller_.ensureSpillSlot(RegClass::XMM, state.spill);
            state.spill.needsSpill = true;
            spills.push_back(spiller_.makeStore(RegClass::XMM, state.spill, state.phys));
        }

        releaseRegister(state.phys, RegClass::XMM);
        state.hasPhys = false;
        state.cachedInBlock = false;
    }
    activeXMM_.swap(nextActiveXMM);

    // Insert spills before the terminator(s).
    if (!spills.empty()) {
        block.instructions.insert(block.instructions.begin() + static_cast<long>(insertPos),
                                  std::make_move_iterator(spills.begin()),
                                  std::make_move_iterator(spills.end()));
    }
}

/// @brief Determine whether operands are read, written, or both.
/// @details The classification drives register materialisation: uses require
///          loads while defs may force spills after the instruction executes.
///          Shared operand-role metadata remains the single source of truth.
/// @param instr Instruction whose operands are being analysed.
/// @return Vector describing the role of each operand.
std::vector<LinearScanAllocator::OperandRole> LinearScanAllocator::classifyOperands(
    const MInstr &instr) const {
    std::vector<OperandRole> roles(instr.operands.size(), OperandRole{false, false});
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        roles[idx] = OperandRole{isUse, isDef};
    }
    return roles;
}

/// @brief Ensure an operand has a valid physical encoding.
/// @details Delegates to @ref processRegOperand for register operands and
///          handles a memory operand's base and optional index registers.
///          Immediate-like operands require no work.
/// @param operand Operand being rewritten in place.
/// @param role Use/def classification for @p operand.
/// @param prefix Instruction list receiving pre-instruction loads or spills.
/// @param suffix Instruction list receiving post-instruction spills.
/// @param scratch Scratch register tracker used to release temporaries.
void LinearScanAllocator::handleOperand(Operand &operand,
                                        const OperandRole &role,
                                        std::vector<MInstr> &prefix,
                                        std::vector<MInstr> &suffix,
                                        std::vector<ScratchRelease> &scratch) {
    /// @brief Rewrites a register variant through `processRegOperand`.
    /// @param reg Register operand to rewrite.
    std::visit(Overload{[&](OpReg &reg) { processRegOperand(reg, role, prefix, suffix, scratch); },
                        /// @brief Rewrites the base and optional index of a memory operand.
                        /// @param mem Memory operand to rewrite.
                        [&](OpMem &mem) {
                            OperandRole baseRole{true, false};
                            processRegOperand(mem.base, baseRole, prefix, suffix, scratch);
                            // Also process the index register if present
                            if (mem.hasIndex) {
                                processRegOperand(mem.index, baseRole, prefix, suffix, scratch);
                            }
                        },
                        /// @brief Leaves immediate and label variants unchanged.
                        [](auto &) {}},
               operand);
}

/// @brief Rewrite a virtual register operand into a physical register operand.
/// @details Handles three scenarios:
///          1. Already-spilled values: reload into a scratch register (for uses)
///             and/or schedule stores (for defs).
///          2. First-time allocations: lease a register, update maps, and mark
///             the register as active.
///          3. Previously allocated values: reuse the recorded physical
///             register.  Any scratch registers acquired are tracked for later
///             release.
/// @param reg Operand mutated in place.
/// @param role Use/def role for @p reg.
/// @param prefix List receiving pre-instruction loads.
/// @param suffix List receiving post-instruction spills.
/// @param scratch Scratch register tracker for release bookkeeping.
void LinearScanAllocator::processRegOperand(OpReg &reg,
                                            const OperandRole &role,
                                            std::vector<MInstr> &prefix,
                                            std::vector<MInstr> &suffix,
                                            std::vector<ScratchRelease> &scratch) {
    if (reg.isPhys) {
        return;
    }

    auto &state = stateFor(reg.cls, reg.idOrPhys);
    if (state.pinnedGlobal) {
        // Pinned globals hold their register for the whole function: no
        // loads, stores, active-set membership, or pool interaction.
        reg = makePhysReg(state.cls, static_cast<uint16_t>(state.phys));
        return;
    }
    if (state.spill.needsSpill) {
        if (state.hasPhys && state.cachedInBlock) {
            // Already cached this block — reuse the register without reloading.
            if (role.isDef) {
                suffix.push_back(spiller_.makeStore(state.cls, state.spill, state.phys));
            }
            reg = makePhysReg(state.cls, static_cast<uint16_t>(state.phys));
            return;
        }
        // First access this block — allocate, load, and cache for subsequent uses.
        spiller_.ensureSpillSlot(state.cls, state.spill);
        const PhysReg phys = takeRegister(state.cls, prefix);
        if (role.isUse) {
            prefix.push_back(spiller_.makeLoad(state.cls, phys, state.spill));
        }
        if (role.isDef) {
            suffix.push_back(spiller_.makeStore(state.cls, state.spill, phys));
        }
        if (canUseScratchReload(reg.idOrPhys, role)) {
            scratch.push_back(ScratchRelease{phys, state.cls});
            reg = makePhysReg(state.cls, static_cast<uint16_t>(phys));
            return;
        }
        state.hasPhys = true;
        state.phys = phys;
        state.cachedInBlock = true;
        addActive(state.cls, reg.idOrPhys);
        result_.vregToPhys[reg.idOrPhys] = phys;
        reg = makePhysReg(state.cls, static_cast<uint16_t>(phys));
        return;
    }

    if (!state.hasPhys) {
        const PhysReg phys = takeRegister(state.cls, prefix);
        state.hasPhys = true;
        state.phys = phys;
        addActive(state.cls, reg.idOrPhys);
        result_.vregToPhys[reg.idOrPhys] = phys;
    }

    reg = makePhysReg(state.cls, static_cast<uint16_t>(state.phys));
}

/// @brief Build a register-to-register move for a specific class.
/// @details Used by the coalescer and allocator to move values without
///          duplicating opcode selection logic.
/// @param cls Register class describing the move type.
/// @param dst Destination physical register.
/// @param src Source physical register.
/// @return Machine instruction encoding the move.
MInstr LinearScanAllocator::makeMove(RegClass cls, PhysReg dst, PhysReg src) const {
    if (cls == RegClass::GPR) {
        return MInstr::make(MOpcode::MOVrr, {makePhysOperand(cls, dst), makePhysOperand(cls, src)});
    }
    return MInstr::make(MOpcode::MOVSDrr, {makePhysOperand(cls, dst), makePhysOperand(cls, src)});
}

/// @brief Check if a physical register is an argument register for the current ABI.
/// @details Used to detect when call argument registers are being set so they can be
///          reserved and not used for spill reloads during call setup.  Checks both
///          GPR and FP argument registers to prevent spill reloads from clobbering
///          marshalled arguments of either class before the CALL executes.
/// @param reg Physical register to check.
/// @return @c true if @p reg is an argument-passing register.
bool LinearScanAllocator::isArgumentRegister(PhysReg reg) const {
    for (std::size_t i = 0; i < target_.maxGPRArgs && i < target_.intArgOrder.size(); ++i) {
        if (target_.intArgOrder[i] == reg) {
            return true;
        }
    }
    for (std::size_t i = 0; i < target_.maxFPArgs && i < target_.f64ArgOrder.size(); ++i) {
        if (target_.f64ArgOrder[i] == reg) {
            return true;
        }
    }
    return false;
}

/// @brief Reserve an argument register during call setup.
/// @details Removes the register from the appropriate free pool (GPR or XMM) and
///          records it so it can be released after the CALL instruction is processed.
///          This prevents spill reloads from clobbering argument values during call
///          setup for both integer and floating-point arguments.
/// @param reg Physical register to reserve.
void LinearScanAllocator::reserveForCall(PhysReg reg) {
    // Linear search is fine: reservedForCall_ holds at most 6+8 argument
    // registers on x86-64, so O(n) with n<=14 beats any fancier structure.
    for (const auto &r : reservedForCall_) {
        if (r.phys == reg)
            return;
    }
    // Determine the class from the register itself.
    const RegClass cls = isXMM(reg) ? RegClass::XMM : RegClass::GPR;
    // Remove from the appropriate free pool.
    auto &pool = poolFor(cls);
    auto it = std::find(pool.begin(), pool.end(), reg);
    if (it != pool.end()) {
        pool.erase(it);
        reservedForCall_.push_back({reg, cls});
    }
}

/// @brief Release all reserved argument registers back to the pool.
/// @details Called after a CALL instruction is processed to make argument
///          registers available for subsequent allocations.  Returns each
///          register to its original class pool (GPR or XMM).
void LinearScanAllocator::releaseCallReserved() {
    for (const auto &r : reservedForCall_) {
        releaseRegister(r.phys, r.cls);
    }
    reservedForCall_.clear();
}

} // namespace zanna::codegen::x64::ra
