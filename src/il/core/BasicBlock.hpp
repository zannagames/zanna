//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/BasicBlock.hpp
// Purpose: Declares the BasicBlock struct -- a maximal sequence of IL
//          instructions with a single entry point, optional block parameters
//          (phi-node equivalents), and a single exit terminator. Basic blocks
//          are the fundamental units of control flow in Zanna IL functions.
// Key invariants:
//   - Labels must be non-empty and unique within the parent function.
//   - Parameter count and types must match incoming branch arguments.
//   - If terminated is true, the last instruction must be a terminator opcode.
//   - All instructions except the last must be non-terminator opcodes.
// Ownership/Lifetime: Function owns BasicBlocks in stable storage. BasicBlock
//          owns all Instructions in stable storage and Params by value. Labels
//          are stored as std::string values owned by the block.
// Links: docs/il/il-guide.md#reference, il/core/Instr.hpp, il/core/Param.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the stable-storage representation of an IL basic block.
 *
 * @details A basic block owns its ordered instructions and incoming SSA block
 *          parameters, carries both textual and interned label identities, and
 *          records whether a terminator has closed the block. The enclosing
 *          function is responsible for label uniqueness and predecessor-edge
 *          argument compatibility.
 */

#pragma once

#include "il/core/Instr.hpp"
#include "il/core/Param.hpp"
#include "il/core/StableList.hpp"
#include "support/symbol.hpp"
#include <string>
#include <vector>

namespace il::core {

/// @brief Sequence of instructions terminated by a control-flow instruction.
struct BasicBlock {
    /// @brief Human-readable identifier for the block within its function.
    ///
    /// @invariant Non-empty and unique in the parent function.
    std::string label;

    /// @brief Parameters representing incoming SSA values.
    ///
    /// @invariant Count and types match each predecessor edge.
    std::vector<Param> params;

    /// @brief Ordered list of IL instructions belonging to this block.
    ///
    /// @invariant If @c terminated is true, the last instruction must be a terminator.
    StableList<Instr> instructions;

    /// @brief Indicates whether the block ends with a control-flow instruction.
    ///
    /// @invariant Reflects whether the last instruction is a terminator.
    bool terminated = false;

    /// @brief Interned handle for @ref label within the owning Module.
    ///
    /// Invalid until populated by a Module helper or by construction paths that
    /// have access to the parent Module.
    il::support::Symbol labelSymbol{};
};

} // namespace il::core
