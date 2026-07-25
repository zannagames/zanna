//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/AsmEmitter.cpp
// Purpose: Materialise textual x86-64 assembly from Machine IR functions while
//          maintaining deterministic literal pools for read-only data.
// Key invariants:
//   - Emission preserves operand ordering, branch destinations, and condition
//     suffixes carried by Machine IR.
//   - Literal pools deduplicate entries, emit stable labels, and are never
//     emitted when empty.
// Ownership/Lifetime:
//   - AsmEmitter borrows the caller-provided RoDataPool; the pool outlives the
//     emitter and continues to own all stored literal buffers.
// Links: codegen/x86_64/AsmEmitter.hpp,
//        codegen/x86_64/asmfmt/Format.hpp,
//        codegen/x86_64/MachineIR.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file AsmEmitter.cpp
 * @brief Implements deterministic AT&T-syntax emission for x86-64 Machine IR.
 *
 * Generated encoding tables select mnemonic and operand policy, while explicit
 * handlers cover control-flow tables, width-sensitive registers, calls, and
 * conditions. The accompanying literal pool interns strings and f64 bit
 * patterns and emits format-appropriate read-only-data directives.
 */

#include "AsmEmitter.hpp"
#include "asmfmt/Format.hpp"
#include "common/Mangle.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace zanna::codegen::x64 {

namespace {

/// @brief AT&T register names indexed by [width][PhysReg index].
/// @details Width slot 0 = 8-bit (byte), slot 1 = 32-bit (long), slot 2 =
///          16-bit (word). 64-bit names are produced by asmfmt::fmt_reg; this
///          table covers only the GPR aliases that x86 exposes for narrower
///          operand sizes. Entries beyond index 15 are unused (XMM regs are
///          not aliased like GPRs).
constexpr std::array<std::array<const char *, 16>, 3> kGprAliasNames = {{
    // 8-bit  (lo byte): RAX..RSP then R8..R15
    {"%al",
     "%bl",
     "%cl",
     "%dl",
     "%sil",
     "%dil",
     "%r8b",
     "%r9b",
     "%r10b",
     "%r11b",
     "%r12b",
     "%r13b",
     "%r14b",
     "%r15b",
     "%bpl",
     "%spl"},
    // 32-bit (dword)
    {"%eax",
     "%ebx",
     "%ecx",
     "%edx",
     "%esi",
     "%edi",
     "%r8d",
     "%r9d",
     "%r10d",
     "%r11d",
     "%r12d",
     "%r13d",
     "%r14d",
     "%r15d",
     "%ebp",
     "%esp"},
    // 16-bit (word)
    {"%ax",
     "%bx",
     "%cx",
     "%dx",
     "%si",
     "%di",
     "%r8w",
     "%r9w",
     "%r10w",
     "%r11w",
     "%r12w",
     "%r13w",
     "%r14w",
     "%r15w",
     "%bp",
     "%sp"},
}};

/// @brief Build an @ref OperandPattern from up to three operand kind slots.
/// @details Stops counting once a @c None slot is encountered, so the
///          resulting @c count field reflects the true arity. Used by the
///          encoding table to express what shape of operand list a given
///          encoding row accepts.
/// @param first First operand constraint.
/// @param second Optional second operand constraint.
/// @param third Optional third operand constraint.
/// @return Pattern whose count ends at the first `None` slot.
constexpr OperandPattern makePattern(OperandKind first = OperandKind::None,
                                     OperandKind second = OperandKind::None,
                                     OperandKind third = OperandKind::None) noexcept {
    OperandPattern pattern{};
    pattern.kinds[0] = first;
    pattern.kinds[1] = second;
    pattern.kinds[2] = third;
    if (first != OperandKind::None) {
        ++pattern.count;
    }
    if (second != OperandKind::None) {
        ++pattern.count;
    }
    if (third != OperandKind::None) {
        ++pattern.count;
    }
    return pattern;
}

/// @brief Bitwise OR for @ref EncodingFlag values.
/// @details The enum is a bitflag set; this operator allows compact
///          composition of flag combinations (e.g.
///          @c RequiresModRM | UsesImm32 | REXW).
/// @param lhs Left flag set.
/// @param rhs Right flag set.
/// @return Bitwise union of both sets.
constexpr EncodingFlag operator|(EncodingFlag lhs, EncodingFlag rhs) noexcept {
    return static_cast<EncodingFlag>(static_cast<std::uint32_t>(lhs) |
                                     static_cast<std::uint32_t>(rhs));
}

/// @brief Test whether an encoding flag set contains a flag.
/// @param value Flag set to inspect.
/// @param flag Flag or combined mask to require.
/// @return `true` when all bits in @p flag are present.
[[maybe_unused]] constexpr bool hasFlag(EncodingFlag value, EncodingFlag flag) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0U;
}

// Include the generated encoding table
#include "generated/EncodingTable.inc"

/// @brief Predicate: does @p operand satisfy the @p kind constraint?
/// @details The @ref OperandKind enum supports union variants like
///          @c RegOrMem, @c LabelOrRegOrMem — this helper decodes those
///          choices into the appropriate @c std::holds_alternative checks
///          against the variant operand.
/// @param kind Pattern constraint.
/// @param operand Variant operand to inspect.
/// @return `true` when @p operand satisfies @p kind.
[[nodiscard]] bool matchesOperandKind(OperandKind kind, const Operand &operand) noexcept {
    switch (kind) {
        case OperandKind::None:
            return false;
        case OperandKind::Reg:
            return std::holds_alternative<OpReg>(operand);
        case OperandKind::Imm:
            return std::holds_alternative<OpImm>(operand);
        case OperandKind::Mem:
            return std::holds_alternative<OpMem>(operand) ||
                   std::holds_alternative<OpRipLabel>(operand);
        case OperandKind::Label:
            return std::holds_alternative<OpLabel>(operand);
        case OperandKind::RipLabel:
            return std::holds_alternative<OpRipLabel>(operand);
        case OperandKind::RegOrMem:
            return std::holds_alternative<OpReg>(operand) || std::holds_alternative<OpMem>(operand);
        case OperandKind::RegOrImm:
            return std::holds_alternative<OpReg>(operand) || std::holds_alternative<OpImm>(operand);
        case OperandKind::LabelOrRegOrMem:
            return std::holds_alternative<OpLabel>(operand) ||
                   std::holds_alternative<OpRipLabel>(operand) ||
                   std::holds_alternative<OpReg>(operand) || std::holds_alternative<OpMem>(operand);
        case OperandKind::Any:
            return true;
    }
    return false;
}

