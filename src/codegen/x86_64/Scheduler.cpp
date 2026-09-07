//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/Scheduler.cpp
// Purpose: Conservative post-RA list scheduler for x86-64 Machine IR.
// Key invariants:
//   - Data dependences are computed per basic block before reordering.
//   - Block boundaries and control flow instructions are never reordered.
// Ownership/Lifetime:
//   - Stateless; mutates caller-owned MFunction in place.
// Links: src/codegen/x86_64/Scheduler.hpp,
//        src/codegen/x86_64/OperandRoles.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/Scheduler.hpp"

#include "codegen/x86_64/OperandRoles.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <unordered_set>

/**
 * @file
 * @brief Implements dependency-aware post-RA list scheduling for x86-64 MIR.
 *
 * Each bounded segment receives an acyclic program-order dependency graph.
 * Ready instructions are prioritized by critical-path height and latency while
 * preserving register, EFLAGS, and memory hazards. Win64 prologue save runs are
 * kept contiguous so emitted unwind metadata remains valid.
 */

namespace zanna::codegen::x64 {
namespace {

/// Upper bound for the quadratic dependency graph built by the list scheduler.
/// Larger straight-line spans are already uncommon after boundary splitting, so
/// leaving them in source order avoids pathological compile-time spikes.
inline constexpr std::size_t kMaxScheduledSegmentInstructions = 256;

/// @brief Explicit and implicit scheduling dependencies for one instruction.
struct InstrDeps {
    std::unordered_set<uint16_t> uses{};
    std::unordered_set<uint16_t> defs{};
    bool usesFlags{false};
    bool defsFlags{false};
    bool memRead{false};
    bool memWrite{false};
    /// Set when the only memory access is a pure RBP+disp frame slot, enabling
    /// exact disambiguation against other frame-slot accesses.
    bool memIsFrameSlot{false};
    int32_t frameDisp{0};
    unsigned latency{1};
};

/// @brief Predicate: does @p op contain any virtual register reference?
/// @details Post-register-allocation scheduling refuses to operate on blocks
///          that still contain virtual operands because their interference
///          isn't yet resolved. Used as a guard by @ref isSchedulingBoundary.
/// @param op Operand to inspect, including memory address components.
/// @return @c true when any referenced register remains virtual.
[[nodiscard]] bool isVirtualOperand(const Operand &op) noexcept {
    if (const auto *reg = std::get_if<OpReg>(&op))
        return !reg->isPhys;
    if (const auto *mem = std::get_if<OpMem>(&op)) {
        if (!mem->base.isPhys)
            return true;
        return mem->hasIndex && !mem->index.isPhys;
    }
    return false;
}

/// @brief Predicate: does @p op name physical register @p reg?
/// @param op Candidate direct register operand.
/// @param reg Physical register to match.
/// @return @c true for an exact physical-register match.
[[nodiscard]] bool operandIsPhysReg(const Operand &op, PhysReg reg) noexcept {
    const auto *operandReg = std::get_if<OpReg>(&op);
    return operandReg != nullptr && operandReg->isPhys &&
           static_cast<PhysReg>(operandReg->idOrPhys) == reg;
}

/// @brief Predicate: is @p reg a Win64 callee-saved GPR/XMM register?
/// @param reg Physical register to classify.
/// @return @c true for a nonvolatile Win64 register saved by this backend.
[[nodiscard]] bool isWin64CalleeSaved(PhysReg reg) noexcept {
    switch (reg) {
        case PhysReg::RBX:
        case PhysReg::RSI:
        case PhysReg::RDI:
        case PhysReg::R12:
        case PhysReg::R13:
        case PhysReg::R14:
        case PhysReg::R15:
        case PhysReg::XMM6:
        case PhysReg::XMM7:
        case PhysReg::XMM8:
        case PhysReg::XMM9:
        case PhysReg::XMM10:
        case PhysReg::XMM11:
        case PhysReg::XMM12:
        case PhysReg::XMM13:
        case PhysReg::XMM14:
        case PhysReg::XMM15:
            return true;
        default:
            return false;
    }
}

/// @brief Predicate: does @p instr push RBP as the Win64 frame-chain save?
/// @param instr Candidate prologue instruction.
/// @return @c true for `PUSH RBP`.
[[nodiscard]] bool isWin64FramePush(const MInstr &instr) noexcept {
    return instr.opcode == MOpcode::PUSH && !instr.operands.empty() &&
           operandIsPhysReg(instr.operands[0], PhysReg::RBP);
}

/// @brief Predicate: does @p instr establish RBP from RSP?
/// @param instr Candidate prologue instruction.
/// @return @c true for the canonical frame-pointer copy.
[[nodiscard]] bool isFramePointerSetup(const MInstr &instr) noexcept {
    return instr.opcode == MOpcode::MOVrr && instr.operands.size() >= 2 &&
           operandIsPhysReg(instr.operands[0], PhysReg::RBP) &&
           operandIsPhysReg(instr.operands[1], PhysReg::RSP);
}

/// @brief Predicate: does @p instr subtract the fixed frame allocation from RSP?
/// @param instr Candidate prologue instruction.
/// @return @c true for a negative ADDri applied to RSP.
[[nodiscard]] bool isStackAllocation(const MInstr &instr) noexcept {
    if (instr.opcode != MOpcode::ADDri || instr.operands.size() < 2 ||
        !operandIsPhysReg(instr.operands[0], PhysReg::RSP))
        return false;
    const auto *imm = std::get_if<OpImm>(&instr.operands[1]);
    return imm != nullptr && imm->val < 0;
}

/// @brief Predicate: does @p instr load the Win64 large-frame probe size?
/// @param instr Candidate prologue instruction.
/// @return @c true for a MOVri destination of RAX.
[[nodiscard]] bool isChkstkSizeLoad(const MInstr &instr) noexcept {
    return instr.opcode == MOpcode::MOVri && instr.operands.size() >= 2 &&
           operandIsPhysReg(instr.operands[0], PhysReg::RAX);
}

/// @brief Predicate: does @p instr call the Win64 stack-probe helper?
/// @param instr Candidate call.
/// @return @c true for a direct call to @c __chkstk.
[[nodiscard]] bool isChkstkCall(const MInstr &instr) noexcept {
    if (instr.opcode != MOpcode::CALL || instr.operands.empty())
        return false;
    const auto *label = std::get_if<OpLabel>(&instr.operands[0]);
    return label != nullptr && label->name == "__chkstk";
}

/// @brief Predicate: does @p instr store a Win64 callee-save register into the frame?
/// @param instr Candidate frame store.
/// @return @c true for a recognized nonvolatile GPR or XMM save.
[[nodiscard]] bool isWin64PrologueFrameSave(const MInstr &instr) noexcept {
    if ((instr.opcode != MOpcode::MOVrm && instr.opcode != MOpcode::MOVUPSrm) ||
        instr.operands.size() < 2)
        return false;
    const auto *mem = std::get_if<OpMem>(&instr.operands[0]);
    if (mem == nullptr || mem->hasIndex || !mem->base.isPhys ||
        static_cast<PhysReg>(mem->base.idOrPhys) != PhysReg::RBP || mem->disp >= 0)
        return false;
    const auto *src = std::get_if<OpReg>(&instr.operands[1]);
    if (src == nullptr || !src->isPhys)
        return false;
    const auto reg = static_cast<PhysReg>(src->idOrPhys);
    if (!isWin64CalleeSaved(reg))
        return false;
    return (instr.opcode == MOpcode::MOVrm && isGPR(reg)) ||
           (instr.opcode == MOpcode::MOVUPSrm && isXMM(reg));
}

/// @brief Add the physical registers used by @p mem to @p deps.uses.
/// @details Memory operands implicitly read their base and index registers.
///          The scheduler records those reads so dependency edges include
///          address computation.
/// @param mem Memory operand whose address registers are recorded.
/// @param deps Dependency descriptor to update.
void addMemRegs(const OpMem &mem, InstrDeps &deps) {
    if (mem.base.isPhys)
        deps.uses.insert(mem.base.idOrPhys);
    if (mem.hasIndex && mem.index.isPhys)
        deps.uses.insert(mem.index.idOrPhys);
}

/// @brief Approximate instruction latency in cycles.
/// @details Hand-tuned numbers loosely based on modern x86 microarchitectures.
///          Memory loads (4 cycles), IMUL (3), FDIV (10), and FP arithmetic
///          (3) form the bulk of the table; everything else defaults to 1.
///          The scheduler uses this to prefer dispatching long-latency
///          instructions earlier in the segment.
/// @param opcode Opcode to estimate.
/// @return Conservative latency weight in abstract cycles.
[[nodiscard]] unsigned opcodeLatency(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::MOVmr:
        case MOpcode::MOVSDmr:
        case MOpcode::MOVUPSmr:
            return 4;
        case MOpcode::IMULrr:
            return 3;
        case MOpcode::FDIV:
            return 10;
        case MOpcode::CVTSI2SD:
        case MOpcode::CVTTSD2SI:
            return 4;
        case MOpcode::FADD:
        case MOpcode::FSUB:
        case MOpcode::FMUL:
            return 3;
        default:
            return 1;
    }
}

/// @brief Predicate: does @p instr terminate a schedulable segment?
/// @details Control transfers, calls, parallel-copy pseudos, and
///          implicit-register opcodes (CQO, IDIV) cannot be reordered around.
///          Instructions that define RSP or RBP, or that still reference
///          virtual operands, also serve as boundaries.
/// @param instr Instruction to classify.
/// @return @c true when scheduling may not cross the instruction.
[[nodiscard]] bool isSchedulingBoundary(const MInstr &instr) noexcept {
    switch (instr.opcode) {
        case MOpcode::LABEL:
        case MOpcode::CALL:
        case MOpcode::RET:
        case MOpcode::JMP:
        case MOpcode::JCC:
        case MOpcode::JUMPTABLE:
        case MOpcode::UD2:
        case MOpcode::PUSH:
        case MOpcode::POP:
        case MOpcode::CQO:
        case MOpcode::IDIVrm:
        case MOpcode::DIVrm:
        case MOpcode::MULr:
        case MOpcode::IMULr:
        case MOpcode::PX_COPY:
        case MOpcode::SELECT_GPR:
        case MOpcode::SELECT_XMM:
        case MOpcode::ADDOvfrr:
        case MOpcode::SUBOvfrr:
        case MOpcode::IMULOvfrr:
            return true;
        default:
            break;
    }

    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        (void)isUse;
        if (!isDef)
            continue;
        const auto *reg = std::get_if<OpReg>(&instr.operands[idx]);
        if (!reg || !reg->isPhys)
            continue;
        const auto phys = static_cast<PhysReg>(reg->idOrPhys);
        if (phys == PhysReg::RSP || phys == PhysReg::RBP)
            return true;
    }

