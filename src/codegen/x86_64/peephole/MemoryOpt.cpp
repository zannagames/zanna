//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/peephole/MemoryOpt.cpp
// Purpose: x86-64 memory access peephole optimizations: dead frame store
//          elimination and store-to-load forwarding.
// Key invariants:
//   - Only frame-relative (RBP-based) memory accesses are considered.
//   - Control-flow boundaries and unknown memory accesses flush tracking state.
// Ownership/Lifetime:
//   - Stateless; all state is owned by the caller.
// Links: src/codegen/x86_64/peephole/MemoryOpt.hpp,
//        src/codegen/x86_64/peephole/PeepholeCommon.hpp,
//        src/codegen/x86_64/OperandRoles.hpp
//
//===----------------------------------------------------------------------===//

#include "MemoryOpt.hpp"

#include "codegen/x86_64/OperandRoles.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <unordered_map>
#include <vector>

/// @file
/// @brief Implements conservative frame-slot store tracking within basic blocks.

namespace zanna::codegen::x64::peephole {
namespace {

/// @brief Description of a frame-relative memory access used by the optimiser.
struct FrameAccess {
    int32_t disp{0};             ///< Signed displacement from %rbp.
    RegClass cls{RegClass::GPR}; ///< Whether the access uses GPR or XMM.
};

/// @brief Decode @p instr as a load from a %rbp-relative frame slot, if applicable.
/// @details Recognises @c MOVmr (GPR load) and @c MOVSDmr (XMM load) with a
///          pure base+disp memory operand whose base is the architectural
///          frame pointer. Anything more exotic (indexed, RIP-relative,
///          RSP-relative) is rejected to keep the analysis conservative.
/// @param instr Candidate machine instruction.
/// @return The frame slot descriptor on success, @c std::nullopt otherwise.
std::optional<FrameAccess> frameLoad(const MInstr &instr) {
    if (instr.opcode != MOpcode::MOVmr && instr.opcode != MOpcode::MOVSDmr)
        return std::nullopt;
    if (instr.operands.size() < 2)
        return std::nullopt;
    const auto *mem = std::get_if<OpMem>(&instr.operands[1]);
    if (!mem || mem->hasIndex || !mem->base.isPhys ||
        static_cast<PhysReg>(mem->base.idOrPhys) != PhysReg::RBP)
        return std::nullopt;
    return FrameAccess{mem->disp, instr.opcode == MOpcode::MOVSDmr ? RegClass::XMM : RegClass::GPR};
}

/// @brief Companion to @ref frameLoad — decodes a frame-slot store.
/// @details Recognises @c MOVrm / @c MOVSDrm into a pure base+disp address
///          whose base is %rbp.
/// @param instr Candidate machine instruction.
/// @return Frame displacement and register class, or @c std::nullopt.
std::optional<FrameAccess> frameStore(const MInstr &instr) {
    if (instr.opcode != MOpcode::MOVrm && instr.opcode != MOpcode::MOVSDrm)
        return std::nullopt;
    if (instr.operands.size() < 2)
        return std::nullopt;
    const auto *mem = std::get_if<OpMem>(&instr.operands[0]);
    if (!mem || mem->hasIndex || !mem->base.isPhys ||
        static_cast<PhysReg>(mem->base.idOrPhys) != PhysReg::RBP)
        return std::nullopt;
    return FrameAccess{mem->disp, instr.opcode == MOpcode::MOVSDrm ? RegClass::XMM : RegClass::GPR};
}

/// @brief Predicate: does @p instr re-define the physical register in @p regOperand?
/// @details Used by the store-forwarding logic to detect when the source
///          register of a recorded store has been clobbered before the
///          subsequent load can reuse it.
/// @param instr Machine instruction whose defining operand roles are inspected.
/// @param regOperand Previously stored physical-register operand.
/// @return @c true when @p instr defines that same register and class.
bool definesOperandReg(const MInstr &instr, const Operand &regOperand) {
    const auto *reg = std::get_if<OpReg>(&regOperand);
    if (!reg || !reg->isPhys)
        return false;
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (!isDef)
            continue;
        const auto *def = std::get_if<OpReg>(&instr.operands[idx]);
        if (def && def->isPhys && def->cls == reg->cls && def->idOrPhys == reg->idOrPhys)
            return true;
    }
    return false;
}

/// @brief Predicate: must we discard tracked frame state before @p instr?
/// @details Returns true for control-flow boundaries (CALL/JMP/JCC/RET/
///          LABEL/UD2) and for any memory access that is not a known
///          frame load or frame store. The conservative treatment of
///          unknown memory ops keeps optimisations sound in the presence
///          of aliasing.
/// @param instr Machine instruction to classify.
/// @return @c true when tracked store state must be cleared before continuing.
bool isMemoryBarrier(const MInstr &instr) {
    if (instr.opcode == MOpcode::CALL || instr.opcode == MOpcode::JMP ||
        instr.opcode == MOpcode::JCC || instr.opcode == MOpcode::RET ||
        instr.opcode == MOpcode::LABEL || instr.opcode == MOpcode::UD2)
        return true;

    // LEA only computes an address: its OpMem operand never touches memory.
    // If the computed address is later dereferenced, that dereference is a
    // separate instruction with its own memory operand and acts as the
    // barrier, so tracking can safely continue across the LEA itself.
    if (instr.opcode == MOpcode::LEA)
        return false;

    const bool knownFrameLoad = frameLoad(instr).has_value();
    const bool knownFrameStore = frameStore(instr).has_value();
    if (knownFrameLoad || knownFrameStore)
        return false;

    for (const auto &op : instr.operands) {
        if (std::holds_alternative<OpMem>(op) || std::holds_alternative<OpRipLabel>(op))
            return true;
    }
    return false;
}

/// @brief Value available for forwarding from a frame store.
struct TrackedFrameStore {
    /// @brief Register operand whose current value equals the tracked slot.
    Operand storedReg;
};

/// @brief Per-register-class frame forwarding state, keyed by RBP displacement.
using FrameStoreMap = std::unordered_map<int32_t, TrackedFrameStore>;

/// @brief Selects the forwarding map for a register class.
/// @param cls Class of the frame access.
/// @param gprStores GPR-valued slot state.
/// @param xmmStores XMM-valued slot state.
/// @return Mutable map corresponding to @p cls.
FrameStoreMap &trackedMapFor(RegClass cls,
                             FrameStoreMap &gprStores,
                             FrameStoreMap &xmmStores) noexcept {
    return cls == RegClass::XMM ? xmmStores : gprStores;
}

/// @brief Forgets both class interpretations of one frame displacement.
/// @param disp RBP-relative displacement being overwritten.
/// @param gprStores GPR-valued slot state.
/// @param xmmStores XMM-valued slot state.
void eraseTrackedAddress(int32_t disp, FrameStoreMap &gprStores, FrameStoreMap &xmmStores) {
    gprStores.erase(disp);
    xmmStores.erase(disp);
}

/// @brief Forgets stores whose source register is redefined by an instruction.
/// @param instr Instruction whose definitions are inspected.
/// @param stores Forwarding state filtered in place.
void eraseStoresClobberedBy(const MInstr &instr, FrameStoreMap &stores) {
    for (auto it = stores.begin(); it != stores.end();) {
        if (definesOperandReg(instr, it->second.storedReg))
            it = stores.erase(it);
        else
            ++it;
    }
}

} // namespace