/// @brief Predicate: does @p operands fit the shape declared by @p pattern?
/// @details First verifies arity then kind by kind. Used by the encoding
///          lookup to disambiguate rows that share an opcode (e.g. ADDri
///          vs. ADDrr).
/// @param pattern Expected operand count and per-slot kinds.
/// @param operands Actual instruction operands.
/// @return `true` when arity and all slot constraints match.
[[nodiscard]] bool matchesPattern(const OperandPattern &pattern,
                                  std::span<const Operand> operands) noexcept {
    if (static_cast<std::size_t>(pattern.count) != operands.size()) {
        return false;
    }
    for (std::size_t i = 0; i < operands.size(); ++i) {
        if (!matchesOperandKind(pattern.kinds[i], operands[i])) {
            return false;
        }
    }
    return true;
}

/// @brief Test whether exactly one operand satisfies a supplied predicate.
/// @param operands Operand sequence to scan.
/// @param predicate Non-null operand-kind predicate.
/// @return `true` when precisely one operand matches.
[[nodiscard]] bool hasSingleOperandKind(std::span<const Operand> operands,
                                        bool (*predicate)(const Operand &)) noexcept {
    bool found = false;
    for (const auto &operand : operands) {
        if (!predicate(operand))
            continue;
        if (found)
            return false;
        found = true;
    }
    return found;
}

/// @brief Recognize an immediate condition-code operand.
/// @param operand Variant operand to inspect.
/// @return `true` for an immediate.
[[maybe_unused]] [[nodiscard]] bool isConditionOperand(const Operand &operand) noexcept {
    return std::holds_alternative<OpImm>(operand);
}

/// @brief Recognize a direct label operand.
/// @param operand Variant operand to inspect.
/// @return `true` for @ref OpLabel.
[[nodiscard]] bool isLabelOperand(const Operand &operand) noexcept {
    return std::holds_alternative<OpLabel>(operand);
}

/// @brief Recognize a legal SETcc register-or-memory destination.
/// @param operand Variant operand to inspect.
/// @return `true` for register or memory operands.
[[nodiscard]] bool isSetccDestinationOperand(const Operand &operand) noexcept {
    return std::holds_alternative<OpReg>(operand) || std::holds_alternative<OpMem>(operand);
}

/// @brief Predicate: does @p operands fit an order-independent condition shape?
/// @details Hand-built MIR in tests historically used both `{cc, dst}` and
///          `{dst, cc}` for SETcc, and both `{cc, label}` and `{label, cc}` for
///          JCC. The opcode-specific emitters below already select operands by
///          kind, so the table lookup must allow the same shapes through.
/// @param op Conditional opcode being matched.
/// @param operands Two operands in either accepted order.
/// @return `true` for a supported JCC or SETcc condition shape.
[[nodiscard]] bool matchesFlexibleConditionPattern(MOpcode op,
                                                   std::span<const Operand> operands) noexcept {
    if (operands.size() != 2)
        return false;
    if (op == MOpcode::JCC) {
        return hasSingleOperandKind(operands, isLabelOperand);
    }
    if (op == MOpcode::SETcc) {
        return hasSingleOperandKind(operands, isSetccDestinationOperand);
    }
    return false;
}

/// @brief Provide an overload set capable of visiting std::variant operands.
/// @details Aggregates multiple lambda visitors into a single callable so
///          @ref std::visit can dispatch over operand kinds without defining a
///          bespoke struct at each call site.
template <typename... Ts> struct Overload : Ts... {
    using Ts::operator()...;
};

/// @brief Deduction guide for constructing an @ref Overload visitor from lambdas.
template <typename... Ts> Overload(Ts...) -> Overload<Ts...>;

enum : std::uint16_t {
    kFmtDirect = 1U << 0U,
    kFmtShift = 1U << 1U,
    kFmtMovzx8 = 1U << 2U,
    kFmtLea = 1U << 3U,
    kFmtCall = 1U << 4U,
    kFmtJump = 1U << 5U,
    kFmtCond = 1U << 6U,
    kFmtSetcc = 1U << 7U,
    kFmtReg32 = 1U << 8U,
    kFmtReg32Imm = 1U << 9U, ///< Immediate + 32-bit register destination.
    kFmtMovsxd = 1U << 10U,  ///< 32-bit source register, 64-bit destination (movslq).
    kFmtReg16 = 1U << 11U,   ///< Both operands printed as 16-bit registers.
    kFmtReg16Imm = 1U << 12U, ///< Immediate + 16-bit register destination.
    kFmtMovsx16 = 1U << 13U,  ///< 16-bit source register, 64-bit destination (movswq).
};

/// @brief Compact generated textual-format descriptor for one opcode.
struct OpFmt {
    ///< Machine opcode handled by the row.
    MOpcode opc;
    ///< AT&T mnemonic.
    const char *mnemonic;
    ///< Required explicit operand count.
    std::uint8_t operandCount;
    ///< Internal formatting-policy bit set.
    std::uint16_t flags;
};

// Include the generated OpFmt table
#include "generated/OpFmtTable.inc"

/// @brief Compute the highest @c MOpcode index referenced by @c kOpFmt.
/// @details Used to size the constexpr lookup table at compile time so
///          opcode -> @ref OpFmt resolution is O(1) regardless of how
///          many entries are present.
/// @return Highest numeric opcode value present in the generated table.
consteval std::size_t maxOpFmtOpcodeIndex() {
    std::size_t maxIndex = 0;
    for (const auto &fmt : kOpFmt)
        maxIndex = std::max(maxIndex, static_cast<std::size_t>(fmt.opc));
    return maxIndex;
}

/// @brief Number of opcode slots needed for the O(1) OpFmt lookup.
constexpr std::size_t kOpFmtLookupSize = maxOpFmtOpcodeIndex() + 1;

/// @brief Build a compile-time lookup table mapping MOpcode -> index in kOpFmt.
/// @details Returns kOpFmt.size() (invalid index) for opcodes not in the table.
/// @return Fully initialized opcode-to-row-index table.
constexpr std::array<std::size_t, kOpFmtLookupSize> buildOpFmtLookup() noexcept {
    std::array<std::size_t, kOpFmtLookupSize> lookup{};
    // Initialize all to invalid index
    for (auto &idx : lookup)
        idx = kOpFmt.size();
    // Populate valid entries
    for (std::size_t i = 0; i < kOpFmt.size(); ++i) {
        const auto opcIdx = static_cast<std::size_t>(kOpFmt[i].opc);
        if (opcIdx < kOpFmtLookupSize)
            lookup[opcIdx] = i;
    }
    return lookup;
}

