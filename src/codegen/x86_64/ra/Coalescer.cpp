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
//   - Scratch registers allocated to break cycles are released after use.
//   - Generated code preserves the semantics of the original parallel copy.
// Ownership/Lifetime:
//   - Operates on MIR provided by the allocator; no ownership taken.
// Links: src/codegen/x86_64/ra/Coalescer.hpp,
//        src/codegen/x86_64/ra/Allocator.hpp,
//        src/codegen/x86_64/ra/Spiller.hpp
//
//===----------------------------------------------------------------------===//

#include "Coalescer.hpp"

#include "Allocator.hpp"
#include "Spiller.hpp"

#include <algorithm>
#include <stdexcept>

/// @file
/// @brief Lowers PX_COPY pseudo instructions into executable move sequences.
/// @details The coalescer collaborates with the linear-scan allocator and
///          spiller to materialise register moves, managing scratch registers and
///          spill reloads to preserve the semantics of the original parallel
///          copy bundles.

namespace zanna::codegen::x64::ra {

namespace {

/// @brief Ticket recording a temporarily-borrowed scratch register.
/// @details Used by the coalescer to return scratch registers to the
///          allocator after a cycle-breaking sequence completes.
struct ScratchRelease {
    PhysReg phys{PhysReg::RAX};  ///< Physical register that was borrowed.
    RegClass cls{RegClass::GPR}; ///< Register class for the allocator's bookkeeping.
};

/// @brief Concrete register or spill-slot node in the parallel-copy graph.
struct CopyLocation {
    /// @brief Storage category of the node.
    enum class Kind { Reg, Mem };

    /// @brief Whether @c reg or @c slot identifies the location.
    Kind kind{Kind::Reg};
    /// @brief Register bank used to distinguish otherwise equal locations.
    RegClass cls{RegClass::GPR};
    /// @brief Physical register when @c kind is @c Reg.
    PhysReg reg{PhysReg::RAX};
    /// @brief Spill-slot index when @c kind is @c Mem.
    int slot{-1};
};

/// @brief Converts a copy task's source into a uniform graph location.
/// @param task Copy task to inspect.
/// @return Register or spill-slot source location.
[[nodiscard]] CopyLocation sourceLocation(const CopyTask &task) {
    if (task.src.kind == CopySource::Kind::Reg) {
        return CopyLocation{CopyLocation::Kind::Reg, task.cls, task.src.reg, -1};
    }
    return CopyLocation{CopyLocation::Kind::Mem, task.cls, PhysReg::RAX, task.src.slot};
}

/// @brief Converts a copy task's destination into a uniform graph location.
/// @param task Copy task to inspect.
/// @return Register or spill-slot destination location.
[[nodiscard]] CopyLocation destLocation(const CopyTask &task) {
    if (task.destKind == CopyTask::DestKind::Reg) {
        return CopyLocation{CopyLocation::Kind::Reg, task.cls, task.destReg, -1};
    }
    return CopyLocation{CopyLocation::Kind::Mem, task.cls, PhysReg::RAX, task.destSlot};
}

/// @brief Compares two typed copy-graph locations.
/// @param lhs First location.
/// @param rhs Second location.
/// @return @c true when kind, class, and register or slot identity match.
[[nodiscard]] bool sameLocation(const CopyLocation &lhs, const CopyLocation &rhs) noexcept {
    if (lhs.kind != rhs.kind || lhs.cls != rhs.cls) {
        return false;
    }
    return lhs.kind == CopyLocation::Kind::Reg ? lhs.reg == rhs.reg : lhs.slot == rhs.slot;
}

/// @brief Tests whether a task reads a specified copy-graph location.
/// @param task Copy task whose source is inspected.
/// @param location Candidate source location.
/// @return @c true when the typed source matches @p location.
[[nodiscard]] bool sourceMatchesLocation(const CopyTask &task,
                                         const CopyLocation &location) noexcept {
    if (location.kind == CopyLocation::Kind::Reg) {
        return task.src.kind == CopySource::Kind::Reg && task.cls == location.cls &&
               task.src.reg == location.reg;
    }
    return task.src.kind == CopySource::Kind::Mem && task.cls == location.cls &&
           task.src.slot == location.slot;
}

/// @brief Return true when @p reg is one of the fixed GPR scratch registers.
/// @details R10/R11 are deliberately excluded from the allocator pool and used
///          by PX_COPY lowering for cycle breaking and memory-to-memory copies.
///          A PX_COPY bundle that explicitly names either register cannot be
///          lowered with those fixed scratch assumptions intact.
/// @param reg Physical register to classify.
/// @return @c true for R10 or R11.
[[nodiscard]] bool isFixedGprScratch(PhysReg reg) noexcept {
    return reg == PhysReg::R10 || reg == PhysReg::R11;
}

/// @brief Validate a physical PX_COPY operand against fixed scratch registers.
/// @param cls Register class of @p reg.
/// @param reg Explicit physical copy operand.
/// @throws std::runtime_error If a GPR copy operand names R10 or R11.
void rejectFixedScratchOperand(RegClass cls, PhysReg reg) {
    if (cls == RegClass::GPR && isFixedGprScratch(reg)) {
        throw std::runtime_error(
            "x86 PX_COPY lowering: R10/R11 cannot appear as explicit copy operands");
    }
}

} // namespace

/// @brief Construct a coalescer tied to a specific allocator and spiller.
/// @details The constructor stores references to the linear-scan allocator and
///          spiller so future copy lowering can request scratch registers and
///          emit loads/stores.  No MIR is mutated during construction; the
///          coalescer simply captures the collaborators it will use later.
/// @param allocator Linear-scan allocator supplying register state.
/// @param spiller Spiller responsible for materialising loads and stores.
Coalescer::Coalescer(LinearScanAllocator &allocator, Spiller &spiller)
    : allocator_(allocator), spiller_(spiller) {}

/// @brief Expand a @c PX_COPY pseudo into executable machine instructions.
/// @details The algorithm proceeds in three phases:
///          1. Analyse the pseudo operands and build @ref CopyTask entries that
///             describe the source and destination for each pair.
///          2. Emit prefix instructions that materialise spilled or unmapped
///             values by requesting temporary registers from the allocator and
///             issuing loads where necessary.
///          3. Consume the copy tasks while respecting dependency cycles,
///             breaking them via scratch registers and ensuring every temporary
///             is released back to the allocator.
///          The resulting instruction stream is appended to @p out in the order
///          it should execute.
/// @param instr @c PX_COPY instruction to lower.
/// @param out Vector receiving the lowered instruction sequence.
/// @throws std::runtime_error If operands are malformed, use reserved scratch
///         registers, disagree in class, or require an unavailable register.
void Coalescer::lower(const MInstr &instr, std::vector<MInstr> &out) {
    std::vector<MInstr> prefix{};
    std::vector<ScratchRelease> scratch{};
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

    std::vector<MInstr> generated{};
    generated.reserve(tasks.size());

    while (!tasks.empty()) {
        bool progress = false;
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            const auto task = tasks[i];
            const CopyLocation dst = destLocation(task);
            const CopyLocation src = sourceLocation(task);
            bool locDestIsSource = false;
            for (std::size_t j = 0; j < tasks.size(); ++j) {
                if (i == j) {
                    continue;
                }
                if (sourceMatchesLocation(tasks[j], dst)) {
                    locDestIsSource = true;
                    break;
                }
            }

            const bool canEmit = !locDestIsSource || sameLocation(dst, src);

            if (!canEmit) {
                continue;
            }

            if (!sameLocation(dst, src))
                emitCopyTask(task, generated);
            tasks.erase(tasks.begin() + static_cast<long>(i));
            progress = true;
            break;
        }

        if (progress) {
            continue;
        }

        /// @brief Prefers a register-to-register edge as the cycle-breaking seed.
        auto it = std::find_if(tasks.begin(), tasks.end(), [](const CopyTask &t) {
            return t.destKind == CopyTask::DestKind::Reg && t.src.kind == CopySource::Kind::Reg;
        });
        if (it == tasks.end()) {
            it = tasks.begin();
        }

        CopyTask cycleTask = *it;
        const CopyLocation savedSource = sourceLocation(cycleTask);

        std::vector<MInstr> tmpPrefix{};
        PhysReg temp = PhysReg::R10;
        if (cycleTask.cls == RegClass::XMM) {
            temp = allocator_.takeRegister(cycleTask.cls, tmpPrefix);
            scratch.push_back(ScratchRelease{temp, cycleTask.cls});
        }
        for (auto &pre : tmpPrefix) {
            generated.push_back(std::move(pre));
        }
        if (savedSource.kind == CopyLocation::Kind::Reg) {
            generated.push_back(allocator_.makeMove(cycleTask.cls, temp, savedSource.reg));
        } else {
            generated.push_back(
                spiller_.makeLoad(cycleTask.cls, temp, SpillPlan{true, savedSource.slot}));
        }
        for (auto &pending : tasks) {
            if (sourceMatchesLocation(pending, savedSource)) {
                pending.src.kind = CopySource::Kind::Reg;
                pending.src.reg = temp;
            }
        }
    }

