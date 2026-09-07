//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/DynStubGen.hpp
// Purpose: Generate synthetic object files containing AArch64 stub trampolines
//          for dynamic symbols and ObjC selector stubs.
// Key invariants:
//   - ObjC selector stubs move symbols out of dynamicSyms (resolved locally)
//   - Dynamic stubs generate GOT entries filled by dyld at load time
//   - Output ObjFiles use ELF relocation format for the reloc applier
// Ownership/Lifetime:
//   - Returned ObjFiles are value types owned by the caller
// Links: codegen/common/linker/NativeLinker.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file DynStubGen.hpp
 * @brief Declares synthetic object generation for loader-backed calls and
 *        Objective-C selector dispatch.
 */

#pragma once

#include "codegen/common/linker/ObjFileReader.hpp"

#include <string>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Generate ObjC selector stub trampolines for AArch64 macOS.
///
/// @details Creates a synthetic ObjFile containing stubs for
///          `objc_msgSend$selector` symbols. Each stub loads the selector
///          reference pointer into x1 and branches to objc_msgSend.
///          Matching symbols are removed from @p dynamicSyms (resolved
///          locally) and `objc_msgSend` is added as a dynamic dependency.
///
/// @param dynamicSyms Mutable set of dynamic symbols; matched entries are removed.
/// @return Synthetic ObjFile with stub text, selector references, and relocations.
/// @throws std::runtime_error If a generated size or symbol index exceeds the
///         linker's representable range.
ObjFile generateObjcSelectorStubsAArch64(std::unordered_set<std::string> &dynamicSyms);

/// @brief Generate dynamic symbol stub trampolines and GOT entries for AArch64.
///
/// @details Creates a synthetic ObjFile containing 12-byte stubs
///          (adrp/ldr/br x16) and 8-byte GOT slots for each dynamic symbol.
///          The GOT entries are filled by dyld at load time via non-lazy binding.
///
/// @param dynamicSyms Set of dynamic symbols requiring stubs.
/// @param copyRelocDataSymbols When `true`, imported data objects receive
///        writable storage plus a loader copy relocation instead of a
///        trampoline. Required for ELF targets, whose non-PIC references load
///        through the symbol directly; Mach-O reaches imported data through the
///        GOT already, so macOS passes `false`.
/// @return Synthetic ObjFile with stub text, GOT data, and relocations.
/// @throws std::runtime_error If a generated size or symbol index exceeds the
///         linker's representable range.
ObjFile generateDynStubsAArch64(const std::unordered_set<std::string> &dynamicSyms,
                                bool copyRelocDataSymbols);

/// @brief Generate dynamic symbol jump stubs and GOT entries for Linux x86_64.
///
/// @details Creates a synthetic ELF ObjFile containing 6-byte x86_64 jump
///          stubs (`jmpq *__got_sym(%rip)`) and 8-byte GOT slots for each
///          dynamic symbol. The executable writer emits dynamic relocations for
///          the GOT slots so the runtime loader resolves them before entry.
///          Imported data objects are the exception: they receive writable
///          storage plus a loader copy relocation, because ELF references to
///          them load through the symbol rather than branching to it.
///
/// @param dynamicSyms Set of dynamic symbols requiring loader-backed GOT slots.
/// @return Synthetic ObjFile with stub text, GOT data, and relocations.
/// @throws std::runtime_error If a generated size or symbol index exceeds the
///         linker's representable range.
ObjFile generateDynStubsX8664(const std::unordered_set<std::string> &dynamicSyms);

/// @brief Synthesize link-time GOT slots for statically resolved x86-64 GOTPCREL references.
/// @details Compilers emit the plain `R_X86_64_GOTPCREL` form (not the `X`
///          variants) for instructions the assembler will not let the linker
///          relax, e.g. `pushq foo@GOTPCREL(%rip)`. The psABI forbids
///          relaxing those, so the referenced symbol needs a real GOT slot
///          even when it is defined inside the link. This builds one
///          `.got.zanna_local` section holding an 8-byte slot per such
///          symbol, each defined as `__gotl_<name>` (the relocation applier
///          consults it after the loader-bound `__got_<name>` key; the
///          distinct prefix keeps absolute-pointer relocations from treating
///          the target as an import) and filled through an `R_X86_64_64`
///          relocation against `<name>`, so ordinary relocation application
///          writes the absolute address and no loader binding is involved.
/// @param objects Input objects whose relocations are scanned. Only symbols
///                that some object defines with global or weak binding and
///                that are not in @p dynamicSyms receive a slot; local
///                (static) symbols are skipped because their names are not
///                unique across objects.
/// @param dynamicSyms Symbols that resolve through the loader and therefore
///                    already own a synthesized GOT slot.
/// @return Synthetic ELF object, or one with no sections when nothing is needed.
ObjFile generateStaticGotSlotsX8664(const std::vector<ObjFile> &objects,
                                    const std::unordered_set<std::string> &dynamicSyms);

} // namespace zanna::codegen::linker
