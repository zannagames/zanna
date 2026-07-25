//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/AsmEmitter.hpp
// Purpose: Declare the x86-64 assembly emitter for Machine IR to AT&T syntax.
//
// Assembly format: GNU Assembler (GAS) AT&T syntax only (LOW-2).
//   - Operand order: source, destination  (mov $1, %rax — not Intel src,dst reversal)
//   - Register names: %-prefixed (%rax, %xmm0)
//   - Immediate values: $-prefixed ($42)
//   - Memory references: disp(base, index, scale)
// The emitted .s files are intended for consumption by `clang`/`gcc`/`as`.
// MASM (ml64.exe) and NASM (.asm) output is not supported.
//
// Key invariants:
//   - Emission preserves operand ordering and branch destinations.
//   - Encoding rows are matched deterministically by opcode and operand pattern.
//   - Rodata labels are unique per literal kind.
// Ownership/Lifetime:
//   - AsmEmitter holds a non-owning reference to a mutable rodata pool.
//   - Emit methods write to ostream and do not retain state.
// Links: codegen/x86_64/AsmEmitter.cpp,
//        codegen/x86_64/MachineIR.hpp,
//        codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file AsmEmitter.hpp
 * @brief Declares AT&T-syntax x86-64 Machine IR and literal-pool emission.
 *
 * Encoding-table rows drive ordinary instruction text, while specialized
 * formatters enforce width, condition, branch, call, and object-format symbol
 * conventions.
 */

#pragma once

#include "MachineIR.hpp"
#include "codegen/common/objfile/ObjectFileWriter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::x64 {

enum class OperandOrder;
enum class OperandKind;
enum class EncodingForm;
enum class EncodingFlag : std::uint32_t;
struct OperandPattern;
struct EncodingRow;

/// @brief Emits AT&T-style assembly for Machine IR functions and literal pools.
/// @details Includes support for synthetic jump tables, trap sequences, narrow
///          register aliases, runtime-name mapping, and per-format symbol spelling.
class AsmEmitter {
  public:
    /// @brief Interned module-level string and f64 literal pool.
    class RoDataPool {
      public:
        /// @brief Intern a byte string literal.
        /// @param bytes Exact payload, including any embedded NUL bytes.
        /// @return Stable canonical pool index.
        [[nodiscard]] int addStringLiteral(std::string bytes);

        /// @brief Intern a 64-bit floating-point literal by bit pattern.
        /// @param value Value to intern.
        /// @return Stable canonical pool index.
        [[nodiscard]] int addF64Literal(double value);

        /// @brief Retrieve the canonical label for a stored string literal.
        /// @param index Index returned by @ref addStringLiteral.
        /// @return `.LC_str_<index>`.
        [[nodiscard]] std::string stringLabel(int index) const;

        /// @brief Retrieve the byte length of a stored string literal.
        /// @param index Index returned by @ref addStringLiteral.
        /// @return Exact payload length.
        /// @throws std::out_of_range for a negative or invalid index.
        [[nodiscard]] std::size_t stringByteLength(int index) const;

        /// @brief Retrieve the canonical label for a stored f64 literal.
        /// @param index Index returned by @ref addF64Literal.
        /// @return `.LC_f64_<index>`.
        [[nodiscard]] std::string f64Label(int index) const;

        /// @brief Emit the target-format read-only-data section.
        /// @param os Destination assembly stream.
        /// @param format Object format selecting section and symbol syntax.
        void emit(std::ostream &os, objfile::ObjFormat format) const;

        /// @brief Determine whether the pool holds no literals.
        /// @return `true` when both literal sequences are empty.
        [[nodiscard]] bool empty() const noexcept;

        /// @brief Return the number of canonical string literals.
        /// @return String-literal count.
        [[nodiscard]] std::size_t stringCount() const noexcept {
            return stringLiterals_.size();
        }

        /// @brief Return the number of canonical f64 literals.
        /// @return Floating-literal count.
        [[nodiscard]] std::size_t f64Count() const noexcept {
            return f64Literals_.size();
        }

        /// @brief Access raw bytes for a stored string literal.
        /// @param index Valid nonnegative pool index.
        /// @return Immutable payload reference.
        [[nodiscard]] const std::string &stringBytes(int index) const {
            return stringLiterals_[static_cast<std::size_t>(index)];
        }