    for (auto &instrOut : generated) {
        out.push_back(std::move(instrOut));
    }

    for (const auto &rel : scratch) {
        allocator_.releaseRegister(rel.phys, rel.cls);
    }
}

/// @brief Materialise one lowered copy task.
/// @details Depending on whether the destination resides in memory or a
///          register, the helper either emits direct moves or synthesises loads
///          into scratch registers before performing the store.  Memory-to-memory
///          copies are handled by loading into a temporary first so the target
///          architecture never observes illegal instructions.  Any temporaries
///          acquired during the call are released once their final use is
///          emitted.
/// @param task Copy description built during @ref lower.
/// @param generated Output buffer receiving the materialised instructions.
/// @throws std::runtime_error If an XMM memory-to-memory copy cannot borrow a register.
void Coalescer::emitCopyTask(const CopyTask &task, std::vector<MInstr> &generated) {
    if (task.destKind == CopyTask::DestKind::Mem) {
        if (task.src.kind == CopySource::Kind::Reg) {
            generated.push_back(
                spiller_.makeStore(task.cls, SpillPlan{true, task.destSlot}, task.src.reg));
        } else {
            std::vector<MInstr> tmpPrefix{};
            PhysReg tmp = PhysReg::R11;
            const bool borrowed = task.cls == RegClass::XMM;
            if (borrowed) {
                tmp = allocator_.takeRegister(task.cls, tmpPrefix);
            }
            for (auto &pre : tmpPrefix) {
                generated.push_back(std::move(pre));
            }
            generated.push_back(spiller_.makeLoad(task.cls, tmp, SpillPlan{true, task.src.slot}));
            generated.push_back(spiller_.makeStore(task.cls, SpillPlan{true, task.destSlot}, tmp));
            if (borrowed) {
                allocator_.releaseRegister(tmp, task.cls);
            }
        }
        return;
    }

    if (task.src.kind == CopySource::Kind::Reg) {
        generated.push_back(allocator_.makeMove(task.cls, task.destReg, task.src.reg));
    } else {
        generated.push_back(
            spiller_.makeLoad(task.cls, task.destReg, SpillPlan{true, task.src.slot}));
    }
}

} // namespace zanna::codegen::x64::ra
