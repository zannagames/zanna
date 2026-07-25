//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/OpcodeMappings.hpp
// Purpose: Declarative IL-opcode -> AArch64-MIR-opcode lookup tables and the
//          O(1) switch-based accessors that drive instruction selection during
//          IL->MIR lowering (binary integer/FP arithmetic, integer/FP compare
//          condition codes, and opcode-category predicates).
// Key invariants:
//   - The constexpr arrays support documentation and category iteration;
//     lookupBinaryOp()/lookupCondition() are authoritative for selection and
//     additionally encode cases such as division and register-based shifts.
//   - Overflow opcodes (IAddOvf/ISubOvf/IMulOvf) map to dedicated *Ovf MIR
//     opcodes; division has no immediate form on AArch64 (supportsImmediate
//     is false for SDiv/UDiv and all FP ops).
//   - FP compare condition codes are selected so FCMP's NZCV result directly
//     implements ordered relational predicates and explicit ord/uno tests.
//   - All returned pointers/strings reference static storage with program
//     lifetime; callers may retain them freely.
// Ownership/Lifetime:
//   - Header-only constexpr/static data and stateless inline functions; no
//     dynamic allocation and no retained mutable state.
// Links: src/codegen/aarch64/MachineIR.hpp,
//        src/codegen/aarch64/FpCompareLowering.hpp,
//        src/il/core/Opcode.hpp, docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"
#include "il/core/Opcode.hpp"

/**
 * @file
 * @brief Defines AArch64 IL-to-MIR opcode and condition-code mappings.
 *
 * The switch-based lookup functions are the instruction selector's
 * authoritative constant-time interface. The constexpr arrays expose the
 * common mapping categories for iteration and documentation. Every returned
 * mapping pointer and condition string refers to static storage.
 */

/**
 * @brief Contains the AArch64-specific opcode mapping tables and lookup helpers.
 *
 * Integer and floating-point arithmetic map to MIR opcodes, while comparisons
 * map to the condition code consumed after `CMP` or `FCMP`.
 *
 * @see MachineIR.hpp
 * @see il/core/Opcode.hpp
 */
