//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/SchedulerPass.cpp
// Purpose: Post-RA critical-path list scheduler for the AArch64 modular pipeline.
//          Each basic block is partitioned into segments at call/guard-branch
//          boundaries; each segment is scheduled independently using a DAG with
//          Apple M1 latencies (loads 4, mul 3, fdiv 10, all else 1).
// Key invariants:
//   - Memory dependency analysis is precise for FP/SP-relative addresses and
//     conservative (may-alias) for base-register accesses.
//   - The reordered block is a permutation — no instructions are added/removed.
//   - Terminators always remain at the end of their block in original order.
//   - Calls act as full memory barriers and clobber all caller-saved registers.
// Ownership/Lifetime:
//   - Stateless pass; mutates MFunction::blocks in place.
// Links: codegen/aarch64/passes/SchedulerPass.hpp,
//        codegen/aarch64/ra/OperandRoles.hpp (def/use role queries)
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/SchedulerPass.hpp"

#include "codegen/aarch64/InstrEffects.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "codegen/aarch64/ra/OperandRoles.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <queue>
#include <vector>

/// @file
/// @brief Implements bounded critical-path list scheduling for allocated AArch64 MIR.

namespace zanna::codegen::aarch64::passes {

namespace {

// ---------------------------------------------------------------------------
// Latency model
// ---------------------------------------------------------------------------

/// @brief Return the output latency (cycles from write to use) for an opcode.
///
/// Latency model tuned for Apple M1/M2/M3 Firestorm (performance) cores.
/// Values are approximate; actual latencies vary by microarchitecture revision
/// and operand width but are close enough for effective scheduling.
///
/// @param opc Machine opcode whose result latency is requested.
/// @return Approximate producer-to-consumer latency in cycles. Unlisted
///         instructions use the conservative one-cycle default.
static unsigned instrLatency(MOpcode opc) noexcept {
    switch (opc) {
        // Loads: L1 hit ~4 cycles on Apple M-series.
        case MOpcode::LdrRegFpImm:
        case MOpcode::LdrRegBaseImm:
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::LdpRegFpImm:
        case MOpcode::LdpFprFpImm:
            return 4;

        // Integer multiply + fused multiply-add/sub: 3 cycles.
        case MOpcode::MulRRR:
        case MOpcode::MSubRRRR:
        case MOpcode::MAddRRRR:
            return 3;

        // Integer divide: 7-12 cycles on M1; model as 7 (optimistic).
        case MOpcode::SDivRRR:
        case MOpcode::UDivRRR:
            return 7;

        // FP add/sub: 3 cycles.
        case MOpcode::FAddRRR:
        case MOpcode::FSubRRR:
            return 3;

        // FP multiply: 4 cycles on M1.
        case MOpcode::FMulRRR:
            return 4;

        // FP divide: 10-15 cycles on M1 (double precision); model as 10.
        case MOpcode::FDivRRR:
            return 10;

        // Int/FP conversions: 3-4 cycles.
        case MOpcode::SCvtF:
        case MOpcode::FCvtZS:
        case MOpcode::UCvtF:
        case MOpcode::FCvtZU:
            return 4;

        // All other instructions: 1 cycle.
        default:
            return 1;
    }
}

// ---------------------------------------------------------------------------
// Memory classification helpers
// ---------------------------------------------------------------------------

/// @brief Classifies the base register of a tracked memory address.
enum class MemBaseKind : uint8_t {
    FramePointer, ///< FP-relative (alloca / spill slots).
    StackPointer, ///< SP-relative (outgoing arg area).
    BaseRegister, ///< Arbitrary base register (heap / object fields; may-alias).
};

/// @brief Symbolic representation of a trackable base-plus-offset address.
struct AddressValue {
    MemBaseKind baseKind{MemBaseKind::BaseRegister};
    long long baseTag{0}; ///< Distinguishes different tracked base registers (unused for FP/SP).
    long long offset{0};  ///< Byte offset from the base.
};

/// @brief Resolved memory access region used for alias analysis.
struct MemoryAccessClass {
    MemBaseKind baseKind{MemBaseKind::FramePointer};
    long long baseTag{0};
    long long offset{0}; ///< Byte offset from base.
    unsigned size{0};    ///< Access size in bytes.
};

/// @brief One entry in the per-block memory history used for dependency building.
struct MemoryHistoryEntry {
    std::size_t instrIdx{0};                      ///< Index into the body instruction array.
    bool isLoad{false};                           ///< True if this entry is a load.
    bool isStore{false};                          ///< True if this entry is a store.
    bool isBarrier{false};                        ///< True if this entry is a call (full barrier).
    std::optional<MemoryAccessClass> accessClass; ///< Resolved address region, if known.
};

/// Total number of AArch64 physical registers (X0..X30, SP, V0..V31 = 64).
static constexpr std::size_t kNumPhysRegs = 64;

/// Flat-array indices for implicit "virtual" registers beyond the physical file.
static constexpr std::size_t kIdxNZCV = kNumPhysRegs;   // 64
static constexpr std::size_t kIdxSP = kNumPhysRegs + 1; // 65

/// Total tracked register slots: 64 physical + NZCV + SP.
static constexpr std::size_t kNumTracked = kNumPhysRegs + 2;

/// Upper bound for the quadratic dependency graph built per schedulable span.
/// Very large straight-line regions are left in source order to avoid compile-time cliffs.
static constexpr std::size_t kMaxScheduledSegmentInstructions = 256;

/// Upper bound for scheduling an entire function. Extremely large generated
/// functions carry dense post-RA spill traffic across hundreds of guard
/// segments; retaining their allocation order avoids invalid cross-segment
/// register reuse while keeping the scheduler active on normal functions.
static constexpr std::size_t kMaxScheduledFunctionInstructions = 1024;

/// @brief Map an AArch64 physical-register identifier to its tracking-array slot.
/// @param reg Numeric `PhysReg` value. Valid physical values occupy `[0, 63]`.
/// @return Zero-based slot corresponding to @p reg.
/// @note The caller must check the returned index before indexing a fixed-size
///       physical-register array.
static std::size_t regIdx(uint32_t reg) noexcept {
    // PhysReg enum values are 0..63, which map to themselves.
    return static_cast<std::size_t>(reg);
}

/// @brief Resolve an instruction's byte range relative to a symbolic base.
/// @param base Tracked base address and any offset accumulated while deriving it.
/// @param accessOffset Additional byte offset encoded by the memory instruction.
/// @param size Width of the memory access in bytes.
/// @return Resolved base identity and half-open byte range information.
static std::optional<MemoryAccessClass> makeResolvedAccessClass(const AddressValue &base,
                                                                long long accessOffset,
                                                                unsigned size) noexcept {
    return MemoryAccessClass{base.baseKind, base.baseTag, base.offset + accessOffset, size};
}

/// @brief Look up the symbolic address currently held by a physical register.
/// @param tracked Per-register address-provenance table.
/// @param reg Numeric physical-register identifier to query.
/// @return The tracked address when @p reg is in range and has known
///         provenance; `std::nullopt` otherwise.
static std::optional<AddressValue> getTrackedAddressValue(
    const std::array<std::optional<AddressValue>, kNumPhysRegs> &tracked, uint32_t reg) noexcept {
    const std::size_t idx = regIdx(reg);
    if (idx >= kNumPhysRegs)
        return std::nullopt;
    return tracked[idx];
}

/// @brief Classify the memory address of a load/store instruction.
///
/// The opcode determines the access width and which operands carry the base and
/// displacement. FP- and SP-relative accesses resolve directly. Base-register
/// accesses resolve only when @p trackedAddrs proves the register was derived
/// from a known address; an untracked heap/object base remains unknown so alias
/// analysis stays conservative.
///
/// @param mi Memory instruction to inspect.
/// @param trackedAddrs Symbolic addresses currently associated with physical GPRs.
/// @return A resolved MemoryAccessClass if the base is FP/SP/tracked-GPR; nullopt otherwise.
static std::optional<MemoryAccessClass> classifyMemoryAccess(
    const MInstr &mi,
    const std::array<std::optional<AddressValue>, kNumPhysRegs> &trackedAddrs) noexcept {
    switch (mi.opc) {
        case MOpcode::LdrRegFpImm:
        case MOpcode::StrRegFpImm:
        case MOpcode::LdrFprFpImm:
        case MOpcode::StrFprFpImm:
            if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Imm)
                return MemoryAccessClass{MemBaseKind::FramePointer, 0, mi.ops[1].imm, 8};
            return std::nullopt;
        case MOpcode::Ldr8RegFpImm:
        case MOpcode::Str8RegFpImm:
            if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Imm)
                return MemoryAccessClass{MemBaseKind::FramePointer, 0, mi.ops[1].imm, 1};
            return std::nullopt;
        case MOpcode::Ldr16RegFpImm:
        case MOpcode::Str16RegFpImm:
            if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Imm)
                return MemoryAccessClass{MemBaseKind::FramePointer, 0, mi.ops[1].imm, 2};
            return std::nullopt;
        case MOpcode::Ldr32RegFpImm:
        case MOpcode::Str32RegFpImm:
            if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Imm)
                return MemoryAccessClass{MemBaseKind::FramePointer, 0, mi.ops[1].imm, 4};
            return std::nullopt;
        case MOpcode::LdpRegFpImm:
        case MOpcode::StpRegFpImm:
        case MOpcode::LdpFprFpImm:
        case MOpcode::StpFprFpImm:
            if (mi.ops.size() >= 3 && mi.ops[2].kind == MOperand::Kind::Imm)
                return MemoryAccessClass{MemBaseKind::FramePointer, 0, mi.ops[2].imm, 16};
            return std::nullopt;
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
            if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Imm)
                return MemoryAccessClass{MemBaseKind::StackPointer, 0, mi.ops[1].imm, 8};
            return std::nullopt;
        case MOpcode::LdrRegBaseImm:
        case MOpcode::StrRegBaseImm:
        case MOpcode::LdrFprBaseImm:
        case MOpcode::StrFprBaseImm:
            if (mi.ops.size() >= 3 && mi.ops[1].kind == MOperand::Kind::Reg &&
                mi.ops[1].reg.isPhys && mi.ops[2].kind == MOperand::Kind::Imm) {
                if (const auto tracked =
                        getTrackedAddressValue(trackedAddrs, mi.ops[1].reg.idOrPhys)) {
                    return makeResolvedAccessClass(*tracked, mi.ops[2].imm, 8);
                }
                // Unknown base-register accesses are heap/object accesses for
                // aliasing purposes. Physical register identity is not a
                // provenance guarantee: two registers may hold the same object.
                return std::nullopt;
            }
            return std::nullopt;
        case MOpcode::Ldr8RegBaseImm:
        case MOpcode::Str8RegBaseImm:
        case MOpcode::Ldr16RegBaseImm:
        case MOpcode::Str16RegBaseImm:
        case MOpcode::Ldr32RegBaseImm:
        case MOpcode::Str32RegBaseImm:
            if (mi.ops.size() >= 3 && mi.ops[1].kind == MOperand::Kind::Reg &&
                mi.ops[1].reg.isPhys && mi.ops[2].kind == MOperand::Kind::Imm) {
                const unsigned size =
                    (mi.opc == MOpcode::Ldr8RegBaseImm || mi.opc == MOpcode::Str8RegBaseImm) ? 1
                    : (mi.opc == MOpcode::Ldr16RegBaseImm || mi.opc == MOpcode::Str16RegBaseImm)
                        ? 2
                        : 4;
                if (const auto tracked =
                        getTrackedAddressValue(trackedAddrs, mi.ops[1].reg.idOrPhys)) {
                    return makeResolvedAccessClass(*tracked, mi.ops[2].imm, size);
                }
                return std::nullopt;
            }
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