/// @brief Static lookup table for O(1) OpFmt access by MOpcode.
static constexpr auto kOpFmtLookup = buildOpFmtLookup();

/// @brief Retrieves fmt value using O(1) lookup table.
/// @details Direct array indexing instead of linear search for better performance.
/// @param opc Machine opcode to resolve.
/// @return Pointer to its formatting row, or null when absent/out of range.
const OpFmt *getFmt(MOpcode opc) noexcept {
    const auto idx = static_cast<std::size_t>(opc);
    if (idx >= kOpFmtLookupSize)
        return nullptr;
    const auto tableIdx = kOpFmtLookup[idx];
    if (tableIdx >= kOpFmt.size())
        return nullptr;
    return &kOpFmt[tableIdx];
}

/// @brief Convert an @ref OpReg to the signed index convention used by asmfmt.
/// @param reg Physical or virtual register operand.
/// @return Nonnegative physical index or negative virtual encoding.
[[nodiscard]] int encodeRegister(const OpReg &reg) noexcept;

/// @brief Predicate: would @p name be a Mach-O "local" symbol?
/// @details Mach-O distinguishes local labels by a leading @c '.' or by
///          known prefixes such as @c L./@c Ltmp/@c LBB. Local symbols are
///          not given the standard underscore prefix when formatted.
/// @param name Candidate symbol spelling.
/// @return `true` when Darwin treats the name as local.
[[nodiscard]] bool isDarwinLocalSymbol(std::string_view name) noexcept {
    return !name.empty() && (name.front() == '.' || name.rfind("L.", 0) == 0 ||
                             name.rfind("Ltmp", 0) == 0 || name.rfind("LBB", 0) == 0);
}

/// @brief Format a symbol reference, applying Mach-O underscore prefixing.
/// @details On Mach-O targets non-local symbols carry a leading underscore
///          in the textual assembly. ELF/COFF use the raw name.
/// @param name Logical symbol spelling.
/// @param format Target object format.
/// @return Assembly-safe symbol reference.
[[nodiscard]] std::string formatSymbolReference(std::string_view name, objfile::ObjFormat format) {
    std::string symbol = asmfmt::format_label(name);
    if (format == objfile::ObjFormat::MachO && !symbol.empty() && !isDarwinLocalSymbol(symbol))
        symbol.insert(symbol.begin(), '_');
    return symbol;
}

/// @brief Format a symbol reference as a RIP-relative memory operand.
/// @details Delegates to @ref formatSymbolReference then appends @c "(%rip)"
///          so the resulting string can be used directly in a load/store.
/// @param name Logical symbol spelling.
/// @param format Target object format.
/// @return Format-adjusted symbol followed by `(%rip)`.
[[nodiscard]] std::string formatRipSymbolReference(std::string_view name,
                                                   objfile::ObjFormat format) {
    std::string result = formatSymbolReference(name, format);
    result += "(%rip)";
    return result;
}

/// @brief Format one MIR operand as AT&T-syntax assembly text into @p out.
/// @details Dispatches on the operand variant (register, immediate, memory,
///          label) via std::visit, delegating register naming to asmfmt and
///          RIP-relative/displacement formatting to the memory-address helper.
/// @tparam Out     Output sink type (e.g. std::ostream or a string builder).
/// @param operand  The MIR operand to render.
/// @param out      Destination text sink.
/// @param target   Target info (reserved; register naming is target-uniform here).
/// @param format   Object-file format, affecting symbol/relocation syntax.
template <typename Out>
void emitOperand(const Operand &operand,
                 Out &out,
                 const TargetInfo &target,
                 objfile::ObjFormat format) {
    static_cast<void>(target);
    std::visit(
        Overload{
            [&](const OpReg &reg) { out << asmfmt::fmt_reg(encodeRegister(reg)); },
            [&](const OpImm &imm) { out << asmfmt::format_imm(imm.val); },
            [&](const OpMem &mem) {
                asmfmt::MemAddr addr{};
                addr.base = encodeRegister(mem.base);
                addr.disp = mem.disp;
                if (mem.hasIndex) {
                    addr.index = encodeRegister(mem.index);
                    addr.scale = mem.scale;
                    addr.has_index = true;
                }
                out << asmfmt::format_mem(addr);
            },
            [&](const OpLabel &label) { out << formatSymbolReference(label.name, format); },
            [&](const OpRipLabel &label) { out << formatRipSymbolReference(label.name, format); }},
        operand);
}

template <typename Out>
/// @brief Emit a comma-separated operand sequence.
/// @tparam Out Output sink supporting stream insertion.
/// @param operands Ordered MIR operands.
/// @param out Destination text sink.
/// @param target Target register information.
/// @param format Object format controlling symbol spelling.
void emitOperands(std::span<const Operand> operands,
                  Out &out,
                  const TargetInfo &target,
                  objfile::ObjFormat format) {
    bool first = true;
    for (const auto &operand : operands) {
        if (!first) {
            out << ", ";
        }
        emitOperand(operand, out, target, format);
        first = false;
    }
}

