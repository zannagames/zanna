//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ra/ParallelCopy.hpp
// Purpose: Backend-neutral sequentialization of a parallel copy: turns a set
//          of simultaneous `dst <- src` assignments over registers and frame
//          slots into an ordered move sequence that never overwrites a value
//          before its last read, breaking permutation cycles through a
//          scratch location the caller provides.
// Key invariants:
//   - Every task's destination is written exactly once and only after every
//     pending task that reads it has been emitted; identity tasks emit nothing.
//   - A cycle is broken by copying one pending source into a scratch location
//     and retargeting every reader of that source to the scratch; the seed
//     prefers a register-to-register task so the scratch copy is a plain move.
//   - Memory-to-memory copies go through a temporary register the emitter
//     supplies, so the emitter only ever sees reg<-reg, reg<-mem, mem<-reg.
//   - Emission order is a deterministic function of the task order.
// Ownership/Lifetime:
//   - Header-only; the emitter object owns every instruction it produces.
// Links: src/codegen/x86_64/ra/Coalescer.cpp (PX_COPY lowering),
//        src/codegen/aarch64 (ParallelCopy pseudo lowering, Phase 3),
//        docs/internals/backend-codegen-review-2026-09.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

/// @file
/// @brief Declares the shared parallel-copy sequentializer.

namespace zanna::codegen::ra {

/// @brief A copy endpoint: a physical register or a frame slot of one class.
struct CopyLoc {
    /// @brief Endpoint kind.
    enum class Kind : std::uint8_t { Reg, Mem };

    Kind kind{Kind::Reg};
    unsigned cls{0}; ///< Backend register class tag (opaque here).
    unsigned reg{0}; ///< Physical register ordinal when `kind == Reg`.
    int slot{0};     ///< Frame slot key or offset when `kind == Mem`.

    [[nodiscard]] static CopyLoc regLoc(unsigned cls, unsigned reg) noexcept {
        CopyLoc loc;
        loc.kind = Kind::Reg;
        loc.cls = cls;
        loc.reg = reg;
        return loc;
    }

    [[nodiscard]] static CopyLoc memLoc(unsigned cls, int slot) noexcept {
        CopyLoc loc;
        loc.kind = Kind::Mem;
        loc.cls = cls;
        loc.slot = slot;
        return loc;
    }

    [[nodiscard]] bool isReg() const noexcept {
        return kind == Kind::Reg;
    }

    /// @brief Endpoint identity.
    /// @details A REGISTER is identified by its class and ordinal: GPR 0 and
    ///          FPR 0 are different registers that share an ordinal. A FRAME
    ///          SLOT is identified by its slot ALONE — a slot is untyped
    ///          storage, and a backend whose spill slots come from one pool
    ///          shared by every class (AArch64: "every class shares one
    ///          8-byte slot pool") will hand the same slot to a GPR value and
    ///          an FPR value whose live ranges do not intersect. Comparing
    ///          the class as well made those two endpoints look like two
    ///          different locations, so the sequentializer below could not
    ///          see that one task's destination was another task's source and
    ///          emitted the write first: the FPR store landed on the slot
    ///          before the GPR read of it, and the reader got the other
    ///          value's bits. (ZB-46: a Float's bit pattern surfaced in an
    ///          Integer at -O1.)
    [[nodiscard]] bool operator==(const CopyLoc &other) const noexcept {
        if (kind != other.kind)
            return false;
        return kind == Kind::Reg ? (cls == other.cls && reg == other.reg) : slot == other.slot;
    }

    [[nodiscard]] bool operator!=(const CopyLoc &other) const noexcept {
        return !(*this == other);
    }
};

/// @brief One simultaneous assignment `dst <- src`.
struct ParallelCopyTask {
    CopyLoc dst;
    CopyLoc src;
};

/// @brief The emitter interface `sequentializeParallelCopy` drives.
/// @details Documentation only; any type with these members works:
/// @code
///   void    move(const CopyLoc &dst, const CopyLoc &src); // reg<-reg, reg<-mem, mem<-reg
///   CopyLoc cycleScratch(unsigned cls);                   // register to hold a cycle value
///   CopyLoc memTemp(unsigned cls);                        // register for a mem<-mem copy
///   void    releaseScratch(const CopyLoc &loc);           // called once per scratch/temp
/// @endcode
struct ParallelCopyEmitterDocumentation {
    ParallelCopyEmitterDocumentation() = delete;
};

/// @brief Emit @p tasks in an order that preserves parallel-copy semantics.
/// @details Algorithm: repeatedly emit any task whose destination is read by no
///          other pending task (skipping identities); when none exists the
///          pending tasks form cycles, so copy the seed's source into a scratch
///          from `cycleScratch`, retarget every pending reader of that source
///          (the seed included) to the scratch, and continue. A scratch is
///          released as soon as no pending task reads it.
/// @param tasks Assignments; every task's classes must agree and destinations
///              must be distinct. Modified in place while pending.
/// @param emit Emitter (see ParallelCopyEmitterDocumentation).
/// @return Number of moves requested from the emitter.
/// @throws std::runtime_error on a class mismatch or a duplicate destination
///         (both are lowering bugs; callers surface them as backend errors).
template <class Emitter>
std::size_t sequentializeParallelCopy(std::vector<ParallelCopyTask> tasks, Emitter &emit) {
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (tasks[i].dst.cls != tasks[i].src.cls)
            throw std::runtime_error("parallel copy: source and destination classes differ");
        for (std::size_t j = i + 1; j < tasks.size(); ++j) {
            if (tasks[i].dst == tasks[j].dst)
                throw std::runtime_error("parallel copy: destination written twice");
        }
    }

