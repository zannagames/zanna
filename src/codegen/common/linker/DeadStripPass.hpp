//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/DeadStripPass.hpp
// Purpose: Dead code/data stripping pass for the native linker.
//          Performs mark-and-sweep GC over input sections to remove
//          unreferenced functions and data from the output.
// Key invariants:
//   - Roots: entry point section, ObjC metadata, TLS, init/fini, and sections
//     explicitly retained (SHF_GNU_RETAIN / S_ATTR_NO_DEAD_STRIP / N_NO_DEAD_STRIP).
//     Synthetic linker-generated objects are always kept.
//   - Liveness flows through relocations (section A references symbol in section B)
//   - Every non-synthetic object (user and archive) is eligible for stripping;
//     only sections reachable from a root survive.
// Links: codegen/common/linker/ObjFileReader.hpp
//        codegen/common/linker/LinkTypes.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file DeadStripPass.hpp
 * @brief Declares section-level mark-and-sweep garbage collection for native links.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <cstddef>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

struct ObjFile;
struct GlobalSymEntry;

/// @brief Removes input sections that are unreachable from linker roots.
/// @details Liveness begins at the entry point, platform-discovered metadata,
///          explicitly retained sections/symbols, optional debug sections, and
///          linker-synthetic objects. It propagates through relocations,
///          associative COMDAT relationships, and Windows unwind associations.
///          Sweeping clears dead section data, relocations, and memory size and
///          marks each section stripped.
/// @param allObjects All object files, including user inputs, archive extracts,
///                   and linker-generated stubs.
/// @param userObjCount Number of leading user-provided objects.  Objects at or
///                     after this index are inspected for synthetic linker
///                     objects; ordinary sections on either side remain
///                     eligible for stripping.
/// @param globalSyms Resolved global symbol table used for entry-point and
///                   relocation target lookup.
/// @param entrySymbol Entry-point symbol name, such as `main`.
/// @param platform Target platform controlling symbol fallback and Windows
///                 unwind/guard handling.
/// @param preserveDebugSections Whether non-allocating debug sections are roots.
/// @param err Stream that receives the dead-strip removal summary.
void deadStrip(std::vector<ObjFile> &allObjects,
               size_t userObjCount,
               const std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
               const std::string &entrySymbol,
               LinkPlatform platform,
               bool preserveDebugSections,
               std::ostream &err);

/// @brief Runs dead stripping while allowing normal debug-section removal.
/// @param allObjects All object files to mutate.
/// @param userObjCount Number of leading user-provided objects.
/// @param globalSyms Resolved global symbol table.
/// @param entrySymbol Entry-point symbol name.
/// @param platform Explicit target platform.
/// @param err Stream that receives the removal summary.
inline void deadStrip(std::vector<ObjFile> &allObjects,
                      size_t userObjCount,
                      const std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                      const std::string &entrySymbol,
                      LinkPlatform platform,
                      std::ostream &err) {
    deadStrip(allObjects, userObjCount, globalSyms, entrySymbol, platform, false, err);
}

/// @brief Runs dead stripping for the detected host platform with debug removal enabled.
/// @param allObjects All object files to mutate.
/// @param userObjCount Number of leading user-provided objects.
/// @param globalSyms Resolved global symbol table.
/// @param entrySymbol Entry-point symbol name.
/// @param err Stream that receives the removal summary.
inline void deadStrip(std::vector<ObjFile> &allObjects,
                      size_t userObjCount,
                      const std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                      const std::string &entrySymbol,
                      std::ostream &err) {
    deadStrip(allObjects, userObjCount, globalSyms, entrySymbol, detectLinkPlatform(), false, err);
}

} // namespace zanna::codegen::linker
