//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/MachOBindRebase.hpp
// Purpose: Mach-O bind/rebase opcode emission and symbol table construction.
//          Used by MachOExeWriter to generate __LINKEDIT content.
// Key invariants:
//   - Bind opcodes use two-level namespace (per-symbol dylib ordinals)
//   - Rebase opcodes sorted by address, run-length encoded
//   - Symbol table: _main as ext-defined, dynamic imports as undefined
//   - nlist desc encodes library ordinal (bits [15:8]) for MH_TWOLEVEL
// Ownership/Lifetime:
//   - Stateless builder functions — output appended to caller-owned vectors
// Links: codegen/common/linker/MachOExeWriter.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOBindRebase.hpp
 * @brief Declares dyld bind/rebase stream and Mach-O symbol-table builders.
 */

#pragma once

#include "codegen/common/linker/LinkTypes.hpp"

#include <cstdint>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Builds dyld bind opcodes for GOT, TLV, and direct data-pointer imports.
/// @details Appends entries to @p bindData and terminates the stream with
///          `BIND_OPCODE_DONE`. Addresses are encoded relative to the supplied
///          data-segment base and use the supplied load-command segment index.
/// @param bindData Destination opcode stream; may contain partial additions on failure.
/// @param gotEntries Loader-populated GOT slots.
/// @param layout Final sections, TLV descriptors, and direct bind entries.
/// @param dataSegVmAddr Virtual-address base of the Mach-O data segment.
/// @param dataSegIndex Load-command segment index encoded into bind opcodes.
/// @param symOrdinals Symbol-to-dylib ordinal map. Zero selects flat lookup;
///                    missing symbols default to ordinal one (libSystem).
/// @param err Diagnostic output stream.
/// @return `false` on a hard layout error, such as a malformed TLV descriptor
///         section; otherwise `true`.
bool buildBindOpcodes(std::vector<uint8_t> &bindData,
                      const std::vector<GotEntry> &gotEntries,
                      const LinkLayout &layout,
                      uint64_t dataSegVmAddr,
                      uint32_t dataSegIndex,
                      const std::unordered_map<std::string, uint32_t> &symOrdinals,
                      std::ostream &err);

/// @brief Builds dyld rebase opcodes for ASLR-sensitive absolute pointers.
/// @details Converts layout entries to unique, sorted data-segment offsets and
///          run-length encodes consecutive eight-byte pointers. If the layout
///          has no rebase entries, nothing is appended.
/// @param rebaseData Destination opcode stream.
/// @param layout Final layout containing pointer fixup locations.
/// @param dataSegVmAddr Virtual-address base of the Mach-O data segment.
/// @param dataSegIndex Load-command segment index encoded into rebase opcodes.
void buildRebaseOpcodes(std::vector<uint8_t> &rebaseData,
                        const LinkLayout &layout,
                        uint64_t dataSegVmAddr,
                        uint32_t dataSegIndex);

/// @brief Builds `nlist_64` records and their string table for `__LINKEDIT`.
/// @details Appends `_main` as the sole external definition when present and
///          sorted dynamic imports as undefined entries. Synthetic `__got_*`
///          names are omitted. The string table begins with NUL and is padded
///          to four bytes, so callers normally pass empty destination buffers.
/// @param symtabData Destination 16-byte `nlist_64` records.
/// @param strtabData Destination NUL-separated string table.
/// @param layout Final global symbol table.
/// @param sectionOrder Layout section indices in Mach-O section-ordinal order
///                     (`__TEXT` sections first, then `__DATA`).
/// @param emitLocalSymbols When true, every placed definition other than the
///                         entry point is written first as a non-external
///                         `N_SECT` symbol so profilers can name addresses.
/// @param dynSyms Dynamic symbol names to publish as undefined.
/// @param symOrdinals Symbol-to-dylib ordinal map used in `n_desc`; zero maps
///                    to `DYNAMIC_LOOKUP_ORDINAL`, and missing names use one.
/// @param nLocal Receives the number of non-external definitions appended.
/// @param nExtDef Receives the number of external definitions appended.
/// @param nUndef Receives the number of undefined imports appended.
void buildSymtab(std::vector<uint8_t> &symtabData,
                 std::vector<uint8_t> &strtabData,
                 const LinkLayout &layout,
                 const std::vector<size_t> &sectionOrder,
                 bool emitLocalSymbols,
                 const std::unordered_set<std::string> &dynSyms,
                 const std::unordered_map<std::string, uint32_t> &symOrdinals,
                 uint32_t &nLocal,
                 uint32_t &nExtDef,
                 uint32_t &nUndef);

} // namespace zanna::codegen::linker