    // Identity tasks carry no work, but their destination still counts as
    // "written" by the copy, so nothing else may target it (checked above).
    for (std::size_t i = 0; i < tasks.size();) {
        if (tasks[i].dst == tasks[i].src)
            tasks.erase(tasks.begin() + static_cast<std::ptrdiff_t>(i));
        else
            ++i;
    }

    std::vector<CopyLoc> scratches;
    std::size_t moves = 0;

    /// Whether any pending task reads @p loc.
    const auto isRead = [&tasks](const CopyLoc &loc) {
        for (const auto &t : tasks)
            if (t.src == loc)
                return true;
        return false;
    };

    /// Emit one task, routing mem<-mem through a temporary register.
    const auto emitTask = [&](const ParallelCopyTask &t) {
        if (!t.dst.isReg() && !t.src.isReg()) {
            const CopyLoc tmp = emit.memTemp(t.dst.cls);
            emit.move(tmp, t.src);
            emit.move(t.dst, tmp);
            emit.releaseScratch(tmp);
            moves += 2;
            return;
        }
        emit.move(t.dst, t.src);
        ++moves;
    };

    /// Release every scratch nothing pending reads any more.
    const auto releaseDeadScratches = [&]() {
        for (std::size_t i = 0; i < scratches.size();) {
            if (!isRead(scratches[i])) {
                emit.releaseScratch(scratches[i]);
                scratches.erase(scratches.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    };

    while (!tasks.empty()) {
        bool progress = false;
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            const CopyLoc dst = tasks[i].dst;
            bool blocked = false;
            for (std::size_t j = 0; j < tasks.size(); ++j) {
                if (j != i && tasks[j].src == dst) {
                    blocked = true;
                    break;
                }
            }
            if (blocked)
                continue;
            const ParallelCopyTask task = tasks[i];
            tasks.erase(tasks.begin() + static_cast<std::ptrdiff_t>(i));
            emitTask(task);
            releaseDeadScratches();
            progress = true;
            break;
        }
        if (progress)
            continue;

        // Every pending destination is still read: break a cycle. Prefer a
        // register-to-register seed so the scratch copy is a plain move.
        std::size_t seed = 0;
        for (std::size_t i = 0; i < tasks.size(); ++i) {
            if (tasks[i].dst.isReg() && tasks[i].src.isReg()) {
                seed = i;
                break;
            }
        }
        const CopyLoc source = tasks[seed].src;
        // A frame slot is untyped storage, so the pending readers of a MEMORY
        // source may not all belong to one class. Each class gets its own
        // scratch, read from the slot with that class's load: retargeting a
        // GPR task on to an FPR scratch would move the bits with the wrong
        // instruction. A register source is class-qualified by its identity,
        // so it can only ever have one class of reader and this loop runs
        // once.
        std::vector<unsigned> readerClasses;
        for (const auto &pending : tasks) {
            if (pending.src != source)
                continue;
            bool seen = false;
            for (unsigned c : readerClasses)
                if (c == pending.dst.cls)
                    seen = true;
            if (!seen)
                readerClasses.push_back(pending.dst.cls);
        }
        for (unsigned cls : readerClasses) {
            const CopyLoc scratch = emit.cycleScratch(cls);
            emit.move(scratch, source);
            ++moves;
            scratches.push_back(scratch);
            for (auto &pending : tasks) {
                // Already-retargeted tasks no longer compare equal to the
                // original source, so each class is rewritten exactly once.
                if (pending.src == source && pending.dst.cls == cls)
                    pending.src = scratch;
            }
        }
    }

    releaseDeadScratches();
    return moves;
}

} // namespace zanna::codegen::ra
