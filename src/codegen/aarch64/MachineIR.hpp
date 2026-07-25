//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/MachineIR.hpp
// Purpose: AArch64 Machine IR (MIR) data structures for code generation.
//
// This file defines the machine-level intermediate representation used between
// IL lowering and assembly emission. MIR instructions are target-specific and
// map closely to AArch64 instructions but still use virtual registers that
// must be allocated to physical registers before emission.
//
// Key invariants:
//   - MInstr operands follow destination-first ordering.
//   - Virtual registers (isPhys=false) must be resolved before emission.
//   - Physical registers (isPhys=true) correspond directly to AArch64 regs.
//
// Ownership/Lifetime:
//   - MFunction owns all blocks; blocks own instructions.
//   - Frame layout is computed during lowering and finalized after allocation.
//
// Links: src/codegen/aarch64/MachineIR.cpp,
//        src/codegen/aarch64/TargetAArch64.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "codegen/aarch64/TargetAArch64.hpp"
#include "support/source_location.hpp"

/**
 * @file
 * @brief Defines the AArch64 machine intermediate representation.
 *
 * MIR is the mutable, target-specific layer between IL lowering and assembly
 * emission. Instructions initially refer to typed virtual registers; register
 * allocation replaces those references with physical AArch64 registers before
 * emission. Functions own blocks, blocks own instructions, and instructions
 * own their operand sequences.
 */

namespace zanna::codegen::aarch64 {

/// @brief Machine IR opcodes for AArch64 code generation.
///
/// Each opcode represents a target-specific operation that maps to one or
/// more AArch64 assembly instructions. Generated from MOpcodeDef.inc —
/// the single source of truth for opcode names. Add new opcodes ONLY in
/// MOpcodeDef.inc; the enum and name table are auto-generated from that file.
enum class MOpcode {
#define ZANNA_MIR_OPCODE(name) name,
#include "codegen/aarch64/MOpcodeDef.inc"
};

/**
 * @brief Identifies a physical or virtual machine register.
 *
 * Before register allocation, `isPhys` is false and `idOrPhys` contains a
 * virtual-register ID. After allocation, physical operands have `isPhys` set
 * and store the underlying `PhysReg` value in `idOrPhys`.
 *
 * @invariant `cls` agrees with the register bank selected by `idOrPhys`.
 */
struct MReg {
    bool isPhys{false};          ///< True if this is a physical register.
    RegClass cls{RegClass::GPR}; ///< Register class (GPR or FPR).
    uint16_t idOrPhys{0U};       ///< Virtual reg ID or PhysReg enum value.
};

/**
 * @brief Tagged operand for a machine IR instruction.
 *
 * Exactly one payload is semantically active according to `kind`. The
 * condition payload is non-owning; labels own their string storage.
 */
struct MOperand {
    enum class Kind {
        Reg,  ///< Physical or virtual register.
        Imm,  ///< Immediate constant.
        Cond, ///< Condition code (eq, ne, lt, etc.).
        Label ///< Symbol or basic block label.
    } kind{Kind::Imm};

    MReg reg{};                ///< Register operand (when kind==Reg).
    long long imm{0};          ///< Immediate value (when kind==Imm).
    const char *cond{nullptr}; ///< Condition code string (when kind==Cond).
    std::string label;         ///< Label name (when kind==Label).

    /**
     * @brief Creates an operand naming a physical register.
     *
     * @param r AArch64 physical register.
     * @return Register operand with its class inferred from @p r.
     */
    static MOperand regOp(PhysReg r) {
        MOperand o{};
        o.kind = Kind::Reg;
        o.reg.isPhys = true;
        o.reg.cls = isGPR(r) ? RegClass::GPR : RegClass::FPR;
        o.reg.idOrPhys = static_cast<uint16_t>(r);
        return o;
    }

    /**
     * @brief Creates an operand naming a virtual register.
     *
     * @param cls Register bank required by the value.
     * @param id Function-local virtual-register identifier.
     * @return Register operand retaining @p cls and @p id.
     */
    static MOperand vregOp(RegClass cls, uint16_t id) {
        MOperand o{};
        o.kind = Kind::Reg;
        o.reg.isPhys = false;
        o.reg.cls = cls;
        o.reg.idOrPhys = id;
        return o;
    }

    /**
     * @brief Creates a signed immediate operand.
     *
     * @param v Immediate value before any opcode-specific range checking.
     * @return Immediate operand containing @p v.
     */
    static MOperand immOp(long long v) {
        MOperand o{};
        o.kind = Kind::Imm;
        o.imm = v;
        return o;
    }

    /**
     * @brief Creates a non-owning condition-code operand.
     *
     * @param c Null-terminated condition spelling such as `"eq"` or `"lt"`;
     *        may be `nullptr` to represent an unavailable condition.
     * @return Condition operand that stores @p c without copying it.
     * @warning A non-null @p c must outlive every use of the returned operand.
     */
    static MOperand condOp(const char *c) {
        MOperand o{};
        o.kind = Kind::Cond;
        o.cond = c;
        return o;
    }

    /**
     * @brief Creates an owning symbol or basic-block label operand.
     *
     * @param name Label text, moved into the returned operand.
     * @return Label operand owning the supplied string.
     */
    static MOperand labelOp(std::string name) {
        MOperand o{};
        o.kind = Kind::Label;
        o.label = std::move(name);
        return o;
    }
};

/**
 * @brief Represents one target-specific machine IR instruction.
 *
 * Operand arity and interpretation are determined by `opc`; defining
 * operands conventionally precede uses. `loc` carries optional source
 * provenance for diagnostics and debug information.
 */
struct MInstr {
    MOpcode opc{};                ///< The operation to perform.
    std::vector<MOperand> ops{};  ///< Instruction operands.
    il::support::SourceLoc loc{}; ///< Source location (for debug info).
};

/**
 * @brief Owns the ordered MIR instructions for a single-entry basic block.
 *
 * The block's name is the branch target emitted for it. Register allocation
 * may additionally annotate physical registers whose values flow directly
 * into a single-predecessor successor.
 */
struct MBasicBlock {
    std::string name;           ///< Block label (used for branches).
    std::vector<MInstr> instrs; ///< Instructions in program order.

