//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/FrameLowering.cpp
// Purpose: Define stack-frame construction utilities for the x86-64 backend.
//          Allocates concrete spill displacements, reserves callee-saved slots,
//          and generates ABI-compliant prologue/epilogue sequences.
// Key invariants:
//   - Spill slots are addressed off %rbp with negative displacements.
//   - Final frame size preserves 16-byte alignment across calls.
// Ownership/Lifetime:
//   - Operates directly on Machine IR owned by the caller; uses only
//     automatic storage duration helpers.
// Links: src/codegen/x86_64/FrameLowering.hpp,
//        src/codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

#include "FrameLowering.hpp"
#include "OperandUtils.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

/**
 * @file
 * @brief Implements checked x86-64 frame layout and prologue/epilogue insertion.
 *
 * The implementation recognizes disjoint symbolic slot ranges for allocas and
 * register-class spills, calculates a deterministic RBP-relative layout, and
 * rewrites MIR memory operands. It supports SysV inline stack probing and Win64
 * `__chkstk`, nonvolatile XMM preservation, shadow space, and unwind planning.
 */

namespace zanna::codegen::x64 {

namespace {

/// @brief Composite key describing a spill slot's register class and index.
struct SlotKey {
    /// Register bank whose slot namespace contains @ref index.
    RegClass cls{RegClass::GPR};
    /// Symbolic slot index from the MIR placeholder.
    int index{0};

