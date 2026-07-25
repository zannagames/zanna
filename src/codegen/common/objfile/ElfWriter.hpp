//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/ElfWriter.hpp
// Purpose: ELF relocatable object file writer for x86_64 and AArch64.
//          Produces valid .o files that the system linker can consume.
// Key invariants:
//   - Produces ELFCLASS64, ELFDATA2LSB (little-endian), ET_REL files
//   - 8 sections: null, .text, .rodata, .rela.text, .symtab, .strtab,
//     .shstrtab, .note.GNU-stack
//   - Local symbols precede globals in .symtab (ELF requirement)
//   - Section symbols for .text and .rodata are emitted for cross-section relocs
// Ownership/Lifetime:
//   - Created via ObjectFileWriter factory; caller owns the unique_ptr
// Links: codegen/common/objfile/ObjectFileWriter.hpp
//        plans/04-elf-writer.md
//
//===----------------------------------------------------------------------===//

/**
 * @file ElfWriter.hpp
 * @brief Declares the little-endian ELF64 relocatable-object writer.
 *
 * The writer supports AMD64 and AArch64, explicit-addend RELA relocations,
 * optional writable/debug sections, and both combined and per-function text
 * layouts.
 */

#pragma once

#include "codegen/common/objfile/ObjectFileWriter.hpp"

namespace zanna::codegen::objfile {

/// @brief Serializes Linux x86-64 or AArch64 ELF64 relocatable objects.
class ElfWriter : public ObjectFileWriter {
  public:
    /// @brief Construct an ELF writer for one target architecture.
    /// @param arch Architecture selecting `e_machine` and relocation numbers.
    explicit ElfWriter(ObjArch arch) : arch_(arch) {}

    /// @brief Write an object containing one combined `.text` section.
    /// @param path Destination filesystem path.
    /// @param text Code, symbols, and relocations to serialize.
    /// @param rodata Read-only data and its symbolic metadata.
    /// @param err Stream that receives validation or output diagnostics.
    /// @return `true` after atomically committing a valid ELF object.
    bool write(const std::string &path,
               const CodeSection &text,
               const CodeSection &rodata,
               std::ostream &err) override;

    /// @brief Write an object with independently discardable per-function text sections.
    /// @param path Destination filesystem path.
    /// @param textSections Ordered function-level code sections.
    /// @param rodata Shared read-only data and symbolic metadata.
    /// @param err Stream that receives validation or output diagnostics.
    /// @return `true` after atomically committing a valid ELF object.
    bool write(const std::string &path,
               const std::vector<CodeSection> &textSections,
               const CodeSection &rodata,
               std::ostream &err) override;

  private:
    ///< Architecture used for ELF machine and relocation encoding.
    ObjArch arch_;
};

} // namespace zanna::codegen::objfile
