//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/NativeLinker.hpp
// Purpose: Top-level native linker orchestrator. Ties together archive reading,
//          object file parsing, symbol resolution, section merging, relocation
//          application, and executable output.
// Key invariants:
//   - Zero external tool dependencies
//   - Writes ELF (Linux), Mach-O (macOS), and PE (Windows) directly
//   - Dynamic imports are implemented for Windows x86_64/AArch64, macOS
//     AArch64, and Linux x86_64/AArch64
// Ownership/Lifetime:
//   - Stateless entry point; each call is independent
// Links: codegen/common/linker/LinkTypes.hpp
//        codegen/common/LinkerSupport.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file NativeLinker.hpp
 * @brief Declares the dependency-free top-level native link pipeline.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Configures one independent native link invocation.
struct NativeLinkerOptions {
    std::string objPath; ///< Path to the user's compiled .o file.
    /// Optional serialized user object supplied directly by native codegen.
    /// When present, the linker parses these bytes instead of reading @ref objPath;
    /// objPath remains the diagnostic display name.
    std::optional<std::vector<uint8_t>> objData;
    std::string exePath;                   ///< Output executable path.
    std::vector<std::string> archivePaths; ///< Runtime archive .a paths (in dependency order).
    /// Archives whose every member is loaded unconditionally (whole-archive /
    /// -force_load semantics). Used so editor-service bridges can override weak
    /// runtime stubs that demand-driven extraction would otherwise satisfy first.
    /// Members participate in resolution as if they were initial objects.
    std::vector<std::string> forceLoadArchivePaths;
    LinkPlatform platform = detectLinkPlatform();
    LinkArch arch = detectLinkArch();
    std::string entrySymbol = "main";       ///< Entry point symbol name.
    std::vector<std::string> extraObjPaths; ///< Additional .o files to link (e.g. asset blob).
    std::size_t stackSize = 0; ///< Requested stack size in bytes; 0 uses format defaults.
    std::optional<bool> windowsDebugRuntime; ///< Override CRT flavor on Windows when set.
    bool fastLink = false; ///< Skip non-essential size-reduction passes for edit/build cycles.
    bool preserveDebugSections = false; ///< Keep non-alloc DWARF/debug sections in output.
    /// Publish every placed definition (functions and data) as a non-external
    /// entry in the executable's symbol table so profilers and debuggers can
    /// name addresses. Loader behaviour is unaffected; `false` writes a
    /// stripped image carrying only the entry symbol and imports.
    bool emitLocalSymbols = true;
};

/// @brief Runs the complete native object-to-executable link pipeline.
/// @details Reads in-memory or on-disk objects and archives, resolves and
///          extracts symbols, synthesizes platform support/import objects,
///          strips and optionally folds content, merges sections, inserts
///          branch islands, applies relocations, and writes ELF, Mach-O, or PE
///          output without invoking external tools.
/// @param opts Input, target, optimization, runtime, and output options.
/// @param out Standard output stream reserved for successful informational output.
/// @param err Diagnostic and optional link-timing output stream.
/// @return Zero on success; one after a diagnosed pipeline failure.
int nativeLink(const NativeLinkerOptions &opts, std::ostream &out, std::ostream &err);

} // namespace zanna::codegen::linker
