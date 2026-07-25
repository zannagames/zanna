//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/Mangle.hpp
// Purpose: Provide unified mangling for linkable symbols from qualified names.
// Key invariants: Linkage mangling is ASCII-only, stable, and case-insensitive;
//                 ordinary C-safe identifiers remain readable, while qualified
//                 or escaped names use a reserved prefix and reversible escapes.
// Ownership/Lifetime: Header-only declarations; implementation in Mangle.cpp.
// Links: src/common/Mangle.cpp, docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

/**
 * @file Mangle.hpp
 * @brief Declares stable ASCII linker-symbol mangling and display demangling.
 *
 * The mapping folds ASCII case. Simple linker-safe identifiers remain
 * readable, while all other byte sequences use a reserved, reversible escape
 * encoding. Demangling also recognizes an older at-sign-prefixed display form.
 */

#pragma once

#include <string>
#include <string_view>

namespace zanna::common {

/// @brief Mangle a qualified name into a safe ASCII linker symbol.
/// @details Plain identifiers that already fit the stable linker subset
///          @c [a-z_][a-z0-9_]* are returned lowercased, except names beginning
///          with the reserved @c vpr_ prefix.  Qualified names, names containing
///          unsupported bytes, and names that would collide with the reserved
///          prefix are encoded with @c vpr_ plus reversible escapes.  The output
///          always contains only lowercase ASCII letters, digits, and
///          underscores. ASCII case information is intentionally not retained.
/// @param qualified Qualified name like "A.B.F" or "Klass.__ctor".
/// @return Linker-safe symbol like "main", "a_b", or "vpr_a_db".
std::string MangleLink(std::string_view qualified);

/// @brief Best-effort demangle of a link symbol back to dotted form.
/// @details Decodes the reserved @c vpr_ escape form produced by
///          @ref MangleLink.  For unprefixed legacy symbols, lowercase text is
///          returned unchanged; for historical @c '@'-prefixed symbols the
///          leading marker is stripped and underscores are shown as dots.
///          Unknown or incomplete reserved escapes are preserved in a
///          best-effort textual form rather than rejected.
/// @param symbol Link symbol like "vpr_a_db" or "main".
/// @return Dotted form like "a.b" or "main".
std::string DemangleLink(std::string_view symbol);

} // namespace zanna::common
