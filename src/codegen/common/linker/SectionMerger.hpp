//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/common/linker/SectionMerger.hpp
// Purpose: Merge input sections from multiple object files into output sections
//          and assign virtual addresses with proper page alignment.
// Key invariants:
//   - macOS arm64: 16KB page alignment (0x4000) — dyld rejects wrong alignment
//   - All other platforms: 4KB (0x1000)
//   - __TEXT contains both code and rodata on Mach-O
//   - Base addresses: macOS=0x100000000, Linux non-PIE=0x400000, Windows=0x140000000
// Ownership/Lifetime:
//   - SectionMerger is stateless; builds LinkLayout from inputs
// Links: codegen/common/linker/LinkTypes.hpp
//        codegen/common/linker/ObjFileReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file SectionMerger.hpp
 * @brief Declares platform-aware section merging and virtual-address assignment.
 *
 * The merger converts per-object sections into a deterministic @ref LinkLayout,
 * retaining input-to-output chunk mappings needed by symbol resolution and
 * relocation. Address assignment observes the target page size, section
 * alignment, and permission boundaries expected by executable writers.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"

#include <ostream>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Recompute virtual addresses for an already ordered output-section list.
/// @details Allocated sections begin after the image-header page. Permission
///          transitions start a new page on ELF and Mach-O, while every PE
///          section begins on a page; individual section alignment is then
///          applied before recording its address.
/// @param layout Ordered sections to update in place; a zero image base is
///               replaced with the target platform's default.
/// @param platform Target executable ABI controlling page-boundary policy.
/// @param err Stream that receives alignment or address-overflow diagnostics.
/// @return `true` when every allocated section receives a valid address.
bool assignSectionVirtualAddresses(LinkLayout &layout, LinkPlatform platform, std::ostream &err);

/// @brief Merge all live input sections and compute their final address layout.
/// @details Applies ABI-specific ordering for ELF init arrays, Mach-O module
///          initialization and TLS sections, and COFF CRT/TLS subsections.
///          Preserved-name metadata and non-allocated debug sections remain
///          independently addressable, while ordinary sections coalesce by
///          semantic class.
/// @param objects All object files, including selected archive members.
/// @param platform Target ABI controlling page size and subsection rules.
/// @param arch Target architecture, including the macOS arm64 page-size choice.
/// @param layout Receives merged sections, chunk mappings, and virtual addresses.
/// @param err Stream that receives the first merge or layout diagnostic.
/// @return `true` when all live sections are merged and assigned successfully.
bool mergeSections(const std::vector<ObjFile> &objects,
                   LinkPlatform platform,
                   LinkArch arch,
                   LinkLayout &layout,
                   std::ostream &err);

} // namespace zanna::codegen::linker
