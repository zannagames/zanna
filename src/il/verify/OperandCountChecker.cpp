//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the operand-count verification helper used by the IL verifier.  The
// checker centralises the rules that determine how many operands each opcode
// accepts and emits consistent diagnostics when bytecode violates those rules.
// Housing the logic here keeps the verification passes concise and ensures every
// opcode reports mismatches using the same phrasing and severity.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the operand-count verification helper used by the IL verifier.
/// @details The checker compares each instruction's operand count against the
///          metadata supplied by @c OpcodeInfo and reports structured diagnostics
///          when the counts fall outside the permitted range.

#include "il/verify/OperandCountChecker.hpp"

#include "il/core/Instr.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/verify/DiagFormat.hpp"

#include <sstream>

using il::support::Expected;
using il::support::makeError;

namespace il::verify::detail {

/// @brief Construct a checker bound to a verification context and opcode metadata.
///
/// @details Each instance remembers the instruction being verified (`VerifyCtx`)
///          and the statically generated metadata that describes the opcode's
///          operand requirements.  The object is lightweight enough to be
///          constructed on the stack for every instruction.
///
/// @param ctx Verification context describing the instruction under inspection.
/// @param spec Opcode specification supplying operand bounds.
OperandCountChecker::OperandCountChecker(const VerifyCtx &ctx, const InstructionSpec &spec)
    : ctx_(ctx), spec_(spec) {}

/// @brief Validate the operand count for the bound instruction.
///
/// @details Reads the operand vector from the instruction and compares its size
///          against the bounds declared in the opcode metadata.  Variadic
///          opcodes are treated specially: they only enforce a minimum operand
///          count and skip the maximum check entirely.  When a violation is
///          detected the routine formats a precise, user-friendly message that
///          explains the expected cardinality.
///
/// @return Empty success on validity; otherwise a structured diagnostic.
Expected<void> OperandCountChecker::run() const {
    const auto &instr = ctx_.instr;
    const size_t operandCount = instr.operands.size();
    const bool variadicOperands = il::core::isVariadicOperandCount(spec_.numOperandsMax);
    if (operandCount < spec_.numOperandsMin ||
        (!variadicOperands && operandCount > spec_.numOperandsMax)) {
        std::ostringstream ss;
        ss << "invalid operand count: ";
        if (spec_.numOperandsMin == spec_.numOperandsMax && !variadicOperands) {
            ss << "expected " << static_cast<unsigned>(spec_.numOperandsMin) << " operand";
            if (spec_.numOperandsMin != 1)
                ss << 's';
        } else if (variadicOperands) {
            ss << "expected at least " << static_cast<unsigned>(spec_.numOperandsMin) << " operand";
            if (spec_.numOperandsMin != 1)
                ss << 's';
        } else {
            ss << "expected between " << static_cast<unsigned>(spec_.numOperandsMin) << " and "
               << static_cast<unsigned>(spec_.numOperandsMax) << " operands";
        }
        return report(ss.str());
    }

    const auto &opcodeInfo = il::core::getOpcodeInfo(instr.op);
    const bool variadicSuccessors = il::core::isVariadicSuccessorCount(opcodeInfo.numSuccessors);
    if (variadicSuccessors) {
        if (instr.labels.empty())
            return report("invalid label count: expected at least 1 label");
        if (!instr.brArgs.empty() && instr.brArgs.size() != instr.labels.size())
            return report(
                "invalid branch argument list count: expected one list per label, or none");
    } else {
        if (instr.labels.size() != opcodeInfo.numSuccessors) {
            std::ostringstream ss;
            ss << "invalid label count: expected "
               << static_cast<unsigned>(opcodeInfo.numSuccessors) << " label";
            if (opcodeInfo.numSuccessors != 1)
                ss << 's';
            return report(ss.str());
        }

        if (instr.brArgs.size() > opcodeInfo.numSuccessors) {
            std::ostringstream ss;
            ss << "invalid branch argument list count: expected at most "
               << static_cast<unsigned>(opcodeInfo.numSuccessors) << " list";
            if (opcodeInfo.numSuccessors != 1)
                ss << 's';
            return report(ss.str());
        }

        if (!instr.brArgs.empty() && instr.brArgs.size() != opcodeInfo.numSuccessors) {
            std::ostringstream ss;
            ss << "invalid branch argument list count: expected "
               << static_cast<unsigned>(opcodeInfo.numSuccessors) << " list";
            if (opcodeInfo.numSuccessors != 1)
                ss << 's';
            ss << ", or none";
            return report(ss.str());
        }
    }

    return {};
}

/// @brief Emit a diagnostic constructed from the supplied message.
///
/// @details The helper decorates the provided message with contextual
///          information—function name, block identifier, and a pretty-printed
///          instruction—before moving it into an `Expected<void>` error slot.
///          Callers can then bubble the failure to the top-level verifier without
///          worrying about formatting consistency.
///
/// @param message Human-readable detail describing the operand mismatch.
/// @return Expected error containing the formatted diagnostic payload.
Expected<void> OperandCountChecker::report(std::string_view message) const {
    return Expected<void>{
        makeError(ctx_.instr.loc, formatInstrDiag(ctx_.fn, ctx_.block, ctx_.instr, message))};
}

} // namespace il::verify::detail