        /// @brief Access a stored f64 literal.
        /// @param index Valid nonnegative pool index.
        /// @return Stored floating value.
        [[nodiscard]] double f64Value(int index) const {
            return f64Literals_[static_cast<std::size_t>(index)];
        }

      private:
        std::vector<std::string> stringLiterals_{};
        std::vector<std::size_t> stringLengths_{};
        std::vector<double> f64Literals_{};
        std::unordered_map<std::string, int> stringLookup_{};
        std::unordered_map<std::uint64_t, int> f64Lookup_{};
    };

    /// @brief Construct an emitter borrowing a literal pool.
    /// @param pool Pool that must outlive the emitter.
    /// @param format Object format controlling symbols and section directives.
    explicit AsmEmitter(RoDataPool &pool,
                        objfile::ObjFormat format = objfile::detectHostFormat()) noexcept;

    /// @brief Emit one complete Machine IR function.
    /// @param os Destination assembly stream.
    /// @param func Function to emit.
    /// @param target Target register and ABI information.
    void emitFunction(std::ostream &os, const MFunction &func, const TargetInfo &target) const;

    /// @brief Emit the module-level literal pool when nonempty.
    /// @param os Destination assembly stream.
    void emitRoData(std::ostream &os) const;

    /// @brief Access the borrowed literal pool.
    /// @return Mutable pool reference.
    [[maybe_unused]] [[nodiscard]] RoDataPool &roDataPool() noexcept;

    /// @brief Access the borrowed literal pool.
    /// @return Immutable pool reference.
    [[maybe_unused]] [[nodiscard]] const RoDataPool &roDataPool() const noexcept;

  private:
    RoDataPool *pool_{nullptr};
    objfile::ObjFormat format_{objfile::detectHostFormat()};

    /// \brief Emit assembly for a single basic block including its label.
    /// \param os Output stream for AT&T syntax assembly.
    /// \param block Machine basic block to emit.
    /// \param target Target information for register naming.
    /// @param format Object format controlling symbol spelling.
    static void emitBlock(std::ostream &os,
                          const MBasicBlock &block,
                          const TargetInfo &target,
                          objfile::ObjFormat format);

    /// \brief Emit assembly for a single machine instruction.
    /// \param os Output stream for AT&T syntax assembly.
    /// \param instr Machine instruction to emit.
    /// \param target Target information for register naming.
    /// @param format Object format controlling symbol spelling.
    static void emitInstruction(std::ostream &os,
                                const MInstr &instr,
                                const TargetInfo &target,
                                objfile::ObjFormat format);

    /// \brief Emit instruction text from a matched encoding row.
    /// \param row Encoding specification describing mnemonic and operand ordering.
    /// \param operands Instruction operands to format.
    /// \param os Output stream for assembly text.
    /// \param target Target information for register naming.
    /// @param format Object format controlling symbol spelling.
    static void emit_from_row(const EncodingRow &row,
                              std::span<const Operand> operands,
                              std::ostream &os,
                              const TargetInfo &target,
                              objfile::ObjFormat format);

    /// \brief Format an operand for AT&T assembly output.
    /// \param operand Operand to format (reg, imm, mem, or label).
    /// \param target Target information for register naming.
    /// @param format Object format controlling symbol spelling.
    /// \return Formatted string (e.g., "%rax", "$42", "8(%rbp)").
    [[nodiscard]] static std::string formatOperand(const Operand &operand,
                                                   const TargetInfo &target,
                                                   objfile::ObjFormat format);

    /// \brief Format a 64-bit register operand.
    /// \param reg Register operand.
    /// \param target Target information for register naming.
    /// \return AT&T format register (e.g., "%rax", "%r8").
    [[nodiscard]] static std::string formatReg(const OpReg &reg, const TargetInfo &target);

    /// \brief Format a register as its 8-bit low byte variant.
    /// \param reg Register operand.
    /// \param target Target information for register naming.
    /// \return AT&T format 8-bit register (e.g., "%al", "%r8b").
    [[nodiscard]] static std::string formatReg8(const OpReg &reg, const TargetInfo &target);

