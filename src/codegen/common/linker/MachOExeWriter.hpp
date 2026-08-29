//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/MachOExeWriter.hpp
// Purpose: Write Mach-O executables from linked sections.
// Key invariants:
//   - MH_EXECUTE with LC_MAIN (no LC_UNIXTHREAD)
//   - __PAGEZERO first segment, then __TEXT, __DATA, __LINKEDIT
//   - Page alignment: 16KB for arm64, 4KB for x86_64
//   - Ad-hoc code signing for arm64 (required macOS Ventura+)
// Ownership/Lifetime:
//   - Stateless writer utility
// Links: codegen/common/linker/LinkTypes.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOExeWriter.hpp
 * @brief Declares direct Mach-O 64-bit executable serialization.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <cstddef>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Describes one ordered `LC_LOAD_DYLIB` dependency.
struct DylibImport {
    std::string path; ///< Dylib path (e.g., "/usr/lib/libSystem.B.dylib").
};

/// @brief Writes a complete Mach-O 64-bit executable from a finalized layout.
/// @details Emits `__PAGEZERO`, `__TEXT`, `__DATA`, and `__LINKEDIT` segments,
///          dyld load/bind/rebase metadata, symbol and string tables, `LC_MAIN`,
///          build-version metadata, optional stack sizing, and an arm64 ad-hoc
///          signature. The completed image is installed through the shared
///          atomic file writer.
/// @param path UTF-8 destination path.
/// @param layout Final merged sections, addresses, symbols, and loader fixups.
/// @param arch Target x86-64 or AArch64 architecture.
/// @param dylibs Ordered dynamic libraries whose positions define ordinals.
/// @param dynSyms Symbols imported from those libraries.
/// @param symOrdinals Symbol-to-one-based-dylib-ordinal map. Zero selects flat
///                    lookup; missing symbols default to ordinal one.
/// @param stackSize Requested `LC_MAIN` stack size; zero selects the writer default.
/// @param emitLocalSymbols Publish every placed definition as a non-external
///        `LC_SYMTAB` entry (profiler/debugger names); `false` keeps only the
///        entry point and imports.
/// @param err Stream that receives validation or file-write diagnostics.
/// @return `true` when a complete executable is installed; otherwise `false`.
bool writeMachOExe(const std::string &path,
                   const LinkLayout &layout,
                   LinkArch arch,
                   const std::vector<DylibImport> &dylibs,
                   const std::unordered_set<std::string> &dynSyms,
                   const std::unordered_map<std::string, uint32_t> &symOrdinals,
                   std::size_t stackSize,
                   bool emitLocalSymbols,
                   std::ostream &err);

/// @brief Writes a Mach-O executable using the default stack size.
/// @param path UTF-8 destination path.
/// @param layout Final merged layout.
/// @param arch Target architecture.
/// @param dylibs Ordered dylib dependencies.
/// @param dynSyms Dynamic import names.
/// @param symOrdinals Per-symbol dylib ordinals.
/// @param err Diagnostic output stream.
/// @return `true` on successful serialization and installation.
inline bool writeMachOExe(const std::string &path,
                          const LinkLayout &layout,
                          LinkArch arch,
                          const std::vector<DylibImport> &dylibs,
                          const std::unordered_set<std::string> &dynSyms,
                          const std::unordered_map<std::string, uint32_t> &symOrdinals,
                          std::ostream &err) {
    return writeMachOExe(path, layout, arch, dylibs, dynSyms, symOrdinals, 0, true, err);
}

} // namespace zanna::codegen::linker
