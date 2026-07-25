//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/BranchTrampoline.hpp
// Purpose: AArch64 branch trampoline insertion for the native linker.
//          Detects out-of-range B/BL (Branch26) relocations after section
//          merging and inserts ADRP+ADD+BR x16 trampolines at reachable
//          executable-section chunk boundaries.
// Key invariants:
//   - Only activates on AArch64 (no-op on x86-64)
//   - Only handles Branch26 (B/BL); B.cond (CondBr19) is left as-is
//   - Trampoline relocations (Page21+PageOff12) are applied inline
//   - Multiple branches to the same out-of-range target share one trampoline
//     per reachable island boundary
//   - Original Branch26 relocations are rewritten to synthetic trampoline
//     symbols so applyRelocations() preserves the patch
//   - VA re-assignment reuses the same logic as SectionMerger
// Links: codegen/common/linker/RelocApplier.cpp
//        codegen/common/linker/RelocClassify.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file BranchTrampoline.hpp
 * @brief Declares AArch64 branch-island insertion for out-of-range `B` and
 *        `BL` relocations.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"

#include <ostream>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Inserts branch islands for out-of-range AArch64 `B`/`BL` instructions.
/// @details Runs after section merging and before general relocation
///          application. Reachable branches remain untouched. For each
///          out-of-range target/addend pair, the pass inserts or reuses a
///          reachable `ADRP`/`ADD`/`BR x16` trampoline, reassigns section
///          addresses, patches the instruction, and retargets its relocation
///          to a synthetic symbol so later relocation processing preserves it.
/// @param objects Object files whose branch relocations and symbol tables may
///                be rewritten.
/// @param layout Merged layout whose executable section, chunks, addresses,
///               and global symbol table may be updated.
/// @param arch Target architecture; architectures other than AArch64 are a
///             successful no-op.
/// @param platform Target platform used when reassigning virtual addresses and
///                 applying symbol-name fallback rules.
/// @param err Stream that receives diagnostics for invalid input or an
///            unrepresentable trampoline layout.
/// @return `true` on success, including when no trampoline is needed;
///         otherwise `false`.
bool insertBranchTrampolines(std::vector<ObjFile> &objects,
                             LinkLayout &layout,
                             LinkArch arch,
                             LinkPlatform platform,
                             std::ostream &err);

} // namespace zanna::codegen::linker