/// @brief Emit all literal-pool entries into the target's read-only-data section.
/// @param stringLiterals String byte payloads in label-index order.
/// @param stringLengths Recorded byte lengths used to validate pool consistency.
/// @param f64Literals Floating values in label-index order.
/// @param format Object format selecting the section directive.
/// @param os Output stream receiving assembly.
/// @throws std::runtime_error when string payload and length metadata disagree.
void emitRoDataPool(std::span<const std::string> stringLiterals,
                    std::span<const std::size_t> stringLengths,
                    std::span<const double> f64Literals,
                    objfile::ObjFormat format,
                    std::ostream &os) {
    if (stringLiterals.empty() && f64Literals.empty()) {
        return;
    }
    if (stringLiterals.size() != stringLengths.size()) {
        throw std::runtime_error("x86-64 asm emitter: rodata string length table mismatch");
    }
    switch (format) {
        case objfile::ObjFormat::MachO:
            os << ".section __TEXT,__const\n";
            break;
        case objfile::ObjFormat::COFF:
            os << ".section .rdata,\"dr\"\n";
            break;
        case objfile::ObjFormat::ELF:
            os << ".section .rodata\n";
            break;
    }
    for (std::size_t i = 0; i < stringLiterals.size(); ++i) {
        if (stringLiterals[i].size() != stringLengths[i]) {
            throw std::runtime_error(
                "x86-64 asm emitter: rodata string byte length mismatch at index " +
                std::to_string(i));
        }
        std::string label;
        label.reserve(16U);
        label.append(".LC_str_");
        label.append(std::to_string(i));
        os << label << ":\n";
        os << asmfmt::format_rodata_bytes(stringLiterals[i]);
    }
    if (!f64Literals.empty()) {
        os << "  .p2align 3\n";
    }
    for (std::size_t i = 0; i < f64Literals.size(); ++i) {
        std::string label;
        label.reserve(16U);
        label.append(".LC_f64_");
        label.append(std::to_string(i));
        os << label << ":\n";
        const auto bits = std::bit_cast<std::uint64_t>(f64Literals[i]);
        const auto oldFlags = os.flags();
        const auto oldFill = os.fill();
        os << "  .quad 0x" << std::hex << std::setw(16) << std::setfill('0') << bits << std::dec;
        os.fill(oldFill);
        os.flags(oldFlags);
        os << '\n';
    }
}

/// @brief Pack an OpReg into the signed integer code used by the asmfmt layer.
/// @details Physical registers map to their non-negative @c idOrPhys value;
///          virtual registers map to @c -(idOrPhys+1) so the sign bit alone
///          distinguishes the two namespaces (and vreg 0 stays distinct from
///          physical reg 0).
/// @param reg Register operand to encode.
/// @return Non-negative for physical regs, negative for virtual regs.
[[nodiscard]] int encodeRegister(const OpReg &reg) noexcept {
    if (reg.isPhys) {
        return static_cast<int>(reg.idOrPhys);
    }
    return -static_cast<int>(reg.idOrPhys) - 1;
}

} // namespace

/// @brief Find the encoding row matching an opcode and operand pattern.
/// @details Linear scan over kEncodingTable (49 entries). The table is small
///          enough that an index is unnecessary; each lookup touches at most
///          49 cache-hot rows with an early-out on opcode mismatch.
/// @param op  Machine opcode to look up.
/// @param operands  Instruction operands used for pattern matching.
/// @return Pointer to the matching row, or nullptr if no encoding exists.
const EncodingRow *find_encoding(MOpcode op, std::span<const Operand> operands) noexcept {
    for (const auto &row : kEncodingTable) {
        if (row.opcode != op) {
            continue;
        }
        if (matchesPattern(row.pattern, operands) ||
            matchesFlexibleConditionPattern(row.opcode, operands)) {
            return &row;
        }
    }
    return nullptr;
}

/// @brief Intern a string literal into the read-only data pool.
/// @details Deduplicates identical byte sequences so repeated literals emit a
///          single `.rodata` entry. New literals are appended to the pool and
///          assigned a stable numeric index.
/// @param bytes Literal payload to store.
/// @return Index referencing the canonicalised literal.
int AsmEmitter::RoDataPool::addStringLiteral(std::string bytes) {
    if (const auto it = stringLookup_.find(bytes); it != stringLookup_.end()) {
        return it->second;
    }
    const int index = static_cast<int>(stringLiterals_.size());
    stringLookup_.emplace(bytes, index);
    stringLengths_.push_back(bytes.size());
    stringLiterals_.push_back(std::move(bytes));
    return index;
}

/// @brief Intern a 64-bit floating literal into the read-only data pool.
/// @details Bit-casts the floating value and deduplicates based on the
///          resulting bit pattern, ensuring `+0.0` and `-0.0` remain distinct.
/// @param value Floating-point literal to store.
/// @return Index referencing the canonical literal entry.
int AsmEmitter::RoDataPool::addF64Literal(double value) {
    const auto bits = std::bit_cast<std::uint64_t>(value);
    if (const auto it = f64Lookup_.find(bits); it != f64Lookup_.end()) {
        return it->second;
    }
    const int index = static_cast<int>(f64Literals_.size());
    f64Lookup_.emplace(bits, index);
    f64Literals_.push_back(value);
    return index;
}

/// @brief Generate the assembly label for a stored string literal.
/// @param index Pool index returned by @ref addStringLiteral.
/// @return Mangled label suitable for use in assembly.
std::string AsmEmitter::RoDataPool::stringLabel(int index) const {
    return ".LC_str_" + std::to_string(index);
}

/// @brief Retrieve the byte length recorded for a string literal entry.
/// @param index Pool index supplied by @ref addStringLiteral.
/// @return Number of bytes stored for the literal.
std::size_t AsmEmitter::RoDataPool::stringByteLength(int index) const {
    if (index < 0)
        throw std::out_of_range("x86-64 rodata string index is negative");
    const auto idx = static_cast<std::size_t>(index);
    if (idx >= stringLengths_.size())
        throw std::out_of_range("x86-64 rodata string index is out of range");
    return stringLengths_[idx];
}

/// @brief Generate the assembly label for a stored 64-bit float literal.
/// @param index Pool index returned by @ref addF64Literal.
/// @return Mangled label suitable for use in assembly.
std::string AsmEmitter::RoDataPool::f64Label(int index) const {
    return ".LC_f64_" + std::to_string(index);
}

/// @brief Emit the `.rodata` directives for all stored literals.
/// @details Writes a `.section .rodata` header followed by labels and
///          directives for each pooled string and floating literal. The method
///          preserves insertion order so indices map consistently to labels.
/// @param os Output stream receiving assembly text.
/// @param format Object format selecting the read-only-data section directive.
void AsmEmitter::RoDataPool::emit(std::ostream &os, objfile::ObjFormat format) const {
    if (empty()) {
        return;
    }
    emitRoDataPool(std::span<const std::string>{stringLiterals_},
                   std::span<const std::size_t>{stringLengths_},
                   std::span<const double>{f64Literals_},
                   format,
                   os);
}

/// @brief Query whether the pool currently holds any literals.
/// @return @c true when no string or floating literals have been interned.
bool AsmEmitter::RoDataPool::empty() const noexcept {
    return stringLiterals_.empty() && f64Literals_.empty();
}

