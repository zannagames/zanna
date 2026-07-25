//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/il/verify/ControlFlowChecker.cpp
// Purpose: Provide control-flow specific verification helpers for the IL
//          verifier including block parameter validation and terminator checks.
// Key invariants: Blocks must honour single-terminator semantics and branch
//                 arguments must align with destination parameter lists.
// Ownership/Lifetime: Operates on caller-managed verifier state without
//                     persisting references beyond each call.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements structured and stream-oriented control-flow verification.
/// @details Internal `Expected` helpers own the checks, while public boolean
///          wrappers preserve the older text-stream interface by translating
///          structured diagnostics without retaining verifier state.

#include "il/verify/ControlFlowChecker.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "il/core/Param.hpp"
#include "il/internal/io/ParserUtil.hpp"
#include "il/verify/BranchVerifier.hpp"
#include "il/verify/DiagFormat.hpp"
#include "il/verify/DiagSink.hpp"
#include "support/diag_expected.hpp"

#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

using namespace il::core;

namespace il::verify {
using il::support::Diag;
using il::support::Expected;
using il::support::makeError;
using il::support::Severity;

using VerifyInstrFnExpected =
    std::function<Expected<void>(const Function &fn,
                                 const BasicBlock &bb,
                                 const Instr &instr,
                                 const BlockMap &blockMap,
                                 const std::unordered_map<std::string, const Extern *> &externs,
                                 const std::unordered_map<std::string, const Function *> &funcs,
                                 TypeInference &types,
                                 DiagSink &sink)>;

namespace {

/// @brief Validates block parameter declarations against IL structural rules.
/// @param fn Function owning @p bb; used for diagnostics.
/// @param bb Basic block whose parameter list is being validated.
/// @param types Type inference context seeded with parameter types.
/// @param paramIds Output container populated with the registered parameter IDs.
/// @return Empty on success; otherwise a diagnostic describing a malformed or
///         duplicate name, duplicate temporary ID, or void parameter.
/// @details Ensures block parameters are unique and non-void so predecessors can
///          match arguments, per docs/il/il-guide.md#reference section "Basic Blocks".
Expected<void> validateBlockParams_impl(const Function &fn,
                                        const BasicBlock &bb,
                                        TypeInference &types,
                                        std::vector<unsigned> &paramIds) {
    std::unordered_set<std::string> paramNames;
    std::unordered_set<unsigned> seenParamIds;
    for (const auto &param : bb.params) {
        if (!il::io::isValidILIdentifier(param.name))
            return Expected<void>{
                makeError({}, formatBlockDiag(fn, bb, "malformed param name %" + param.name))};

        if (!paramNames.insert(param.name).second)
            return Expected<void>{
                makeError({}, formatBlockDiag(fn, bb, "duplicate param %" + param.name))};

        if (!seenParamIds.insert(param.id).second) {
            std::ostringstream message;
            message << "duplicate param id %" << param.id;
            return Expected<void>{makeError({}, formatBlockDiag(fn, bb, message.str()))};
        }

        if (param.type.kind == Type::Kind::Void)
            return Expected<void>{
                makeError({}, formatBlockDiag(fn, bb, "param %" + param.name + " has void type"))};

        types.addTemp(param.id, param.type);
        paramIds.push_back(param.id);
    }
    return {};
}

/// @brief Walks instructions within a block and invokes verifier callbacks.
/// @param fn Function containing @p bb.
/// @param bb Block whose instructions are being analyzed.
/// @param blockMap Map of reachable block labels for branch validation.
/// @param externs Known extern declarations supplied to instruction checks.
/// @param funcs Known function declarations supplied to instruction checks.
/// @param types Type inference context used to validate operand availability.
/// @param verifyInstrFn Callback providing instruction-specific verification.
/// @param sink Receives non-fatal diagnostics emitted by the callback.
/// @return Propagates the first verification error produced by operand checking
///         or the callback; otherwise empty.
/// @details Stops after the first terminator to honour the single-terminator
///          rule outlined in docs/il/il-guide.md#reference ("Explicit control flow").
Expected<void> iterateBlockInstructions_impl(
    const Function &fn,
    const BasicBlock &bb,
    const BlockMap &blockMap,
    const std::unordered_map<std::string, const Extern *> &externs,
    const std::unordered_map<std::string, const Function *> &funcs,
    TypeInference &types,
    const VerifyInstrFnExpected &verifyInstrFn,
    DiagSink &sink) {
    for (const auto &instr : bb.instructions) {
        if (auto result = types.ensureOperandsDefined_E(fn, bb, instr); !result)
            return result;

        if (auto result = verifyInstrFn(fn, bb, instr, blockMap, externs, funcs, types, sink);
            !result)
            return result;

        if (isTerminator(instr.op))
            break;
    }
    return {};
}

/// @brief Ensures each block terminates exactly once as required by the IL spec.
/// @param fn Function owning @p bb.
/// @param bb Basic block whose terminator structure is being checked.
/// @return Diagnostic if the block is empty, has multiple terminators, contains
///         instructions after a terminator, or is missing a terminator.
/// @details Implements the "explicit control flow" requirement described in
///          docs/il/il-guide.md#reference: every block ends with exactly one terminator.
Expected<void> checkBlockTerminators_impl(const Function &fn, const BasicBlock &bb) {
    if (bb.instructions.empty())
        return Expected<void>{makeError({}, formatBlockDiag(fn, bb, "empty block"))};

    bool seenTerm = false;
    for (const auto &instr : bb.instructions) {
        if (isTerminator(instr.op)) {
            if (seenTerm)
                return Expected<void>{
                    makeError(instr.loc, formatInstrDiag(fn, bb, instr, "multiple terminators"))};
            seenTerm = true;
            continue;
        }
        if (seenTerm)
            return Expected<void>{makeError(
                instr.loc, formatInstrDiag(fn, bb, instr, "instruction after terminator"))};
    }

    if (!isTerminator(bb.instructions.back().op))
        return Expected<void>{makeError({}, formatBlockDiag(fn, bb, "missing terminator"))};

    return {};
}

/// @brief Warning and error messages decoded from a legacy verifier stream.
struct ParsedCapture {
    /// @brief Warning messages with the textual severity prefix removed.
    std::vector<std::string> warnings;