    return std::any_of(instr.operands.begin(), instr.operands.end(), isVirtualOperand);
}

/// @brief Predicate: does this opcode READ from its memory operand?
/// @details MOVmr-family opcodes load; MOVrm-family opcodes store. LEA only
///          computes an address. Everything else with a memory operand is
///          conservatively treated as both reading and writing.
/// @param opcode Opcode with a memory operand.
/// @return @c true when that operand may read memory.
[[nodiscard]] bool opcodeReadsMem(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::MOVmr:
        case MOpcode::MOVSDmr:
        case MOpcode::MOVUPSmr:
        case MOpcode::ADDrm:
        case MOpcode::SUBrm:
        case MOpcode::ANDrm:
        case MOpcode::ORrm:
        case MOpcode::XORrm:
        case MOpcode::CMPrm:
        case MOpcode::IMULrm:
            return true;
        case MOpcode::MOVrm:
        case MOpcode::MOVSDrm:
        case MOpcode::MOVUPSrm:
        case MOpcode::LEA:
            return false;
        default:
            return true;
    }
}

/// @brief Predicate: does this opcode WRITE to its memory operand?
/// @param opcode Opcode with a memory operand.
/// @return @c true when that operand may write memory.
[[nodiscard]] bool opcodeWritesMem(MOpcode opcode) noexcept {
    switch (opcode) {
        case MOpcode::MOVrm:
        case MOpcode::MOVSDrm:
        case MOpcode::MOVUPSrm:
            return true;
        case MOpcode::MOVmr:
        case MOpcode::MOVSDmr:
        case MOpcode::MOVUPSmr:
        case MOpcode::ADDrm:
        case MOpcode::SUBrm:
        case MOpcode::ANDrm:
        case MOpcode::ORrm:
        case MOpcode::XORrm:
        case MOpcode::CMPrm:
        case MOpcode::IMULrm:
        case MOpcode::LEA:
            return false;
        default:
            return true;
    }
}

