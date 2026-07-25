//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/SymbolResolver.hpp
// Purpose: Global symbol table construction and iterative archive resolution.
// Key invariants:
//   - Strong definitions override weak; multiple strong = error
//   - Archives searched iteratively until no new definitions found
//   - Dynamic library symbols left as undefined after resolution
// Ownership/Lifetime:
//   - SymbolResolver builds the global symbol table from parsed ObjFiles
// Links: codegen/common/linker/LinkTypes.hpp
//        codegen/common/linker/ObjFileReader.hpp
//        codegen/common/linker/ArchiveReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file SymbolResolver.hpp
 * @brief Declares native global-symbol resolution and lazy archive extraction.
 *
 * The resolver builds the link-wide symbol table from direct objects and only
 * those archive members needed by unresolved references. It implements strong,
 * weak, common, weak-external, and COMDAT selection semantics before separating
 * permitted loader imports from hard unresolved-symbol errors.
 */

#pragma once

#include "codegen/common/linker/ArchiveReader.hpp"
#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"

#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Resolve direct objects against mutable parsed archives.
/// @details This convenience overload creates immutable views of @p archives
///          and delegates to the pointer-based resolver.
/// @param initialObjects Object files supplied directly by the caller.
/// @param archives Ordered archives searched lazily for missing definitions.
/// @param globalSyms Receives the resolved global symbol table.
/// @param allObjects Receives direct objects plus extracted and synthetic objects.
/// @param dynamicSyms Receives unresolved names accepted as loader imports.
/// @param err Stream that receives archive, duplicate, or unresolved diagnostics.
/// @param platform Target ABI governing spelling, COMDAT exceptions, and imports.
/// @return `true` when every reference resolves statically or as a permitted import.
bool resolveSymbols(const std::vector<ObjFile> &initialObjects,
                    std::vector<Archive> &archives,
                    std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                    std::vector<ObjFile> &allObjects,
                    std::unordered_set<std::string> &dynamicSyms,
                    std::ostream &err,
                    LinkPlatform platform = detectLinkPlatform());

/// @brief Resolve symbols using non-owning immutable archive pointers.
/// @details This overload lets persistent build processes retain parsed archives
///          in shared immutable storage without copying their raw byte buffers.
///          Every pointer must remain valid for the duration of the call. Archive
///          members are extracted to a fixed point, losing COMDATs are stripped,
///          common symbols are materialized, and remaining allowlisted names
///          become dynamic imports.
/// @param initialObjects Initial object files supplied directly by the caller.
/// @param archives Ordered, non-null parsed archives searched for definitions.
/// @param globalSyms Receives the resolved global symbol table.
/// @param allObjects Receives direct objects plus extracted archive members.
/// @param dynamicSyms Receives unresolved symbols accepted as loader imports.
/// @param err Diagnostic stream for malformed archives and resolution failures.
/// @param platform Target platform governing symbol spelling and import policy.
/// @return `true` when every reference resolves statically or as a permitted import.
bool resolveSymbols(const std::vector<ObjFile> &objects,
                    const std::vector<const Archive *> &archives,
                    std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                    std::vector<ObjFile> &allObjects,
                    std::unordered_set<std::string> &dynamicSyms,
                    std::ostream &err,
                    LinkPlatform platform = detectLinkPlatform());

} // namespace zanna::codegen::linker