    /// \brief Format a register as its 32-bit variant.
    /// \param reg Register operand.
    /// \param target Target information for register naming.
    /// \return AT&T format 32-bit register (e.g., "%eax", "%r8d").
    [[nodiscard]] static std::string formatReg32(const OpReg &reg, const TargetInfo &target);

    /// \brief Format a register as its 16-bit variant.
    /// \param reg Register operand.
    /// \param target Target information for register naming.
    /// \return AT&T format 16-bit register (e.g., "%ax", "%r8w").
    [[nodiscard]] static std::string formatReg16(const OpReg &reg, const TargetInfo &target);

    /// \brief Format an immediate operand.
    /// \param imm Immediate operand containing integer value.
    /// \return AT&T format immediate (e.g., "$42", "$-1").
    [[maybe_unused]] [[nodiscard]] static std::string formatImm(const OpImm &imm);

    /// \brief Format a memory operand with base+displacement addressing.
    /// \param mem Memory operand containing base, index, scale, and displacement.
    /// \param target Target information for register naming.
    /// \return AT&T format memory reference (e.g., "8(%rbp)", "(%rax,%rcx,4)").
    [[nodiscard]] static std::string formatMem(const OpMem &mem, const TargetInfo &target);

    /// \brief Format a symbolic label operand.
    /// \param label Label operand containing symbol name.
    /// @param format Object format controlling Darwin underscore prefixing.
    /// \return Label string for use in jumps/calls.
    [[maybe_unused]] [[nodiscard]] static std::string formatLabel(const OpLabel &label,
                                                                  objfile::ObjFormat format);

    /// \brief Format a RIP-relative label operand.
    /// \param label RIP-relative label operand.
    /// @param format Object format controlling Darwin underscore prefixing.
    /// \return RIP-relative format (e.g., "_symbol(%rip)").
    [[nodiscard]] static std::string formatRipLabel(const OpRipLabel &label,
                                                    objfile::ObjFormat format);

    /// \brief Format a shift/rotate count operand.
    /// \details Handles both immediate counts and %cl register counts.
    /// \param operand Shift count (immediate or register).
    /// \param target Target information for register naming.
    /// @param format Object format controlling any symbolic formatting.
    /// \return Formatted shift count (e.g., "$3", "%cl").
    [[nodiscard]] static std::string formatShiftCount(const Operand &operand,
                                                      const TargetInfo &target,
                                                      objfile::ObjFormat format);

    /// \brief Format the source operand for LEA instructions.
    /// \param operand LEA source (typically memory-style addressing).
    /// \param target Target information for register naming.
    /// @param format Object format controlling symbol spelling.
    /// \return Formatted LEA source address expression.
    [[nodiscard]] static std::string formatLeaSource(const Operand &operand,
                                                     const TargetInfo &target,
                                                     objfile::ObjFormat format);

    /// \brief Format a CALL target operand.
    /// \details Handles direct symbols, indirect registers, and memory targets.
    /// \param operand Call target (label, register, or memory).
    /// \param target Target information for register naming.
    /// @param format Object format controlling symbol spelling.
    /// \return Formatted call target (e.g., "_func", "*%rax").
    [[nodiscard]] static std::string formatCallTarget(const Operand &operand,
                                                      const TargetInfo &target,
                                                      objfile::ObjFormat format);

    /// \brief Get the condition code suffix for Jcc/SETcc instructions.
    /// \param code Condition code value (0=EQ, 1=NE, etc.).
    /// \return Condition suffix string (e.g., "e", "ne", "l", "g").
    [[nodiscard]] static std::string_view conditionSuffix(std::int64_t code);
};

