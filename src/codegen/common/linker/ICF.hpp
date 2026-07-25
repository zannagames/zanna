//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ICF.hpp
// Purpose: Identical Code Folding (ICF) pass for the native linker.
//          Identifies per-function .text sections with identical bytes AND
//          identical relocation signatures, redirects duplicates to a single
//          canonical copy.
// Key invariants:
//   - Only non-generic executable sections with one Global symbol at offset 0
//   - Identity = (section bytes, sorted relocation signatures)
//   - Address-taken functions (all non-branch references) are excluded
//   - Non-canonical sections have both data AND relocs cleared
// Links: codegen/common/linker/StringDedup.hpp
//        codegen/common/linker/LinkTypes.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ICF.hpp
 * @brief Declares conservative identical-code folding for native object sections.
 */

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

struct ObjFile;
struct GlobalSymEntry;

/// @brief Folds identical per-function executable sections across object files.
/// @details Candidates must contain a single global definition at offset zero,
///          have no address-taking references, and come from a known non-COFF
///          target. Identity requires equal bytes and normalized relocation
///          signatures, not merely equal hashes. Folded definitions are
///          redirected to a canonical section and their old data/relocations
///          are cleared.
/// @param allObjects Object files modified in place during folding.
/// @param globalSyms Global definitions redirected to canonical locations.
/// @return Number of sections folded (eliminated).
size_t foldIdenticalCode(std::vector<ObjFile> &allObjects,
                         std::unordered_map<std::string, GlobalSymEntry> &globalSyms);

} // namespace zanna::codegen::linker