    /// Physical registers (PhysReg values) the register allocator may carry
    /// live across this block's exit WITHOUT any instruction in this block
    /// referencing them: a single-predecessor successor can re-adopt a value
    /// directly from the register it occupied at this block's end. Post-RA
    /// block-local rewrites must treat these registers as live-out — no
    /// instruction marks their liveness. Populated during allocation; sorted.
    std::vector<uint16_t> carriedExitRegs;
};

/**
 * @brief Owns an AArch64 MIR function and its finalized frame metadata.
 *
 * Blocks remain in layout order. Lowering populates stack objects, while
 * register allocation records preserved physical registers and may update
 * leaf status.
 */
struct MFunction {
    std::string name;                ///< Function symbol name.
    std::vector<MBasicBlock> blocks; ///< Basic blocks in layout order.

    /// True when the function contains no Bl or Blr instructions (no calls).
    /// Leaf functions can skip saving/restoring the link register (x30) and
    /// frame pointer (x29) when no callee-saved registers are used and the
    /// local frame is empty.
    bool isLeaf{true};

    /// Callee-saved GPRs that must be preserved across calls.
    std::vector<PhysReg> savedGPRs;

    /// Callee-saved FPRs (D-registers) that must be preserved across calls.
    std::vector<PhysReg> savedFPRs;

    /// Total size of the local stack frame in bytes (16-byte aligned).
    int localFrameSize{0};

    /// @brief Describes one addressable IL local in the stack frame.
    struct StackLocal {
        unsigned tempId{0}; ///< IL temporary ID this slot is for.
        int size{0};        ///< Size in bytes.
        int align{8};       ///< Alignment requirement.
        int offset{0};      ///< FP-relative offset (negative).
    };

    /// @brief Describes one allocator or lowering spill slot in the stack frame.
    struct SpillSlot {
        uint32_t vreg{0}; ///< Virtual register or spill-key identifier.
        int size{8};      ///< Size in bytes.
        int align{8};     ///< Alignment requirement.
        int offset{0};    ///< FP-relative offset (negative).
    };

    /**
     * @brief Owns local/spill placements and aggregate frame-size metadata.
     *
     * Valid local and spill offsets are negative relative to the frame pointer;
     * zero is therefore available as a lookup sentinel.
     */
    struct FrameLayout {
        std::vector<StackLocal> locals; ///< Local variable slots.
        std::vector<SpillSlot> spills;  ///< Spill slots for virtual registers.
        int totalBytes{0};              ///< Total frame size (aligned to 16 bytes).
        int maxOutgoingBytes{0};        ///< Space reserved for outgoing call arguments.

        /**
         * @brief Looks up an IL local's frame-pointer-relative offset.
         *
         * @param tempId IL temporary identifier associated with the local.
         * @return The first matching negative offset, or `0` when no local has
         *         @p tempId.
         */
        int getLocalOffset(unsigned tempId) const {
            for (const auto &L : locals)
                if (L.tempId == tempId)
                    return L.offset;
            return 0;
        }
    } frame; ///< Stack frame layout.
};

// -----------------------------------------------------------------------------
// Pretty printing helpers (for debugging only)
// -----------------------------------------------------------------------------

/**
 * @brief Maps a MIR opcode to its generated enumerator spelling.
 *
 * @param opc Opcode to identify.
 * @return Pointer to static storage containing the opcode name, or
 *         `"<unknown>"` for a value outside the generated cases.
 */
[[nodiscard]] const char *opcodeName(MOpcode opc) noexcept;

/**
 * @brief Maps a register class to its debug-output suffix.
 *
 * @param cls Register class to identify.
 * @return `"gpr"`, `"fpr"`, or `"unknown"`; the returned pointer has static
 *         storage duration.
 */
[[nodiscard]] const char *regClassSuffix(RegClass cls) noexcept;

/**
 * @brief Renders a machine register in diagnostic MIR notation.
 *
 * @param reg Register to render.
 * @return Owned text such as `%v12:gpr` or `@x0:gpr`.
 */
[[nodiscard]] std::string toString(const MReg &reg);

/**
 * @brief Renders an arbitrary MIR operand in diagnostic notation.
 *
 * @param op Operand to render according to its active kind.
 * @return Owned register, immediate, condition, or label text.
 */
[[nodiscard]] std::string toString(const MOperand &op);

/**
 * @brief Renders a MIR instruction without a trailing newline.
 *
 * @param instr Instruction whose operands are emitted in storage order.
 * @return Opcode name followed by comma-separated operands.
 */
[[nodiscard]] std::string toString(const MInstr &instr);

/**
 * @brief Renders a labeled MIR basic block.
 *
 * @param block Block to render.
 * @return Label and indented instructions, with a newline after every line.
 */
[[nodiscard]] std::string toString(const MBasicBlock &block);

/**
 * @brief Renders a complete MIR function for diagnostics.
 *
 * @param func Function whose blocks are rendered in layout order.
 * @return Owned multi-line textual representation.
 */
[[nodiscard]] std::string toString(const MFunction &func);

} // namespace zanna::codegen::aarch64