    /// @brief Equality comparison required by the unordered_map cache.
    /// @param other Key to compare.
    /// @return @c true when class and index are identical.
    bool operator==(const SlotKey &other) const noexcept {
        return cls == other.cls && index == other.index;
    }
};

/// @brief Hash functor for @ref SlotKey enabling unordered maps.
struct SlotKeyHash {
    /// @brief Combine the register class and index into a small hash code.
    /// @param key Slot key to hash.
    /// @return Hash value suitable for @c std::unordered_map.
    std::size_t operator()(const SlotKey &key) const noexcept {
        const auto clsVal = static_cast<std::size_t>(key.cls);
        const auto idxVal = static_cast<std::size_t>(key.index);
        return (idxVal << 1) ^ clsVal;
    }
};

/// @brief Add two frame byte counts with signed-int overflow checking.
/// @param lhs Existing nonnegative byte count.
/// @param rhs Nonnegative byte count to add.
/// @param what Human-readable frame-area name used in diagnostics.
/// @return Checked sum.
/// @throws std::invalid_argument if @p rhs is negative.
/// @throws std::overflow_error if the sum exceeds int range.
[[nodiscard]] int checkedFrameAdd(int lhs, int rhs, const char *what) {
    if (rhs < 0)
        throw std::invalid_argument(std::string("x86 frame ") + what +
                                    " byte count must be non-negative");
    if (lhs > std::numeric_limits<int>::max() - rhs)
        throw std::overflow_error(std::string("x86 frame ") + what + " exceeds int range");
    return lhs + rhs;
}

/// @brief Multiply a slot count by a slot size with signed-int overflow checking.
/// @param count Number of slots.
/// @param slotSize Positive byte size of one slot.
/// @param what Human-readable frame-area name used in diagnostics.
/// @return Checked product represented as @c int.
/// @throws std::invalid_argument if @p slotSize is not positive.
/// @throws std::overflow_error if the product exceeds int range.
[[nodiscard]] int checkedFrameMul(std::size_t count, int slotSize, const char *what) {
    if (slotSize <= 0)
        throw std::invalid_argument(std::string("x86 frame ") + what +
                                    " slot size must be positive");
    const auto maxCount = static_cast<std::size_t>(std::numeric_limits<int>::max() / slotSize);
    if (count > maxCount)
        throw std::overflow_error(std::string("x86 frame ") + what + " exceeds int range");
    return static_cast<int>(count) * slotSize;
}

/// @brief Convert a size_t frame quantity to int with overflow checking.
/// @param value Nonnegative byte count to narrow.
/// @param what Human-readable frame-area name used in diagnostics.
/// @return @p value represented as @c int.
/// @throws std::overflow_error if @p value exceeds int range.
[[nodiscard]] int checkedFrameSizeFromSizeT(std::size_t value, const char *what) {
    if (value > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::overflow_error(std::string("x86 frame ") + what + " exceeds int range");
    return static_cast<int>(value);
}

/// @brief Return a negative FP-relative offset for a positive frame extent.
/// @param extent Positive distance below the frame pointer.
/// @param what Human-readable slot name used in diagnostics.
/// @return Negative RBP-relative displacement.
/// @throws std::invalid_argument if @p extent is not positive.
[[nodiscard]] int negativeFrameOffset(int extent, const char *what) {
    if (extent <= 0)
        throw std::invalid_argument(std::string("x86 frame ") + what +
                                    " offset extent must be positive");
    return -extent;
}

/// @brief Decode an RBP-relative placeholder displacement into a slot index.
/// @details During earlier lowering, frame slots are represented as negative
///          displacements from RBP using 8-byte placeholder stepping. The first
///          frame pass treats malformed placeholders as hard errors, while the
///          rewrite pass ignores malformed negative offsets that were not
///          generated by the placeholder scheme.
/// @param disp RBP-relative displacement to inspect.
/// @param slotIndex Receives the zero-based slot index when @p disp is valid.
/// @param strict When true, malformed negative placeholders raise diagnostics.
/// @return True when @p disp encodes a valid placeholder slot.
[[nodiscard]] bool decodeFrameSlotPlaceholder(int32_t disp, int &slotIndex, bool strict) {
    if (disp >= 0) {
        return false;
    }
    if (disp == std::numeric_limits<int32_t>::min()) {
        if (strict) {
            throw std::overflow_error(
                "x86 frame placeholder offset cannot be represented as a positive value");
        }
        return false;
    }
    const int placeholder = -disp;
    if ((placeholder % kSlotSizeBytes) != 0) {
        if (strict) {
            throw std::runtime_error(
                "x86 frame placeholder offsets must use positive 8-byte stepping");
        }
        return false;
    }
    slotIndex = placeholder / kSlotSizeBytes - 1;
    return true;
}

/// @brief Build a set of callee-saved registers for O(1) lookup.
/// @details Pre-computes the union of GPR and XMM callee-saved registers
///          into an unordered_set for efficient membership testing.
/// @param target Target description providing ABI details.
/// @return Set containing all callee-saved physical registers.
[[nodiscard]] std::unordered_set<PhysReg> buildCalleeSavedSet(const TargetInfo &target) {
    std::unordered_set<PhysReg> result{};
    result.reserve(target.calleeSavedGPR.size() + target.calleeSavedFPR.size());
    result.insert(target.calleeSavedGPR.begin(), target.calleeSavedGPR.end());
    result.insert(target.calleeSavedFPR.begin(), target.calleeSavedFPR.end());
    return result;
}

/// @brief Determine whether @p reg belongs to the callee-saved set.
/// @details Uses a pre-computed set for O(1) lookup instead of O(n) linear search.
/// @param calleeSavedSet Pre-computed set of callee-saved registers.
/// @param reg Physical register being queried.
/// @return True when the register must be preserved across calls.
[[nodiscard]] bool isCalleeSaved(const std::unordered_set<PhysReg> &calleeSavedSet, PhysReg reg) {
    return calleeSavedSet.count(reg) > 0;
}

/// @brief Record a physical callee-saved register use when frame lowering must preserve it.
/// @details RBP/RSP are managed by the canonical frame setup and are therefore excluded here.
/// @param reg Register operand to classify.
/// @param calleeSavedSet ABI nonvolatile-register set.
/// @param usedCalleeSaved Destination set updated for a qualifying register.
void markUsedCalleeSaved(const OpReg &reg,
                         const std::unordered_set<PhysReg> &calleeSavedSet,
                         std::unordered_set<PhysReg> &usedCalleeSaved) {
    if (!reg.isPhys) {
        return;
    }
    const auto phys = static_cast<PhysReg>(reg.idOrPhys);
    if (phys == PhysReg::RBP || phys == PhysReg::RSP) {
        return;
    }
    if (isCalleeSaved(calleeSavedSet, phys)) {
        usedCalleeSaved.insert(phys);
    }
}

/// @brief Disjoint storage categories encoded by placeholder index ranges.
enum class FrameSlotKind { Alloca, SpillGPR, SpillXMM };

/// @brief Classify a decoded placeholder slot index into its frame-slot kind.
/// @details Indices below @c kSpillSlotOffsetGPR denote allocas, the next range
///          denotes GPR spills, and indices at or above
///          @c kSpillSlotOffsetXMM denote XMM spills.
/// @param slotIndex Decoded nonnegative placeholder index.
/// @return Storage category selected by the reserved index range.
[[nodiscard]] FrameSlotKind classifyFrameSlot(int slotIndex) noexcept {
    if (slotIndex >= kSpillSlotOffsetXMM)
        return FrameSlotKind::SpillXMM;
    if (slotIndex >= kSpillSlotOffsetGPR)
        return FrameSlotKind::SpillGPR;
    return FrameSlotKind::Alloca;
}

/// @brief Byte size needed to spill one callee-saved register.
/// @details GPR registers need 8 bytes; XMM registers need 16 bytes to
///          preserve the full 128-bit value (using MOVUPS).
/// @param reg Physical register to size.
/// @return Eight bytes for a GPR or 16 bytes for an XMM register.
[[nodiscard]] int calleeSaveSlotSize(PhysReg reg) {
    return isXMM(reg) ? 16 : kSlotSizeBytes;
}

/// @brief Offsets and total extent of the nonvolatile-register save area.
struct CalleeSavedLayout {
    /// Negative RBP-relative offset parallel to the input register sequence.
    std::vector<int> offsets{};
    /// Total bytes occupied, including XMM-alignment padding.
    int totalBytes{0};
};

/// @brief Compute stack offsets for all callee-saved register spill slots.
/// @details GPR slots are 8 bytes. XMM slots are 16 bytes and must start at a
///          16-byte unwind offset on Win64, so padding is inserted before XMM
///          saves when an odd number of preceding GPR saves would misalign them.
///          Offsets are all negative and relative to %rbp.
/// @param regs Ordered nonvolatile registers that require preservation.
/// @return Parallel offsets and total save-area size.
/// @throws std::exception If the area size exceeds the supported integer range.
[[nodiscard]] CalleeSavedLayout calleeSavedLayout(const std::vector<PhysReg> &regs) {
    CalleeSavedLayout layout{};
    layout.offsets.reserve(regs.size());
    int running = 0;
    for (auto reg : regs) {
        if (isXMM(reg)) {
            running = roundUp(running, kStackAlignment);
        }
        running = checkedFrameAdd(running, calleeSaveSlotSize(reg), "callee-saved area");
        layout.offsets.push_back(-running);
    }
    layout.totalBytes = running;
    return layout;
}

/// @brief Convert an RBP-relative save slot into a positive offset from final RSP.
/// @param frameSize Final allocation below RBP.
/// @param rbpRelativeOffset Nonpositive save-slot displacement from RBP.
/// @return Save-slot byte offset measured from the post-prologue RSP.
/// @throws std::runtime_error If the input or resulting offset is invalid.
[[nodiscard]] uint32_t unwindOffsetFromFinalRsp(int frameSize, int rbpRelativeOffset) {
    if (rbpRelativeOffset > 0)
        throw std::runtime_error("x86-64 frame lowering: callee-saved slot is above RBP");
    const int64_t offset =
        static_cast<int64_t>(frameSize) + static_cast<int64_t>(rbpRelativeOffset);
    if (offset < 0 || offset > static_cast<int64_t>(std::numeric_limits<uint32_t>::max()))
        throw std::runtime_error("x86-64 frame lowering: Win64 unwind save offset is out of range");
    return static_cast<uint32_t>(offset);
}

} // namespace

/// @copydoc assignSpillSlots
void assignSpillSlots(MFunction &func, const TargetInfo &target, FrameInfo &frame) {
    // Pre-compute callee-saved set for O(1) lookup instead of O(n) linear search.
    const auto calleeSavedSet = buildCalleeSavedSet(target);

    std::unordered_set<PhysReg> usedCalleeSaved{};
    std::set<int> gprSpillSlots{};
    std::set<int> xmmSpillSlots{};
    int maxAllocaSlotIndex = -1; // Track the highest alloca slot index
    bool hasCall = false;

    // Alloca slots use indices 0..N; spill slots use the disjoint per-class
    // placeholder ranges starting at kSpillSlotOffsetGPR / kSpillSlotOffsetXMM
    // (defined in TargetX64.hpp).

    for (auto &block : func.blocks) {
        for (auto &instr : block.instructions) {
            hasCall = hasCall || instr.opcode == MOpcode::CALL;
            for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
                auto &operand = instr.operands[idx];
                if (const auto *reg = std::get_if<OpReg>(&operand)) {
                    markUsedCalleeSaved(*reg, calleeSavedSet, usedCalleeSaved);
                }
                auto *mem = std::get_if<OpMem>(&operand);
                if (!mem) {
                    continue;
                }
                markUsedCalleeSaved(mem->base, calleeSavedSet, usedCalleeSaved);
                if (mem->hasIndex) {
                    markUsedCalleeSaved(mem->index, calleeSavedSet, usedCalleeSaved);
                }
                if (!mem->base.isPhys) {
                    continue;
                }
                const auto basePhys = static_cast<PhysReg>(mem->base.idOrPhys);
                if (basePhys != PhysReg::RBP) {
                    continue;
                }
                int slotIndex = -1;
                if (!decodeFrameSlotPlaceholder(mem->disp, slotIndex, true)) {
                    continue;
                }

                switch (classifyFrameSlot(slotIndex)) {
                    case FrameSlotKind::SpillXMM:
                        xmmSpillSlots.insert(slotIndex);
                        break;
                    case FrameSlotKind::SpillGPR:
                        gprSpillSlots.insert(slotIndex);
                        break;
                    case FrameSlotKind::Alloca:
                        // Alloca slots also need remapping to come after the
                        // callee-saved area; track the max for frame layout.
                        maxAllocaSlotIndex = std::max(maxAllocaSlotIndex, slotIndex);
                        break;
                }
            }
        }
    }