/// @brief Return true if two memory accesses may overlap.
/// @details Returns true conservatively when either access class is unknown.
///          Two known accesses with different base kinds cannot alias.
///          Two known FP/SP accesses with non-overlapping ranges do not alias.
/// @param lhs First access region, or `std::nullopt` when unresolved.
/// @param rhs Second access region, or `std::nullopt` when unresolved.
/// @return `false` only when the available base and range information proves
///         that the accesses are disjoint; `true` otherwise.
static bool mayAliasMemory(const std::optional<MemoryAccessClass> &lhs,
                           const std::optional<MemoryAccessClass> &rhs) noexcept {
    if (!lhs || !rhs)
        return true;
    if (lhs->baseKind != rhs->baseKind)
        return false;
    if (lhs->baseKind == MemBaseKind::BaseRegister)
        return true;
    const long long lhsEnd = lhs->offset + static_cast<long long>(lhs->size);
    const long long rhsEnd = rhs->offset + static_cast<long long>(rhs->size);
    return !(lhsEnd <= rhs->offset || rhsEnd <= lhs->offset);
}

/// @brief Derive a symbolic AddressValue for the destination of @p mi if trackable.
/// @details Handles MovRR (copy), AddRI/SubRI (offset arithmetic), and AddFpImm.
///          Returns nullopt for instructions whose result cannot be statically analysed.
/// @param mi Instruction whose destination address is being derived.
/// @param trackedAddrs Symbolic input-register addresses available before @p mi.
/// @return The symbolic destination address, or `std::nullopt` when the opcode,
///         operands, or source provenance cannot be tracked.
static std::optional<AddressValue> deriveTrackedAddressValue(
    const MInstr &mi,
    const std::array<std::optional<AddressValue>, kNumPhysRegs> &trackedAddrs) noexcept {
    /// @brief Returns the tracked address of a physical-register operand.
    /// @param opIdx Operand index to inspect.
    /// @return Symbolic address, or `std::nullopt` when the operand is not tracked.
    auto regTracked = [&](std::size_t opIdx) -> std::optional<AddressValue> {
        if (opIdx >= mi.ops.size() || mi.ops[opIdx].kind != MOperand::Kind::Reg ||
            !mi.ops[opIdx].reg.isPhys) {
            return std::nullopt;
        }
        return getTrackedAddressValue(trackedAddrs, mi.ops[opIdx].reg.idOrPhys);
    };

    switch (mi.opc) {
        case MOpcode::MovRR: {
            return regTracked(1);
        }
        case MOpcode::AddRI:
        case MOpcode::SubRI:
        case MOpcode::AddsRI:
        case MOpcode::SubsRI: {
            if (mi.ops.size() < 3 || mi.ops[2].kind != MOperand::Kind::Imm)
                return std::nullopt;
            auto base = regTracked(1);
            if (!base)
                return std::nullopt;
            const long long delta = (mi.opc == MOpcode::SubRI || mi.opc == MOpcode::SubsRI)
                                        ? -mi.ops[2].imm
                                        : mi.ops[2].imm;
            base->offset += delta;
            return base;
        }
        case MOpcode::AddFpImm:
            if (mi.ops.size() >= 2 && mi.ops[1].kind == MOperand::Kind::Imm)
                return AddressValue{MemBaseKind::FramePointer, 0, mi.ops[1].imm};
            return std::nullopt;
        default:
            return std::nullopt;
    }
}

