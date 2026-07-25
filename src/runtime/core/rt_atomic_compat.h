//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_atomic_compat.h
/// @file
/// @brief Selects the platform atomic-compatibility layer for runtime code.
///
// Purpose: Provides the stable include point used by code written against
//          GCC/Clang-style __atomic_* operations. On MSVC it imports the
//          compatibility definitions from rt_platform.h; on compilers with
//          native builtins it intentionally contributes no declarations.
//
// Key invariants:
//   - On GCC/Clang this header is a no-op (builtins are native).
//   - The MSVC implementation and memory-order mapping are centralized in the
//     approved runtime platform adapter rather than duplicated here.
//
// Ownership/Lifetime:
//   - Header-only, no state.
//
// Links: src/runtime/core/rt_crc32.c, rt_output.c, rt_gc.c (consumers)
//
//===----------------------------------------------------------------------===//

#pragma once

#ifdef _MSC_VER

#include "../rt_platform.h"

#endif /* _MSC_VER */