    // Compute the alloca area size (number of 8-byte alloca slots)
    // +1 because slotIndex is 0-based and we need space for slots 0..maxAllocaSlotIndex
    const std::size_t allocaSlotCount =
        maxAllocaSlotIndex >= 0 ? static_cast<std::size_t>(maxAllocaSlotIndex) + 1U : 0U;
    const int allocaAreaBytes = checkedFrameMul(allocaSlotCount, kSlotSizeBytes, "alloca area");

    frame.usedCalleeSaved.clear();
    for (auto reg : target.calleeSavedGPR) {
        if (reg == PhysReg::RBP) {
            continue; // %rbp handled separately by the standard prologue/epilogue.
        }
        if (usedCalleeSaved.contains(reg)) {
            frame.usedCalleeSaved.push_back(reg);
        }
    }
    for (auto reg : target.calleeSavedFPR) {
        if (usedCalleeSaved.contains(reg)) {
            frame.usedCalleeSaved.push_back(reg);
        }
    }

    const CalleeSavedLayout csLayout = calleeSavedLayout(frame.usedCalleeSaved);
    const int calleeSavedBytes = csLayout.totalBytes;

    std::unordered_map<SlotKey, int, SlotKeyHash> slotOffsets{};

    // Remap alloca slots to come AFTER the callee-saved area
    // Alloca slot N (with placeholder offset -(N+1)*8) maps to -(calleeSavedBytes + (N+1)*8)
    for (int slot = 0; slot <= maxAllocaSlotIndex; ++slot) {
        const int slotBytes =
            checkedFrameMul(static_cast<std::size_t>(slot) + 1U, kSlotSizeBytes, "alloca slot");
        const int newOffset = negativeFrameOffset(
            checkedFrameAdd(calleeSavedBytes, slotBytes, "alloca slot"), "alloca slot");
        slotOffsets.emplace(SlotKey{RegClass::GPR, slot}, newOffset);
    }