/// @brief Test whether an opcode is treated as a basic-block terminator.
/// @param opc Machine opcode to classify.
/// @return `true` for returns, direct or conditional branches, test branches,
///         compare-and-branches, and jump tables.
static bool isTerminator(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::Ret:
        case MOpcode::Br:
        case MOpcode::BCond:
        case MOpcode::Cbz:
        case MOpcode::Cbnz:
        case MOpcode::Tbz:
        case MOpcode::Tbnz:
        case MOpcode::JumpTable:
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Flag (NZCV) classification helpers
// ---------------------------------------------------------------------------


// ---------------------------------------------------------------------------
// Stack pointer helpers
// ---------------------------------------------------------------------------

/// @brief Test whether an opcode defines the implicit stack-pointer resource.
/// @param opc Machine opcode to classify.
/// @return `true` for the modeled immediate stack-pointer adjustments.
static bool modifiesSP(MOpcode opc) noexcept {
    return opc == MOpcode::SubSpImm || opc == MOpcode::AddSpImm;
}

/// @brief Returns true if the opcode implicitly reads the stack pointer
///        (SP-relative loads/stores for outgoing arguments).
/// @param opc Machine opcode to classify.
/// @return `true` when @p opc depends on the current stack-pointer value.
static bool usesSP(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::StrRegSpImm:
        case MOpcode::StrFprSpImm:
        case MOpcode::SubSpImm:
        case MOpcode::AddSpImm:
            return true;
        default:
            return false;
    }
}

