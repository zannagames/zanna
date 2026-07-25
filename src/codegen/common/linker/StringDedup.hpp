//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/StringDedup.hpp
// Purpose: Cross-module string deduplication pass for the native linker.
//          Identifies identical NUL-terminated rodata strings across object
//          files and promotes them to shared global symbols so all relocations
//          resolve to a single canonical copy.
// Key invariants:
//   - Only LOCAL symbols in non-executable, non-writable, allocatable sections
//   - Content must be NUL-terminated within section bounds
//   - Strings with identical content but different lengths are NOT merged
//   - Non-canonical copies remain as dead bytes (no byte removal)
// Links: codegen/common/linker/ObjFileReader.hpp
//        codegen/common/linker/LinkTypes.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file StringDedup.hpp
 * @brief Declares the native linker's cross-object C-string deduplication pass.
 *
 * Deduplication runs after dead stripping and before section merging. It
 * rewrites duplicate local string symbols to a shared synthetic global and,
 * when a C-string section is proven to contain no untracked bytes or incoming
 * cross-section references, removes redundant copies from that section.
 */

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

struct ObjFile;
struct GlobalSymEntry;

/// @brief Deduplicate identical, explicitly identified C-string literals.
/// @details Scans local symbols at exact string boundaries, groups candidates
///          by bytes including the terminating NUL, and promotes each duplicate
///          group to one collision-free global symbol. Safe, fully covered
///          sections are compacted; ambiguous sections retain their bytes while
///          still sharing the canonical symbol.
/// @param allObjects Object files modified in place, including symbol names,
///                   bindings, section indices, offsets, and compacted data.
/// @param globalSyms Global table updated with canonical synthetic definitions
///                   and any offsets changed by compaction.
/// @return Number of non-canonical string occurrences redirected to canonical copies.
size_t deduplicateStrings(std::vector<ObjFile> &allObjects,
                          std::unordered_map<std::string, GlobalSymEntry> &globalSyms);

} // namespace zanna::codegen::linker
