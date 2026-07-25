//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/il/Module.hpp
// Purpose: Stable public entry point for IL core aggregates used by frontends.
// Key invariants: Re-exports only supported IL core structures; avoid leaking internals.
// Ownership/Lifetime: Types mirror definitions in il::core and retain their semantics.
// Links: docs/il/il-guide.md#reference, src/il/core/
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/BasicBlock.hpp"
#include "il/core/Extern.hpp"
#include "il/core/Function.hpp"
#include "il/core/Global.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Param.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"

/**
 * @file include/zanna/il/Module.hpp
 * @brief Aggregates the stable, concrete data types that form an IL module.
 *
 * @details
 * Language frontends and embedding clients can include this single header to
 * obtain complete definitions for modules, functions, blocks, instructions,
 * operands, types, globals, extern declarations, and parameters. The façade
 * intentionally excludes parsing, verification, builder helpers, and internal
 * implementation utilities so those dependencies remain explicit.
 *
 * The included types retain the ownership, mutation, and reference-stability
 * contracts documented by their declarations in `src/il/core/`.
 *
 * @note The normative semantics of these types and their opcodes are defined
 *       by `docs/il/il-guide.md#reference`.
 */

namespace il
{
namespace core
{
// Forward declarations live in il/core/fwd.hpp; this header intentionally
// includes concrete definitions so downstream clients receive full type
// information without depending on src/ paths.
} // namespace core
} // namespace il