    // Start spill slots AFTER the callee-saved area AND the alloca area
    int runningOffset = checkedFrameAdd(calleeSavedBytes, allocaAreaBytes, "spill base");

    for (int slot : gprSpillSlots) {
        runningOffset = checkedFrameAdd(runningOffset, kSlotSizeBytes, "GPR spill area");
        slotOffsets.emplace(SlotKey{RegClass::GPR, slot},
                            negativeFrameOffset(runningOffset, "GPR spill"));
    }
    for (int slot : xmmSpillSlots) {
        runningOffset = checkedFrameAdd(runningOffset, kSlotSizeBytes, "XMM spill area");
        slotOffsets.emplace(SlotKey{RegClass::XMM, slot},
                            negativeFrameOffset(runningOffset, "XMM spill"));
    }

    frame.spillAreaGPR = checkedFrameMul(gprSpillSlots.size(), kSlotSizeBytes, "GPR spill area");
    frame.spillAreaXMM = checkedFrameMul(xmmSpillSlots.size(), kSlotSizeBytes, "XMM spill area");

    if (frame.outgoingArgArea < 0) {
        frame.outgoingArgArea = 0;
    }
    if (target.shadowSpace != 0 && (hasCall || func.name == "main" || func.name == "@main")) {
        frame.outgoingArgArea = std::max(
            frame.outgoingArgArea, checkedFrameSizeFromSizeT(target.shadowSpace, "shadow space"));
    }
    frame.outgoingArgArea = roundUp(frame.outgoingArgArea, kStackAlignment);