/// @brief Construct an emitter bound to a shared read-only data pool.
/// @param pool Pool responsible for owning literal buffers referenced by the
///             emitted assembly.
/// @param format Object format controlling Darwin symbol and section spelling.
AsmEmitter::AsmEmitter(RoDataPool &pool, objfile::ObjFormat format) noexcept
    : pool_{&pool}, format_{format} {}

/// @brief Emit an assembly function, including basic blocks and instructions.
/// @details Writes the `.text` header, global symbol directive, function label,
///          and each Machine IR block. The first block is treated as the entry
///          and emitted without a label when it already matches the function
///          name.
/// @param os Output stream receiving the assembly.
/// @param func Machine IR function to serialise.
/// @param target Target lowering information controlling register selection.
void AsmEmitter::emitFunction(std::ostream &os,
                              const MFunction &func,
                              const TargetInfo &target) const {
    os << ".text\n";
    const std::string linkName =
        formatSymbolReference(zanna::common::MangleLink(func.name), format_);
    os << ".globl " << linkName << "\n";
    if (format_ == objfile::ObjFormat::ELF) {
        os << ".type " << linkName << ", @function\n";
    }
    os << linkName << ":\n";

    for (std::size_t i = 0; i < func.blocks.size(); ++i) {
        const auto &block = func.blocks[i];
        const bool isEntry = (i == 0U && block.label == func.name);
        if (isEntry) {
            for (const auto &instr : block.instructions) {
                emitInstruction(os, instr, target, format_);
            }
        } else {
            emitBlock(os, block, target, format_);
        }
        if (i + 1 < func.blocks.size()) {
            os << '\n';
        }
    }
}

/// @brief Emit the `.rodata` section for literals referenced by emitted code.
/// @details Forwards to the shared pool only when it contains entries so that
///          translation units without literals avoid spurious section headers.
/// @param os Output stream receiving the assembly.
void AsmEmitter::emitRoData(std::ostream &os) const {
    if (pool_ && !pool_->empty()) {
        pool_->emit(os, format_);
    }
}

/// @brief Access the underlying literal pool.
/// @return Mutable reference to the associated pool.
[[maybe_unused]] AsmEmitter::RoDataPool &AsmEmitter::roDataPool() noexcept {
    return *pool_;
}

/// @brief Access the underlying literal pool (const overload).
/// @return Const reference to the associated pool.
[[maybe_unused]] const AsmEmitter::RoDataPool &AsmEmitter::roDataPool() const noexcept {
    return *pool_;
}

/// @brief Emit a labelled basic block and all contained instructions.
/// @details Prints the block label when present and delegates each instruction
///          to @ref emitInstruction.
/// @param os Output stream receiving the assembly.
/// @param block Machine basic block to serialise.
/// @param target Target lowering information controlling operand formatting.
/// @param format Object format controlling symbol spelling.
void AsmEmitter::emitBlock(std::ostream &os,
                           const MBasicBlock &block,
                           const TargetInfo &target,
                           objfile::ObjFormat format) {
    if (!block.label.empty()) {
        os << formatSymbolReference(block.label, format) << ":\n";
    }
    for (const auto &instr : block.instructions) {
        emitInstruction(os, instr, target, format);
    }
}

/// @brief Emit a single Machine IR instruction in AT&T syntax.
/// @details Handles opcode-specific quirks (such as operand ordering for
///          `MOV`, condition suffixes, and synthetic PX_COPY) before falling
///          back to a generic visitor that prints each operand.
/// @param os Output stream receiving the assembly.
/// @param instr Instruction to serialise.
/// @param target Target lowering information controlling operand formatting.
/// @param format Object format controlling symbol spelling.
void AsmEmitter::emitInstruction(std::ostream &os,
                                 const MInstr &instr,
                                 const TargetInfo &target,
                                 objfile::ObjFormat format) {
    if (instr.opcode == MOpcode::LABEL) {
        if (instr.operands.empty()) {
            os << ".L?\n";
            return;
        }
        const auto *label = std::get_if<OpLabel>(&instr.operands.front());
        if (!label) {
            os << "# <invalid label>\n";
            return;
        }
        os << formatSymbolReference(label->name, format) << ":\n";
        return;
    }

    if (instr.opcode == MOpcode::JUMPTABLE) {
        // Operands: [0]=index reg, [1]=table label, [2..]=case labels. The
        // dispatch tail runs in the reserved R10/R11 scratch registers; the
        // table is emitted inline right after the indirect jump with entries
        // anchored to the table start.
        if (instr.operands.size() < 3) {
            throw std::runtime_error("x86-64 asm emitter: JUMPTABLE operand shortfall");
        }
        const auto *idx = std::get_if<OpReg>(&instr.operands[0]);
        const auto *name = std::get_if<OpLabel>(&instr.operands[1]);
        if (!idx || !name) {
            throw std::runtime_error("x86-64 asm emitter: JUMPTABLE operand shapes invalid");
        }
        const std::string tblName = formatSymbolReference(name->name, format);
        const std::string idxReg = formatOperand(instr.operands[0], target, format);
        os << "  leaq " << tblName << "(%rip), %r10\n";
        os << "  movslq (%r10," << idxReg << ",4), %r11\n";
        os << "  addq %r10, %r11\n";
        os << "  jmp *%r11\n";
        os << "  .p2align 2\n";
        os << tblName << ":\n";
        for (std::size_t i = 2; i < instr.operands.size(); ++i) {
            const auto *caseLabel = std::get_if<OpLabel>(&instr.operands[i]);
            if (caseLabel == nullptr) {
                throw std::runtime_error("x86-64 asm emitter: JUMPTABLE case must be a label");
            }
            os << "  .long " << formatSymbolReference(caseLabel->name, format) << " - " << tblName
               << '\n';
        }
        return;
    }

    if (instr.opcode == MOpcode::PX_COPY) {
        std::string line;
        const auto estimate = 12U + instr.operands.size() * 24U;
        line.reserve(estimate);
        line.append("  # px_copy");
        bool first = true;
        for (const auto &operand : instr.operands) {
            line.append(first ? " " : ", ");
            line.append(formatOperand(operand, target, format));
            first = false;
        }
        line.push_back('\n');
        os << line;
        return;
    }

    if (instr.opcode == MOpcode::PUSH || instr.opcode == MOpcode::POP) {
        const char *mnemonic = (instr.opcode == MOpcode::PUSH) ? "pushq" : "popq";
        os << "  " << mnemonic;
        if (!instr.operands.empty())
            os << ' ' << formatOperand(instr.operands.front(), target, format);
        os << '\n';
        return;
    }

    const auto operands = std::span<const Operand>{instr.operands};
    const auto *row = find_encoding(instr.opcode, operands);
    if (!row) {
        throw std::runtime_error("x86-64 asm emitter: unknown opcode " +
                                 std::to_string(static_cast<int>(instr.opcode)) + " with " +
                                 std::to_string(operands.size()) + " operand(s)");
    }

    emit_from_row(*row, operands, os, target, format);
}

