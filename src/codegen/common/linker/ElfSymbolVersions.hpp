//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ElfSymbolVersions.hpp
// Purpose: Resolve the default symbol version each dynamic import must bind to,
//          so the ELF executable writer can emit `.gnu.version` /
//          `.gnu.version_r` instead of leaving every reference unversioned.
// Key invariants:
//   - Resolution is advisory: an unlocatable or unparsable library yields no
//     requirement for its symbols, and the writer falls back to an unversioned
//     reference for them.
//   - Only default (non-hidden) version definitions are reported; those are the
//     ones a symbolic reference is expected to select.
// Ownership/Lifetime: stateless; the returned map owns its strings.
// Links: src/codegen/common/linker/ElfExeWriter.cpp,
//        src/codegen/common/linker/LinuxImportPlanner.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ElfSymbolVersions.hpp
 * @brief Declares default-symbol-version discovery for ELF dynamic imports.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

/// @brief The versioned definition a dynamic import is expected to bind to.
struct ElfSymbolVersion {
    std::string library; ///< SONAME as it appears in `DT_NEEDED`.
    std::string version; ///< Version definition name, e.g. `GLIBC_2.3.2`.
};

/// @brief Map each import to the default version its providing library defines.
/// @details A reference carrying no version information does not reliably select
///          a library's current definition: where several versions of a name
///          exist, the loader may bind the compatibility one, silently swapping
///          in an implementation kept only for old binaries. This inspects the
///          libraries that will actually satisfy the link and reports the
///          default version of each requested name, which is what a versioned
///          reference selects.
///
///          Libraries are searched along the loader's usual directories for
///          @p arch. Anything not found, not an ELF64 shared object for the
///          expected machine, or carrying no version definitions simply
///          contributes no entries, so a cross-link from a host without the
///          target's libraries degrades to today's unversioned references
///          rather than failing.
/// @param neededLibs Ordered `DT_NEEDED` SONAMEs; earlier entries win ties, matching
///        the loader's search order.
/// @param dynSymbols Imported symbol names to resolve.
/// @param arch Target architecture, selecting the library search directories.
/// @return Requirements for the subset of @p dynSymbols that resolved.
std::unordered_map<std::string, ElfSymbolVersion>
resolveElfSymbolVersions(const std::vector<std::string> &neededLibs,
                         const std::vector<std::string> &dynSymbols,
                         LinkArch arch);

} // namespace zanna::codegen::linker
