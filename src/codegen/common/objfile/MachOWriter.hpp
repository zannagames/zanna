//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/MachOWriter.hpp
// Purpose: Mach-O relocatable object file writer for x86_64 and AArch64.
//          Produces valid .o files that Apple's ld64 linker can consume.
// Key invariants:
//   - Produces MH_MAGIC_64 MH_OBJECT files with LC_SEGMENT_64, LC_BUILD_VERSION,
//     LC_SYMTAB, and LC_DYSYMTAB load commands
//   - Two sections: __TEXT,__text (code) and __TEXT,__const (read-only data)
//   - Symbol names get underscore prefix (Darwin convention)
//   - Relocations sorted in descending address order per section
//   - Addends embedded in instruction bytes (Mach-O REL, not RELA)
// Ownership/Lifetime:
//   - Created via ObjectFileWriter factory; caller owns the unique_ptr
// Links: codegen/common/objfile/ObjectFileWriter.hpp
//        plans/05-macho-writer.md
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOWriter.hpp
 * @brief Declares the macOS x86-64 and arm64 relocatable-object writer.
 *
 * The writer emits 64-bit Mach-O objects with Darwin symbol mangling,
 * architecture-specific relocation records, optional compact unwind/debug
 * sections, and subsection-via-symbol support for merged function atoms.
 */

#pragma once

#include "codegen/common/objfile/ObjectFileWriter.hpp"

namespace zanna::codegen::objfile {

/// @brief Serializes macOS x86-64 or arm64 `MH_OBJECT` files.
class MachOWriter : public ObjectFileWriter {
  public:
    /// @brief Construct a Mach-O writer for one target architecture.
    /// @param arch Architecture selecting CPU identifiers and relocation encodings.
    explicit MachOWriter(ObjArch arch) : arch_(arch) {}

    /// @brief Write a Mach-O object with combined text and const sections.
    /// @param path Destination filesystem path.
    /// @param text Code, symbols, relocations, and compact-unwind metadata.
    /// @param rodata Read-only data and symbolic metadata.
    /// @param err Stream that receives validation or output diagnostics.
    /// @return `true` after atomically committing a valid object.
    bool write(const std::string &path,
               const CodeSection &text,
               const CodeSection &rodata,
               std::ostream &err) override;

    /// @brief Write multiple function atoms using subsections-via-symbols.
    /// @details Function sections are concatenated after duplicate local labels
    ///          are disambiguated; symbol boundaries retain atom granularity.
    /// @param path Destination filesystem path.
    /// @param textSections Ordered function-level code sections.
    /// @param rodata Shared read-only data and symbolic metadata.
    /// @param err Stream that receives validation or output diagnostics.
    /// @return `true` after atomically committing a valid object.
    bool write(const std::string &path,
               const std::vector<CodeSection> &textSections,
               const CodeSection &rodata,
               std::ostream &err) override;

  private:
    ///< Architecture used for CPU and relocation encoding.
    ObjArch arch_;
};

} // namespace zanna::codegen::objfile