    /// @brief Error or unclassified messages with any error prefix removed.
    std::vector<std::string> errors;
};

/// @brief Split captured verifier output into warning and error buckets.
///
/// @details Parses newline-separated diagnostic text emitted by callback-based
///          verifiers. Lines prefixed with "warning: " are catalogued as
///          warnings, while "error: " prefixes and all other content are
///          recorded as errors so that callers can mirror structured reporting.
///
/// @param text Aggregated stdout/stderr text captured from a verifier.
/// @return Categorised diagnostic lists ready for further processing.
ParsedCapture parseCapturedLines(const std::string &text) {
    ParsedCapture parsed;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line.rfind("warning: ", 0) == 0)
            parsed.warnings.push_back(line.substr(9));
        else if (line.rfind("error: ", 0) == 0)
            parsed.errors.push_back(line.substr(7));
        else
            parsed.errors.push_back(line);
    }
    return parsed;
}

/// @brief Join diagnostic messages with newline separators.
///
/// @details Maintains the original message ordering while producing a
///          printable, multi-line string for terminal output.
///
/// @param messages Collection of message strings to concatenate.
/// @return Combined message separated by newline characters.
std::string joinMessages(const std::vector<std::string> &messages) {
    std::ostringstream oss;
    for (size_t i = 0; i < messages.size(); ++i) {
        if (i != 0)
            oss << '\n';
        oss << messages[i];
    }
    return oss.str();
}

} // namespace

/// @brief Validates block parameters against IL structural rules.
///
/// @details Delegates to the implementation helper so the error-reporting and
///          utility versions share logic.  The helper ensures uniqueness and
///          non-void types, recording parameter temporaries for SSA tracking.
///
/// @param fn Function owning @p bb.
/// @param bb Basic block whose parameters are inspected.
/// @param types Type inference context receiving parameter definitions.
/// @param paramIds Output list populated with parameter temporary identifiers.
/// @return Success or a diagnostic describing the violation.
Expected<void> validateBlockParams_E(const Function &fn,
                                     const BasicBlock &bb,
                                     TypeInference &types,
                                     std::vector<unsigned> &paramIds) {
    return validateBlockParams_impl(fn, bb, types, paramIds);
}

