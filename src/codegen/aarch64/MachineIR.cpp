//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/MachineIR.cpp
// Purpose: AArch64 Machine IR pretty-printing and opcode name table.
// Key invariants:
//   - All toString() functions are debug-only; they are not called on hot paths.
//   - opcodeName() is generated from MOpcodeDef.inc to stay in sync with the enum.
// Ownership/Lifetime:
//   - Returns owned std::string values; callers are responsible for lifetime.
// Links: src/codegen/aarch64/MachineIR.hpp,
//        src/codegen/aarch64/MOpcodeDef.inc
//
//===----------------------------------------------------------------------===//

#include "MachineIR.hpp"
#include "TargetAArch64.hpp"

#include <sstream>

/**
 * @file
 * @brief Implements stable names and human-readable rendering for AArch64 MIR.
 *
 * These helpers intentionally expose the target MIR's stored structure rather
 * than assembler syntax. Their output is used for diagnostics and pass dumps;
 * returned strings own their storage, while generated opcode and register
 * class names point to static string literals.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Maps a machine IR opcode to its generated diagnostic name.
 *
 * @param opc Opcode enumerator to identify.
 * @return Static enumerator spelling, or `"<unknown>"` if @p opc is invalid.
 */
[[nodiscard]] const char *opcodeName(MOpcode opc) noexcept {
    switch (opc) {
#define ZANNA_MIR_OPCODE(name)                                                                     \
    case MOpcode::name:                                                                            \
        return #name;
#include "codegen/aarch64/MOpcodeDef.inc"
    }
    return "<unknown>";
}

/**
 * @brief Maps a register bank to the suffix used by MIR register rendering.
 *
 * @param cls Register class to identify.
 * @return Static `"gpr"`, `"fpr"`, or fallback `"unknown"` text.
 */
[[nodiscard]] const char *regClassSuffix(RegClass cls) noexcept {
    switch (cls) {
        case RegClass::GPR:
            return "gpr";
        case RegClass::FPR:
            return "fpr";
    }
    return "unknown";
}

/**
 * @brief Renders a physical or virtual register and its register class.
 *
 * @param reg Register descriptor to render.
 * @return Owned MIR notation using an `@` physical or `%v` virtual prefix.
 */
std::string toString(const MReg &reg) {
    std::ostringstream os;
    if (reg.isPhys) {
        const auto phys = static_cast<PhysReg>(reg.idOrPhys);
        os << '@' << regName(phys);
    } else {
        os << "%v" << static_cast<unsigned>(reg.idOrPhys);
    }
    os << ':' << regClassSuffix(reg.cls);
    return os.str();
}

/**
 * @brief Renders the payload selected by a MIR operand's kind tag.
 *
 * @param op Operand to render.
 * @return Owned operand text; null conditions and invalid kinds receive
 *         diagnostic placeholders.
 */
std::string toString(const MOperand &op) {
    switch (op.kind) {
        case MOperand::Kind::Reg:
            return toString(op.reg);
        case MOperand::Kind::Imm: {
            std::ostringstream os;
            os << '#' << op.imm;
            return os.str();
        }
        case MOperand::Kind::Cond:
            return op.cond ? op.cond : "<cond?>";
        case MOperand::Kind::Label:
            return op.label;
    }
    return "<unknown>";
}

/**
 * @brief Renders an opcode followed by its comma-separated operands.
 *
 * @param instr Instruction to render without mutation.
 * @return Owned single-line text without a trailing newline.
 */
std::string toString(const MInstr &instr) {
    std::ostringstream os;
    os << opcodeName(instr.opc);
    bool first = true;
    for (const auto &op : instr.ops) {
        if (first) {
            os << ' ' << toString(op);
            first = false;
        } else {
            os << ", " << toString(op);
        }
    }
    return os.str();
}

/**
 * @brief Renders a block label and each instruction on an indented line.
 *
 * @param block Basic block to render in instruction order.
 * @return Owned block text ending in a newline.
 */
std::string toString(const MBasicBlock &block) {
    std::ostringstream os;
    os << block.name << ":\n";
    for (const auto &instr : block.instrs) {
        os << "  " << toString(instr) << '\n';
    }
    return os.str();
}

/**
 * @brief Renders a function heading followed by its blocks in layout order.
 *
 * @param func MIR function to render.
 * @return Owned multi-line function dump.
 */
std::string toString(const MFunction &func) {
    std::ostringstream os;
    os << "function " << func.name << '\n';
    for (const auto &block : func.blocks) {
        os << toString(block);
    }
    return os.str();
}

} // namespace zanna::codegen::aarch64
