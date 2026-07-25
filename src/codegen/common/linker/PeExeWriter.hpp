//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/PeExeWriter.hpp
// Purpose: Write PE executables from linked sections.
// Key invariants:
//   - DOS stub + PE signature + COFF header + Optional Header (PE32+)
//   - Preserves SectionMerger-assigned section RVAs for linked code/data
//   - Emits a PE import directory when DLL imports are provided
//   - Emits an x64 startup shim that terminates through ExitProcess when needed
// Ownership/Lifetime:
//   - Stateless writer utility
// Links: codegen/common/linker/LinkTypes.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file PeExeWriter.hpp
 * @brief Declares direct Windows PE32+ executable serialization.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <cstddef>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Groups linker-visible imports supplied by one Windows DLL.
struct DllImport {
    std::string dllName;                ///< DLL name (e.g., "kernel32.dll").
    std::vector<std::string> functions; ///< Linker-visible symbol names.
    std::unordered_map<std::string, std::string>
        importNames; ///< Optional PE import-name overrides keyed by symbol name.
};

/// @brief Write a PE32+ executable from linked sections, DLL imports, and relocation data.
/// @details Produces a valid Windows .exe with DOS stub, PE headers, section data,
///          and an .idata import directory. Optionally emits an x64 startup shim
///          that calls main() and terminates via ExitProcess.
/// @param path UTF-8 destination path.
/// @param layout Final merged sections, addresses, symbols, and rebase entries.
/// @param arch Target AMD64 or AArch64 architecture.
/// @param imports Ordered DLL import groups.
/// @param slotRvas Optional preallocated IAT slot RVAs keyed by linker symbol.
/// @param emitStartupStub Whether to synthesize a `main`/`ExitProcess` entry shim.
/// @param stackSize Requested stack reserve; zero selects the PE default.
/// @param err Diagnostic output stream.
/// @return `true` when the executable is serialized and installed.
bool writePeExe(const std::string &path,
                const LinkLayout &layout,
                LinkArch arch,
                const std::vector<DllImport> &imports,
                const std::unordered_map<std::string, uint32_t> &slotRvas,
                bool emitStartupStub,
                std::size_t stackSize,
                std::ostream &err);

/// @brief Convenience overload — emits the startup stub, no IAT slot overrides, default stack.
/// @param path UTF-8 destination path.
/// @param layout Final merged layout.
/// @param arch Target architecture.
/// @param imports Ordered DLL import groups.
/// @param err Diagnostic output stream.
/// @return `true` on successful serialization and installation.
inline bool writePeExe(const std::string &path,
                       const LinkLayout &layout,
                       LinkArch arch,
                       const std::vector<DllImport> &imports,
                       std::ostream &err) {
    static const std::unordered_map<std::string, uint32_t> kNoSlots;
    return writePeExe(path, layout, arch, imports, kNoSlots, true, 0, err);
}

} // namespace zanna::codegen::linker