    const int rawFrameSize = checkedFrameAdd(runningOffset, frame.outgoingArgArea, "total size");
    frame.frameSize = roundUp(rawFrameSize, kStackAlignment);

    for (auto &block : func.blocks) {
        for (auto &instr : block.instructions) {
            for (std::size_t idx = 0; idx < instr.operands.size(); ++idx) {
                auto *mem = std::get_if<OpMem>(&instr.operands[idx]);
                if (!mem) {
                    continue;
                }
                if (!mem->base.isPhys) {
                    continue;
                }
                const auto basePhys = static_cast<PhysReg>(mem->base.idOrPhys);
                if (basePhys != PhysReg::RBP) {
                    continue;
                }
                int slotIndex = -1;
                if (!decodeFrameSlotPlaceholder(mem->disp, slotIndex, false)) {
                    continue;
                }
                const RegClass cls =
                    classifyFrameSlot(slotIndex) == FrameSlotKind::SpillXMM ? RegClass::XMM
                                                                            : RegClass::GPR;
                const SlotKey key{cls, slotIndex};
                auto it = slotOffsets.find(key);
                if (it != slotOffsets.end()) {
                    mem->disp = it->second;
                }
            }
        }
    }
}

/// @brief Predicate: does any instruction read incoming stack params via %rbp+disp?
/// @details A non-leaf frame must keep its prologue so callers can find their
///          spilled stack arguments through the canonical RBP-relative
///          addressing. Walks every operand once; short-circuits on first hit.
/// @param func Function to inspect.
/// @return @c true when a memory operand uses a positive RBP displacement.
static bool functionReadsIncomingStackParams(const MFunction &func) {
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            for (const auto &operand : instr.operands) {
                const auto *mem = std::get_if<OpMem>(&operand);
                if (!mem || !mem->base.isPhys)
                    continue;
                if (static_cast<PhysReg>(mem->base.idOrPhys) == PhysReg::RBP && mem->disp > 0)
                    return true;
            }
        }
    }
    return false;
}

/// @brief Predicate: does @p func contain any CALL instruction?
/// @param func Function to inspect.
/// @return @c true on the first direct or indirect CALL pseudo.
static bool functionHasCall(const MFunction &func) {
    for (const auto &block : func.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.opcode == MOpcode::CALL)
                return true;
        }
    }
    return false;
}

/// @brief Emit the stack-allocation step of the prologue.
/// @details Windows large frames call @c __chkstk to probe pages safely; Unix
///          inlines per-page touches. Frames within one page just subtract.
///          Sets @ref FrameInfo::usesChkstk and pushes the Win64 unwind op.
/// @param prologue Instruction vector extended with allocation and probe MIR.
/// @param frame Final frame size plus Win64 probe/unwind output state.
/// @param isWin64 Select @c __chkstk instead of inline probing.
/// @param rspOperand Physical RSP operand used by arithmetic instructions.
/// @param rspBase Physical RSP base used by probe memory reads.
static void emitStackProbe(std::vector<MInstr> &prologue,
                           FrameInfo &frame,
                           bool isWin64,
                           const Operand &rspOperand,
                           const OpReg &rspBase) {
    if (frame.frameSize <= 0)
        return;
    if (isWin64) {
        if (frame.frameSize > kPageSize) {
            const auto raxOperand = makePhysOperand(RegClass::GPR, PhysReg::RAX);
            prologue.push_back(
                MInstr::make(MOpcode::MOVri, {raxOperand, makeImmOperand(frame.frameSize)}));
            prologue.push_back(MInstr::make(MOpcode::CALL, {makeLabelOperand("__chkstk")}));
            prologue.push_back(
                MInstr::make(MOpcode::ADDri, {rspOperand, makeImmOperand(-frame.frameSize)}));
            frame.usesChkstk = true;
        } else {
            prologue.push_back(
                MInstr::make(MOpcode::ADDri, {rspOperand, makeImmOperand(-frame.frameSize)}));
        }
        frame.win64UnwindOps.push_back(
            {Win64UnwindOpKind::AllocStack, PhysReg::RAX, static_cast<uint32_t>(frame.frameSize)});
        return;
    }
    // Unix/macOS: inline page-by-page probe for large frames so the OS guard
    // page is touched in order, then a final tail subtraction.
    if (frame.frameSize > kPageSize) {
        const auto raxOperand = makePhysOperand(RegClass::GPR, PhysReg::RAX);
        int remaining = frame.frameSize;
        while (remaining > kPageSize) {
            prologue.push_back(
                MInstr::make(MOpcode::ADDri, {rspOperand, makeImmOperand(-kPageSize)}));
            prologue.push_back(
                MInstr::make(MOpcode::MOVmr, {raxOperand, makeMemOperand(rspBase, 0)}));
            remaining -= kPageSize;
        }
        if (remaining > 0) {
            prologue.push_back(
                MInstr::make(MOpcode::ADDri, {rspOperand, makeImmOperand(-remaining)}));
        }
    } else {
        prologue.push_back(
            MInstr::make(MOpcode::ADDri, {rspOperand, makeImmOperand(-frame.frameSize)}));
    }
}

