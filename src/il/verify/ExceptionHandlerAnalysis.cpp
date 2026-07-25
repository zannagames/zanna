//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// This source file is part of the Zanna project.
//
// File: src/il/verify/ExceptionHandlerAnalysis.cpp
// Purpose: Provide verifier utilities that describe exception-handler blocks,
//          validating their entry signature before downstream analyses run.
// Key invariants: Handler blocks must start with `eh.entry`, declare exactly two
//                 parameters, and name them `%err` and `%tok` with the `Error`
//                 and `ResumeTok` types respectively. Violations surface as
//                 structured diagnostics.
// Ownership/Lifetime: Operates on references to IR structures owned by the
//                     caller; no allocations or state are retained.
// Links: docs/il/il-guide.md#exception-handling, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Exception-handling analysis utilities for the IL verifier.
/// @details Provides functions that inspect handler blocks, ensuring they
///          expose the correct entry signature before other EH checks execute.

#include "il/verify/ExceptionHandlerAnalysis.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Type.hpp"
#include "il/verify/DiagFormat.hpp"

#include <optional>
#include <string>

using namespace il::core;

namespace il::verify {
using il::support::Expected;
using il::support::makeError;

/// @brief Inspect a basic block to determine whether it is a valid handler.
/// @details Validates that the block begins with `eh.entry`, contains the
///          canonical parameter pair, and reports descriptive diagnostics when
///          invariants are violated. On success the helper returns the ids of
///          the `%err` and `%tok` parameters so consumers can wire them into
///          downstream analyses. An `eh.entry` appearing after the first
///          instruction is reported even though the block is not
///          handler-shaped.
/// @param fn Function owning the block, used for diagnostics.
/// @param bb Candidate handler block to validate.
/// @return Empty optional for an ordinary block, a populated signature for a
///         valid handler, or a diagnostic for malformed handler structure.
Expected<std::optional<HandlerSignature>> analyzeHandlerBlock(const Function &fn,
                                                              const BasicBlock &bb) {
    if (bb.instructions.empty())
        return std::optional<HandlerSignature>{};

    const Instr &first = bb.instructions.front();
    if (first.op != Opcode::EhEntry) {
        for (size_t idx = 1; idx < bb.instructions.size(); ++idx) {
            if (bb.instructions[idx].op == Opcode::EhEntry) {
                return Expected<std::optional<HandlerSignature>>{
                    makeError(bb.instructions[idx].loc,
                              formatInstrDiag(
                                  fn,
                                  bb,
                                  bb.instructions[idx],
                                  "eh.entry only allowed as first instruction of handler block"))};
            }
        }
        return std::optional<HandlerSignature>{};
    }

    if (bb.params.size() != 2)
        return Expected<std::optional<HandlerSignature>>{makeError(
            {},
            formatBlockDiag(fn, bb, "handler blocks must declare (%err:Error, %tok:ResumeTok)"))};

    if (bb.params[0].type.kind != Type::Kind::Error ||
        bb.params[1].type.kind != Type::Kind::ResumeTok)
        return Expected<std::optional<HandlerSignature>>{makeError(
            {}, formatBlockDiag(fn, bb, "handler params must be (%err:Error, %tok:ResumeTok)"))};

    if (bb.params[0].name != "err" || bb.params[1].name != "tok")
        return Expected<std::optional<HandlerSignature>>{
            makeError({}, formatBlockDiag(fn, bb, "handler params must be named %err and %tok"))};

    HandlerSignature sig = {bb.params[0].id, bb.params[1].id};
    return std::optional<HandlerSignature>{sig};
}

} // namespace il::verify