/// @brief Build the dependency descriptor for @p instr.
/// @details Walks each operand and records physical register uses/defs,
///          whether the instruction reads/writes EFLAGS, the kind of memory
///          access performed (read/write, frame-slot precision), and the
///          assumed latency. The result feeds the dependency graph
///          constructed by @ref scheduleSegment.
/// @param instr Instruction to analyze.
/// @return Complete dependency and latency descriptor.
[[nodiscard]] InstrDeps analyseInstr(const MInstr &instr) {
    InstrDeps deps{};
    deps.usesFlags = usesEFlags(instr.opcode);
    deps.defsFlags = definesEFlags(instr.opcode);
    deps.latency = opcodeLatency(instr.opcode);

    std::size_t memOperands = 0;
    for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
        const auto [isUse, isDef] = operandRoles(instr, idx);
        const Operand &op = instr.operands[idx];

        if (const auto *reg = std::get_if<OpReg>(&op)) {
            if (reg->isPhys) {
                if (isUse)
                    deps.uses.insert(reg->idOrPhys);
                if (isDef)
                    deps.defs.insert(reg->idOrPhys);
            }
            continue;
        }

        if (const auto *mem = std::get_if<OpMem>(&op)) {
            // LEA's memory operand is an address computation, not an access:
            // record only the register reads.
            addMemRegs(*mem, deps);
            if (instr.opcode == MOpcode::LEA)
                continue;

            ++memOperands;
            deps.memRead = deps.memRead || opcodeReadsMem(instr.opcode);
            deps.memWrite = deps.memWrite || opcodeWritesMem(instr.opcode);
            // Frame-slot precision applies only to the 8-byte scalar moves:
            // wider accesses (MOVUPS saves 16 bytes) span multiple slots and
            // unknown opcodes have unknown widths.
            const bool eightByteMov =
                instr.opcode == MOpcode::MOVrm || instr.opcode == MOpcode::MOVmr ||
                instr.opcode == MOpcode::MOVSDrm || instr.opcode == MOpcode::MOVSDmr;
            if (eightByteMov && !mem->hasIndex && mem->base.isPhys &&
                static_cast<PhysReg>(mem->base.idOrPhys) == PhysReg::RBP) {
                deps.memIsFrameSlot = true;
                deps.frameDisp = mem->disp;
            }
            continue;
        }

        if (std::holds_alternative<OpRipLabel>(op)) {
            // RIP-relative references load constants/rodata: model as a read.
            ++memOperands;
            deps.memRead = true;
        }
    }

    // Implicit fixed-register operands (CQO, IDIV/DIV, MUL/IMUL, shift-by-CL)
    // from the shared role model. These opcodes are also scheduling
    // boundaries, so this mainly keeps the dependency record complete for the
    // instructions on either side of them.
    const PhysRegMask implicitUses = implicitUseMask(instr.opcode);
    const PhysRegMask implicitDefs = implicitDefMask(instr.opcode);
    for (unsigned bit = 0; bit < 64; ++bit) {
        const PhysRegMask mask = PhysRegMask{1} << bit;
        if ((implicitUses & mask) != 0)
            deps.uses.insert(static_cast<uint16_t>(bit));
        if ((implicitDefs & mask) != 0)
            deps.defs.insert(static_cast<uint16_t>(bit));
    }

    // Frame-slot precision is only valid when the instruction touches exactly
    // one memory location.
    if (memOperands != 1)
        deps.memIsFrameSlot = false;

    return deps;
}

