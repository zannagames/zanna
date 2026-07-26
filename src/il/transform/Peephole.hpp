//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/Peephole.hpp
// Purpose: Table-driven peephole optimisation pass -- matches instruction
//          patterns (constant operand, same-operands) and replaces them with
//          simpler equivalents (operand forwarding or literal synthesis).
//          Covers integer/float arithmetic identities, bitwise identities,
//          reflexive comparisons, and division/remainder simplifications.
// Key invariants:
//   - kRules is a compile-time constexpr array; adding rules does not require
//     modifying the pass engine.
//   - SSA form and value uses are maintained after replacement.
// Ownership/Lifetime: Free function operating on a caller-owned Module.
//          Rule table is static constexpr.
// Links: il/core/Opcode.hpp, il/core/fwd.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares table-driven local simplification rules for IL instructions.
 *
 * @details Each constexpr rule matches an opcode by a constant operand or
 *          identical operands, then either forwards an existing value or
 *          synthesizes a literal. The pass also coordinates related local CFG,
 *          strength-reduction, and string-literal folds while preserving trap
 *          and IEEE floating-point behavior.
 */

#pragma once

#include <array>

#include "il/core/Opcode.hpp"
#include "il/core/fwd.hpp"

namespace il::transform {

/// @brief Pattern describing when a rule should trigger.
struct Match {
    /// @brief Kinds of patterns the peephole engine supports.
    enum class Kind {
        ConstOperand,      ///< Match a specific integer constant at operand index.
        ConstFloatOperand, ///< Match a specific float constant at operand index.
        SameOperands       ///< Match when both operands are identical.
    };

    /// @brief Opcode to match.
    core::Opcode op{core::Opcode::Count};
    /// @brief Matching strategy to apply.
    Kind kind{Kind::ConstOperand};
    /// @brief Constant-bearing operand index, unused by same-operand matches.
    unsigned constIdx = 0;
    /// @brief Required integer payload for `ConstOperand`.
    long long value = 0;
    /// @brief Required floating payload for `ConstFloatOperand`.
    double floatValue = 0.0;
};

/// @brief Replacement describing how to rewrite a matched instruction.
struct Replace {
    /// @brief Strategy for producing the replacement value.
    enum class Kind {
        Operand,   ///< Forward an existing operand.
        Const,     ///< Synthesize an integer/boolean literal.
        ConstFloat ///< Synthesize a floating-point literal.
    };