/// @brief Test whether an opcode is a direct or indirect call.
/// @param opc Machine opcode to classify.
/// @return `true` for `Bl` or `Blr`; `false` otherwise.
static bool isCall(MOpcode opc) noexcept {
    return opc == MOpcode::Bl || opc == MOpcode::Blr;
}

// ---------------------------------------------------------------------------
// Dependency node
// ---------------------------------------------------------------------------

struct DepNode {
    std::size_t instrIdx{0};        ///< Index into the body instruction array.
    std::vector<std::size_t> preds; ///< Predecessor indices (must complete first).
    std::vector<unsigned> predLat;  ///< Latency for each predecessor edge.
    unsigned critPath{0};           ///< Critical-path length from this node to end.
    std::size_t predsDone{0};       ///< Count of predecessors already scheduled.
};

/// @brief Priority queue entry for ready-to-schedule instructions.
/// @details Higher critical-path values are selected first; ties keep source
///          order, which avoids integer narrowing from the old negative-priority
///          encoding and keeps scheduling deterministic.
struct ReadyNode {
    unsigned critPath{0};
    std::size_t instrIdx{0};
};

/// @brief std::priority_queue comparator for @ref ReadyNode.
struct ReadyNodeLess {
    /// @brief Order ready nodes for a max-priority critical-path queue.
    /// @param lhs Left queue entry.
    /// @param rhs Right queue entry.
    /// @return `true` when @p lhs has lower scheduling priority than @p rhs.
    bool operator()(const ReadyNode &lhs, const ReadyNode &rhs) const noexcept {
        if (lhs.critPath != rhs.critPath)
            return lhs.critPath < rhs.critPath;
        return lhs.instrIdx > rhs.instrIdx;
    }
};

// ---------------------------------------------------------------------------
// Block scheduler
// ---------------------------------------------------------------------------

