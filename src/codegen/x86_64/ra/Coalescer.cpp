//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/ra/Coalescer.cpp
// Purpose: Lower PX_COPY bundles into executable move sequences while
//          respecting spill state managed by the linear-scan allocator.
// Key invariants:
//   - Operand resolution (virtual -> physical or slot) is the only allocator
//     state this file mutates; ordering and cycle breaking are the shared
//     sequentializer's.
//   - Borrowed XMM scratch registers are released as soon as the sequencer
//     no longer reads them; R10/R11 are fixed and never released.
// Ownership/Lifetime:
//   - Operates on MIR provided by the allocator; no ownership taken.
// Links: src/codegen/x86_64/ra/Coalescer.hpp,
//        src/codegen/x86_64/ra/Allocator.hpp,
//        src/codegen/x86_64/ra/Spiller.hpp,
//        src/codegen/common/ra/ParallelCopy.hpp
//
//===----------------------------------------------------------------------===//

#include "Coalescer.hpp"

#include "Allocator.hpp"
#include "Spiller.hpp"

#include <stdexcept>

/// @file
/// @brief Lowers PX_COPY pseudo instructions into executable move sequences.

namespace zanna::codegen::x64::ra {

namespace {

using codegen::ra::CopyLoc;
using codegen::ra::ParallelCopyTask;

/// @brief Return true when @p reg is one of the fixed GPR scratch registers.
/// @details R10/R11 are deliberately excluded from the allocator pool and used
///          by PX_COPY lowering for cycle breaking and memory-to-memory copies.
///          A PX_COPY bundle that explicitly names either register cannot be
///          lowered with those fixed scratch assumptions intact.
[[nodiscard]] bool isFixedGprScratch(PhysReg reg) noexcept {
    return reg == PhysReg::R10 || reg == PhysReg::R11;
}

/// @brief Validate a physical PX_COPY operand against fixed scratch registers.
/// @throws std::runtime_error If a GPR copy operand names R10 or R11.
void rejectFixedScratchOperand(RegClass cls, PhysReg reg) {
    if (cls == RegClass::GPR && isFixedGprScratch(reg)) {
        throw std::runtime_error(
            "x86 PX_COPY lowering: R10/R11 cannot appear as explicit copy operands");
    }
}

/// @brief Location of a task's destination in the shared sequencer's terms.
[[nodiscard]] CopyLoc destLoc(const CopyTask &task) noexcept {
    const auto cls = static_cast<unsigned>(task.cls);
    return task.destKind == CopyTask::DestKind::Reg
               ? CopyLoc::regLoc(cls, static_cast<unsigned>(task.destReg))
               : CopyLoc::memLoc(cls, task.destSlot);
}

/// @brief Location of a task's source in the shared sequencer's terms.
[[nodiscard]] CopyLoc srcLoc(const CopyTask &task) noexcept {
    const auto cls = static_cast<unsigned>(task.cls);
    return task.src.kind == CopySource::Kind::Reg
               ? CopyLoc::regLoc(cls, static_cast<unsigned>(task.src.reg))
               : CopyLoc::memLoc(cls, task.src.slot);
}

} // namespace

/// @brief Adapter the shared sequentializer drives; owns nothing.
struct Coalescer::CopyEmitter {
    Coalescer &owner;
    std::vector<MInstr> &generated;

    void move(const CopyLoc &dst, const CopyLoc &src) {
        owner.emitMove(dst, src, generated);
    }

    /// @brief GPR cycles break through the fixed R10; XMM borrows a register.
    CopyLoc cycleScratch(unsigned cls) {
        const auto regClass = static_cast<RegClass>(cls);
        if (regClass == RegClass::GPR)
            return CopyLoc::regLoc(cls, static_cast<unsigned>(PhysReg::R10));
        return CopyLoc::regLoc(cls,
                               static_cast<unsigned>(owner.borrowRegister(regClass, generated)));
    }

    /// @brief Memory-to-memory copies go through the fixed R11; XMM borrows.
    CopyLoc memTemp(unsigned cls) {
        const auto regClass = static_cast<RegClass>(cls);
        if (regClass == RegClass::GPR)
            return CopyLoc::regLoc(cls, static_cast<unsigned>(PhysReg::R11));
        return CopyLoc::regLoc(cls,
                               static_cast<unsigned>(owner.borrowRegister(regClass, generated)));
    }