/// @copydoc AsmEmitter::emit_from_row
void AsmEmitter::emit_from_row(const EncodingRow &row,
                               std::span<const Operand> operands,
                               std::ostream &os,
                               const TargetInfo &target,
                               objfile::ObjFormat format) {
    const auto *fmt = getFmt(row.opcode);
    const auto mnemonic = fmt ? std::string_view{fmt->mnemonic} : row.mnemonic;
    if (fmt && mnemonic != row.mnemonic) {
        throw std::runtime_error("x86-64 asm emitter: generated encoding table mnemonic mismatch");
    }
    os << "  " << mnemonic;

    if (!fmt) {
        if (!operands.empty()) {
            os << ' ';
            emitOperands(operands, os, target, format);
        }
        os << '\n';
        return;
    }

    if (fmt->operandCount == 0U) {
        os << '\n';
        return;
    }

    const auto flags = fmt->flags;

    if ((flags & kFmtLea) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        os << ' ' << formatLeaSource(operands[1], target, format) << ", "
           << formatOperand(operands[0], target, format) << '\n';
        return;
    }

    if ((flags & kFmtMovzx8) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        const auto *dest = std::get_if<OpReg>(&operands[0]);
        const auto *src = std::get_if<OpReg>(&operands[1]);
        if (!dest || !src) {
            os << " #<invalid>\n";
            return;
        }
        if (dest->cls != RegClass::GPR || src->cls != RegClass::GPR) {
            throw std::runtime_error("x86-64 asm emitter: MOVZXrr8 requires GPR operands");
        }
        os << ' ' << formatReg8(*src, target) << ", " << formatReg(*dest, target) << '\n';
        return;
    }

    if ((flags & kFmtReg32) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        const auto *dest = std::get_if<OpReg>(&operands[0]);
        const auto *src = std::get_if<OpReg>(&operands[1]);
        if (!dest || !src) {
            os << " #<invalid>\n";
            return;
        }
        if (dest->cls != RegClass::GPR || src->cls != RegClass::GPR) {
            throw std::runtime_error(
                "x86-64 asm emitter: 32-bit GPR operation requires GPR operands");
        }
        os << ' ' << formatReg32(*src, target) << ", " << formatReg32(*dest, target) << '\n';
        return;
    }

    if ((flags & kFmtReg32Imm) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        const auto *dest = std::get_if<OpReg>(&operands[0]);
        if (!dest || dest->cls != RegClass::GPR) {
            throw std::runtime_error(
                "x86-64 asm emitter: 32-bit reg-imm operation requires a GPR destination");
        }
        os << ' ';
        emitOperand(operands[1], os, target, format);
        os << ", " << formatReg32(*dest, target) << '\n';
        return;
    }

    if ((flags & kFmtMovsxd) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        const auto *dest = std::get_if<OpReg>(&operands[0]);
        const auto *src = std::get_if<OpReg>(&operands[1]);
        if (!dest || !src || dest->cls != RegClass::GPR || src->cls != RegClass::GPR) {
            throw std::runtime_error("x86-64 asm emitter: MOVSXD requires GPR operands");
        }
        os << ' ' << formatReg32(*src, target) << ", " << formatReg(*dest, target) << '\n';
        return;
    }

    if ((flags & kFmtReg16) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        const auto *dest = std::get_if<OpReg>(&operands[0]);
        const auto *src = std::get_if<OpReg>(&operands[1]);
        if (!dest || !src || dest->cls != RegClass::GPR || src->cls != RegClass::GPR) {
            throw std::runtime_error(
                "x86-64 asm emitter: 16-bit GPR operation requires GPR operands");
        }
        os << ' ' << formatReg16(*src, target) << ", " << formatReg16(*dest, target) << '\n';
        return;
    }

    if ((flags & kFmtReg16Imm) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        const auto *dest = std::get_if<OpReg>(&operands[0]);
        if (!dest || dest->cls != RegClass::GPR) {
            throw std::runtime_error(
                "x86-64 asm emitter: 16-bit reg-imm operation requires a GPR destination");
        }
        os << ' ';
        emitOperand(operands[1], os, target, format);
        os << ", " << formatReg16(*dest, target) << '\n';
        return;
    }

    if ((flags & kFmtMovsx16) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        const auto *dest = std::get_if<OpReg>(&operands[0]);
        const auto *src = std::get_if<OpReg>(&operands[1]);
        if (!dest || !src || dest->cls != RegClass::GPR || src->cls != RegClass::GPR) {
            throw std::runtime_error("x86-64 asm emitter: MOVSXrr16 requires GPR operands");
        }
        os << ' ' << formatReg16(*src, target) << ", " << formatReg(*dest, target) << '\n';
        return;
    }

    if ((flags & kFmtCall) != 0U) {
        if (operands.empty()) {
            os << " #<missing>\n";
            return;
        }
        os << ' ' << formatCallTarget(operands.front(), target, format) << '\n';
        return;
    }

    if ((flags & kFmtJump) != 0U) {
        if ((flags & kFmtCond) != 0U) {
            const Operand *branchTarget = nullptr;
            const OpImm *cond = nullptr;
            for (const auto &operand : operands) {
                if (!cond) {
                    cond = std::get_if<OpImm>(&operand);
                }
                if (!branchTarget && std::holds_alternative<OpLabel>(operand)) {
                    branchTarget = &operand;
                }
            }
            if (!branchTarget && !operands.empty()) {
                branchTarget = &operands.back();
            }
            if (!cond) {
                throw std::runtime_error("x86-64 asm emitter: JCC requires a condition code");
            }
            const auto suffix = conditionSuffix(cond->val);
            os << suffix << ' ';
            if (!branchTarget) {
                os << "#<missing>\n";
                return;
            }
            if (!std::holds_alternative<OpLabel>(*branchTarget)) {
                throw std::runtime_error("x86-64 asm emitter: JCC requires a label target");
            }
            const auto &label = std::get<OpLabel>(*branchTarget);
            os << formatSymbolReference(label.name, format) << '\n';
        } else {
            if (operands.empty()) {
                os << " #<missing>\n";
                return;
            }
            os << ' ';
            const auto &targetOp = operands.front();
            if (std::holds_alternative<OpLabel>(targetOp)) {
                const auto &label = std::get<OpLabel>(targetOp);
                os << formatSymbolReference(label.name, format) << '\n';
            } else if (std::holds_alternative<OpImm>(targetOp)) {
                throw std::runtime_error(
                    "x86-64 asm emitter: JMP requires a label, register, or memory target");
            } else {
                if (const auto *reg = std::get_if<OpReg>(&targetOp);
                    reg && reg->cls != RegClass::GPR) {
                    throw std::runtime_error(
                        "x86-64 asm emitter: JMP requires a GPR register target");
                }
                os << '*';
                /// @brief Emits operand.
                emitOperand(targetOp, os, target, format);
                os << '\n';
            }
        }
        return;
    }

    if ((flags & kFmtSetcc) != 0U) {
        const Operand *dest = nullptr;
        const OpImm *cond = nullptr;
        for (const auto &operand : operands) {
            if (!cond) {
                cond = std::get_if<OpImm>(&operand);
            }
            if (!dest && (std::holds_alternative<OpReg>(operand) ||
                          std::holds_alternative<OpMem>(operand))) {
                dest = &operand;
            }
        }
        if (!cond) {
            throw std::runtime_error("x86-64 asm emitter: SETcc requires a condition code");
        }
        const auto suffix = conditionSuffix(cond->val);
        os << suffix << ' ';
        if (dest) {
            // SETcc requires 8-bit destination register
            if (const auto *reg = std::get_if<OpReg>(dest)) {
                if (reg->cls != RegClass::GPR) {
                    throw std::runtime_error(
                        "x86-64 asm emitter: SETcc requires GPR or memory destination");
                }
                os << formatReg8(*reg, target) << '\n';
            } else {
                os << formatOperand(*dest, target, format) << '\n';
            }
        } else {
            os << "#<missing>\n";
        }
        return;
    }

    if ((flags & kFmtShift) != 0U) {
        if (operands.size() < 2) {
            os << " #<missing>\n";
            return;
        }
        os << ' ' << formatShiftCount(operands[1], target, format) << ", "
           << formatOperand(operands[0], target, format) << '\n';
        return;
    }

    if ((flags & kFmtDirect) != 0U) {
        if (operands.empty()) {
            os << '\n';
            return;
        }
        os << ' ';
        /// @brief Emits operands.
        emitOperands(operands, os, target, format);
        os << '\n';
        return;
    }

    switch (fmt->operandCount) {
        case 1: {
            if (operands.empty()) {
                os << " #<missing>\n";
                return;
            }
            os << ' ';
            /// @brief Emits operand.
            emitOperand(operands.front(), os, target, format);
            os << '\n';
            return;
        }
        case 2: {
            if (operands.size() < 2) {
                os << " #<missing>\n";
                return;
            }
            os << ' ';
            /// @brief Emits operand.
            emitOperand(operands[1], os, target, format);
            os << ", ";
            /// @brief Emits operand.
            emitOperand(operands[0], os, target, format);
            os << '\n';
            return;
        }
        case 3: {
            if (operands.size() < 3) {
                os << " #<missing>\n";
                return;
            }
            os << ' ';
            /// @brief Emits operand.
            emitOperand(operands[2], os, target, format);
            os << ", ";
            /// @brief Emits operand.
            emitOperand(operands[1], os, target, format);
            os << ", ";
            /// @brief Emits operand.
            emitOperand(operands[0], os, target, format);
            os << '\n';
            return;
        }
        default: {
            if (operands.empty()) {
                os << '\n';
                return;
            }
            os << ' ';
            /// @brief Emits operands.
            emitOperands(operands, os, target, format);
            os << '\n';
            return;
        }
    }
}