/// @brief Build the dependency graph for a single basic-block body.
/// @details Walks the body in program order, tracking last-def / uses-since-def
///          per tracked register slot, an FP-relative memory history, and the
///          caller-saved register set.  Adds RAW / WAW / WAR / memory / call /
///          flag / SP dependency edges.  Returns deduplicated predecessor lists.
///
///          State invariants:
///            * `lastDef[ri] == kNone` means no instruction has yet defined `ri`.
///            * `usesSinceDef[ri]` contains every reader between the last def
///              and now; cleared on each new definition.
///            * `memoryHistory` lists every memory access plus call barriers in
///              program order; `memoryStoreHistory` is the store-only filter
///              used by load-only RAW edges.
/// @param body Straight-line instruction segment in original program order.
/// @param target Target description supplying caller-saved GPR and FPR sets.
/// @return One dependency node per instruction, with deduplicated predecessor
///         edges and edge latencies.
static std::vector<DepNode> buildDependencyGraph(const std::vector<MInstr> &body,
                                                 const TargetInfo &target) {
    const std::size_t N = body.size();
    std::vector<DepNode> nodes(N);
    for (std::size_t i = 0; i < N; ++i)
        nodes[i].instrIdx = i;

    /// @brief Adds an edge from a predecessor to a consumer.
    /// @param i Consumer instruction index.
    /// @param p Predecessor instruction index.
    /// @param lat Dependency latency.
    auto addDep = [&](std::size_t i, std::size_t p, unsigned lat) {
        if (p != i) {
            nodes[i].preds.push_back(p);
            nodes[i].predLat.push_back(lat);
        }
    };

    constexpr auto kNone = static_cast<std::size_t>(~0ULL);
    std::array<std::size_t, kNumTracked> lastDef;
    lastDef.fill(kNone);
    std::array<std::vector<std::size_t>, kNumTracked> usesSinceDef;
    std::array<std::optional<AddressValue>, kNumPhysRegs> trackedAddrs;
    trackedAddrs.fill(std::nullopt);

    std::vector<MemoryHistoryEntry> memoryHistory;
    memoryHistory.reserve(N);
    std::vector<MemoryHistoryEntry> memoryStoreHistory;
    memoryStoreHistory.reserve(N);

    std::vector<uint32_t> callerSaved;
    callerSaved.reserve(target.callerSavedGPR.size() + target.callerSavedFPR.size());
    for (const PhysReg reg : target.callerSavedGPR)
        callerSaved.push_back(static_cast<uint32_t>(reg));
    for (const PhysReg reg : target.callerSavedFPR)
        callerSaved.push_back(static_cast<uint32_t>(reg));

    // --- Per-dep-type emitters -------------------------------------------------
    /// @brief Adds true dependencies from register uses to their last definitions.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitRegRAW = [&](std::size_t i, const MInstr &mi) {
        for (std::size_t opIdx = 0; opIdx < mi.ops.size(); ++opIdx) {
            const auto roles = ra::operandRoles(mi, opIdx);
            if (!roles.first)
                continue;
            const auto &op = mi.ops[opIdx];
            if (op.kind != MOperand::Kind::Reg || !op.reg.isPhys)
                continue;
            const std::size_t ri = regIdx(op.reg.idOrPhys);
            if (lastDef[ri] != kNone)
                addDep(i, lastDef[ri], instrLatency(body[lastDef[ri]].opc));
            usesSinceDef[ri].push_back(i);
        }
    };

    /// @brief Adds the NZCV definition-to-use edge for a flag consumer.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitFlagUse = [&](std::size_t i, const MInstr &mi) {
        if (readsFlags(mi.opc)) {
            if (lastDef[kIdxNZCV] != kNone)
                addDep(i, lastDef[kIdxNZCV], 1);
            usesSinceDef[kIdxNZCV].push_back(i);
        }
    };

    /// @brief Serializes implicit SP reads and writes while preserving WAR and WAW order.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitStackPointer = [&](std::size_t i, const MInstr &mi) {
        if (usesSP(mi.opc)) {
            if (lastDef[kIdxSP] != kNone)
                addDep(i, lastDef[kIdxSP], 1);
            usesSinceDef[kIdxSP].push_back(i);
        }
        if (modifiesSP(mi.opc)) {
            if (lastDef[kIdxSP] != kNone)
                addDep(i, lastDef[kIdxSP], 1);
            for (auto u : usesSinceDef[kIdxSP])
                addDep(i, u, 1);
            usesSinceDef[kIdxSP].clear();
            lastDef[kIdxSP] = i;
        }
    };

    /// @brief Adds conservative alias dependencies and records a memory access.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitMemoryDeps = [&](std::size_t i, const MInstr &mi) {
        const bool memLoad = isLoadOpcode(mi.opc);
        const bool memStore = isStoreOpcode(mi.opc);
        if (!memLoad && !memStore)
            return;
        const auto memClass = classifyMemoryAccess(mi, trackedAddrs);
        const auto &dependencyHistory = (memLoad && !memStore) ? memoryStoreHistory : memoryHistory;
        for (const auto &prev : dependencyHistory) {
            if (prev.isBarrier) {
                addDep(i, prev.instrIdx, 1);
                continue;
            }
            if (!mayAliasMemory(memClass, prev.accessClass))
                continue;
            if (memLoad && prev.isStore)
                addDep(i, prev.instrIdx, 1);
            if (memStore && (prev.isLoad || prev.isStore))
                addDep(i, prev.instrIdx, 1);
        }
        MemoryHistoryEntry entry{i, memLoad, memStore, false, memClass};
        memoryHistory.push_back(entry);
        if (memStore)
            memoryStoreHistory.push_back(entry);
    };

    /// @brief Adds call dependencies and records a full memory barrier.
    /// @details The call depends on prior memory effects, live caller-saved
    ///          definitions, and the most recent NZCV definition.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitCallBarrier = [&](std::size_t i, const MInstr &mi) {
        if (!isCall(mi.opc))
            return;
        for (const auto &prev : memoryHistory)
            addDep(i, prev.instrIdx, 1);
        for (uint32_t r : callerSaved) {
            const std::size_t ri = regIdx(r);
            if (lastDef[ri] != kNone)
                addDep(i, lastDef[ri], 1);
        }
        if (lastDef[kIdxNZCV] != kNone)
            addDep(i, lastDef[kIdxNZCV], 1);
        MemoryHistoryEntry barrier{i, false, false, true, std::nullopt};
        memoryHistory.push_back(barrier);
        memoryStoreHistory.push_back(barrier);
    };

    /// @brief Adds WAW/WAR edges for explicit definitions and updates register state.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitRegDefs = [&](std::size_t i, const MInstr &mi) {
        for (std::size_t opIdx = 0; opIdx < mi.ops.size(); ++opIdx) {
            const auto [isUse, isDef] = ra::operandRoles(mi, opIdx);
            if (!isDef)
                continue;
            if (mi.ops[opIdx].kind != MOperand::Kind::Reg || !mi.ops[opIdx].reg.isPhys)
                continue;
            const std::size_t ri = regIdx(mi.ops[opIdx].reg.idOrPhys);
            if (lastDef[ri] != kNone)
                addDep(i, lastDef[ri], 1); // WAW
            for (auto u : usesSinceDef[ri])
                addDep(i, u, 1); // WAR
            usesSinceDef[ri].clear();
            lastDef[ri] = i;
            if (ri < kNumPhysRegs && mi.ops[opIdx].reg.cls == RegClass::GPR)
                trackedAddrs[ri] = std::nullopt;
        }
    };

    /// @brief Models every implicit register definition of @p mi as a def.
    /// @details Implicit defs come from the shared effects model: caller-saved
    ///          registers and LR at calls, the jump-table dispatch scratch, and
    ///          (until ExpandPseudosPass) the reserved scratch GPRs the emitter
    ///          writes while materializing wide immediates or large offsets.
    ///          The plan-88 bisect (a throw-backup leg's end time became a
    ///          stack address) was such a scratch write hoisted between two
    ///          instructions that kept a value in x9.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitImplicitDefs = [&](std::size_t i, const MInstr &mi) {
        const InstrEffects fx = effectsOf(mi, target);
        PhysRegSet explicitDefs;
        for (std::size_t opIdx = 0; opIdx < mi.ops.size(); ++opIdx) {
            const auto &op = mi.ops[opIdx];
            if (op.kind == MOperand::Kind::Reg && op.reg.isPhys &&
                ra::operandRoles(mi, opIdx).second)
                explicitDefs.add(static_cast<PhysReg>(op.reg.idOrPhys));
        }
        for (unsigned bit = 0; bit < 64; ++bit) {
            const uint64_t mask = uint64_t{1} << bit;
            if ((fx.defs.bits & mask) == 0 || (explicitDefs.bits & mask) != 0)
                continue;
            const std::size_t ri = bit; // PhysRegSet bit layout == regIdx layout
            if (lastDef[ri] != kNone)
                addDep(i, lastDef[ri], 1); // WAW
            for (auto u : usesSinceDef[ri])
                addDep(i, u, 1); // WAR
            usesSinceDef[ri].clear();
            lastDef[ri] = i;
            if (ri < kNumPhysRegs)
                trackedAddrs[ri] = std::nullopt;
        }
    };

    /// @brief Adds WAW/WAR edges for an NZCV definition and updates flag state.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitFlagDef = [&](std::size_t i, const MInstr &mi) {
        if (!setsFlags(mi.opc))
            return;
        if (lastDef[kIdxNZCV] != kNone)
            addDep(i, lastDef[kIdxNZCV], 1);
        for (auto u : usesSinceDef[kIdxNZCV])
            addDep(i, u, 1);
        usesSinceDef[kIdxNZCV].clear();
        lastDef[kIdxNZCV] = i;
    };

    /// @brief Serializes the NZCV clobber of a call after its register clobbers.
    /// @param i Current instruction index.
    /// @param mi Current instruction.
    auto emitCallFlagClobber = [&](std::size_t i, const MInstr &mi) {
        if (!isCall(mi.opc))
            return;
        if (lastDef[kIdxNZCV] != kNone)
            addDep(i, lastDef[kIdxNZCV], 1);
        for (auto u : usesSinceDef[kIdxNZCV])
            addDep(i, u, 1);
        usesSinceDef[kIdxNZCV].clear();
        lastDef[kIdxNZCV] = i;
    };

    // --- Main loop -------------------------------------------------------------
    for (std::size_t i = 0; i < N; ++i) {
        const MInstr &mi = body[i];

        emitRegRAW(i, mi);
        emitFlagUse(i, mi);
        emitStackPointer(i, mi);
        emitMemoryDeps(i, mi);
        emitCallBarrier(i, mi);
        emitRegDefs(i, mi);
        emitImplicitDefs(i, mi);

        // Track derived address values (e.g., adr+add page-off chains) for the
        // memory alias analysis above.
        if (const auto tracked = deriveTrackedAddressValue(mi, trackedAddrs);
            tracked && !mi.ops.empty() && mi.ops[0].kind == MOperand::Kind::Reg &&
            mi.ops[0].reg.isPhys && mi.ops[0].reg.cls == RegClass::GPR) {
            const std::size_t ri = regIdx(mi.ops[0].reg.idOrPhys);
            if (ri < kNumPhysRegs)
                trackedAddrs[ri] = tracked;
        }

        emitFlagDef(i, mi);
        emitCallFlagClobber(i, mi);
    }

    // Deduplicate predecessor lists (multiple emitters may add the same edge).
    for (auto &n : nodes) {
        auto &p = n.preds;
        auto &l = n.predLat;
        std::vector<std::pair<std::size_t, unsigned>> pl;
        pl.reserve(p.size());
        for (std::size_t k = 0; k < p.size(); ++k)
            pl.emplace_back(p[k], l[k]);
        std::sort(pl.begin(), pl.end());
        pl.erase(std::unique(pl.begin(),
                             pl.end(),
                             /// @brief Tests whether two edges share a predecessor.
                             /// @param a Left predecessor-latency pair.
                             /// @param b Right predecessor-latency pair.
                             /// @return `true` when both pairs name the same predecessor.
                             [](const auto &a, const auto &b) { return a.first == b.first; }),
                 pl.end());
        p.clear();
        l.clear();
        for (auto &[pidx, lat] : pl) {
            p.push_back(pidx);
            l.push_back(lat);
        }
        n.predsDone = 0;
    }

    return nodes;
}