/// \brief Enumerates operand orderings handled by the emitter table.
enum class OperandOrder {
    NONE,      ///< Instruction does not print operands.
    DIRECT,    ///< Emit operands exactly as provided.
    R,         ///< Single register operand.
    M,         ///< Single memory operand.
    I,         ///< Single immediate operand.
    R_R,       ///< Destination register with register source.
    R_R32,     ///< Destination register with register source (32-bit operands).
    R32_I,     ///< 32-bit destination register with immediate source.
    R_R16,     ///< Destination register with register source (16-bit operands).
    R16_I,     ///< 16-bit destination register with immediate source.
    MOVSX16_RR, ///< movswq: 16-bit source register, 64-bit destination register.
    R_M,       ///< Destination register with memory source.
    M_R,       ///< Destination memory with register source.
    R_I,       ///< Destination register with immediate source.
    M_I,       ///< Destination memory with immediate source.
    R_R_R,     ///< Three operands following src2, src1, dest ordering.
    SHIFT,     ///< Shift/rotate with specialised count formatting.
    MOVZX_RR8, ///< movzbq-like instruction requiring 8-bit source formatting.
    MOVSXD_RR, ///< movslq: 32-bit source register, 64-bit destination register.
    LEA,       ///< LEA with custom source handling.
    CALL,      ///< CALL-style operand formatting.
    JUMP,      ///< JMP-style operand formatting.
    JCC,       ///< Conditional branch formatting with suffix.
    SETCC      ///< SETcc formatting with suffix.
};

/// \brief Categorises operand variants for encoding table matching.
enum class OperandKind {
    None,            ///< No operand expected in this slot.
    Reg,             ///< Register operand.
    Imm,             ///< Immediate operand.
    Mem,             ///< Memory operand using base+disp addressing.
    Label,           ///< Symbolic label operand.
    RipLabel,        ///< RIP-relative label operand.
    RegOrMem,        ///< Register or memory operand.
    RegOrImm,        ///< Register or immediate operand.
    LabelOrRegOrMem, ///< Label, RIP label, register, or memory operand.
    Any              ///< Any operand kind accepted.
};

/// \brief Describes high-level encoding forms used to differentiate rows.
enum class EncodingForm {
    Nullary,   ///< No explicit operands (e.g. CQO, RET).
    Unary,     ///< Single explicit operand (e.g. JMP target).
    RegReg,    ///< Register destination with register source.
    RegImm,    ///< Register destination with immediate source.
    RegMem,    ///< Register destination with memory source.
    MemReg,    ///< Memory destination with register source.
    Lea,       ///< LEA-style operand combination.
    ShiftImm,  ///< Shift/rotate with immediate count.
    ShiftReg,  ///< Shift/rotate with register count (CL).
    Call,      ///< CALL target operand.
    Jump,      ///< JMP target operand.
    Condition, ///< Conditional branch (Jcc) using a predicate immediate.
    Setcc      ///< SETcc predicate with destination operand.
};

/// \brief Flags describing ModRM, SIB, immediate, and prefix requirements.
enum class EncodingFlag : std::uint32_t {
    None = 0U,
    RequiresModRM = 1U << 0, ///< Instruction consumes a ModRM byte.
    UsesImm8 = 1U << 1,      ///< Instruction encodes an 8-bit immediate.
    UsesImm32 = 1U << 2,     ///< Instruction encodes a 32-bit immediate.
    UsesImm64 = 1U << 3,     ///< Instruction encodes a 64-bit immediate.
    REXW = 1U << 4,          ///< Requires a REX.W prefix.
    UsesCondition = 1U << 5  ///< Reads a condition-code immediate.
};

/// \brief Operand pattern describing the expected operand kinds for a row.
struct OperandPattern {
    std::array<OperandKind, 3> kinds{}; ///< Ordered operand kinds.
    std::uint8_t count{0U};             ///< Number of operands expected.
};

/// \brief Descriptor tying opcodes to textual mnemonics and operand policies.
struct EncodingRow {
    MOpcode opcode{MOpcode::MOVrr};           ///< Opcode handled by this specification.
    std::string_view mnemonic;                ///< Canonical mnemonic without condition suffixes.
    EncodingForm form{EncodingForm::Nullary}; ///< Encoding form describing operand semantics.
    OperandOrder order{OperandOrder::NONE};   ///< Operand emission policy for the opcode.
    OperandPattern pattern{};                 ///< Operand kind pattern used for lookup.
    EncodingFlag flags{EncodingFlag::None};   ///< ModRM/SIB/immediate/prefix metadata.
};

/// @brief Locate the encoding row matching an opcode and operand sequence.
/// @param op Machine opcode to match.
/// @param operands Actual operand variants.
/// @return Matching generated row, or null when no row accepts the shape.
[[nodiscard]] const EncodingRow *find_encoding(MOpcode op,
                                               std::span<const Operand> operands) noexcept;

} // namespace zanna::codegen::x64