/// @brief Iterate instructions within a block, invoking a verifier callback.
///
/// @details Provides the `Expected`-returning variant used by the primary
///          verifier pipeline.  Operand availability is ensured before the
///          callback executes.  Execution stops after encountering a terminator
///          instruction, mirroring the IL requirement that no code follows a
///          terminator.
///
/// @param fn Function containing @p bb.
/// @param bb Block whose instructions are traversed.
/// @param blockMap Lookup from labels to block definitions for branch checking.
/// @param externs Map of known extern declarations.
/// @param funcs Map of known function declarations.
/// @param types Type inference context tracking SSA values.
/// @param verifyInstrFn Callback producing instruction-specific diagnostics.
/// @param sink Diagnostic sink capturing warnings.
/// @return Success or the first diagnostic raised by operand or callback checks.
Expected<void> iterateBlockInstructions_E(
    const Function &fn,
    const BasicBlock &bb,
    const BlockMap &blockMap,
    const std::unordered_map<std::string, const Extern *> &externs,
    const std::unordered_map<std::string, const Function *> &funcs,
    TypeInference &types,
    const VerifyInstrFnExpected &verifyInstrFn,
    DiagSink &sink) {
    return iterateBlockInstructions_impl(
        fn, bb, blockMap, externs, funcs, types, verifyInstrFn, sink);
}

/// @brief Validate terminator placement for a block using the Expected API.
///
/// @param fn Function owning @p bb.
/// @param bb Block whose terminator structure is checked.
/// @return Success or a diagnostic describing missing or duplicate terminators.
Expected<void> checkBlockTerminators_E(const Function &fn, const BasicBlock &bb) {
    return checkBlockTerminators_impl(fn, bb);
}

/// @brief Identify whether an opcode terminates a basic block.
///
/// @param op Opcode to classify.
/// @return @c true when @p op ends a block per the IL specification.
bool isTerminator(Opcode op) {
    return getOpcodeInfo(op).isTerminator;
}

/// @brief Validate block parameters while emitting diagnostics to a stream.
///
/// @details Wraps @ref validateBlockParams_E and forwards any diagnostic to the
///          provided stream for tooling that expects textual output instead of
///          structured diagnostics.
///
/// @param fn Function owning @p bb.
/// @param bb Block whose parameter list is validated.
/// @param types Type inference context receiving parameter definitions.
/// @param paramIds Output list populated with parameter temporary identifiers.
/// @param err Stream receiving formatted diagnostics on failure.
/// @return @c true on success; otherwise @c false after reporting the error.
bool validateBlockParams(const Function &fn,
                         const BasicBlock &bb,
                         TypeInference &types,
                         std::vector<unsigned> &paramIds,
                         std::ostream &err) {
    if (auto result = validateBlockParams_E(fn, bb, types, paramIds); !result) {
        il::support::printDiag(result.error(), err);
        return false;
    }
    return true;
}