/// @brief Reorder one straight-line segment using critical-path list scheduling.
///
/// Dependency-ready instructions are prioritized by descending critical-path
/// length, with original instruction index as the deterministic tie breaker.
/// Segments with zero or one instruction, or more than the configured bound,
/// are returned unchanged. If dependency construction unexpectedly yields a
/// cycle, unscheduled instructions are appended in their original order.
///
/// @param body Instructions to schedule; boundary instructions are excluded by
///             the caller.
/// @param target Target information used to model ABI call clobbers.
/// @return A vector containing exactly the input instructions in scheduled order.
static std::vector<MInstr> scheduleBlock(const std::vector<MInstr> &body,
                                         const TargetInfo &target) {
    const std::size_t N = body.size();
    if (N <= 1)
        return body;
    if (N > kMaxScheduledSegmentInstructions)
        return body;

    // -----------------------------------------------------------------------
    // Phase 1: Build dependency graph.
    // -----------------------------------------------------------------------
    std::vector<DepNode> nodes = buildDependencyGraph(body, target);

    // -----------------------------------------------------------------------
    // Compute successors (reverse of preds) for predsDone tracking.
    // -----------------------------------------------------------------------
    std::vector<std::vector<std::size_t>> succs(N);
    for (std::size_t i = 0; i < N; ++i)
        for (auto p : nodes[i].preds)
            succs[p].push_back(i);

    // -----------------------------------------------------------------------
    // Compute critical-path lengths (backward pass).
    // -----------------------------------------------------------------------
    // Process in reverse topological order (reverse program order is
    // approximately topological for sequential code).
    for (std::size_t i = N; i-- > 0;) {
        // critPath(i) = instrLatency(i) + max over successors s of critPath(s).
        // This gives the total latency from scheduling node i to the end of
        // the DAG, making loads (4 cycles) and multiplies (3 cycles) higher
        // priority than single-cycle ALU ops.
        unsigned maxSuccCrit = 0;
        for (auto s : succs[i]) {
            if (nodes[s].critPath > maxSuccCrit)
                maxSuccCrit = nodes[s].critPath;
        }
        const unsigned latency = instrLatency(body[i].opc);
        nodes[i].critPath = latency > std::numeric_limits<unsigned>::max() - maxSuccCrit
                                ? std::numeric_limits<unsigned>::max()
                                : latency + maxSuccCrit;
    }

    // -----------------------------------------------------------------------
    // List scheduling (greedy, critical-path priority).
    // -----------------------------------------------------------------------

    // Count predecessors for each node to initialise the ready list.
    for (std::size_t i = 0; i < N; ++i)
        nodes[i].predsDone = 0;

    // Build initial ready list: nodes with no predecessors.
    // Priority: (critPath DESC, instrIdx ASC) — higher critPath first.
    std::priority_queue<ReadyNode, std::vector<ReadyNode>, ReadyNodeLess> ready;

    std::vector<std::size_t> predCount(N, 0);
    for (std::size_t i = 0; i < N; ++i)
        predCount[i] = nodes[i].preds.size();

    for (std::size_t i = 0; i < N; ++i)
        if (predCount[i] == 0)
            ready.push(ReadyNode{nodes[i].critPath, i});

    std::vector<MInstr> scheduled;
    scheduled.reserve(N);
    std::vector<bool> done(N, false);

    while (!ready.empty()) {
        const ReadyNode readyNode = ready.top();
        ready.pop();
        const std::size_t idx = readyNode.instrIdx;

        if (done[idx])
            continue;
        done[idx] = true;
        scheduled.push_back(body[idx]);

        // Decrement pred counts for successors; enqueue newly-ready ones.
        for (auto s : succs[idx]) {
            if (done[s])
                continue;
            ++nodes[s].predsDone;
            if (nodes[s].predsDone >= predCount[s])
                ready.push(ReadyNode{nodes[s].critPath, s});
        }
    }

    // If the graph was cyclic (shouldn't happen in SSA-like post-RA MIR),
    // fall back to original order by appending any unscheduled instructions.
    for (std::size_t i = 0; i < N; ++i)
        if (!done[i])
            scheduled.push_back(body[i]);

    return scheduled;
}