    Kind kind{Kind::Operand};
    /// @brief Operand index forwarded when kind is `Operand`.
    unsigned operandIdx = 0;
    /// @brief Integer payload synthesized when kind is `Const`.
    long long constValue = 0;
    /// @brief Floating payload synthesized when kind is `ConstFloat`.
    double floatConstValue = 0.0;
    /// @brief Whether an integer replacement is an i1 boolean rather than i64.
    bool isBool = false;
};

/// @brief A peephole rule mapping a match to its replacement.
struct Rule {
    Match match;      ///< Match pattern.
    Replace repl;     ///< Replacement action.
    const char *name{nullptr}; ///< Debug identifier for tracing.
};

/// @brief Compile-time registry of peephole rules in matching order.
inline constexpr std::array<Rule, 61> kRules{{
    // Integer arithmetic identities (checked overflow variants)
    {{core::Opcode::IAddOvf, Match::Kind::ConstOperand, 0, 0},
     {Replace::Kind::Operand, 1, 0, false},
     "iadd.ovf+x0"},
    {{core::Opcode::IAddOvf, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "iadd.ovf+0x"},
    {{core::Opcode::ISubOvf, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "isub.ovf-x0"},
    {{core::Opcode::IMulOvf, Match::Kind::ConstOperand, 0, 1},
     {Replace::Kind::Operand, 1, 0, false},
     "imul.ovf*1x"},
    {{core::Opcode::IMulOvf, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Operand, 0, 0, false},
     "imul.ovf*x1"},
    {{core::Opcode::IMulOvf, Match::Kind::ConstOperand, 0, 0},
     {Replace::Kind::Const, 0, 0, false},
     "imul.ovf*0x"},
    {{core::Opcode::IMulOvf, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Const, 0, 0, false},
     "imul.ovf*x0"},
    {{core::Opcode::ISubOvf, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 0, false},
     "isub.ovf-xx"},

    // Bitwise identities
    {{core::Opcode::And, Match::Kind::ConstOperand, 0, -1},
     {Replace::Kind::Operand, 1, 0, false},
     "and-1x"},
    {{core::Opcode::And, Match::Kind::ConstOperand, 1, -1},
     {Replace::Kind::Operand, 0, 0, false},
     "andx-1"},
    {{core::Opcode::And, Match::Kind::ConstOperand, 0, 0},
     {Replace::Kind::Const, 0, 0, false},
     "and0x"},
    {{core::Opcode::And, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Const, 0, 0, false},
     "andx0"},
    {{core::Opcode::And, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "andxx"},
    {{core::Opcode::Or, Match::Kind::ConstOperand, 0, 0},
     {Replace::Kind::Operand, 1, 0, false},
     "or0x"},
    {{core::Opcode::Or, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "orx0"},
    {{core::Opcode::Or, Match::Kind::ConstOperand, 0, -1},
     {Replace::Kind::Const, 0, -1, false},
     "or-1x"},
    {{core::Opcode::Or, Match::Kind::ConstOperand, 1, -1},
     {Replace::Kind::Const, 0, -1, false},
     "orx-1"},
    {{core::Opcode::Or, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "orxx"},
    {{core::Opcode::Xor, Match::Kind::ConstOperand, 0, 0},
     {Replace::Kind::Operand, 1, 0, false},
     "xor0x"},
    {{core::Opcode::Xor, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "xorx0"},
    {{core::Opcode::Xor, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 0, false},
     "xorxx"},

    // Shift identities
    {{core::Opcode::Shl, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "shl0"},
    {{core::Opcode::LShr, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "lshr0"},
    {{core::Opcode::AShr, Match::Kind::ConstOperand, 1, 0},
     {Replace::Kind::Operand, 0, 0, false},
     "ashr0"},
    {{core::Opcode::Shl, Match::Kind::ConstOperand, 0, 0},
     {Replace::Kind::Const, 0, 0, false},
     "0shl"},
    {{core::Opcode::LShr, Match::Kind::ConstOperand, 0, 0},
     {Replace::Kind::Const, 0, 0, false},
     "0lshr"},
    {{core::Opcode::AShr, Match::Kind::ConstOperand, 0, 0},
     {Replace::Kind::Const, 0, 0, false},
     "0ashr"},

    // Reflexive comparisons
    {{core::Opcode::ICmpEq, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 1, 0.0, true},
     "icmp.eq-xx"},
    {{core::Opcode::ICmpNe, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 0, 0.0, true},
     "icmp.ne-xx"},
    {{core::Opcode::SCmpLT, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 0, 0.0, true},
     "scmp.lt-xx"},
    {{core::Opcode::SCmpLE, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 1, 0.0, true},
     "scmp.le-xx"},
    {{core::Opcode::SCmpGT, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 0, 0.0, true},
     "scmp.gt-xx"},
    {{core::Opcode::SCmpGE, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 1, 0.0, true},
     "scmp.ge-xx"},

    // Unsigned reflexive comparisons
    {{core::Opcode::UCmpLT, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 0, 0.0, true},
     "ucmp.lt-xx"},
    {{core::Opcode::UCmpLE, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 1, 0.0, true},
     "ucmp.le-xx"},
    {{core::Opcode::UCmpGT, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 0, 0.0, true},
     "ucmp.gt-xx"},
    {{core::Opcode::UCmpGE, Match::Kind::SameOperands, 0, 0},
     {Replace::Kind::Const, 0, 1, 0.0, true},
     "ucmp.ge-xx"},

    // Float reflexive comparisons: REMOVED
    // NaN == NaN is false, NaN != NaN is true, etc. (IEEE 754)
    // Cannot fold fcmp.* %x, %x because %x might be NaN at runtime.

    // Float arithmetic identities: x * 1.0 = x
    {{core::Opcode::FMul, Match::Kind::ConstFloatOperand, 0, 0, 1.0},
     {Replace::Kind::Operand, 1},
     "fmul*1x"},
    {{core::Opcode::FMul, Match::Kind::ConstFloatOperand, 1, 0, 1.0},
     {Replace::Kind::Operand, 0},
     "fmul*x1"},

    // Float arithmetic identities: x / 1.0 = x
    {{core::Opcode::FDiv, Match::Kind::ConstFloatOperand, 1, 0, 1.0},
     {Replace::Kind::Operand, 0},
     "fdiv/x1"},

    // Float x + 0.0 is intentionally not folded: IEEE signed-zero semantics
    // can make the replacement observably different.

    // Float arithmetic identities: x - 0.0 = x
    {{core::Opcode::FSub, Match::Kind::ConstFloatOperand, 1, 0, 0.0},
     {Replace::Kind::Operand, 0},
     "fsub-x0"},

    // Float arithmetic identities: x * 0.0 = 0.0 (note: not valid for NaN/Inf)
    // Skipped for safety - FP semantics require caution

    // Float arithmetic identities: x - x = 0.0 (note: not valid for NaN)
    // Skipped for safety - FP semantics require caution

    // Integer division identities: x / 1 = x
    {{core::Opcode::SDivChk0, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Operand, 0},
     "sdiv/x1"},
    {{core::Opcode::UDivChk0, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Operand, 0},
     "udiv/x1"},
    {{core::Opcode::SDiv, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Operand, 0},
     "sdiv.plain/x1"},
    {{core::Opcode::UDiv, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Operand, 0},
     "udiv.plain/x1"},

    // Integer remainder identities: x % 1 = 0
    {{core::Opcode::SRemChk0, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Const, 0, 0},
     "srem%x1"},
    {{core::Opcode::URemChk0, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Const, 0, 0},
     "urem%x1"},
    {{core::Opcode::SRem, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Const, 0, 0},
     "srem.plain%x1"},
    {{core::Opcode::URem, Match::Kind::ConstOperand, 1, 1},
     {Replace::Kind::Const, 0, 0},
     "urem.plain%x1"},

    // 0 / x and 0 % x are intentionally not folded for checked division:
    // the operation must still trap when x is zero.
}};

/// @brief Run peephole simplifications over @p m using registered rules.
/// @param m Module whose functions and literal-global table may be rewritten.
/// @details Applies control-flow folds, the compile-time rule table, power-of-two
///          strength reduction, and local literal-string concatenation folding.
void peephole(core::Module &m);

} // namespace il::transform