/// @brief Does @p a share any element with @p b?
/// @details Recursion onto the smaller set keeps the hash-set lookup count
///          minimal. Used by @ref dependsOn to detect def-use, use-def, and
///          def-def conflicts.
/// @param a First physical-register set.
/// @param b Second physical-register set.
/// @return @c true when the sets overlap.
[[nodiscard]] bool intersects(const std::unordered_set<uint16_t> &a,
                              const std::unordered_set<uint16_t> &b) {
    if (a.size() > b.size())
        return intersects(b, a);
    return std::any_of(a.begin(), a.end(), [&](uint16_t reg) { return b.count(reg) != 0; });
}

/// @brief Predicate: must @p second execute after @p first?
/// @details A dependency exists when:
///          - one instruction's def aliases another's use (data dep),
///          - they both define the same register (output dep),
///          - one defines EFLAGS while the other reads or redefines it,
///          - or their memory accesses may conflict. Two loads never
///            conflict, and two pure RBP+disp frame-slot accesses with
///            different displacements are provably disjoint (slots are
///            8-byte stepped and non-overlapping by frame construction);
///            anything else involving at least one write is ordered.
/// @param first Earlier instruction's dependency descriptor.
/// @param second Later instruction's dependency descriptor.
/// @return @c true when program order must be preserved.
[[nodiscard]] bool dependsOn(const InstrDeps &first, const InstrDeps &second) {
    if (intersects(first.defs, second.uses))
        return true;
    if (intersects(first.uses, second.defs))
        return true;
    if (intersects(first.defs, second.defs))
        return true;

    if (first.defsFlags && (second.usesFlags || second.defsFlags))
        return true;
    if (first.usesFlags && second.defsFlags)
        return true;

    const bool firstAccesses = first.memRead || first.memWrite;
    const bool secondAccesses = second.memRead || second.memWrite;
    if (!firstAccesses || !secondAccesses)
        return false;

    // Read-read pairs can always reorder.
    if (!first.memWrite && !second.memWrite)
        return false;

    // Distinct frame slots cannot alias each other.
    if (first.memIsFrameSlot && second.memIsFrameSlot && first.frameDisp != second.frameDisp)
        return false;

    return true;
}