/// @brief Push callee-saved registers (GPR via MOVrm, XMM via MOVUPSrm) and
///        record their Win64 unwind ops when applicable.
/// @param prologue Instruction vector extended with save operations.
/// @param frame Ordered registers to save and unwind records to update.
/// @param csLayout RBP-relative slot offsets parallel to the register order.
/// @param isWin64 Whether to record COFF unwind operations.
/// @param rbpBase Physical RBP base for save-slot memory operands.
static void emitSaveCalleeSaved(std::vector<MInstr> &prologue,
                                FrameInfo &frame,
                                const CalleeSavedLayout &csLayout,
                                bool isWin64,
                                const OpReg &rbpBase) {
    const auto &csOffsets = csLayout.offsets;
    for (std::size_t idx = 0; idx < frame.usedCalleeSaved.size(); ++idx) {
        const auto reg = frame.usedCalleeSaved[idx];
        const int offset = csOffsets[idx];
        const MOpcode opc = isGPR(reg) ? MOpcode::MOVrm : MOpcode::MOVUPSrm;
        const RegClass cls = isGPR(reg) ? RegClass::GPR : RegClass::XMM;
        prologue.push_back(
            MInstr::make(opc, {makeMemOperand(rbpBase, offset), makePhysOperand(cls, reg)}));
        if (isWin64) {
            frame.win64UnwindOps.push_back(
                {isGPR(reg) ? Win64UnwindOpKind::SaveNonVol : Win64UnwindOpKind::SaveXmm128,
                 reg,
                 unwindOffsetFromFinalRsp(frame.frameSize, offset)});
        }
    }
}

/// @brief Restore callee-saved registers in reverse-push order.
/// @param epilogue Instruction vector extended with restore operations.
/// @param frame Ordered nonvolatile-register set saved by the prologue.
/// @param csLayout RBP-relative slot offsets parallel to the register order.
/// @param rbpBase Physical RBP base for restore-slot memory operands.
static void emitRestoreCalleeSaved(std::vector<MInstr> &epilogue,
                                   const FrameInfo &frame,
                                   const CalleeSavedLayout &csLayout,
                                   const OpReg &rbpBase) {
    const auto &csOffsets = csLayout.offsets;
    for (std::size_t idx = frame.usedCalleeSaved.size(); idx > 0; --idx) {
        const auto reg = frame.usedCalleeSaved[idx - 1];
        const int offset = csOffsets[idx - 1];
        const MOpcode opc = isGPR(reg) ? MOpcode::MOVmr : MOpcode::MOVUPSmr;
        const RegClass cls = isGPR(reg) ? RegClass::GPR : RegClass::XMM;
        epilogue.push_back(
            MInstr::make(opc, {makePhysOperand(cls, reg), makeMemOperand(rbpBase, offset)}));
    }
}

