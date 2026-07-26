//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/fwd.hpp
// Purpose: Forward declarations for core IL types (Module, Function,
//          BasicBlock, Instr, Value). Reduces compile-time dependencies by
//          exposing only incomplete type declarations.
// Key invariants: None -- declarations only; no definitions or storage.
// Ownership/Lifetime: N/A -- no objects are created by this header.
// Links: il/core/Module.hpp, il/core/Function.hpp, il/core/BasicBlock.hpp,
//        il/core/Instr.hpp, il/core/Value.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Provides dependency-light forward declarations for core IL entities.
 *
 * @details Include this header when an interface needs only pointers or
 *          references to modules, functions, blocks, instructions, or values.
 *          Consumers requiring object layout or member access must include the
 *          corresponding defining header.
 */

#pragma once

namespace il::core {
/// @brief Forward declaration for the module-level IL container.
struct Module;
/// @brief Forward declaration for an IL function definition.
struct Function;
/// @brief Forward declaration for a basic block in the IL control-flow graph.
struct BasicBlock;
/// @brief Forward declaration for a single IL instruction.
struct Instr;
/// @brief Forward declaration for an IL operand or constant value.
struct Value;
} // namespace il::core
