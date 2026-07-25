//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/StringTable.hpp
// Purpose: Preserves the BASIC frontend include path for the common
//          deterministic string-interning table.
// Key invariants:
//   - The BASIC name denotes the exact common StringTable type.
//   - No wrapper state or divergent frontend behavior is introduced.
// Ownership/Lifetime:
//   - All ownership semantics come directly from the aliased common type.
// Links: src/frontends/basic/StringTable.cpp,
//        src/frontends/common/StringTable.hpp,
//        src/frontends/basic/Lowerer.hpp
//
//===----------------------------------------------------------------------===//
#pragma once

#include "frontends/common/StringTable.hpp"

/// @file
/// @brief Re-exports the common string table through the BASIC namespace.
/// @details Existing BASIC code may retain this legacy include path. New
///          cross-frontend code should include the common header directly.

namespace il::frontends::basic {

/// @brief Backward-compatible alias for the shared string interning table.
/// @details The aliased type manages string literal deduplication and
///          deterministic label generation for IL globals. Prefer the common
///          namespace in new code to keep cross-frontend dependencies explicit.
/// @see il::frontends::common::StringTable
using ::il::frontends::common::StringTable;

} // namespace il::frontends::basic
