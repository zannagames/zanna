//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the factory function for constructing the default set of
// instruction verification strategies used by the FunctionVerifier. The strategy
// pattern enables modular, extensible verification logic organized by instruction
// category.
//
// The FunctionVerifier uses a pluggable strategy architecture where each strategy
// handles a specific category of instructions (control flow, generic opcodes, etc.).
// This file provides the factory that instantiates the standard strategy set,
// allowing future extension with additional strategies without modifying the core
// verifier infrastructure.
//
// Key Responsibilities:
// - Instantiate the default control-flow terminator verification strategy
// - Instantiate the default generic instruction verification strategy
// - Return strategies with proper ownership transfer to the caller
//
// Design Notes:
// The factory returns a vector of unique_ptr to InstructionStrategy, transferring
// ownership to the FunctionVerifier. Each strategy implements the matches() and
// verify() interface, enabling the verifier to dispatch instructions to the
// appropriate checker based on opcode properties. The separation between strategy
// construction and usage allows testing individual strategies in isolation.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares construction of the standard instruction strategy chain.
/// @details The returned owned strategies place dedicated control-flow
///          verification before the generic table-driven fallback.

#pragma once

#include "il/verify/FunctionVerifier.hpp"

#include <memory>
#include <vector>

namespace il::verify {

/// @brief Construct the default set of instruction strategies used by FunctionVerifier.
/// @return Owned strategies in dispatch priority order.
std::vector<std::unique_ptr<FunctionVerifier::InstructionStrategy>>
makeDefaultInstructionStrategies();

} // namespace il::verify