/// @brief Iterate instructions, invoking a boolean-based verification callback.
///
/// @details Used by legacy tooling that expects a bool return value rather than
///          `Expected`. Captured diagnostic text is parsed via
///          @ref parseCapturedLines so warnings and errors are replayed through
///          structured sinks before being printed to @p err.
///
/// @param verifyInstrFn Callback returning success/failure and textual
///        diagnostics.
/// @param fn Function containing the block under inspection.
/// @param bb Block being verified.
/// @param blockMap Mapping from labels to block definitions.
/// @param externs Map of extern declarations.
/// @param funcs Map of function declarations.
/// @param types Type inference context tracking SSA values.
/// @param err Stream receiving formatted diagnostics on failure.
/// @return @c true when verification succeeds; otherwise @c false after
///         printing errors and warnings.
bool iterateBlockInstructions(VerifyInstrFn verifyInstrFn,
                              const Function &fn,
                              const BasicBlock &bb,
                              const BlockMap &blockMap,
                              const std::unordered_map<std::string, const Extern *> &externs,
                              const std::unordered_map<std::string, const Function *> &funcs,
                              TypeInference &types,
                              std::ostream &err) {
    CollectingDiagSink warnings;
    /// @brief Adapt a streaming boolean callback to the structured verifier API.
    /// @details Captures callback output, forwards parsed warnings to the sink,
    ///          and converts failure text into an `Expected` error anchored to
    ///          the current instruction.
    VerifyInstrFnExpected shim =
        [&](const Function &fnRef,
            const BasicBlock &bbRef,
            const Instr &instrRef,
            const BlockMap &blockMapRef,
            const std::unordered_map<std::string, const Extern *> &externsRef,
            const std::unordered_map<std::string, const Function *> &funcsRef,
            TypeInference &typesRef,
            DiagSink &warningSink) -> Expected<void> {
        std::ostringstream capture;
        bool ok = verifyInstrFn(
            fnRef, bbRef, instrRef, blockMapRef, externsRef, funcsRef, typesRef, capture);
        ParsedCapture parsed = parseCapturedLines(capture.str());
        for (const auto &msg : parsed.warnings)
            warningSink.report(Diag{Severity::Warning, msg, instrRef.loc, {}});
        if (!ok) {
            std::string message = joinMessages(parsed.errors);
            if (message.empty())
                message = formatInstrDiag(fnRef, bbRef, instrRef, "verification failed");
            return Expected<void>{makeError(instrRef.loc, message)};
        }
        return {};
    };

    if (auto result =
            iterateBlockInstructions_E(fn, bb, blockMap, externs, funcs, types, shim, warnings);
        !result) {
        for (const auto &warning : warnings.diagnostics())
            il::support::printDiag(warning, err);
        il::support::printDiag(result.error(), err);
        return false;
    }

    for (const auto &warning : warnings.diagnostics())
        il::support::printDiag(warning, err);
    return true;
}

/// @brief Validate terminator placement and report failures to a stream.
///
/// @param fn Function owning @p bb.
/// @param bb Block whose terminator structure is inspected.
/// @param err Stream receiving formatted diagnostics on failure.
/// @return @c true when the block satisfies terminator rules; otherwise @c false.
bool checkBlockTerminators(const Function &fn, const BasicBlock &bb, std::ostream &err) {
    if (auto result = checkBlockTerminators_E(fn, bb); !result) {
        il::support::printDiag(result.error(), err);
        return false;
    }
    return true;
}

/// @brief Validate an unconditional branch and stream diagnostics when needed.
///
/// @param fn Function containing the instruction.
/// @param bb Block holding the instruction.
/// @param instr Branch instruction under validation.
/// @param blockMap Map from labels to block definitions.
/// @param types Type inference context used for operand type lookups.
/// @param err Stream receiving diagnostics on failure.
/// @return @c true on success; otherwise @c false after emitting diagnostics.
bool verifyBr(const Function &fn,
              const BasicBlock &bb,
              const Instr &instr,
              const BlockMap &blockMap,
              TypeInference &types,
              std::ostream &err) {
    if (auto result = verifyBr_E(fn, bb, instr, blockMap, types); !result) {
        il::support::printDiag(result.error(), err);
        return false;
    }
    return true;
}

/// @brief Validate a conditional branch and emit diagnostics to a stream.
///
/// @param fn Function containing the instruction.
/// @param bb Block holding the instruction.
/// @param instr Conditional branch instruction.
/// @param blockMap Map from labels to block definitions.
/// @param types Type inference context used for operand type lookups.
/// @param err Stream receiving diagnostics on failure.
/// @return @c true on success; otherwise @c false.
bool verifyCBr(const Function &fn,
               const BasicBlock &bb,
               const Instr &instr,
               const BlockMap &blockMap,
               TypeInference &types,
               std::ostream &err) {
    if (auto result = verifyCBr_E(fn, bb, instr, blockMap, types); !result) {
        il::support::printDiag(result.error(), err);
        return false;
    }
    return true;
}

/// @brief Validate a return instruction and emit diagnostics to a stream.
///
/// @param fn Function containing the instruction.
/// @param bb Block holding the instruction.
/// @param instr Return instruction under validation.
/// @param types Type inference context used for operand type lookups.
/// @param err Stream receiving diagnostics on failure.
/// @return @c true on success; otherwise @c false after reporting the issue.
bool verifyRet(const Function &fn,
               const BasicBlock &bb,
               const Instr &instr,
               TypeInference &types,
               std::ostream &err) {
    if (auto result = verifyRet_E(fn, bb, instr, types); !result) {
        il::support::printDiag(result.error(), err);
        return false;
    }
    return true;
}

} // namespace il::verify