    void releaseScratch(const CopyLoc &loc) {
        const auto regClass = static_cast<RegClass>(loc.cls);
        if (regClass == RegClass::GPR)
            return; // fixed scratch, never borrowed
        owner.returnRegister(static_cast<PhysReg>(loc.reg), regClass);
    }
};

/// @brief Construct a coalescer tied to a specific allocator and spiller.
/// @param allocator Linear-scan allocator supplying register state.
/// @param spiller Spiller responsible for materialising loads and stores.
Coalescer::Coalescer(LinearScanAllocator &allocator, Spiller &spiller)
    : allocator_(allocator), spiller_(spiller) {}

/// @copydoc Coalescer::emitMove
void Coalescer::emitMove(const CopyLoc &dst, const CopyLoc &src, std::vector<MInstr> &generated) {
    const auto cls = static_cast<RegClass>(dst.cls);
    if (dst.isReg() && src.isReg()) {
        generated.push_back(
            allocator_.makeMove(cls, static_cast<PhysReg>(dst.reg), static_cast<PhysReg>(src.reg)));
    } else if (dst.isReg()) {
        generated.push_back(
            spiller_.makeLoad(cls, static_cast<PhysReg>(dst.reg), SpillPlan{true, src.slot}));
    } else if (src.isReg()) {
        generated.push_back(
            spiller_.makeStore(cls, SpillPlan{true, dst.slot}, static_cast<PhysReg>(src.reg)));
    } else {
        throw std::runtime_error("x86 PX_COPY lowering: memory-to-memory move reached the emitter");
    }
}

/// @copydoc Coalescer::borrowRegister
PhysReg Coalescer::borrowRegister(RegClass cls, std::vector<MInstr> &generated) {
    std::vector<MInstr> prefix{};
    const PhysReg reg = allocator_.takeRegister(cls, prefix);
    for (auto &pre : prefix)
        generated.push_back(std::move(pre));
    return reg;
}

/// @copydoc Coalescer::returnRegister
void Coalescer::returnRegister(PhysReg reg, RegClass cls) {
    allocator_.releaseRegister(reg, cls);
}

/// @brief Expand a @c PX_COPY pseudo into executable machine instructions.
/// @details Two phases: resolve every operand pair to a @ref CopyTask (taking
///          registers from the allocator for unmapped virtual registers and
///          emitting any victim spills first), then run the shared
///          sequentializer over the location pairs with @ref CopyEmitter
///          producing the x86-64 instructions in dependency-safe order.
/// @param instr @c PX_COPY instruction to lower.
/// @param out Vector receiving the lowered instruction sequence.
/// @throws std::runtime_error If operands are malformed, use reserved scratch
///         registers, disagree in class, or require an unavailable register.
void Coalescer::lower(const MInstr &instr, std::vector<MInstr> &out) {
    std::vector<MInstr> prefix{};
    std::vector<CopyTask> tasks{};

    if ((instr.operands.size() % 2U) != 0U) {
        throw std::runtime_error("x86 PX_COPY lowering: operand count must be even");
    }

    for (std::size_t i = 0; i + 1 < instr.operands.size(); i += 2) {
        const auto &dstOp = instr.operands[i];
        const auto &srcOp = instr.operands[i + 1];

        const auto *dstReg = std::get_if<OpReg>(&dstOp);
        const auto *srcReg = std::get_if<OpReg>(&srcOp);
        if (!dstReg || !srcReg) {
            throw std::runtime_error("x86 PX_COPY lowering: expected register operand pairs");
        }
        if (dstReg->cls != srcReg->cls) {
            throw std::runtime_error("x86 PX_COPY lowering: source and destination classes differ");
        }

        CopyTask task{};
        task.cls = dstReg->cls;
        task.destVReg.reset();

        if (dstReg->isPhys) {
            task.destKind = CopyTask::DestKind::Reg;
            task.destReg = static_cast<PhysReg>(dstReg->idOrPhys);
            rejectFixedScratchOperand(task.cls, task.destReg);
        } else {
            auto &dstState = allocator_.stateFor(dstReg->cls, dstReg->idOrPhys);
            task.destVReg = dstReg->idOrPhys;
            if (dstState.spill.needsSpill) {
                spiller_.ensureSpillSlot(dstState.cls, dstState.spill);
                task.destKind = CopyTask::DestKind::Mem;
                task.destSlot = dstState.spill.slot;
            } else {
                if (!dstState.hasPhys) {
                    const PhysReg phys = allocator_.takeRegister(dstState.cls, prefix);
                    dstState.hasPhys = true;
                    dstState.phys = phys;
                    allocator_.addActive(dstState.cls, dstReg->idOrPhys);
                    allocator_.result_.vregToPhys[dstReg->idOrPhys] = phys;
                }
                task.destKind = CopyTask::DestKind::Reg;
                task.destReg = dstState.phys;
            }
        }

        if (srcReg->isPhys) {
            task.src.kind = CopySource::Kind::Reg;
            task.src.reg = static_cast<PhysReg>(srcReg->idOrPhys);
            rejectFixedScratchOperand(task.cls, task.src.reg);
        } else {
            auto &srcState = allocator_.stateFor(srcReg->cls, srcReg->idOrPhys);
            if (srcState.spill.needsSpill) {
                spiller_.ensureSpillSlot(srcState.cls, srcState.spill);
                // Keep spilled sources in memory until their copy is emitted so
                // large edge copies do not reserve one scratch register per source.
                task.src.kind = CopySource::Kind::Mem;
                task.src.slot = srcState.spill.slot;
            } else {
                if (!srcState.hasPhys) {
                    const PhysReg phys = allocator_.takeRegister(srcState.cls, prefix);
                    srcState.hasPhys = true;
                    srcState.phys = phys;
                    allocator_.addActive(srcState.cls, srcReg->idOrPhys);
                    allocator_.result_.vregToPhys[srcReg->idOrPhys] = phys;
                }
                task.src.kind = CopySource::Kind::Reg;
                task.src.reg = srcState.phys;
            }
        }

        tasks.push_back(task);
    }

    for (auto &pre : prefix) {
        out.push_back(std::move(pre));
    }

    std::vector<ParallelCopyTask> pairs;
    pairs.reserve(tasks.size());
    for (const auto &task : tasks)
        pairs.push_back(ParallelCopyTask{destLoc(task), srcLoc(task)});

    std::vector<MInstr> generated{};
    generated.reserve(tasks.size());
    CopyEmitter emitter{*this, generated};
    (void)codegen::ra::sequentializeParallelCopy(std::move(pairs), emitter);

    for (auto &instrOut : generated) {
        out.push_back(std::move(instrOut));
    }
}

} // namespace zanna::codegen::x64::ra
