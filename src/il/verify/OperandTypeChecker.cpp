//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the helper that cross-checks instruction operand types against the
// metadata published by @ref il::core::OpcodeInfo.  The checker feeds the
// verifier diagnostic sink when mismatches are detected.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Operand type checking helper for the IL verifier.
/// @details Encapsulates the logic required to compare inferred operand types
///          against opcode metadata, handling integer range checks and
///          instruction-type-dependent operands.

#include "il/verify/OperandTypeChecker.hpp"

#include "il/core/Instr.hpp"
#include "il/verify/DiagFormat.hpp"
#include "il/verify/InstructionCheckUtils.hpp"
#include "il/verify/TypeInference.hpp"

#include <sstream>

using il::core::kindToString;
using il::core::TypeCategory;
using il::support::Expected;
using il::support::makeError;
using il::verify::detail::fitsInIntegerKind;
using il::verify::detail::kindFromCategory;

namespace il::verify::detail {
namespace {

/// @brief Permit i64 bitwise operations to consume narrower integer values.
/// @details BASIC widens i16/i32 values by masking the sign-extended scalar slot
///          with an i64 bitwise op.  The operation result remains i64; this only
///          relaxes the operand width for integer temps that already occupy the
///          VM/native scalar register width.
/// @param op Opcode whose operands are being checked.
/// @param expectedKind Metadata-required kind.
/// @param actualKind Inferred operand kind.
/// @return `true` only for i16/i32 operands of i64 `and`, `or`, or `xor`.
bool acceptsWidenedBitwiseOperand(il::core::Opcode op,
                                  il::core::Type::Kind expectedKind,
                                  il::core::Type::Kind actualKind) {
    if (expectedKind != il::core::Type::Kind::I64)
        return false;

    switch (op) {
        case il::core::Opcode::And:
        case il::core::Opcode::Or:
        case il::core::Opcode::Xor:
            return actualKind == il::core::Type::Kind::I16 ||
                   actualKind == il::core::Type::Kind::I32;
        default:
            return false;
    }
}

} // namespace

/// @brief Construct an operand checker bound to a verification context.
/// @details Stores references to the current @ref VerifyCtx and opcode metadata
///          so later calls to @ref run can validate operands without additional
///          lookups.
/// @param ctx Verification context for the instruction under inspection.
/// @param spec Opcode specification describing operand expectations.
OperandTypeChecker::OperandTypeChecker(const VerifyCtx &ctx, const InstructionSpec &spec)
    : ctx_(ctx), spec_(spec) {}

/// @brief Validate each operand against the opcode's type requirements.
/// @details Iterates over the instruction operands, mapping opcode categories to
///          concrete kinds and comparing them with the inferred types.  Integer
///          literals are range-checked to avoid silent truncation, and missing
///          type information results in diagnostics.  Success returns an empty
///          Expected; failures propagate the diagnostic produced by @ref report.
/// @return Empty Expected on success or the emitted diagnostic on failure.
Expected<void> OperandTypeChecker::run() const {
    const auto &instr = ctx_.instr;

    for (size_t index = 0; index < instr.operands.size() && index < spec_.operandTypes.size();
         ++index) {
        const TypeCategory category = spec_.operandTypes[index];
        if (category == TypeCategory::None || category == TypeCategory::Any ||
            category == TypeCategory::Dynamic)
            continue;

        il::core::Type::Kind expectedKind;
        if (category == TypeCategory::InstrType) {
            if (instr.type.kind == il::core::Type::Kind::Void) {
                if (spec_.strategy == VerifyStrategy::IntegerBinary)
                    continue;
                return report("instruction type must be non-void");
            }
            expectedKind = instr.type.kind;
        } else if (auto mapped = kindFromCategory(category)) {
            expectedKind = *mapped;
        } else {
            continue;
        }

        const auto &operand = instr.operands[index];
        if (operand.kind == il::core::Value::Kind::ConstInt) {
            switch (expectedKind) {
                case il::core::Type::Kind::I1:
                    if (!fitsInIntegerKind(operand.i64, expectedKind)) {
                        std::ostringstream ss;
                        ss << "operand " << index << " constant out of range for i1";
                        return report(ss.str());
                    }
                    continue;
                case il::core::Type::Kind::I16:
                case il::core::Type::Kind::I32:
                case il::core::Type::Kind::I64:
                    if (!fitsInIntegerKind(operand.i64, expectedKind)) {
                        std::ostringstream ss;
                        ss << "operand " << index << " constant out of range for "
                           << kindToString(expectedKind);
                        return report(ss.str());
                    }
                    continue;
                default:
                    break;
            }
        }

        bool missing = false;
        const il::core::Type actual = ctx_.types.valueType(operand, &missing);
        if (missing) {
            std::ostringstream ss;
            ss << "operand " << index << " type is unknown";
            return report(ss.str());
        }

        if (actual.kind != expectedKind) {
            if (acceptsWidenedBitwiseOperand(ctx_.instr.op, expectedKind, actual.kind))
                continue;

            std::ostringstream ss;
            ss << "operand type mismatch: ";
            if (expectedKind == il::core::Type::Kind::Ptr) {
                ss << "pointer type mismatch: operand " << index << " must be ptr";
            } else {
                ss << "operand " << index << " must be " << kindToString(expectedKind);
            }
            return report(ss.str());
        }
    }

    return {};
}

/// @brief Emit a formatted diagnostic describing a type mismatch.
/// @details Wraps @ref makeError with instruction-specific context so callers
///          receive actionable feedback.
/// @param message Human-readable description of the violation.
/// @return Expected containing the emitted diagnostic for chaining.
Expected<void> OperandTypeChecker::report(std::string_view message) const {
    return Expected<void>{
        makeError(ctx_.instr.loc, formatInstrDiag(ctx_.fn, ctx_.block, ctx_.instr, message))};
}

} // namespace il::verify::detail
