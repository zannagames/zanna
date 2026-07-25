//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/RelocApplier.hpp
// Purpose: Apply relocations to merged section data, patching machine code
//          with resolved symbol addresses.
// Key invariants:
//   - x86_64 PCRel: S + A - P (32-bit), addend typically -4
//   - AArch64 CALL26: ((S + A - P) >> 2) masked to 26 bits
//   - AArch64 ADRP: Page(S+A) - Page(P), split into immhi/immlo
//   - Range checking for branches (±128MB for B/BL, ±1MB for B.cond)
// Ownership/Lifetime:
//   - Stateless utility; modifies OutputSection data in-place
// Links: codegen/common/linker/LinkTypes.hpp
//        codegen/common/linker/ObjFileReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file RelocApplier.hpp
 * @brief Declares checked cross-format relocation application for merged layouts.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"

#include <ostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Resolves symbol addresses and applies every relocation to merged bytes.
/// @details Builds input-to-output placement maps, finalizes global addresses,
///          resolves local/global/GOT and dynamic targets, validates patch-site
///          bounds and instruction forms, records loader bind/rebase entries
///          where static patching is inappropriate, and performs target-specific
///          postprocessing such as Windows `.pdata` ordering.
/// @param objects Parsed input objects whose relocation records are consumed.
/// @param layout Merged layout modified in place, including section bytes,
///               resolved symbols, and loader fixup lists.
/// @param dynamicSyms Symbols expected to resolve through shared libraries.
/// @param platform Target executable ABI and object naming policy.
/// @param arch Target instruction-set architecture.
/// @param err Stream that receives the first relocation diagnostic.
/// @return `true` after all relocations and postprocessing succeed.
bool applyRelocations(const std::vector<ObjFile> &objects,
                      LinkLayout &layout,
                      const std::unordered_set<std::string> &dynamicSyms,
                      LinkPlatform platform,
                      LinkArch arch,
                      std::ostream &err);

} // namespace zanna::codegen::linker