namespace zanna::codegen::aarch64 {

/**
 * @brief Describes register and optional immediate MIR forms for one binary IL opcode.
 *
 * `mirOp` is used when both operands reside in registers. When
 * `supportsImmediate` is true, instruction lowering may select `immOp` after
 * separately validating that the constant is encodable.
 */
struct BinaryOpMapping {
    il::core::Opcode ilOp{};      ///< The IL opcode this mapping applies to.
    MOpcode mirOp{};              ///< The register-register-register MIR opcode (e.g., ADD Xd, Xn, Xm).
    bool supportsImmediate{false}; ///< True if this operation has an immediate variant.
    MOpcode immOp{};              ///< The register-immediate MIR opcode (e.g., ADD Xd, Xn, #imm).
};

/**
 * @brief Associates an IL comparison with an AArch64 NZCV condition code.
 *
 * The condition is consumed after `CMP` or `FCMP` by instructions such as
 * `CSET`, `CSEL`, and `B.cond`.
 */
struct CompareMapping {
    il::core::Opcode ilOp{};       ///< The IL comparison opcode.
    const char *condition{nullptr}; ///< The AArch64 condition code string (e.g., "eq", "lt").
};

/// @brief Mapping entry for unary IL operations to AArch64 instructions.
///
/// Maps single-operand IL operations (like negation, bitwise NOT) to their
/// corresponding AArch64 machine instructions.
struct UnaryOpMapping {
    il::core::Opcode ilOp{}; ///< The IL unary opcode.
    MOpcode mirOp{};         ///< The MIR opcode for this unary operation.
};

/**
 * @brief Iteration table for common integer arithmetic and bitwise operations.
 *
 * Checked arithmetic maps to dedicated overflow pseudo-opcodes. This compact
 * table does not encode every selector distinction: use `lookupBinaryOp()` for
 * authoritative lowering, including division and register-based shift forms.
 */
constexpr BinaryOpMapping kBinaryIntOps[] = {
    {il::core::Opcode::Add, MOpcode::AddRRR, true, MOpcode::AddRI},
    {il::core::Opcode::IAddOvf, MOpcode::AddOvfRRR, true, MOpcode::AddOvfRI},
    {il::core::Opcode::Sub, MOpcode::SubRRR, true, MOpcode::SubRI},
    {il::core::Opcode::ISubOvf, MOpcode::SubOvfRRR, true, MOpcode::SubOvfRI},
    {il::core::Opcode::Mul, MOpcode::MulRRR, false, MOpcode::MulRRR},
    {il::core::Opcode::IMulOvf, MOpcode::MulOvfRRR, false, MOpcode::MulOvfRRR},
    {il::core::Opcode::And, MOpcode::AndRRR, true, MOpcode::AndRI},
    {il::core::Opcode::Or, MOpcode::OrrRRR, true, MOpcode::OrrRI},
    {il::core::Opcode::Xor, MOpcode::EorRRR, true, MOpcode::EorRI},
    {il::core::Opcode::Shl, MOpcode::LslRI, true, MOpcode::LslRI},
    {il::core::Opcode::LShr, MOpcode::LsrRI, true, MOpcode::LsrRI},
    {il::core::Opcode::AShr, MOpcode::AsrRI, true, MOpcode::AsrRI},
};

/**
 * @brief Iteration table for binary64 arithmetic operations.
 *
 * AArch64 floating-point arithmetic has no immediate operand form, so every
 * entry repeats its register opcode in `immOp` and sets `supportsImmediate`
 * to false.
 */
constexpr BinaryOpMapping kBinaryFpOps[] = {
    {il::core::Opcode::FAdd, MOpcode::FAddRRR, false, MOpcode::FAddRRR},
    {il::core::Opcode::FSub, MOpcode::FSubRRR, false, MOpcode::FSubRRR},
    {il::core::Opcode::FMul, MOpcode::FMulRRR, false, MOpcode::FMulRRR},
    {il::core::Opcode::FDiv, MOpcode::FDivRRR, false, MOpcode::FDivRRR},
};

/**
 * @brief Iteration table for integer comparisons and their NZCV conditions.
 *
 * Signed comparisons use `lt/le/gt/ge`; unsigned comparisons use the readable
 * lower/higher aliases `lo/ls/hi/hs`.
 */
constexpr CompareMapping kCompareOps[] = {
    {il::core::Opcode::ICmpEq, "eq"},
    {il::core::Opcode::ICmpNe, "ne"},
    {il::core::Opcode::SCmpLT, "lt"},
    {il::core::Opcode::SCmpLE, "le"},
    {il::core::Opcode::SCmpGT, "gt"},
    {il::core::Opcode::SCmpGE, "ge"},
    {il::core::Opcode::UCmpLT, "lo"},
    {il::core::Opcode::UCmpLE, "ls"},
    {il::core::Opcode::UCmpGT, "hi"},
    {il::core::Opcode::UCmpGE, "hs"},
};

/**
 * @brief Looks up the authoritative MIR forms for a binary IL opcode.
 *
 * @param op IL opcode to classify.
 * @return Pointer to an immutable, function-local static mapping, or `nullptr`
 *         when @p op is not a supported binary arithmetic/bitwise operation.
 * @note The returned pointer remains valid for the lifetime of the program.
 */
inline const BinaryOpMapping *lookupBinaryOp(il::core::Opcode op) {
    using Opc = il::core::Opcode;
    switch (op) {
        // Integer operations
        case Opc::Add: {
            static constexpr BinaryOpMapping m{Opc::Add, MOpcode::AddRRR, true, MOpcode::AddRI};
            return &m;
        }
        case Opc::IAddOvf: {
            static constexpr BinaryOpMapping m{
                Opc::IAddOvf, MOpcode::AddOvfRRR, true, MOpcode::AddOvfRI};
            return &m;
        }
        case Opc::Sub: {
            static constexpr BinaryOpMapping m{Opc::Sub, MOpcode::SubRRR, true, MOpcode::SubRI};
            return &m;
        }
        case Opc::ISubOvf: {
            static constexpr BinaryOpMapping m{
                Opc::ISubOvf, MOpcode::SubOvfRRR, true, MOpcode::SubOvfRI};
            return &m;
        }
        case Opc::Mul: {
            static constexpr BinaryOpMapping m{Opc::Mul, MOpcode::MulRRR, false, MOpcode::MulRRR};
            return &m;
        }
        case Opc::IMulOvf: {
            static constexpr BinaryOpMapping m{
                Opc::IMulOvf, MOpcode::MulOvfRRR, false, MOpcode::MulOvfRRR};
            return &m;
        }
        case Opc::And: {
            static constexpr BinaryOpMapping m{Opc::And, MOpcode::AndRRR, true, MOpcode::AndRI};
            return &m;
        }
        case Opc::Or: {
            static constexpr BinaryOpMapping m{Opc::Or, MOpcode::OrrRRR, true, MOpcode::OrrRI};
            return &m;
        }
        case Opc::Xor: {
            static constexpr BinaryOpMapping m{Opc::Xor, MOpcode::EorRRR, true, MOpcode::EorRI};
            return &m;
        }
        case Opc::Shl: {
            static constexpr BinaryOpMapping m{Opc::Shl, MOpcode::LslvRRR, true, MOpcode::LslRI};
            return &m;
        }
        case Opc::LShr: {
            static constexpr BinaryOpMapping m{Opc::LShr, MOpcode::LsrvRRR, true, MOpcode::LsrRI};
            return &m;
        }
        case Opc::AShr: {
            static constexpr BinaryOpMapping m{Opc::AShr, MOpcode::AsrvRRR, true, MOpcode::AsrRI};
            return &m;
        }
        // Division (no immediate form on AArch64)
        case Opc::SDiv: {
            static constexpr BinaryOpMapping m{
                Opc::SDiv, MOpcode::SDivRRR, false, MOpcode::SDivRRR};
            return &m;
        }
        case Opc::UDiv: {
            static constexpr BinaryOpMapping m{
                Opc::UDiv, MOpcode::UDivRRR, false, MOpcode::UDivRRR};
            return &m;
        }
        // Floating-point operations
        case Opc::FAdd: {
            static constexpr BinaryOpMapping m{
                Opc::FAdd, MOpcode::FAddRRR, false, MOpcode::FAddRRR};
            return &m;
        }
        case Opc::FSub: {
            static constexpr BinaryOpMapping m{
                Opc::FSub, MOpcode::FSubRRR, false, MOpcode::FSubRRR};
            return &m;
        }
        case Opc::FMul: {
            static constexpr BinaryOpMapping m{
                Opc::FMul, MOpcode::FMulRRR, false, MOpcode::FMulRRR};
            return &m;
        }
        case Opc::FDiv: {
            static constexpr BinaryOpMapping m{
                Opc::FDiv, MOpcode::FDivRRR, false, MOpcode::FDivRRR};
            return &m;
        }
        default:
            return nullptr;
    }
}

/**
 * @brief Looks up the condition code for an integer comparison.
 *
 * @param op IL opcode to classify.
 * @return Pointer to a static AArch64 condition spelling, or `nullptr` when
 *         @p op is not a supported integer comparison.
 */
inline const char *lookupCondition(il::core::Opcode op) {
    using Opc = il::core::Opcode;
    switch (op) {
        case Opc::ICmpEq:
            return "eq";
        case Opc::ICmpNe:
            return "ne";
        case Opc::SCmpLT:
            return "lt";
        case Opc::SCmpLE:
            return "le";
        case Opc::SCmpGT:
            return "gt";
        case Opc::SCmpGE:
            return "ge";
        case Opc::UCmpLT:
            return "lo";
        case Opc::UCmpLE:
            return "ls";
        case Opc::UCmpGT:
            return "hi";
        case Opc::UCmpGE:
            return "hs";
        default:
            return nullptr;
    }
}

/**
 * @brief Looks up the FCMP condition for a floating-point comparison.
 *
 * The selected relational conditions evaluate false for FCMP's unordered
 * result; `FCmpOrd` and `FCmpUno` directly test the V flag with `vc` and `vs`.
 *
 * @param op IL opcode to classify.
 * @return Pointer to a static condition spelling, or `nullptr` for a
 *         non-floating-point comparison.
 */
[[nodiscard]] inline const char *lookupFpCondition(il::core::Opcode op) {
    switch (op) {
        case il::core::Opcode::FCmpEQ:
            return "eq";
        case il::core::Opcode::FCmpNE:
            return "ne";
        case il::core::Opcode::FCmpLT:
            return "mi";
        case il::core::Opcode::FCmpLE:
            return "ls";
        case il::core::Opcode::FCmpGT:
            return "gt";
        case il::core::Opcode::FCmpGE:
            return "ge";
        case il::core::Opcode::FCmpOrd:
            return "vc";
        case il::core::Opcode::FCmpUno:
            return "vs";
        default:
            return nullptr;
    }
}

/**
 * @brief Looks up the condition code for any supported integer or FP comparison.
 *
 * @param op IL opcode to classify.
 * @return Static condition spelling, preferring the integer table, or
 *         `nullptr` when @p op is not a comparison.
 */
[[nodiscard]] inline const char *lookupAnyCondition(il::core::Opcode op) {
    if (const char *cc = lookupCondition(op))
        return cc;
    return lookupFpCondition(op);
}

/**
 * @brief Tests whether an IL opcode is a supported integer or FP comparison.
 *
 * @param op IL opcode to classify.
 * @return `true` when `lookupAnyCondition()` supplies a condition code.
 */
inline bool isCompareOp(il::core::Opcode op) {
    return lookupAnyCondition(op) != nullptr;
}

/**
 * @brief Tests whether an IL opcode is a supported floating-point comparison.
 *
 * @param op IL opcode to classify.
 * @return `true` when `lookupFpCondition()` supplies a condition code.
 */
inline bool isFloatingPointCompareOp(il::core::Opcode op) {
    return lookupFpCondition(op) != nullptr;
}

/**
 * @brief Tests whether an IL opcode is binary floating-point arithmetic.
 *
 * @param op IL opcode to classify.
 * @return `true` for `FAdd`, `FSub`, `FMul`, or `FDiv`; otherwise `false`.
 */
inline bool isFloatingPointOp(il::core::Opcode op) {
    switch (op) {
        case il::core::Opcode::FAdd:
        case il::core::Opcode::FSub:
        case il::core::Opcode::FMul:
        case il::core::Opcode::FDiv:
            return true;
        default:
            return false;
    }
}

} // namespace zanna::codegen::aarch64
