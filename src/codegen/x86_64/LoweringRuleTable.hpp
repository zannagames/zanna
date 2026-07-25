//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/LoweringRuleTable.hpp
// Purpose: Describe declarative lowering rules for x86-64 emission.
// Key invariants:
//   - Rule table entries are immutable and indexed by opcode prefix.
//   - Operand patterns must align with IL operand encodings.
//   - Emit callbacks may only append to the MIRBuilder, never remove.
// Ownership/Lifetime:
//   - Shared across lowering translation units via inline constexpr data.
// Links: src/codegen/x86_64/LoweringRuleTable.cpp,
//        src/codegen/x86_64/LoweringRules.hpp,
//        src/codegen/x86_64/LowerILToMIR.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

/**
 * @file
 * @brief Declares x86-64 lowering emitters and their declarative dispatch table.
 *
 * Emitters translate one backend-facing IL instruction into MIR or delayed
 * lowering metadata. RuleSpec combines an opcode match, arity and operand-kind
 * constraints, and the emitter callback. Lookup uses immutable exact-opcode and
 * prefix-family indices built from the master table.
 */

namespace zanna::codegen::x64 {

struct ILInstr;
class MIRBuilder;

namespace lowering {

/// @brief Lower integer or F64 @c add using the result register class.
/// @details Integer immediates use ADDri when encodable; other cases use the
///          appropriate GPR or XMM register form.
///
/// @param instr The IL add instruction with two value operands.
/// @param builder The MIR builder to append instructions to.
void emitAdd(const ILInstr &instr, MIRBuilder &builder);

/// @brief Lower integer or F64 @c sub using a destructive two-operand form.
///
/// @param instr The IL sub instruction with two value operands.
/// @param builder The MIR builder to append instructions to.
void emitSub(const ILInstr &instr, MIRBuilder &builder);

/// @brief Lower integer or F64 @c mul to the matching GPR or XMM operation.
///
/// @param instr The IL mul instruction with two value operands.
/// @param builder The MIR builder to append instructions to.
void emitMul(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits overflow-checked integer addition pseudo-op.
/// @param instr The IL iadd.ovf instruction with two integer operands.
/// @param builder The MIR builder to append instructions to.
void emitAddOvf(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits overflow-checked integer subtraction pseudo-op.
/// @param instr The IL isub.ovf instruction with two integer operands.
/// @param builder The MIR builder to append instructions to.
void emitSubOvf(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits overflow-checked integer multiplication pseudo-op.
/// @param instr The IL imul.ovf instruction with two integer operands.
/// @param builder The MIR builder to append instructions to.
void emitMulOvf(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `fdiv` instruction (floating-point division).
///
/// Generates the backend's scalar-double FDIV MIR operation in XMM registers.
///
/// @param instr The IL fdiv instruction with two floating-point operands.
/// @param builder The MIR builder to append instructions to.
void emitFDiv(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `and` instruction (bitwise AND).
///
/// Generates an AND instruction that computes the bitwise AND of two operands.
/// Sets the ZF flag if the result is zero, which can be used for conditional branching.
///
/// @param instr The IL and instruction with two value operands.
/// @param builder The MIR builder to append instructions to.
void emitAnd(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `or` instruction (bitwise OR).
///
/// Generates an OR instruction that computes the bitwise inclusive OR of two operands.
///
/// @param instr The IL or instruction with two value operands.
/// @param builder The MIR builder to append instructions to.
void emitOr(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `xor` instruction (bitwise exclusive OR).
///
/// Generates a XOR instruction. A common idiom is XOR reg, reg to zero a register
/// (shorter encoding than MOV reg, 0), but this function handles the general case.
///
/// @param instr The IL xor instruction with two value operands.
/// @param builder The MIR builder to append instructions to.
void emitXor(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `icmp_*` family (integer comparisons).
///
/// Handles all integer comparison variants: icmp_eq, icmp_ne, scmp_lt, scmp_le,
/// scmp_gt, scmp_ge (signed), ucmp_lt, ucmp_le, ucmp_gt, ucmp_ge (unsigned).
/// Generates a CMP instruction followed by a SETcc to materialize the boolean result.
///
/// @param instr The IL comparison instruction with opcode prefix "icmp_" or "scmp_"/"ucmp_".
/// @param builder The MIR builder to append instructions to.
void emitICmp(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `fcmp_*` family (floating-point comparisons).
///
/// Handles floating-point comparisons using UCOMISD/UCOMISS instructions. These
/// set the EFLAGS differently than integer comparisons (unordered results set PF).
/// The comparison predicate is encoded in the opcode suffix.
///
/// @param instr The IL fcmp instruction with opcode prefix "fcmp_".
/// @param builder The MIR builder to append instructions to.
void emitFCmp(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL division family (div, sdiv, udiv, srem, urem, rem).
///
/// Handles all division and remainder operations by emitting a signed or
/// unsigned div/rem pseudo. A later pass performs constant strength reduction
/// or expands the pseudo around x86-64's implicit RDX:RAX pair.
///
/// @param instr The IL division instruction (div, sdiv, udiv, rem, srem, urem).
/// @param builder The MIR builder to append instructions to.
void emitDivFamily(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `shl` instruction (shift left).
///
/// Generates a SHL instruction. The shift amount can be an immediate (0-63) or
/// in the CL register. This function may need to move the shift amount to CL
/// if it's in another register.
///
/// @param instr The IL shl instruction with value and shift amount operands.
/// @param builder The MIR builder to append instructions to.
void emitShiftLeft(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `lshr` instruction (logical shift right).
///
/// Generates a SHR instruction for unsigned (logical) right shift. Zeros are
/// shifted in from the left, regardless of the sign bit.
///
/// @param instr The IL lshr instruction with value and shift amount operands.
/// @param builder The MIR builder to append instructions to.
void emitShiftLshr(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `ashr` instruction (arithmetic shift right).
///
/// Generates a SAR instruction for signed (arithmetic) right shift. The sign bit
/// is replicated into the vacated high-order bits, preserving the sign of the value.
///
/// @param instr The IL ashr instruction with value and shift amount operands.
/// @param builder The MIR builder to append instructions to.
void emitShiftAshr(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for explicit CMP instruction.
///
/// Generates a CMP instruction without materializing a boolean result. Used when
/// the comparison result is consumed directly by a conditional branch rather than
/// stored in a register.
///
/// @param instr The IL cmp instruction with two value operands.
/// @param builder The MIR builder to append instructions to.
void emitCmpExplicit(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `select` instruction (conditional select).
///
/// Generates a CMOV (conditional move) instruction that selects between two values
/// based on a condition. Equivalent to the C ternary operator: `cond ? true_val : false_val`.
///
/// @param instr The IL select instruction with condition and two value operands.
/// @param builder The MIR builder to append instructions to.
void emitSelect(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `br` instruction (unconditional branch).
///
/// Generates a JMP instruction to the target basic block label.
///
/// @param instr The IL br instruction with a label operand.
/// @param builder The MIR builder to append instructions to.
void emitBranch(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `cbr` instruction (conditional branch).
///
/// Generates a TEST and Jcc (conditional jump) sequence. The condition value is
/// tested against zero, and control transfers to either the true or false target
/// based on the result.
///
/// @param instr The IL cbr instruction with condition, true label, and false label.
/// @param builder The MIR builder to append instructions to.
void emitCondBranch(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `ret` instruction (function return).
///
/// Moves an optional result into the target descriptor's integer or F64 return
/// register and emits RET. Frame lowering inserts the epilogue later.
///
/// @param instr The IL ret instruction with optional return value operand.
/// @param builder The MIR builder to append instructions to.
void emitReturn(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `call` instruction (direct function call).
///
/// Records a target-independent call plan, emits a tagged direct CALL, and
/// captures an optional result. ABI argument placement is deferred.
///
/// @param instr The IL call instruction with function label and argument operands.
/// @param builder The MIR builder to append instructions to.
void emitCall(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `call.indirect` instruction (indirect function call).
///
/// Generates a CALL instruction through a function pointer. The first operand is
/// the address to call (in a register), followed by the arguments.
///
/// @param instr The IL call.indirect instruction with address and argument operands.
/// @param builder The MIR builder to append instructions to.
void emitCallIndirect(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `load` instruction (memory load).
///
/// Emits a GPR or scalar-double load from a base pointer with an optional byte
/// displacement, folding a proven indexed-address pattern when possible.
///
/// @param instr The IL load instruction with address and optional offset operands.
/// @param builder The MIR builder to append instructions to.
void emitLoadAuto(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `store` instruction (memory store).
///
/// Generates a MOV instruction to store a value to memory. The operands are the
/// destination address, the value to store, and an optional immediate offset.
///
/// @param instr The IL store instruction with address, value, and optional offset.
/// @param builder The MIR builder to append instructions to.
void emitStore(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL type conversion instructions (zext, sext, trunc).
///
/// Copies the source and emits masks or shift pairs according to source/result
/// bit widths so narrow values remain canonically extended.
///
/// @param instr The IL conversion instruction (zext, sext, or trunc).
/// @param builder The MIR builder to append instructions to.
void emitZSTrunc(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `sitofp` instruction (signed int to floating-point).
///
/// Generates CVTSI2SD for the backend's F64 result representation.
///
/// @param instr The IL sitofp instruction with integer operand.
/// @param builder The MIR builder to append instructions to.
void emitSIToFP(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `fptosi` instruction (floating-point to signed int).
///
/// Rejects NaN and values outside the destination width, then generates
/// CVTTSD2SI using truncation toward zero.
///
/// @param instr The IL fptosi instruction with floating-point operand.
/// @param builder The MIR builder to append instructions to.
void emitFPToSI(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for checked `fptosi_chk` (float to signed int with NaN/overflow trap).
///
/// Rounds to nearest-even through the runtime helper, performs explicit NaN
/// and range checks, then generates CVTTSD2SI or a typed runtime trap.
///
/// @param instr The IL fptosi_chk instruction with floating-point operand.
/// @param builder The MIR builder to append instructions to.
void emitFPToSIChecked(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `fptoui` instruction (float to unsigned int, checked).
/// @param instr The IL fptoui instruction with F64 operand.
/// @param builder The MIR builder to append instructions to.
void emitFpToUi(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `uitofp` instruction (unsigned int to float).
/// @param instr The IL uitofp instruction with integer operand.
/// @param builder The MIR builder to append instructions to.
void emitUiToFp(const ILInstr &instr, MIRBuilder &builder);

/// @brief Narrow a signed integer and trap if re-extension changes its value.
/// @param instr Checked signed-narrowing instruction.
/// @param builder MIR builder receiving the check and result.
void emitSiNarrowChecked(const ILInstr &instr, MIRBuilder &builder);

/// @brief Narrow an unsigned integer and trap if re-extension changes its value.
/// @param instr Checked unsigned-narrowing instruction.
/// @param builder MIR builder receiving the check and result.
void emitUiNarrowChecked(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `const_null` instruction (null pointer constant).
/// @param instr The IL const_null instruction (no operands).
/// @param builder The MIR builder to append instructions to.
void emitConstNull(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `const_f64` instruction (F64 constant).
/// @param instr The IL const_f64 instruction with F64 value operand.
/// @param builder The MIR builder to append instructions to.
void emitConstF64(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `gaddr` instruction (global address).
/// @param instr The IL gaddr instruction with global reference operand.
/// @param builder The MIR builder to append instructions to.
void emitGAddr(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `addr_of` instruction (address of local alloca).
/// @param instr The IL addr_of instruction with alloca value operand.
/// @param builder The MIR builder to append instructions to.
void emitAddrOf(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `idx_chk` instruction (bounds check).
/// @details Emits inline CMP + JCC + UD2 sequences to trap on out-of-bounds access.
/// @param instr The IL idx_chk instruction: index, lower bound, upper bound.
/// @param builder The MIR builder to append instructions to.
void emitIdxChk(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emit x86-64 MIR for an IL @c switch_i32 multi-way branch.
/// @details Selects a short linear chain, balanced compare tree, or dense jump
///          table according to validated case count and density.
/// @param instr The IL switch_i32 instruction with scrutinee, case pairs, default.
/// @param builder The MIR builder to append instructions to.
void emitSwitchI32(const ILInstr &instr, MIRBuilder &builder);

/// @brief Ignore a residual @c eh.push marker after NativeEHLowering.
///
/// @param instr The IL eh.push instruction with handler label operand.
/// @param builder The MIR builder to append instructions to.
void emitEhPush(const ILInstr &instr, MIRBuilder &builder);

/// @brief Ignore a residual @c eh.pop marker after NativeEHLowering.
///
/// @param instr The IL eh.pop instruction (no operands).
/// @param builder The MIR builder to append instructions to.
void emitEhPop(const ILInstr &instr, MIRBuilder &builder);

/// @brief Ignore a residual @c eh.entry marker after NativeEHLowering.
///
/// @param instr The IL eh.entry instruction (no operands).
/// @param builder The MIR builder to append instructions to.
void emitEhEntry(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `trap` instruction (raise runtime error).
///
/// Plans a call to @c rt_trap_string with the optional managed-string payload
/// and appends UD2 as a non-returning safety net.
///
/// @param instr The IL trap instruction with optional trap code operand.
/// @param builder The MIR builder to append instructions to.
void emitTrap(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `const_str` instruction (string constant).
///
/// Adds the literal bytes to rodata, calls @c rt_str_from_lit through the normal
/// ABI plan, and places the managed string handle in the result vreg.
///
/// @param instr The IL const_str instruction with string index operand.
/// @param builder The MIR builder to append instructions to.
void emitConstStr(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `alloca` instruction (stack allocation).
///
/// Reserves space on the stack for local variables. Unlike C's alloca, this
/// typically happens at function entry with a fixed size, adjusting RSP once
/// rather than dynamically.
///
/// @param instr The IL alloca instruction with size immediate operand.
/// @param builder The MIR builder to append instructions to.
void emitAlloca(const ILInstr &instr, MIRBuilder &builder);

/// @brief Emits x86-64 MIR for IL `gep` instruction (get element pointer).
///
/// Computes a pointer from a base plus either an immediate byte displacement or
/// a dynamic GPR offset.
///
/// @param instr The IL gep instruction with base pointer and index operands.
/// @param builder The MIR builder to append instructions to.
void emitGEP(const ILInstr &instr, MIRBuilder &builder);

/// @brief Bitflags that modify how a lowering rule matches IL instructions.
///
/// Rule flags customize the matching behavior for instruction selection. Currently
/// the only flag is `Prefix`, which enables prefix-based opcode matching for opcodes
/// that share a common handler (e.g., all `icmp_*` variants use one emit function).
///
/// ## Flag Combinations
///
/// Flags can be combined using the bitwise OR operator:
/// ```cpp
/// RuleFlags combined = RuleFlags::Prefix | RuleFlags::SomeOtherFlag;
/// ```
///
/// @see RuleSpec::flags for usage in rule definitions
enum class RuleFlags : std::uint8_t {
    /// @brief No special matching behavior; opcode must match exactly.
    None = 0,

    /// @brief The rule's opcode string is a prefix, not an exact match.
    ///
    /// When this flag is set, a rule with opcode "icmp_" will match instructions
    /// with opcodes like "icmp_eq", "icmp_ne", "icmp_lt", etc. This allows a
    /// single rule to handle a family of related opcodes that share the same
    /// emit logic, with the emit function examining the full opcode to determine
    /// the specific variant.
    Prefix = 1U << 0,
};

/// @brief Combines two RuleFlags values using bitwise OR.
/// @param lhs The left-hand flag value.
/// @param rhs The right-hand flag value.
/// @return The combined flags.
constexpr RuleFlags operator|(RuleFlags lhs, RuleFlags rhs) noexcept {
    return static_cast<RuleFlags>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

/// @brief Computes the intersection of two RuleFlags values using bitwise AND.
/// @param lhs The left-hand flag value.
/// @param rhs The right-hand flag value.
/// @return The flags present in both operands.
constexpr RuleFlags operator&(RuleFlags lhs, RuleFlags rhs) noexcept {
    return static_cast<RuleFlags>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

/// @brief Tests whether a specific flag is set in a flags value.
/// @param flags The flags value to test.
/// @param flag The specific flag to check for.
/// @return True if the flag is set, false otherwise.
constexpr bool hasFlag(RuleFlags flags, RuleFlags flag) noexcept {
    return (flags & flag) != RuleFlags::None;
}

/// @brief Specifies what kind of operand is expected at a given position.
///
/// When matching IL instructions to lowering rules, each operand position can have
/// a constraint on what kind of operand is allowed. This helps ensure rules are
/// only applied to instructions with compatible operand types.
///
/// ## Operand Kind Hierarchy
///
/// The IL has three fundamental operand kinds:
/// - **Value**: Any non-label value, including SSA references and immediates
/// - **Label**: A basic block label (e.g., `@entry`, `@loop_header`)
/// - **Immediate**: A literal constant (e.g., `42`, `3.14`)
///
/// The `Any` pattern matches all three kinds. `Value` intentionally accepts
/// both SSA and immediate values; `Label` and `Immediate` are exact constraints.
///
/// @see OperandShape for combining patterns into instruction-level constraints
enum class OperandKindPattern : std::uint8_t {
    /// @brief Matches any operand kind (value, label, or immediate).
    /// Use this for operands where the emit function handles all cases.
    Any,

    /// @brief Matches any non-label value, whether SSA-backed or immediate.
    /// Emitters remain responsible for register materialization when required.
    Value,

    /// @brief Matches only label operands (basic block references).
    /// Used for branch targets and call destinations (for direct calls).
    Label,

    /// @brief Matches only immediate operands (literal constants).
    /// Used for constants that can be encoded directly in the instruction.
    Immediate,
};

/// @brief Describes the expected shape of an IL instruction's operand list.
///
/// An operand shape specifies constraints on both the number of operands (arity)
/// and the kind of each operand (value, label, immediate). The lowering rule
/// matcher uses this information to filter candidate rules before invoking the
/// emit callback.
///
/// ## Arity Constraints
///
/// The `minArity` and `maxArity` fields define the acceptable range of operand
/// counts. For example:
/// - `{1, 1}`: Exactly one operand (unary operation)
/// - `{2, 2}`: Exactly two operands (binary operation)
/// - `{0, 1}`: Zero or one operand (optional result like `ret`)
/// - `{1, 255}`: One or more operands (variadic like `call`)
///
/// ## Kind Patterns
///
/// The `kinds` array specifies the expected kind for up to 4 operands. The
/// `kindCount` field indicates how many entries in `kinds` are meaningful.
/// Operands beyond `kindCount` are not checked (implicitly `Any`).
///
/// @par Example: Binary Value Operation
/// ```cpp
/// OperandShape{2U, 2U, 2U,
///     {OperandKindPattern::Value, OperandKindPattern::Value,
///      OperandKindPattern::Any, OperandKindPattern::Any}}
/// ```
/// This matches exactly two non-label operands.
///
/// @see RuleSpec for the complete rule definition structure
struct OperandShape {
    /// @brief Minimum number of operands required for a match.
    std::uint8_t minArity{0};

    /// @brief Maximum number of operands allowed for a match.
    /// Use `std::numeric_limits<std::uint8_t>::max()` for variadic operations.
    std::uint8_t maxArity{std::numeric_limits<std::uint8_t>::max()};

    /// @brief Number of entries in `kinds` that should be checked.
    /// Set to 0 if operand kinds don't matter, only arity.
    std::uint8_t kindCount{0};

    /// @brief Expected operand kind for positions 0-3.
    /// Only the first `kindCount` entries are checked during matching.
    std::array<OperandKindPattern, 4> kinds{OperandKindPattern::Any,
                                            OperandKindPattern::Any,
                                            OperandKindPattern::Any,
                                            OperandKindPattern::Any};
};

/// @brief Complete specification of a lowering rule for instruction selection.
///
/// A RuleSpec binds together all the information needed to match an IL instruction
/// and emit the corresponding x86-64 MIR. The lowering pass iterates through the
/// rule table, finds matching rules, and invokes their emit callbacks.
///
/// ## Matching Process
///
/// A rule matches an IL instruction if:
/// 1. The opcode matches (exact or prefix, depending on flags)
/// 2. The operand count is within [minArity, maxArity]
/// 3. Each operand satisfies the corresponding pattern (if kindCount > 0)
///
/// ## Example Rule
///
/// ```cpp
/// RuleSpec{"add",
///          OperandShape{2U, 2U, 2U,
///              {OperandKindPattern::Value, OperandKindPattern::Value,
///               OperandKindPattern::Any, OperandKindPattern::Any}},
///          RuleFlags::None,
///          &emitAdd,
///          "add"}
/// ```
///
/// This rule:
/// - Matches IL opcode "add" exactly (no Prefix flag)
/// - Requires exactly 2 operands (minArity=2, maxArity=2)
/// - Both operands must be values (register references)
/// - Invokes `emitAdd()` to generate the MIR
///
/// @see kLoweringRuleTable for the complete set of rules
/// @see matchesRuleSpec() for the matching implementation
struct RuleSpec {
    /// @brief The IL opcode string to match.
    /// If RuleFlags::Prefix is set, this is a prefix (e.g., "icmp_" matches "icmp_eq").
    /// Otherwise, this must match the instruction's opcode exactly.
    std::string_view opcode{};

    /// @brief Constraints on the instruction's operand list (arity and kinds).
    OperandShape operands{};

    /// @brief Flags that modify matching behavior (e.g., prefix matching).
    RuleFlags flags{RuleFlags::None};

    /// @brief The emit callback that generates MIR for matched instructions.
    /// This function reads the IL instruction and appends MIR to the builder.
    /// Must never be nullptr for valid rules.
    void (*emit)(const ILInstr &, MIRBuilder &) = nullptr;

    /// @brief Human-readable name for diagnostics and debugging.
    /// Typically the same as `opcode` but without the trailing underscore for prefix rules.
    const char *name{nullptr};
};

/// @brief Master table of all x86-64 instruction lowering rules.
///
/// This array contains the complete set of rules for transforming IL
/// instructions into x86-64 MIR. The lowering pass searches this table
/// (via lookupRuleSpec) to find matching rules for each IL instruction.
/// Defined in LoweringRuleTable.cpp.
///
/// @see lookupRuleSpec() to find a rule for an instruction
/// @see matchesRuleSpec() for the rule matching implementation
extern const std::array<RuleSpec, 57> kLoweringRuleTable;

} // namespace lowering

/// @brief Tests whether a lowering rule matches an IL instruction.
///
/// Performs the full matching algorithm to determine if a RuleSpec can handle
/// a given IL instruction. The matching process checks:
///
/// 1. **Opcode Match**: The instruction's opcode must match the rule's opcode.
///    If RuleFlags::Prefix is set, the rule's opcode is treated as a prefix
///    (e.g., rule "icmp_" matches instruction "icmp_eq").
///
/// 2. **Arity Check**: The instruction's operand count must be within the
///    rule's [minArity, maxArity] range (inclusive).
///
/// 3. **Kind Check**: For each operand position up to `kindCount`, the operand's
///    kind must match the pattern. OperandKindPattern::Any matches anything.
///
/// @param spec The lowering rule specification to test.
/// @param instr The IL instruction to match against.
/// @return True if the rule can handle this instruction, false otherwise.
///
/// @par Example Usage
/// ```cpp
/// for (const auto& rule : kLoweringRuleTable) {
///     if (matchesRuleSpec(rule, instr)) {
///         rule.emit(instr, builder);
///         break;
///     }
/// }
/// ```
///
/// @see lookupRuleSpec() for a convenience wrapper that returns the first match
/// @see RuleSpec for details on the matching criteria
bool matchesRuleSpec(const lowering::RuleSpec &spec, const ILInstr &instr);

/// @brief Finds the first lowering rule that matches an IL instruction.
///
/// Probes the cached exact-opcode hash table first, then the small ordered list
/// of prefix rules, and returns the first candidate whose operand shape matches.
/// This is the primary entry point for instruction selection during lowering.
///
/// ## Performance Note
///
/// Exact opcodes normally require one hash lookup plus a short same-key candidate
/// scan. Only prefix families such as @c icmp_ require linear probing.
///
/// ## No Match Handling
///
/// If no rule matches, this function returns nullptr. The caller should handle
/// this case, typically by reporting an error (unknown IL instruction) or
/// falling back to a generic lowering strategy.
///
/// @param instr The IL instruction to find a rule for.
/// @return Pointer to the matching RuleSpec, or nullptr if no rule matches.
///
/// @par Example Usage
/// ```cpp
/// void lowerInstruction(const ILInstr& instr, MIRBuilder& builder) {
///     const auto* rule = lookupRuleSpec(instr);
///     if (!rule) {
///         reportError("Unknown IL instruction: " + instr.opcode);
///         return;
///     }
///     rule->emit(instr, builder);
/// }
/// ```
///
/// @see kLoweringRuleTable for the complete set of rules
/// @see matchesRuleSpec() for the matching algorithm details
const lowering::RuleSpec *lookupRuleSpec(const ILInstr &instr);

} // namespace zanna::codegen::x64