/// @brief Convert a Machine IR operand into its assembly representation.
/// @details Dispatches on the operand variant and delegates to specialised
///          formatting helpers for registers, immediates, memory operands, and
///          labels.
/// @param operand Operand to print.
/// @param target Target lowering information controlling register names.
/// @param format Object format controlling symbol spelling.
/// @return Textual representation of the operand.
std::string AsmEmitter::formatOperand(const Operand &operand,
                                      const TargetInfo &target,
                                      objfile::ObjFormat format) {
    std::ostringstream buffer;
    emitOperand(operand, buffer, target, format);
    return std::move(buffer).str();
}

/// @brief Format a register operand.
/// @details Returns the physical register name for hardware registers or a
///          synthetic `%vN` name for virtual registers to aid debugging.
/// @param reg Register operand to print.
/// @param target Target lowering context (unused for now but preserved for
///               future extensions).
/// @return Assembly string naming the register.
std::string AsmEmitter::formatReg(const OpReg &reg, const TargetInfo &) {
    return asmfmt::fmt_reg(encodeRegister(reg));
}

/// @copydoc AsmEmitter::formatReg8
std::string AsmEmitter::formatReg8(const OpReg &reg, const TargetInfo &target) {
    if (!reg.isPhys) {
        std::ostringstream os;
        os << "%v" << static_cast<unsigned>(reg.idOrPhys) << ".b";
        return os.str();
    }
    const auto idx = static_cast<std::size_t>(reg.idOrPhys);
    if (idx < kGprAliasNames[0].size())
        return kGprAliasNames[0][idx];
    return formatReg(reg, target);
}

/// @copydoc AsmEmitter::formatReg32
std::string AsmEmitter::formatReg32(const OpReg &reg, const TargetInfo &target) {
    if (!reg.isPhys) {
        std::ostringstream os;
        os << "%v" << static_cast<unsigned>(reg.idOrPhys) << ".d";
        return os.str();
    }
    const auto idx = static_cast<std::size_t>(reg.idOrPhys);
    if (idx < kGprAliasNames[1].size())
        return kGprAliasNames[1][idx];
    return formatReg(reg, target);
}

/// @copydoc AsmEmitter::formatReg16
std::string AsmEmitter::formatReg16(const OpReg &reg, const TargetInfo &target) {
    if (!reg.isPhys) {
        std::ostringstream os;
        os << "%v" << static_cast<unsigned>(reg.idOrPhys) << ".w";
        return os.str();
    }
    const auto idx = static_cast<std::size_t>(reg.idOrPhys);
    if (idx < kGprAliasNames[2].size())
        return kGprAliasNames[2][idx];
    return formatReg(reg, target);
}

