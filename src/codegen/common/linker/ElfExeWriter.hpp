//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ElfExeWriter.hpp
// Purpose: Write ELF executables from linked sections.
// Key invariants:
//   - ET_EXEC with static base address (non-PIE for simplicity)
//   - PT_LOAD segments page-aligned
//   - PT_GNU_STACK with non-executable flags
// Ownership/Lifetime:
//   - Stateless writer utility
// Links: codegen/common/linker/LinkTypes.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ElfExeWriter.hpp
 * @brief Declares Linux ELF64 executable serialization for merged link layouts.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <cstddef>
#include <ostream>
#include <string>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Writes an ELF64 executable from a finalized merged layout.
/// @details Emits a non-PIE `ET_EXEC` image with page-aligned load segments,
///          a non-executable GNU stack declaration, optional dynamic-loader
///          metadata, TLS metadata when present, and an optional startup shim.
///          The completed image is installed through the shared atomic file
///          writer and marked executable.
/// @param path UTF-8 destination path.
/// @param layout Final merged sections and resolved symbols.
/// @param arch Target architecture, currently x86-64 or AArch64.
/// @param neededLibs Ordered ELF `DT_NEEDED` library names.
/// @param dynSyms Loader-resolved symbols requiring dynamic metadata.
/// @param stackSize Requested GNU stack memory size; zero uses the platform default.
/// @param emitStartupStub Whether to synthesize the runtime startup shim before `main`.
/// @param err Stream that receives validation or file-write diagnostics.
/// @return `true` when a complete executable is installed; otherwise `false`.
bool writeElfExe(const std::string &path,
                 const LinkLayout &layout,
                 LinkArch arch,
                 const std::vector<std::string> &neededLibs,
                 const std::unordered_set<std::string> &dynSyms,
                 std::size_t stackSize,
                 bool emitStartupStub,
                 std::ostream &err);

/// @brief Writes a potentially dynamic ELF image with default stack and startup behavior.
/// @param path UTF-8 destination path.
/// @param layout Final merged sections and resolved symbols.
/// @param arch Target architecture.
/// @param neededLibs Ordered dynamic-library dependencies.
/// @param dynSyms Loader-resolved symbols.
/// @param err Diagnostic output stream.
/// @return `true` on successful serialization and installation.
inline bool writeElfExe(const std::string &path,
                        const LinkLayout &layout,
                        LinkArch arch,
                        const std::vector<std::string> &neededLibs,
                        const std::unordered_set<std::string> &dynSyms,
                        std::ostream &err) {
    return writeElfExe(path, layout, arch, neededLibs, dynSyms, 0, false, err);
}

/// @brief Writes a fully static ELF image with an explicit GNU stack size.
/// @param path UTF-8 destination path.
/// @param layout Final merged sections and resolved symbols.
/// @param arch Target architecture.
/// @param stackSize Requested stack size in bytes.
/// @param err Diagnostic output stream.
/// @return `true` on successful serialization and installation.
inline bool writeElfExe(const std::string &path,
                        const LinkLayout &layout,
                        LinkArch arch,
                        std::size_t stackSize,
                        std::ostream &err) {
    return writeElfExe(path, layout, arch, {}, {}, stackSize, false, err);
}

/// @brief Writes a fully static ELF image with default link options.
/// @param path UTF-8 destination path.
/// @param layout Final merged sections and resolved symbols.
/// @param arch Target architecture.
/// @param err Diagnostic output stream.
/// @return `true` on successful serialization and installation.
inline bool writeElfExe(const std::string &path,
                        const LinkLayout &layout,
                        LinkArch arch,
                        std::ostream &err) {
    return writeElfExe(path, layout, arch, {}, {}, 0, false, err);
}

} // namespace zanna::codegen::linker
