//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/PlatformImportPlanner.hpp
// Purpose: Declare per-platform dynamic-import planners used by the native
//          linker. Each planner converts a set of unresolved dynamic symbols
//          into the platform-specific data structures (Mach-O dylibs +
//          ordinal table, ELF DT_NEEDED list, or COFF idata import directory)
//          that the matching exe writer can serialise directly.
// Key invariants:
//   - Planners are pure: they consult only the input symbol set and a fixed,
//     baked-in symbol-to-library map. They never touch the filesystem.
//   - Producing a plan never imposes a specific symbol order beyond what is
//     required by the platform ABI (e.g., LC_LOAD_DYLIB ordinals).
// Ownership/Lifetime: Output structs are returned by reference; their internal
//                     vectors/maps are owned by the caller's plan object.
// Links: NativeLinker.cpp, MachOExeWriter.hpp, PeExeWriter.hpp, ElfExeWriter.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file PlatformImportPlanner.hpp
 * @brief Declares deterministic target-specific dynamic-import planning.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/MachOExeWriter.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/PeExeWriter.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Result of resolving Windows dynamic-library imports.
/// @details The synthetic @c obj carries the .idata$* sections produced from
///          @c imports so the standard SectionMerger pass can place them.
struct WindowsImportPlan {
    ObjFile obj;
    std::vector<DllImport> imports;
};

/// @brief Result of resolving Mach-O dynamic-library imports.
/// @details @c dylibs is the LC_LOAD_DYLIB list and @c symOrdinals maps each
///          imported symbol to the 1-based ordinal of its providing dylib used
///          by the LC_DYLD_INFO bind opcode stream.
struct MacImportPlan {
    std::vector<DylibImport> dylibs;
    std::unordered_map<std::string, uint32_t> symOrdinals;
};

/// @brief Result of resolving Linux dynamic imports — just the DT_NEEDED list.
/// @details The list uses stable ABI-conventional ordering and is owned by the plan.
struct LinuxImportPlan {
    std::vector<std::string> neededLibs;
};

/// @brief Plan Mach-O dylib imports for a set of dynamic symbols.
/// @details Walks @p dynamicSyms, looks up each symbol in the bundled symbol-
///          to-dylib table, and populates @p plan with the unique dylib list
///          plus per-symbol ordinals. Returns false (and writes to @p err) on
///          an unrecognised symbol.
/// @param dynamicSyms Unresolved symbols accepted by macOS dynamic policy.
/// @param plan Destination dylib list and symbol ordinals, cleared before planning.
/// @param err Diagnostic output stream.
/// @return `true` when every symbol has a valid provider or lookup policy.
bool planMacImports(const std::unordered_set<std::string> &dynamicSyms,
                    MacImportPlan &plan,
                    std::ostream &err);

/// @brief Plan Linux DT_NEEDED entries for a set of dynamic symbols.
/// @details Resolves each symbol to its providing shared object (libc.so.6,
///          libpthread.so.0, etc.) and produces the deduplicated NEEDED list.
///          libc is included even when @p dynamicSyms is empty.
/// @param dynamicSyms Unresolved symbols to validate and classify.
/// @param plan Destination dependency list, cleared before planning and on failure.
/// @param err Diagnostic output stream.
/// @return `true` when every symbol is recognized for Linux.
bool planLinuxImports(const std::unordered_set<std::string> &dynamicSyms,
                      LinuxImportPlan &plan,
                      std::ostream &err);

/// @brief Plan Windows .idata import directory contents for dynamic symbols.
/// @param arch         Target architecture (selects ARM64 vs x64 ABI quirks).
/// @param dynamicSyms  Set of unresolved import symbols.
/// @param debugRuntime When true, prefer ucrtbased.dll / vcruntime140d.dll.
/// @param plan         Receives the synthetic ObjFile + imports list.
/// @param err          Stream for diagnostics on unrecognised symbols.
/// @return `true` when every symbol is mapped or synthesized.
bool generateWindowsImports(LinkArch arch,
                            const std::unordered_set<std::string> &dynamicSyms,
                            bool debugRuntime,
                            WindowsImportPlan &plan,
                            std::ostream &err);

} // namespace zanna::codegen::linker