/// @brief Compute the critical-path height of each instruction.
/// @details Height = instruction's own latency plus the maximum height of
///          its successors. A higher height means more downstream work, so
///          the list scheduler prioritises dispatching that instruction
///          earlier to expose parallelism.
/// @param deps Per-instruction dependency descriptors.
/// @param succs Dependency graph successor lists.
/// @return Per-instruction critical-path height in cycles.
[[nodiscard]] std::vector<unsigned> criticalHeights(
    const std::vector<InstrDeps> &deps, const std::vector<std::vector<std::size_t>> &succs) {
    std::vector<unsigned> heights(deps.size(), 0);
    for (std::size_t i = deps.size(); i-- > 0;) {
        unsigned succHeight = 0;
        for (std::size_t succ : succs[i])
            succHeight = std::max(succHeight, heights[succ]);
        heights[i] = deps[i].latency > std::numeric_limits<unsigned>::max() - succHeight
                         ? std::numeric_limits<unsigned>::max()
                         : deps[i].latency + succHeight;
    }
    return heights;
}

/// @brief Reorder a straight-line instruction segment using list scheduling.
/// @details Builds an upper-triangular dependency graph (for each i < j,
///          adds i→j when they conflict), tracks ready instructions
///          (predecessor count zero), and at each step picks the ready
///          instruction with the highest critical-path height. Ties broken
///          by latency then by program-order index (preferring the later
///          instruction to delay it less).
/// @param segment Instructions to reorder (consumed by move).
/// @param changed Output: set to true if the order actually changed.
/// @return Reordered instruction stream.
[[nodiscard]] std::vector<MInstr> scheduleSegment(std::vector<MInstr> segment, bool &changed) {
    changed = false;
    const std::size_t n = segment.size();
    if (n < 2)
        return segment;
    if (n > kMaxScheduledSegmentInstructions)
        return segment;

    std::vector<InstrDeps> deps;
    deps.reserve(n);
    for (const auto &instr : segment)
        deps.push_back(analyseInstr(instr));

    std::vector<std::vector<std::size_t>> succs(n);
    std::vector<std::size_t> predCount(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (!dependsOn(deps[i], deps[j]))
                continue;
            succs[i].push_back(j);
            ++predCount[j];
        }
    }

    const std::vector<unsigned> heights = criticalHeights(deps, succs);

    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i) {
        if (predCount[i] == 0)
            ready.push_back(i);
    }

    std::vector<bool> scheduled(n, false);
    std::vector<std::size_t> order;
    order.reserve(n);

    while (!ready.empty()) {
        /// Rank ready nodes by critical height, latency, then stable source order.
        auto best =
            std::max_element(ready.begin(), ready.end(), [&](std::size_t lhs, std::size_t rhs) {
                if (heights[lhs] != heights[rhs])
                    return heights[lhs] < heights[rhs];
                if (deps[lhs].latency != deps[rhs].latency)
                    return deps[lhs].latency < deps[rhs].latency;
                return lhs > rhs;
            });

        const std::size_t idx = *best;
        ready.erase(best);
        if (scheduled[idx])
            continue;

        scheduled[idx] = true;
        order.push_back(idx);

        for (std::size_t succ : succs[idx]) {
            if (predCount[succ] > 0)
                --predCount[succ];
            if (predCount[succ] == 0 && !scheduled[succ])
                ready.push_back(succ);
        }
    }

    if (order.size() != n)
        throw std::runtime_error("x86-64 scheduler: dependency graph did not schedule every node");

    for (std::size_t i = 0; i < n; ++i) {
        if (order[i] != i) {
            changed = true;
            break;
        }
    }
    if (!changed)
        return segment;

    std::vector<MInstr> scheduledSegment;
    scheduledSegment.reserve(n);
    for (std::size_t idx : order)
        scheduledSegment.push_back(std::move(segment[idx]));
    return scheduledSegment;
}

