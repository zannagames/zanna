//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/CoffWriter.hpp
// Purpose: COFF/PE object file writer for Windows (x86_64 and AArch64).
//          Produces valid .obj files that MSVC link.exe and LLD can consume.
// Key invariants:
//   - Produces IMAGE_FILE_MACHINE_AMD64 (0x8664) or IMAGE_FILE_MACHINE_ARM64 (0xAA64)
//   - 3 sections: .text, .rdata, .reloc (relocations inline after section data)
//   - Symbol names > 8 bytes use string table offset (/N format)
//   - String table has 4-byte size prefix (COFF convention)
//   - Relocations have no explicit addend (addend embedded in instruction bytes)
// Ownership/Lifetime:
//   - Created via ObjectFileWriter factory; caller owns the unique_ptr
// Links: codegen/common/objfile/ObjectFileWriter.hpp
//        plans/06-pe-coff-writer.md
//
//===----------------------------------------------------------------------===//

/**
 * @file CoffWriter.hpp
 * @brief Declares the Windows AMD64 and ARM64 COFF relocatable-object writer.
 *
 * The writer consumes encoder @ref CodeSection objects and produces standard
 * COFF objects with embedded addends, symbol and string tables, optional debug
 * line data, and generated `.xdata/.pdata` unwind metadata.
 */

#pragma once

#include "codegen/common/objfile/ObjectFileWriter.hpp"

namespace zanna::codegen::objfile {

/// @brief Serializes Windows x86-64 or ARM64 relocatable COFF objects.
/// @details Supports a conventional combined `.text` section and a
///          function-section mode used for per-function linker elimination.
class CoffWriter : public ObjectFileWriter {
  public:
    /// @brief Construct a COFF writer for one target architecture.
    /// @param arch Windows object architecture; selects machine and relocation codes.
    explicit CoffWriter(ObjArch arch) : arch_(arch) {}

    /// @brief Write an object containing one combined `.text` section.
    /// @param path Destination filesystem path.
    /// @param text Code, symbols, relocations, and unwind metadata to serialize.
    /// @param rodata Read-only data section and its symbolic metadata.
    /// @param err Stream that receives validation or output diagnostics.
    /// @return `true` after atomically committing a valid COFF object.
    bool write(const std::string &path,
               const CodeSection &text,
               const CodeSection &rodata,
               std::ostream &err) override;

    /// @brief Write an object with one independently discardable text section per function.
    /// @param path Destination filesystem path.
    /// @param textSections Ordered function-level code sections.
    /// @param rodata Shared read-only data section and symbolic metadata.
    /// @param err Stream that receives validation or output diagnostics.
    /// @return `true` after atomically committing a valid COFF object.
    bool write(const std::string &path,
               const std::vector<CodeSection> &textSections,
               const CodeSection &rodata,
               std::ostream &err) override;

  private:
    ///< Target architecture used for machine identifiers and relocation encoding.
    ObjArch arch_;
};

} // namespace zanna::codegen::objfile