/// @brief Drop frame stores that are overwritten before any load reads them.
/// @details Walks the block forward tracking, per (slot, class), the index
///          of the most-recent unread store. When a fresh store hits the
///          same address, the prior store is marked dead. Loads, memory
///          barriers, and aliasing between GPR/XMM slot writes clear the
///          tracker so we never delete a live value.
/// @param instrs Block instructions, mutated in place.
/// @param stats Pass-wide statistics accumulator.
/// @return Number of dead stores eliminated.
std::size_t eliminateDeadFrameStores(std::vector<MInstr> &instrs, PeepholeStats &stats) {
    std::unordered_map<int32_t, std::size_t> lastGprStore;
    std::unordered_map<int32_t, std::size_t> lastXmmStore;
    std::vector<bool> remove(instrs.size(), false);

    /// @brief Selects the unread-store index map for @p cls.
    /// @param cls Register class stored in the slot.
    /// @return Mutable GPR or XMM tracking map.
    auto mapFor = [&](RegClass cls) -> std::unordered_map<int32_t, std::size_t> & {
        return cls == RegClass::XMM ? lastXmmStore : lastGprStore;
    };

    std::size_t removed = 0;
    for (std::size_t i = 0; i < instrs.size(); ++i) {
        if (auto store = frameStore(instrs[i])) {
            auto &stores = mapFor(store->cls);
            auto it = stores.find(store->disp);
            if (it != stores.end() && !remove[it->second]) {
                remove[it->second] = true;
                ++removed;
            }
            auto &otherStores = mapFor(store->cls == RegClass::XMM ? RegClass::GPR : RegClass::XMM);
            auto otherIt = otherStores.find(store->disp);
            if (otherIt != otherStores.end() && !remove[otherIt->second]) {
                remove[otherIt->second] = true;
                ++removed;
            }
            otherStores.erase(store->disp);
            stores[store->disp] = i;
            continue;
        }

        if (auto load = frameLoad(instrs[i])) {
            lastGprStore.erase(load->disp);
            lastXmmStore.erase(load->disp);
            continue;
        }

        if (isMemoryBarrier(instrs[i])) {
            lastGprStore.clear();
            lastXmmStore.clear();
        }
    }

    if (removed != 0) {
        removeMarkedInstructions(instrs, remove);
        stats.deadCodeEliminated += removed;
    }
    return removed;
}