/// @brief Schedule @p segment and append its result to @p out.
/// @details Helper used by the block-walker to drain accumulated
///          schedulable instructions whenever a boundary is hit. The
///          @p changedSegments counter is bumped only when the
///          reordering actually mutated the stream.
/// @param out Rewritten block instruction stream to extend.
/// @param segment Accumulated schedulable instructions, consumed and cleared.
/// @param changedSegments Counter incremented when ordering changes.
void flushSegment(std::vector<MInstr> &out,
                  std::vector<MInstr> &segment,
                  std::size_t &changedSegments) {
    if (segment.empty())
        return;

    bool changed = false;
    std::vector<MInstr> scheduled = scheduleSegment(std::move(segment), changed);
    if (changed)
        ++changedSegments;
    out.insert(out.end(),
               std::make_move_iterator(scheduled.begin()),
               std::make_move_iterator(scheduled.end()));
    segment.clear();
}

/// @brief State machine protecting unwind-described Win64 prologue saves.
enum class Win64PrologueScan {
    /// No prologue pattern is being recognized.
    Inactive,
    /// Entry block has not yet been checked for PUSH RBP.
    ExpectPush,
    /// PUSH RBP matched; expect the RBP-from-RSP setup.
    ExpectFramePointer,
    /// Accept stack setup/probe operations or the first frame save.
    ExpectSetupOrSave,
    /// Keep a contiguous run of recognized nonvolatile saves unscheduled.
    InSaveRun,
};

} // namespace

/// @copydoc scheduleFunction
std::size_t scheduleFunction(MFunction &fn) {
    std::size_t changedSegments = 0;

    for (std::size_t blockIndex = 0; blockIndex < fn.blocks.size(); ++blockIndex) {
        auto &block = fn.blocks[blockIndex];
        std::vector<MInstr> rewritten;
        rewritten.reserve(block.instructions.size());

        std::vector<MInstr> segment;
        Win64PrologueScan prologueScan =
            blockIndex == 0 ? Win64PrologueScan::ExpectPush : Win64PrologueScan::Inactive;
        for (auto &instr : block.instructions) {
            bool protectWin64PrologueSave = false;
            switch (prologueScan) {
                case Win64PrologueScan::ExpectPush:
                    prologueScan = isWin64FramePush(instr) ? Win64PrologueScan::ExpectFramePointer
                                                           : Win64PrologueScan::Inactive;
                    break;
                case Win64PrologueScan::ExpectFramePointer:
                    prologueScan = isFramePointerSetup(instr) ? Win64PrologueScan::ExpectSetupOrSave
                                                              : Win64PrologueScan::Inactive;
                    break;
                case Win64PrologueScan::ExpectSetupOrSave:
                    if (isWin64PrologueFrameSave(instr)) {
                        protectWin64PrologueSave = true;
                        prologueScan = Win64PrologueScan::InSaveRun;
                    } else if (isStackAllocation(instr) || isChkstkSizeLoad(instr) ||
                               isChkstkCall(instr)) {
                        prologueScan = Win64PrologueScan::ExpectSetupOrSave;
                    } else {
                        prologueScan = Win64PrologueScan::Inactive;
                    }
                    break;
                case Win64PrologueScan::InSaveRun:
                    if (isWin64PrologueFrameSave(instr)) {
                        protectWin64PrologueSave = true;
                    } else {
                        prologueScan = Win64PrologueScan::Inactive;
                    }
                    break;
                case Win64PrologueScan::Inactive:
                    break;
            }

            if (protectWin64PrologueSave) {
                flushSegment(rewritten, segment, changedSegments);
                rewritten.push_back(std::move(instr));
                continue;
            }

            if (isSchedulingBoundary(instr)) {
                flushSegment(rewritten, segment, changedSegments);
                rewritten.push_back(std::move(instr));
                continue;
            }
            segment.push_back(std::move(instr));
        }
        flushSegment(rewritten, segment, changedSegments);
        block.instructions.swap(rewritten);
    }

    return changedSegments;
}

/// @copydoc scheduleModule
std::size_t scheduleModule(std::vector<MFunction> &mir) {
    std::size_t changed = 0;
    for (auto &fn : mir)
        changed += scheduleFunction(fn);
    return changed;
}

} // namespace zanna::codegen::x64