// ---------------------------------------------------------------------------
// Per-function entry point
// ---------------------------------------------------------------------------

/// @brief Apply list scheduling to every basic block in @p fn.
/// @details Partitions each block into schedulable body segments at call/guard-branch
///          boundaries; terminators are always appended in original relative order.
///          Functions exceeding the configured instruction limit are retained
///          unchanged.
/// @param[in,out] fn Allocated MIR function whose eligible blocks are reordered.
/// @param target Target information used by each segment's dependency model.
static void scheduleFunction(MFunction &fn, const TargetInfo &target) {
    std::size_t instructionCount = 0;
    for (const auto &bb : fn.blocks) {
        if (bb.instrs.size() > kMaxScheduledFunctionInstructions - instructionCount)
            return;
        instructionCount += bb.instrs.size();
    }

    for (auto &bb : fn.blocks) {
        if (bb.instrs.size() <= 1)
            continue;

        // Split the instruction stream into segments separated by calls and
        // mid-block terminators (BCond, Cbz, Cbnz that appear before the final
        // terminator group).  Each segment is scheduled independently, then
        // reassembled with the separators in their original positions.  Calls
        // remain hard scheduling boundaries because the dependency model tracks
        // register and memory effects, but does not model all runtime side
        // effects or helper-call conventions precisely enough to safely move
        // unrelated arithmetic across them.
        //
        // Example block layout:
        //   adds x15, x12, #3      ← segment 0
        //   b.vs L.Ltrap_ovf       ← separator 0 (mid-block guard)
        //   mov x14, #20           ← segment 1
        //   cbz x14, L.Ltrap_div0  ← separator 1 (mid-block guard)
        //   sdiv x13, x15, x14     ← segment 2
        //   ...
        //   b.ne Lbc_oob0          ← final terminator
        //   b Lbc_ok0              ← final terminator

        // First, find where the final terminator group starts (consecutive
        // terminators at the end of the block).
        std::size_t termStart = bb.instrs.size();
        while (termStart > 0 && isTerminator(bb.instrs[termStart - 1].opc))
            --termStart;

        // Now collect segments and separators from the body (indices 0..termStart-1).
        std::vector<MInstr> result;
        result.reserve(bb.instrs.size());
        std::vector<MInstr> segment;

        for (std::size_t i = 0; i < termStart; ++i) {
            auto &mi = bb.instrs[i];
            if (isTerminator(mi.opc) || isCall(mi.opc)) {
                // Mid-block terminator or call — schedule the preceding segment,
                // then append the boundary instruction in place.
                if (segment.size() > 1)
                    segment = scheduleBlock(std::move(segment), target);
                result.insert(result.end(), segment.begin(), segment.end());
                segment.clear();
                result.push_back(mi);
            } else {
                segment.push_back(mi);
            }
        }

        // Schedule the final body segment (between the last mid-block branch
        // and the final terminator group).
        if (segment.size() > 1)
            segment = scheduleBlock(std::move(segment), target);
        result.insert(result.end(), segment.begin(), segment.end());

        // Append final terminators in original order.
        for (std::size_t i = termStart; i < bb.instrs.size(); ++i)
            result.push_back(bb.instrs[i]);

        bb.instrs = std::move(result);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Pass implementation
// ---------------------------------------------------------------------------

/// @copydoc SchedulerPass::run
bool SchedulerPass::run(AArch64Module &module, Diagnostics &diags) {
    if (module.ti == nullptr) {
        diags.error("aarch64 scheduler: target info is required");
        return false;
    }

    for (auto &fn : module.mir)
        scheduleFunction(fn, *module.ti);
    return true;
}

} // namespace zanna::codegen::aarch64::passes