/// @copydoc insertPrologueEpilogue
void insertPrologueEpilogue(MFunction &func, const TargetInfo &target, FrameInfo &frame) {
    if (func.blocks.empty())
        return;

    const bool isWin64 = target.shadowSpace != 0;
    const bool isMain = (func.name == "main" || func.name == "@main");

    // Leaf-function frame elimination: skip the prologue/epilogue entirely
    // when nothing in the body would require it. Saves 3–5 instructions.
    if (!functionHasCall(func) && frame.usedCalleeSaved.empty() && frame.frameSize == 0 &&
        !functionReadsIncomingStackParams(func) && !isMain) {
        frame.prologueEmitted = false;
        frame.usesChkstk = false;
        frame.win64UnwindOps.clear();
        return;
    }

    const auto rspOperand = makePhysOperand(RegClass::GPR, PhysReg::RSP);
    const auto rbpOperand = makePhysOperand(RegClass::GPR, PhysReg::RBP);
    const auto rspBase = makePhysBase(PhysReg::RSP);
    const auto rbpBase = makePhysBase(PhysReg::RBP);

    // Canonical prologue: push %rbp; mov %rsp, %rbp; sub $frameSize, %rsp.
    // Windows uses real PUSH/POP so its COFF unwind records match; SysV keeps
    // the explicit store form for consistency with other tools.
    std::vector<MInstr> prologue{};
    prologue.reserve(4 + frame.usedCalleeSaved.size());
    if (isWin64) {
        prologue.push_back(MInstr::make(MOpcode::PUSH, {rbpOperand}));
    } else {
        prologue.push_back(
            MInstr::make(MOpcode::ADDri, {rspOperand, makeImmOperand(-kSlotSizeBytes)}));
        prologue.push_back(MInstr::make(MOpcode::MOVrm, {makeMemOperand(rspBase, 0), rbpOperand}));
    }
    prologue.push_back(MInstr::make(MOpcode::MOVrr, {rbpOperand, rspOperand}));

    if (isWin64) {
        frame.prologueEmitted = true;
        frame.usesChkstk = false;
        frame.win64UnwindOps.clear();
        frame.win64UnwindOps.push_back({Win64UnwindOpKind::PushNonVol, PhysReg::RBP, 0});
    }

    emitStackProbe(prologue, frame, isWin64, rspOperand, rspBase);

    const CalleeSavedLayout csLayout = calleeSavedLayout(frame.usedCalleeSaved);
    emitSaveCalleeSaved(prologue, frame, csLayout, isWin64, rbpBase);

    // The main function installs the runtime's stack-safety handler.
    if (isMain) {
        prologue.push_back(MInstr::make(MOpcode::CALL, {makeLabelOperand("rt_init_stack_safety")}));
    }

    auto &entry = func.blocks.front();
    std::vector<MInstr> updatedEntry{};
    updatedEntry.reserve(prologue.size() + entry.instructions.size());
    updatedEntry.insert(updatedEntry.end(), prologue.begin(), prologue.end());
    updatedEntry.insert(updatedEntry.end(), entry.instructions.begin(), entry.instructions.end());
    entry.instructions = std::move(updatedEntry);

    // Epilogue mirrors the canonical add $frameSize, %rsp; pop %rbp; ret form.
    std::vector<MInstr> epilogue{};
    epilogue.reserve(3 + frame.usedCalleeSaved.size());
    emitRestoreCalleeSaved(epilogue, frame, csLayout, rbpBase);

    if (isWin64) {
        if (frame.frameSize > 0) {
            epilogue.push_back(
                MInstr::make(MOpcode::ADDri, {rspOperand, makeImmOperand(frame.frameSize)}));
        }
        epilogue.push_back(MInstr::make(MOpcode::POP, {rbpOperand}));
    } else {
        epilogue.push_back(MInstr::make(MOpcode::MOVrr, {rspOperand, rbpOperand}));
        epilogue.push_back(MInstr::make(MOpcode::MOVmr, {rbpOperand, makeMemOperand(rspBase, 0)}));
        epilogue.push_back(
            MInstr::make(MOpcode::ADDri, {rspOperand, makeImmOperand(kSlotSizeBytes)}));
    }

    for (auto &block : func.blocks) {
        for (std::size_t idx = 0; idx < block.instructions.size(); ++idx) {
            if (block.instructions[idx].opcode == MOpcode::RET) {
                block.instructions.insert(block.instructions.begin() +
                                              static_cast<std::ptrdiff_t>(idx),
                                          epilogue.begin(),
                                          epilogue.end());
                idx += epilogue.size();
            }
        }
    }
}

} // namespace zanna::codegen::x64
