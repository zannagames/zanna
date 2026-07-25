//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/MachineIR.cpp
// Purpose: Provide the concrete implementations for the lightweight Machine IR
//          types used by the x86-64 code generator.
// Key invariants:
//   - Helpers preserve operand invariants declared in the headers.
//   - Never fabricate invalid register classes.
//   - Maintains the stable textual form relied upon by debugging utilities.
// Ownership/Lifetime:
//   - All Machine IR nodes own their operands by value; helper functions
//     return new values without introducing aliasing.
// Links: src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

#include "MachineIR.hpp"

#include "codegen/common/LabelUtil.hpp"

#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

/**
 * @file
 * @brief Implements x86-64 MIR construction, validation, labels, and formatting.
 *
 * Memory operands are validated against SIB restrictions at construction time.
 * Function-local labels incorporate a sanitized function stem for module-wide
 * textual uniqueness, and deterministic stringification exposes every operand,
 * instruction, block, and function for diagnostics and debugging.
 */

namespace zanna::codegen::x64 {

namespace {
/// @brief Map a Machine IR opcode to a descriptive string for diagnostics.
/// @param opc Opcode enumerator to translate.
/// @return Null-terminated string naming the opcode.
[[nodiscard]] const char *opcodeName(MOpcode opc) noexcept {
    switch (opc) {
        case MOpcode::PUSH:
            return "PUSH";
        case MOpcode::POP:
            return "POP";
        case MOpcode::MOVrr:
            return "MOVrr";
        case MOpcode::MOVri:
            return "MOVri";
        case MOpcode::MOVrm:
            return "MOVrm";
        case MOpcode::MOVmr:
            return "MOVmr";
        case MOpcode::CMOVNErr:
            return "CMOVNErr";
        case MOpcode::SELECT_GPR:
            return "SELECT_GPR";
        case MOpcode::SELECT_XMM:
            return "SELECT_XMM";
        case MOpcode::LEA:
            return "LEA";
        case MOpcode::ADDrr:
            return "ADDrr";
        case MOpcode::ADDri:
            return "ADDri";
        case MOpcode::ANDrr:
            return "ANDrr";
        case MOpcode::ANDri:
            return "ANDri";
        case MOpcode::ORrr:
            return "ORrr";
        case MOpcode::ORri:
            return "ORri";
        case MOpcode::XORrr:
            return "XORrr";
        case MOpcode::XORri:
            return "XORri";
        case MOpcode::SUBrr:
            return "SUBrr";
        case MOpcode::SHLri:
            return "SHLri";
        case MOpcode::SHLrc:
            return "SHLrc";
        case MOpcode::SHRri:
            return "SHRri";
        case MOpcode::SHRrc:
            return "SHRrc";
        case MOpcode::SARri:
            return "SARri";
        case MOpcode::SARrc:
            return "SARrc";
        case MOpcode::IMULrr:
            return "IMULrr";
        case MOpcode::DIVS64rr:
            return "DIVS64rr";
        case MOpcode::REMS64rr:
            return "REMS64rr";
        case MOpcode::DIVS64Chk0rr:
            return "DIVS64Chk0rr";
        case MOpcode::REMS64Chk0rr:
            return "REMS64Chk0rr";
        case MOpcode::DIVU64rr:
            return "DIVU64rr";
        case MOpcode::REMU64rr:
            return "REMU64rr";
        case MOpcode::DIVU64Chk0rr:
            return "DIVU64Chk0rr";
        case MOpcode::REMU64Chk0rr:
            return "REMU64Chk0rr";
        case MOpcode::CQO:
            return "CQO";
        case MOpcode::IDIVrm:
            return "IDIVrm";
        case MOpcode::MULr:
            return "MULr";
        case MOpcode::IMULr:
            return "IMULr";
        case MOpcode::DIVrm:
            return "DIVrm";
        case MOpcode::XORrr32:
            return "XORrr32";
        case MOpcode::CMPrr:
            return "CMPrr";
        case MOpcode::CMPri:
            return "CMPri";
        case MOpcode::SETcc:
            return "SETcc";
        case MOpcode::MOVZXrr8:
            return "MOVZXrr8";
        case MOpcode::MOVZXrr32:
            return "MOVZXrr32";
        case MOpcode::ADDrr32:
            return "ADDrr32";
        case MOpcode::SUBrr32:
            return "SUBrr32";
        case MOpcode::IMULrr32:
            return "IMULrr32";
        case MOpcode::ADDri32:
            return "ADDri32";
        case MOpcode::CMPrr32:
            return "CMPrr32";
        case MOpcode::MOVSXD:
            return "MOVSXD";
        case MOpcode::ADDrr16:
            return "ADDrr16";
        case MOpcode::SUBrr16:
            return "SUBrr16";
        case MOpcode::IMULrr16:
            return "IMULrr16";
        case MOpcode::ADDri16:
            return "ADDri16";
        case MOpcode::MOVSXrr16:
            return "MOVSXrr16";
        case MOpcode::ADDrm:
            return "ADDrm";
        case MOpcode::SUBrm:
            return "SUBrm";
        case MOpcode::ANDrm:
            return "ANDrm";
        case MOpcode::ORrm:
            return "ORrm";
        case MOpcode::XORrm:
            return "XORrm";
        case MOpcode::CMPrm:
            return "CMPrm";
        case MOpcode::IMULrm:
            return "IMULrm";
        case MOpcode::JUMPTABLE:
            return "JUMPTABLE";
        case MOpcode::TESTrr:
            return "TESTrr";
        case MOpcode::JMP:
            return "JMP";
        case MOpcode::JCC:
            return "JCC";
        case MOpcode::LABEL:
            return "LABEL";
        case MOpcode::CALL:
            return "CALL";
        case MOpcode::UD2:
            return "UD2"; // traps hard alignment failures
        case MOpcode::RET:
            return "RET";
        case MOpcode::PX_COPY:
            return "PX_COPY";
        case MOpcode::FADD:
            return "FADD";
        case MOpcode::FSUB:
            return "FSUB";
        case MOpcode::FMUL:
            return "FMUL";
        case MOpcode::FDIV:
            return "FDIV";
        case MOpcode::UCOMIS:
            return "UCOMIS";
        case MOpcode::CVTSI2SD:
            return "CVTSI2SD";
        case MOpcode::CVTTSD2SI:
            return "CVTTSD2SI";
        case MOpcode::MOVQrx:
            return "MOVQrx";
        case MOpcode::MOVQxr:
            return "MOVQxr";
        case MOpcode::MOVSDrr:
            return "MOVSDrr";
        case MOpcode::MOVSDrm:
            return "MOVSDrm";
        case MOpcode::MOVSDmr:
            return "MOVSDmr";
        case MOpcode::MOVUPSrm:
            return "MOVUPSrm";
        case MOpcode::MOVUPSmr:
            return "MOVUPSmr";
        case MOpcode::ADDOvfrr:
            return "ADDOvfrr";
        case MOpcode::SUBOvfrr:
            return "SUBOvfrr";
        case MOpcode::IMULOvfrr:
            return "IMULOvfrr";
    }
    return "<unknown>";
}

/// @brief Map a register class to the textual suffix used in debug output.
/// @param cls Register class enumerator to translate.
/// @return Short suffix identifying the class (e.g., "gpr").
[[nodiscard]] std::string_view regClassSuffix(RegClass cls) noexcept {
    switch (cls) {
        case RegClass::GPR:
            return "gpr";
        case RegClass::XMM:
            return "xmm";
    }
    return "unknown";
}

/// @brief Reject non-GPR registers used in memory-operand base/index slots.
/// @details x86-64 addressing modes can only consume GPR base and index
///          registers; XMM cannot serve as either. We throw rather than
///          assert so misuses produced by upstream lowering are reportable
///          via the diagnostic path instead of crashing the compiler.
/// @param reg Register being placed in the memory operand.
/// @param role @c "base" or @c "index", used to build the error message.
/// @throws std::invalid_argument when @p reg has a non-GPR class.
void validateMemReg(const OpReg &reg, const char *role) {
    if (reg.cls != RegClass::GPR) {
        throw std::invalid_argument(std::string("x86-64 memory operand ") + role +
                                    " register must be GPR, got " +
                                    std::string(regClassSuffix(reg.cls)));
    }
}

/// @brief Validate a SIB scale value against the architectural set {1,2,4,8}.
/// @details SIB encoding has only two bits for the scale, so any other value
///          would silently truncate. Throw to surface programmer errors.
/// @param scale Scale to validate.
/// @throws std::invalid_argument for any value other than 1/2/4/8.
void validateMemScale(uint8_t scale) {
    switch (scale) {
        case 1:
        case 2:
        case 4:
        case 8:
            return;
        default:
            throw std::invalid_argument("x86-64 memory operand scale must be one of 1, 2, 4, or 8");
    }
}

/// @brief Detect whether @p reg refers to the architectural @c %rsp register.
/// @details @c %rsp cannot serve as an index register in SIB encoding (its
///          slot is reserved as "no index"); callers use this predicate to
///          reject such operands at construction time.
/// @param reg Register operand to inspect.
/// @return True when @p reg is physical @c %rsp.
bool isRspIndex(const OpReg &reg) noexcept {
    return reg.isPhys && reg.cls == RegClass::GPR &&
           static_cast<PhysReg>(reg.idOrPhys) == PhysReg::RSP;
}
} // namespace

/// @brief Construct an instruction by pairing an opcode with operands.
/// @details Returns a value-based instruction object, transferring ownership of
///          the supplied operand list.
/// @param opc Opcode describing the instruction semantics.
/// @param ops Operand vector consumed by the instruction.
/// @return Newly constructed Machine IR instruction.
MInstr MInstr::make(MOpcode opc, std::vector<Operand> ops) {
    MInstr instr{};
    instr.opcode = opc;
    instr.operands = std::move(ops);
    return instr;
}

/// @brief Convenience overload that accepts an initializer list of operands.
/// @param opc Opcode describing the instruction semantics.
/// @param ops Operand initializer list copied into the instruction.
/// @return Newly constructed Machine IR instruction.
MInstr MInstr::make(MOpcode opc, std::initializer_list<Operand> ops) {
    MInstr instr{};
    instr.opcode = opc;
    instr.operands = std::vector<Operand>{ops};
    return instr;
}

/// @brief Append an operand to the instruction.
/// @details Operands are stored by value, so the provided operand is moved into
///          the instruction's operand array.
/// @param op Operand to append.
/// @return Reference to the mutated instruction for chaining.
MInstr &MInstr::addOperand(Operand op) {
    operands.push_back(std::move(op));
    return *this;
}

/// @brief Append a basic block to the function.
/// @details The block is moved into the function's block list and a reference
///          to the stored instance is returned for immediate population.
/// @param block Basic block to append.
/// @return Reference to the inserted block.
MBasicBlock &MFunction::addBlock(MBasicBlock block) {
    blocks.push_back(std::move(block));
    return blocks.back();
}

/// @brief Generate an assembler-local label that is unique across this module's functions.
/// @param prefix Prefix to prepend to the generated label.
/// @return New label string guaranteed unique across the emitted assembly module.
std::string MFunction::makeLocalLabel(std::string_view prefix) {
    std::string label;
    const std::string functionStem = common::sanitizeLabel(name);
    label.reserve(prefix.size() + functionStem.size() + 14);
    label = prefix;
    label += '.';
    label += functionStem;
    label += '.';
    label += std::to_string(localLabelCounter++);
    return label;
}

/// @brief Append an instruction to the basic block.
/// @param instr Instruction to append.
/// @return Reference to the stored instruction for further mutation.
MInstr &MBasicBlock::append(MInstr instr) {
    instructions.push_back(std::move(instr));
    return instructions.back();
}

/// @brief Create a virtual register operand descriptor.
/// @param cls Register class of the virtual register.
/// @param id Dense identifier assigned to the virtual register.
/// @return Register descriptor marking the operand as virtual.
OpReg makeVReg(RegClass cls, uint16_t id) noexcept {
    OpReg result{};
    result.isPhys = false;
    result.cls = cls;
    result.idOrPhys = id;
    return result;
}

/// @brief Create a physical register operand descriptor.
/// @param cls Register class of the physical register.
/// @param phys Architecture-defined register number.
/// @return Register descriptor marking the operand as physical.
OpReg makePhysReg(RegClass cls, uint16_t phys) noexcept {
    OpReg result{};
    result.isPhys = true;
    result.cls = cls;
    result.idOrPhys = phys;
    return result;
}

/// @brief Wrap a virtual register descriptor inside a generic operand variant.
/// @param cls Register class of the virtual register.
/// @param id Dense identifier assigned to the virtual register.
/// @return Operand variant representing the register.
Operand makeVRegOperand(RegClass cls, uint16_t id) {
    return Operand{makeVReg(cls, id)};
}

/// @brief Wrap a physical register descriptor inside a generic operand variant.
/// @param cls Register class of the physical register.
/// @param phys Architecture-defined register number.
/// @return Operand variant representing the register.
Operand makePhysRegOperand(RegClass cls, uint16_t phys) {
    return Operand{makePhysReg(cls, phys)};
}

/// @brief Construct an immediate operand variant.
/// @param value Immediate constant carried by the operand.
/// @return Operand variant storing the constant.
Operand makeImmOperand(int64_t value) {
    return Operand{OpImm{value}};
}

/// @brief Construct a memory operand referencing @p base with displacement.
/// @param base Base register used to form the address.
/// @param disp Signed displacement added to the base register.
/// @return Operand variant describing the memory access.
Operand makeMemOperand(OpReg base, int32_t disp) {
    validateMemReg(base, "base");
    OpMem m{};
    m.base = std::move(base);
    m.disp = disp;
    return Operand{m};
}

/// @brief Construct a SIB-form memory operand.
/// @details Builds an indexed memory operand with the canonical
///          @c [base + index*scale + disp] shape. Both registers are
///          validated to be GPRs, @p scale is checked against the legal
///          set, and @c %rsp is forbidden as an index.
/// @param base Base register.
/// @param index Index register (cannot be @c %rsp).
/// @param scale Architectural scale (1/2/4/8).
/// @param disp Signed 32-bit displacement.
/// @return @c Operand variant wrapping the constructed @c OpMem.
Operand makeMemOperand(OpReg base, OpReg index, uint8_t scale, int32_t disp) {
    validateMemReg(base, "base");
    validateMemReg(index, "index");
    validateMemScale(scale);
    if (isRspIndex(index)) {
        throw std::invalid_argument("x86-64 memory operand index register cannot be %rsp");
    }

    OpMem m{};
    m.base = std::move(base);
    m.index = std::move(index);
    m.scale = scale;
    m.disp = disp;
    m.hasIndex = true;
    return Operand{m};
}

/// @brief Construct a label operand variant.
/// @param name Name of the label referenced by the operand.
/// @return Operand variant storing the label metadata.
Operand makeLabelOperand(std::string name) {
    return Operand{OpLabel{std::move(name)}};
}

/// @brief Construct a RIP-relative label operand variant.
/// @param name Name of the label referenced by the operand.
/// @return Operand variant storing the RIP-relative label metadata.
Operand makeRipLabelOperand(std::string name) {
    return Operand{OpRipLabel{std::move(name)}};
}

/// @brief Render a register operand as human-readable text.
/// @details Physical registers are prefixed with '@' and virtual registers with
///          `%v` to aid debugging dumps.
/// @param op Register operand to print.
/// @return Textual representation of the register operand.
std::string toString(const OpReg &op) {
    std::ostringstream os;
    if (op.isPhys) {
        const auto phys = static_cast<PhysReg>(op.idOrPhys);
        os << '@' << regName(phys);
    } else {
        os << "%v" << static_cast<unsigned>(op.idOrPhys);
    }
    os << ':' << regClassSuffix(op.cls);
    return os.str();
}

/// @brief Render an immediate operand as human-readable text.
/// @param op Immediate operand to print.
/// @return Textual representation prefixed with '#'.
std::string toString(const OpImm &op) {
    std::ostringstream os;
    os << '#' << op.val;
    return os.str();
}

/// @brief Render a memory operand as human-readable text.
/// @details Produces the canonical `[base +/- disp]` representation.
/// @param op Memory operand to print.
/// @return Textual representation of the memory operand.
std::string toString(const OpMem &op) {
    std::ostringstream os;
    os << '[' << toString(op.base);
    if (op.hasIndex) {
        os << " + " << toString(op.index);
        if (op.scale != 1) {
            os << " * " << static_cast<int>(op.scale);
        }
    }
    if (op.disp != 0) {
        if (op.disp > 0)
            os << " + " << op.disp;
        else
            os << " - " << -op.disp;
    }
    os << ']';
    return os.str();
}

/// @brief Render a label operand as human-readable text.
/// @param op Label operand to print.
/// @return Underlying label name.
std::string toString(const OpLabel &op) {
    return op.name;
}

/// @brief Render a RIP-relative label operand as human-readable text.
/// @param op RIP-relative label operand to print.
/// @return Label name followed by the (%rip) suffix.
std::string toString(const OpRipLabel &op) {
    std::string result = op.name;
    result += "(%rip)";
    return result;
}

/// @brief Render a generic operand by visiting the active variant.
/// @param operand Operand variant to print.
/// @return Textual representation selected by the active operand kind.
std::string toString(const Operand &operand) {
    /// Dispatch formatting to the active operand alternative.
    return std::visit([](const auto &value) { return toString(value); }, operand);
}

/// @brief Render an instruction, including mnemonic and operands.
/// @param instr Instruction to print.
/// @return Multi-operand textual representation suitable for dumps.
std::string toString(const MInstr &instr) {
    std::ostringstream os;
    os << opcodeName(instr.opcode);
    bool first = true;
    for (const auto &operand : instr.operands) {
        if (first) {
            os << ' ' << toString(operand);
            first = false;
        } else {
            os << ", " << toString(operand);
        }
    }
    return os.str();
}

/// @brief Render a basic block and its instructions as human-readable text.
/// @param block Basic block to print.
/// @return String containing the label and formatted instruction lines.
std::string toString(const MBasicBlock &block) {
    std::ostringstream os;
    os << block.label << ":\n";
    for (const auto &inst : block.instructions) {
        os << "  " << toString(inst) << '\n';
    }
    return os.str();
}

/// @brief Render an entire Machine IR function for debugging.
/// @details Prints the function header, vararg marker, and each basic block in
///          order using @ref toString(const MBasicBlock &).
/// @param func Function to print.
/// @return String containing the formatted function.
std::string toString(const MFunction &func) {
    std::ostringstream os;
    os << "function " << func.name;
    if (func.metadata.isVarArg) {
        os << " (vararg)";
    }
    os << "\n";
    for (const auto &block : func.blocks) {
        os << toString(block);
    }
    return os.str();
}

} // namespace zanna::codegen::x64