/// @brief Replace subsequent loads of a frame slot with the stored register.
/// @details Tracks the most recent store for each displacement and register
///          class. A matching load is replaced by a register-to-register move
///          while the stored source remains unclobbered. Barriers discard all
///          state; later stores invalidate both class interpretations of the
///          overlapping address.
/// @param instrs Block instructions, mutated in place.
/// @param stats Pass-wide statistics accumulator.
/// @return Number of memory loads rewritten through forwarding.
std::size_t forwardFrameStoreLoads(std::vector<MInstr> &instrs, PeepholeStats &stats) {
    std::size_t forwarded = 0;
    FrameStoreMap lastGprStore;
    FrameStoreMap lastXmmStore;

    for (std::size_t i = 0; i < instrs.size(); ++i) {
        auto &instr = instrs[i];

        if (isMemoryBarrier(instr)) {
            lastGprStore.clear();
            lastXmmStore.clear();
            continue;
        }

        eraseStoresClobberedBy(instr, lastGprStore);
        eraseStoresClobberedBy(instr, lastXmmStore);

        if (auto load = frameLoad(instr)) {
            auto &stores = trackedMapFor(load->cls, lastGprStore, lastXmmStore);
            auto it = stores.find(load->disp);
            if (it == stores.end() || instr.operands.empty())
                continue;

            const MOpcode mov = load->cls == RegClass::XMM ? MOpcode::MOVSDrr : MOpcode::MOVrr;
            instr = MInstr::make(mov, {instr.operands[0], it->second.storedReg});
            ++forwarded;
            continue;
        }

        auto store = frameStore(instr);
        if (!store || instr.operands.size() < 2)
            continue;

        eraseTrackedAddress(store->disp, lastGprStore, lastXmmStore);
        auto &stores = trackedMapFor(store->cls, lastGprStore, lastXmmStore);
        stores[store->disp] = TrackedFrameStore{instr.operands[1]};
    }

    stats.deadCodeEliminated += forwarded;
    return forwarded;
}

} // namespace zanna::codegen::x64::peephole
