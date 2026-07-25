//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the result-type verification helper used by the IL verifier.  The
// checker ensures instructions emit results when mandated, suppress results when
// forbidden, and respect the concrete IL type expectations baked into opcode
// metadata.  Centralising this logic maintains consistent diagnostics across the
// various verification passes.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the result-type verification helper used by the IL verifier.
/// @details The checker validates whether an instruction produces a result when
///          required and whether the result's type matches expectations from the
///          opcode metadata.

#include "il/verify/ResultTypeChecker.hpp"

#include "il/core/Instr.hpp"
#include "il/verify/DiagFormat.hpp"
#include "il/verify/InstructionCheckUtils.hpp"

#include <sstream>

using il::core::kindToString;
using il::core::ResultArity;
using il::core::TypeCategory;
using il::support::Expected;
using il::support::makeError;
using il::verify::detail::kindFromCategory;

namespace il::verify::detail {
namespace {

/// @brief Identify opcodes whose strategy owns detailed result validation.
/// @param op Opcode to classify.
/// @return `true` when the table checker must defer result-kind validation.
bool opcodeHasStrategyValidatedResultType(il::core::Opcode op) {
    switch (op) {
        case il::core::Opcode::IAddOvf:
        case il::core::Opcode::ISubOvf:
        case il::core::Opcode::IMulOvf:
        case il::core::Opcode::SDivChk0:
        case il::core::Opcode::UDivChk0:
        case il::core::Opcode::SRemChk0:
        case il::core::Opcode::URemChk0:
        case il::core::Opcode::CastFpToSiRteChk:
        case il::core::Opcode::CastFpToUiRteChk:
        case il::core::Opcode::CastSiNarrowChk:
        case il::core::Opcode::CastUiNarrowChk:
        case il::core::Opcode::ConstNull:
            return true;
        default:
            return false;
    }
}

} // namespace

/// @brief Construct a checker bound to a verification context and opcode metadata.
///
/// @details The `VerifyCtx` parameter provides the instruction under inspection
///          as well as its surrounding function and block.  `OpcodeInfo`
///          describes whether the opcode produces zero, one, or optional results
///          and, when relevant, the type category those results must inhabit.
///
/// @param ctx Verification context describing the current instruction.
/// @param spec Opcode specification containing result-type requirements.
ResultTypeChecker::ResultTypeChecker(const VerifyCtx &ctx, const InstructionSpec &spec)
    : ctx_(ctx), spec_(spec) {}

/// @brief Validate the presence and type of an instruction's result value.
///
/// @details The checker examines three properties in order:
///          1. Cardinality — verifies the opcode's `ResultArity` contract by
///             ensuring required results exist and forbidden results are absent.
///          2. Special instructions — opcodes flagged as `InstrType` must carry a
///             non-void instruction type, with whitelisted exceptions for index
///             checks that synthesise their types elsewhere.
///          3. Concrete type — when metadata specifies a `TypeCategory`, the
///             checker resolves it to a `Type::Kind` and compares it against the
///             instruction's declared type unless the opcode is explicitly marked
///             as a dynamic cast.
///          Any failure results in a richly formatted diagnostic via @ref report.
///
/// @return Empty success on validity; otherwise a structured diagnostic error.
Expected<void> ResultTypeChecker::run() const {
    const auto &instr = ctx_.instr;
    const bool hasResult = instr.result.has_value();

    switch (spec_.resultArity) {
        case ResultArity::None:
            if (hasResult)
                return report("unexpected result");
            return {};
        case ResultArity::One:
            if (!hasResult)
                return report("missing result");
            break;
        case ResultArity::Optional:
            if (!hasResult)
                return {};
            break;
    }

    if (spec_.resultType == TypeCategory::InstrType) {
        if (instr.op != il::core::Opcode::IdxChk &&
            !opcodeHasStrategyValidatedResultType(instr.op) &&
            instr.type.kind == il::core::Type::Kind::Void) {
            return report("instruction type must be non-void");
        }
    } else if (auto expectedKind = kindFromCategory(spec_.resultType)) {
        if (opcodeHasStrategyValidatedResultType(instr.op))
            return {};
        if (instr.type.kind != *expectedKind) {
            std::ostringstream message;
            message << "result type mismatch: expected " << il::core::kindToString(*expectedKind)
                    << " but instruction declares " << il::core::kindToString(instr.type.kind);
            return report(message.str());
        }
    }

    return {};
}

/// @brief Emit a formatted diagnostic for a result-type mismatch.
///
/// @details Augments the supplied message with function, block, and instruction
///          context then returns it in an `Expected<void>` error state.  The
///          helper keeps diagnostic phrasing consistent across different
///          verification checks.
///
/// @param message Human-readable description of the mismatch.
/// @return Expected error containing the diagnostic payload.
Expected<void> ResultTypeChecker::report(std::string_view message) const {
    return Expected<void>{
        makeError(ctx_.instr.loc, formatInstrDiag(ctx_.fn, ctx_.block, ctx_.instr, message))};
}

} // namespace il::verify::detail