/// @brief Format an immediate operand using AT&T syntax.
/// @param imm Immediate operand to print.
/// @return Assembly string beginning with '$'.
[[maybe_unused]] std::string AsmEmitter::formatImm(const OpImm &imm) {
    return asmfmt::format_imm(imm.val);
}

/// @brief Format a memory operand.
/// @details Produces the canonical `disp(base)` representation, eliding the
///          displacement when zero.
/// @param mem Memory operand to print.
/// @param target Target lowering information for register formatting.
/// @return Assembly string describing the memory reference.
std::string AsmEmitter::formatMem(const OpMem &mem, const TargetInfo &target) {
    static_cast<void>(target);
    asmfmt::MemAddr addr{};
    addr.base = encodeRegister(mem.base);
    addr.disp = mem.disp;
    if (mem.hasIndex) {
        addr.index = encodeRegister(mem.index);
        addr.scale = mem.scale;
        addr.has_index = true;
    }
    return asmfmt::format_mem(addr);
}

/// @brief Format a label operand.
/// @param label Label operand to print.
/// @param format Object format controlling Darwin underscore prefixing.
/// @return Raw label text.
[[maybe_unused]] std::string AsmEmitter::formatLabel(const OpLabel &label,
                                                     objfile::ObjFormat format) {
    return formatSymbolReference(label.name, format);
}

/// @brief Format a RIP-relative label operand.
/// @param label RIP-relative label to print.
/// @param format Object format controlling Darwin underscore prefixing.
/// @return Label text suffixed with the RIP-relative addressing mode.
std::string AsmEmitter::formatRipLabel(const OpRipLabel &label, objfile::ObjFormat format) {
    return formatRipSymbolReference(label.name, format);
}

/// @brief Format a shift count operand, rewriting RCX to CL when required.
/// @param operand Operand describing the shift count.
/// @param target Target lowering context for fallback formatting.
/// @param format Object format controlling any symbolic fallback.
/// @return Assembly string for the shift count operand.
std::string AsmEmitter::formatShiftCount(const Operand &operand,
                                         const TargetInfo &target,
                                         objfile::ObjFormat format) {
    if (const auto *reg = std::get_if<OpReg>(&operand)) {
        if (reg->isPhys && reg->cls == RegClass::GPR &&
            reg->idOrPhys == static_cast<uint16_t>(PhysReg::RCX)) {
            return "%cl";
        }
        throw std::runtime_error("x86-64 asm emitter: register-count shift requires RCX/CL");
    }
    if (!std::holds_alternative<OpImm>(operand)) {
        throw std::runtime_error("x86-64 asm emitter: shift count must be immediate or RCX/CL");
    }
    return formatOperand(operand, target, format);
}

/// @brief Format the source operand for an @c LEA instruction.
/// @details Labels are converted into RIP-relative references to match how
///          immediate addresses are encoded on x86-64.
/// @param operand Operand supplying the effective address computation.
/// @param target Target lowering context for register/memory formatting.
/// @param format Object format controlling symbol spelling.
/// @return Assembly string representing the effective address source.
std::string AsmEmitter::formatLeaSource(const Operand &operand,
                                        const TargetInfo &target,
                                        objfile::ObjFormat format) {
    return std::visit(
        Overload{[&](const OpLabel &label) { return formatRipSymbolReference(label.name, format); },
                 [&](const OpMem &mem) { return formatMem(mem, target); },
                 [&](const OpReg &) -> std::string {
                     throw std::runtime_error(
                         "x86-64 asm emitter: LEA requires a memory or RIP-relative source");
                 },
                 [&](const OpImm &) -> std::string {
                     throw std::runtime_error(
                         "x86-64 asm emitter: LEA requires a memory or RIP-relative source");
                 },
                 [&](const OpRipLabel &label) { return formatRipLabel(label, format); }},
        operand);
}

/// @brief Format the target operand for @c CALL instructions.
/// @details Ensures indirect targets are prefixed with `*` per AT&T syntax
///          while direct labels are passed through verbatim.
/// @param operand Operand describing the call target.
/// @param target Target lowering context for register/memory formatting.
/// @param format Object format controlling direct-symbol spelling.
/// @return Assembly string representing the call target.
std::string AsmEmitter::formatCallTarget(const Operand &operand,
                                         const TargetInfo &target,
                                         objfile::ObjFormat format) {
    return std::visit(
        Overload{[&](const OpLabel &label) {
                     if (auto mapped = il::runtime::mapCanonicalRuntimeName(label.name))
                         return formatSymbolReference(std::string{*mapped}, format);
                     return formatSymbolReference(zanna::common::MangleLink(label.name), format);
                 },
                 [&](const OpReg &reg) {
                     if (reg.cls != RegClass::GPR) {
                         throw std::runtime_error(
                             "x86-64 asm emitter: CALL requires a GPR register target");
                     }
                     return std::string{"*"} + formatReg(reg, target);
                 },
                 [&](const OpMem &mem) { return std::string{"*"} + formatMem(mem, target); },
                 [&](const OpImm &) -> std::string {
                     throw std::runtime_error(
                         "x86-64 asm emitter: CALL requires a label, register, or memory target");
                 },
                 [&](const OpRipLabel &label) {
                     return std::string{"*"} + formatRipLabel(label, format);
                 }},
        operand);
}

/// @brief Translate a Machine IR condition code into an x86 suffix.
/// @param code Integer encoding produced by the selector.
/// @return String view containing the condition suffix.
/// @throws std::runtime_error when @p code is not a known Machine IR condition.
std::string_view AsmEmitter::conditionSuffix(std::int64_t code) {
    switch (static_cast<int>(code)) {
        case 0:
            return "e";
        case 1:
            return "ne";
        case 2:
            return "l";
        case 3:
            return "le";
        case 4:
            return "g";
        case 5:
            return "ge";
        case 6:
            return "a";
        case 7:
            return "ae";
        case 8:
            return "b";
        case 9:
            return "be";
        case 10:
            return "p";
        case 11:
            return "np";
        case 12:
            return "o";
        case 13:
            return "no";
        default:
            throw std::runtime_error("x86-64 asm emitter: unknown condition code " +
                                     std::to_string(code));
    }
}

} // namespace zanna::codegen::x64
