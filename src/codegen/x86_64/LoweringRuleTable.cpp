//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/LoweringRuleTable.cpp
// Purpose: Translate declarative lowering rules into efficient opcode dispatch
//          queries for the x86-64 backend.
// Key invariants:
//   - Operand classifications must match the IL operand encodings.
//   - Dispatch tables remain immutable after initialisation.
// Ownership/Lifetime:
//   - Dispatch tables are computed on first use and cached for the lifetime
//     of the process.
// Links: src/codegen/x86_64/LoweringRuleTable.hpp,
//        src/codegen/x86_64/LoweringRules.cpp
//
//===----------------------------------------------------------------------===//

#include "LoweringRuleTable.hpp"

#include "LowerILToMIR.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

/**
 * @file
 * @brief Defines the declarative x86-64 lowering registry and indexed lookup.
 *
 * Rule specifications bind opcode names or prefixes and operand-shape
 * constraints to emitter callbacks. A process-lifetime dispatch cache partitions
 * exact opcodes from prefix families while preserving table order among
 * candidates that share a key.
 */

namespace zanna::codegen::x64 {

namespace lowering {

/**
 * @brief Complete ordered registry of IL-to-x86-64 lowering rules.
 *
 * Exact and prefix entries share one immutable source of truth. The order is
 * significant for overlapping candidates because lookup returns the first rule
 * whose opcode and operand shape both match.
 */
const std::array<RuleSpec, 57> kLoweringRuleTable = {
    // === Arithmetic Operations ===
    RuleSpec{"add",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitAdd,
             "add"},
    RuleSpec{"sub",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitSub,
             "sub"},
    RuleSpec{"mul",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitMul,
             "mul"},
    // === Overflow-Checked Arithmetic ===
    RuleSpec{"iadd.ovf",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitAddOvf,
             "iadd.ovf"},
    RuleSpec{"isub.ovf",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitSubOvf,
             "isub.ovf"},
    RuleSpec{"imul.ovf",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitMulOvf,
             "imul.ovf"},
    RuleSpec{"fdiv",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitFDiv,
             "fdiv"},
    // === Bitwise Operations ===
    RuleSpec{"and",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitAnd,
             "and"},
    RuleSpec{"or",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitOr,
             "or"},
    RuleSpec{"xor",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitXor,
             "xor"},
    // === Comparison Operations ===
    RuleSpec{"icmp_",
             OperandShape{2U,
                          3U,
                          3U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Immediate,
                           OperandKindPattern::Any}},
             RuleFlags::Prefix,
             &emitICmp,
             "icmp"},
    RuleSpec{"fcmp_",
             OperandShape{2U,
                          3U,
                          3U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Immediate,
                           OperandKindPattern::Any}},
             RuleFlags::Prefix,
             &emitFCmp,
             "fcmp"},
    // === Division/Remainder ===
    RuleSpec{"div",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "div"},
    RuleSpec{"sdiv",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "sdiv"},
    RuleSpec{"sdiv.chk0",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "sdiv.chk0"},
    RuleSpec{"srem",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "srem"},
    RuleSpec{"srem.chk0",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "srem.chk0"},
    RuleSpec{"udiv",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "udiv"},
    RuleSpec{"udiv.chk0",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "udiv.chk0"},
    RuleSpec{"urem",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "urem"},
    RuleSpec{"urem.chk0",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "urem.chk0"},
    RuleSpec{"rem",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitDivFamily,
             "rem"},
    // === Shift Operations ===
    RuleSpec{"shl",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitShiftLeft,
             "shl"},
    RuleSpec{"lshr",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitShiftLshr,
             "lshr"},
    RuleSpec{"ashr",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitShiftAshr,
             "ashr"},
    // === Comparison (explicit CMP) ===
    RuleSpec{"cmp",
             OperandShape{2U,
                          3U,
                          3U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Immediate,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitCmpExplicit,
             "cmp"},
    // === Control Flow ===
    RuleSpec{"select",
             OperandShape{3U,
                          3U,
                          3U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitSelect,
             "select"},
    RuleSpec{"br",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Label,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitBranch,
             "br"},
    RuleSpec{"cbr",
             OperandShape{3U,
                          3U,
                          3U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Label,
                           OperandKindPattern::Label,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitCondBranch,
             "cbr"},
    RuleSpec{"ret",
             OperandShape{0U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitReturn,
             "ret"},
    // === Calls ===
    RuleSpec{"call",
             OperandShape{1U,
                          std::numeric_limits<std::uint8_t>::max(),
                          1U,
                          {OperandKindPattern::Label,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitCall,
             "call"},
    RuleSpec{"call.indirect",
             OperandShape{1U,
                          std::numeric_limits<std::uint8_t>::max(),
                          1U,
                          {OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitCallIndirect,
             "call.indirect"},
    // === Memory Operations ===
    RuleSpec{"load",
             OperandShape{1U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Immediate,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitLoadAuto,
             "load"},
    RuleSpec{"store",
             OperandShape{2U,
                          3U,
                          3U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Immediate,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitStore,
             "store"},
    // === Type Conversions ===
    RuleSpec{"zext",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitZSTrunc,
             "zext"},
    RuleSpec{"sext",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitZSTrunc,
             "sext"},
    RuleSpec{"trunc",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitZSTrunc,
             "trunc"},
    RuleSpec{"sitofp",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitSIToFP,
             "sitofp"},
    RuleSpec{"fptosi",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitFPToSI,
             "fptosi"},
    RuleSpec{"fptosi_chk",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitFPToSIChecked,
             "fptosi_chk"},
    RuleSpec{"si_narrow_chk",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitSiNarrowChecked,
             "si_narrow_chk"},
    RuleSpec{"ui_narrow_chk",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitUiNarrowChecked,
             "ui_narrow_chk"},
    // === Exception Handling ===
    RuleSpec{"eh.push",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Label,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitEhPush,
             "eh.push"},
    RuleSpec{"eh.pop", OperandShape{0U, 0U, 0U, {}}, RuleFlags::None, &emitEhPop, "eh.pop"},
    RuleSpec{"eh.entry", OperandShape{0U, 0U, 0U, {}}, RuleFlags::None, &emitEhEntry, "eh.entry"},
    // === Miscellaneous ===
    RuleSpec{"trap",
             OperandShape{0U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitTrap,
             "trap"},
    RuleSpec{"const_str",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitConstStr,
             "const_str"},
    RuleSpec{"alloca",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Immediate,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitAlloca,
             "alloca"},
    RuleSpec{"gep",
             OperandShape{2U,
                          2U,
                          2U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitGEP,
             "gep"},
    // === Constants, Addresses, Casts, Checks, Control Flow ===
    RuleSpec{"const_null",
             OperandShape{0U,
                          0U,
                          0U,
                          {OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitConstNull,
             "const_null"},
    RuleSpec{"const_f64",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitConstF64,
             "const_f64"},
    RuleSpec{"gaddr",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitGAddr,
             "gaddr"},
    RuleSpec{"addr_of",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitAddrOf,
             "addr_of"},
    RuleSpec{"idx_chk",
             OperandShape{3U,
                          3U,
                          3U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Value,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitIdxChk,
             "idx_chk"},
    RuleSpec{"switch_i32",
             OperandShape{2U,
                          std::numeric_limits<std::uint8_t>::max(),
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitSwitchI32,
             "switch_i32"},
    RuleSpec{"fptoui",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitFpToUi,
             "fptoui"},
    RuleSpec{"uitofp",
             OperandShape{1U,
                          1U,
                          1U,
                          {OperandKindPattern::Value,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any,
                           OperandKindPattern::Any}},
             RuleFlags::None,
             &emitUiToFp,
             "uitofp"},
};

} // namespace lowering

namespace {

using lowering::hasFlag;
using lowering::OperandKindPattern;
using lowering::RuleFlags;
using lowering::RuleSpec;

/// @brief Categorise an IL operand for lowering rule matching.
/// @details Values tagged as labels represent control-flow targets, negative
///          identifiers denote immediates emitted during lowering, and all
///          remaining operands map to SSA temporaries.  The categories mirror
///          those referenced by the declarative lowering table.
/// @param value Operand extracted from an IL instruction.
/// @return Operand kind classification used by the rule engine.
OperandKindPattern classifyOperand(const ILValue &value) noexcept {
    if (value.kind == ILValue::Kind::LABEL) {
        return OperandKindPattern::Label;
    }
    if (value.id < 0) {
        return OperandKindPattern::Immediate;
    }
    return OperandKindPattern::Value;
}

/// @brief Check whether an instruction satisfies a declarative operand shape.
/// @details Validates arity constraints before comparing each operand against
///          the expected kind.  Patterns may mark elements as `Any` to bypass
///          matching and may allow extra operands when `maxArity` is `UINT8_MAX`.
/// @param shape Operand requirements captured in the lowering rule.
/// @param instr Instruction whose operands are being tested.
/// @return True when all mandatory constraints are met.
bool matchesOperandPattern(const lowering::OperandShape &shape, const ILInstr &instr) noexcept {
    const std::size_t arity = instr.ops.size();
    if (arity < shape.minArity) {
        return false;
    }
    if (shape.maxArity != std::numeric_limits<std::uint8_t>::max() && arity > shape.maxArity) {
        return false;
    }

    const std::size_t checkCount = std::min<std::size_t>(shape.kindCount, arity);
    for (std::size_t idx = 0; idx < checkCount; ++idx) {
        const OperandKindPattern expected = shape.kinds[idx];
        if (expected == OperandKindPattern::Any) {
            continue;
        }

        const OperandKindPattern actual = classifyOperand(instr.ops[idx]);
        if (expected == OperandKindPattern::Value) {
            if (actual == OperandKindPattern::Label) {
                return false;
            }
            continue;
        }

        if (expected != actual) {
            return false;
        }
    }

    return true;
}

/// @brief Determine whether a rule spec targets a given opcode.
/// @details Rules marked with the prefix flag treat their opcode string as a
///          prefix match; all other rules require an exact string match.
/// @param spec Rule specification to evaluate.
/// @param opcode Opcode mnemonic from the instruction.
/// @return True when the opcode falls within the rule's domain.
bool opcodeMatches(const RuleSpec &spec, std::string_view opcode) noexcept {
    if (hasFlag(spec.flags, RuleFlags::Prefix)) {
        return opcode.starts_with(spec.opcode);
    }
    return opcode == spec.opcode;
}

/// @brief Partitioned lookup tables for fast rule dispatch.
/// @details Splits the declarative rule table into:
///          - @c exact: an opcode-keyed hash map for O(1) exact-match lookup.
///          - @c prefix: a vector of rules whose opcode is a prefix (e.g.
///            "icmp_") that must be probed linearly.
///          Most IL opcodes hit the exact table; prefix probing is reserved
///          for opcode families.
struct DispatchTables {
    std::unordered_map<std::string_view, std::vector<const RuleSpec *>> exact{};
    std::vector<const RuleSpec *> prefix{};
};

/// @brief Construct the cached dispatch tables for rule lookup.
/// @details Partitions the declarative lowering table into exact and prefix
///          groups so that hot-path lookups can avoid scanning unrelated rules.
///          The resulting structure is consumed by @ref dispatchTables().
/// @return Aggregated dispatch tables ready for reuse across lookups.
DispatchTables buildDispatchTables() {
    DispatchTables tables{};
    for (const auto &spec : lowering::kLoweringRuleTable) {
        if (hasFlag(spec.flags, RuleFlags::Prefix)) {
            tables.prefix.push_back(&spec);
        } else {
            tables.exact[spec.opcode].push_back(&spec);
        }
    }
    return tables;
}

/// @brief Access the lazily constructed dispatch tables.
/// @details Builds the tables the first time the function is called and then
///          returns the cached instance on subsequent invocations.  Thread-safe
///          initialisation is guaranteed by the C++ static initialisation rules.
/// @return Reference to the cached dispatch tables.
const DispatchTables &dispatchTables() {
    static const DispatchTables tables = buildDispatchTables();
    return tables;
}

} // namespace

/// @brief Determine whether a lowering rule applies to an instruction.
/// @details Combines opcode and operand checks, providing a convenient wrapper
///          for callers that already have a concrete rule candidate.
/// @param spec Candidate rule specification.
/// @param instr Instruction being lowered.
/// @return True when the rule constraints are satisfied.
bool matchesRuleSpec(const lowering::RuleSpec &spec, const ILInstr &instr) {
    if (!opcodeMatches(spec, instr.opcode)) {
        return false;
    }
    return matchesOperandPattern(spec.operands, instr);
}

/// @brief Find the first lowering rule that matches an instruction.
/// @details Consults the exact-match table before scanning prefix rules,
///          returning as soon as a compatible candidate is identified.
/// @param instr Instruction that requires a lowering rule.
/// @return Pointer to the matching rule or nullptr if none applies.
const lowering::RuleSpec *lookupRuleSpec(const ILInstr &instr) {
    const auto &tables = dispatchTables();

    if (const auto it = tables.exact.find(instr.opcode); it != tables.exact.end()) {
        for (const auto *candidate : it->second) {
            if (matchesOperandPattern(candidate->operands, instr)) {
                return candidate;
            }
        }
    }

    for (const auto *candidate : tables.prefix) {
        if (opcodeMatches(*candidate, instr.opcode) &&
            matchesOperandPattern(candidate->operands, instr)) {
            return candidate;
        }
    }

    return nullptr;
}

} // namespace zanna::codegen::x64
