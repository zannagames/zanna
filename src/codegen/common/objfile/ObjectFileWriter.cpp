//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/ObjectFileWriter.cpp
// Purpose: Factory for creating object file writers.
// Key invariants:
//   - Returns nullptr for unimplemented format/arch combinations
// Ownership/Lifetime:
//   - Caller owns the returned unique_ptr
// Links: codegen/common/objfile/ObjectFileWriter.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ObjectFileWriter.cpp
 * @brief Implements shared object-writer dispatch, memory capture, and factory logic.
 */

#include "codegen/common/objfile/ObjectFileWriter.hpp"
#include "codegen/common/objfile/CoffWriter.hpp"
#include "codegen/common/objfile/ElfWriter.hpp"
#include "codegen/common/objfile/MachOWriter.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"
#include "common/Filesystem.hpp"

#include <fstream>

namespace zanna::codegen::objfile {

/// @copydoc ObjectFileWriter::write(const std::string&,
///                                  const std::vector<CodeSection>&,
///                                  const CodeSection&, std::ostream&)
bool ObjectFileWriter::write(const std::string &path,
                             const std::vector<CodeSection> &textSections,
                             const CodeSection &rodata,
                             std::ostream &err) {
    // Default: merge all text sections into one and delegate to single-section write().
    CodeSection merged;
    for (const auto &ts : textSections)
        merged.appendSection(ts);
    return write(path, merged, rodata, err);
}

/// @copydoc ObjectFileWriter::writeToMemory(std::vector<uint8_t>&,
///                                          const CodeSection&,
///                                          const CodeSection&, std::ostream&)
bool ObjectFileWriter::writeToMemory(std::vector<uint8_t> &output,
                                     const CodeSection &text,
                                     const CodeSection &rodata,
                                     std::ostream &err) {
    output.clear();
    memoryOutput_ = &output;
    bool ok = false;
    try {
        ok = write("<memory-object>", text, rodata, err);
    } catch (...) {
        memoryOutput_ = nullptr;
        output.clear();
        throw;
    }
    memoryOutput_ = nullptr;
    if (!ok)
        output.clear();
    return ok;
}

/// @copydoc ObjectFileWriter::writeToMemory(std::vector<uint8_t>&,
///                                          const std::vector<CodeSection>&,
///                                          const CodeSection&, std::ostream&)
bool ObjectFileWriter::writeToMemory(std::vector<uint8_t> &output,
                                     const std::vector<CodeSection> &textSections,
                                     const CodeSection &rodata,
                                     std::ostream &err) {
    output.clear();
    memoryOutput_ = &output;
    bool ok = false;
    try {
        ok = write("<memory-object>", textSections, rodata, err);
    } catch (...) {
        memoryOutput_ = nullptr;
        output.clear();
        throw;
    }
    memoryOutput_ = nullptr;
    if (!ok)
        output.clear();
    return ok;
}

/// @copydoc ObjectFileWriter::commitOutput
bool ObjectFileWriter::commitOutput(const std::string &path,
                                    const std::vector<uint8_t> &bytes,
                                    const char *writerName,
                                    std::ostream &err) {
    if (memoryOutput_ != nullptr) {
        *memoryOutput_ = bytes;
        return true;
    }
    std::ofstream ofs(zanna::filesystem::pathFromUtf8(path), std::ios::binary | std::ios::trunc);
    if (!ofs) {
        err << writerName << ": cannot open " << path << " for writing\n";
        return false;
    }
    return checkedWriteAll(ofs, bytes, writerName, path, err);
}

/// @copydoc createObjectFileWriter
std::unique_ptr<ObjectFileWriter> createObjectFileWriter(ObjFormat format, ObjArch arch) {
    switch (format) {
        case ObjFormat::ELF:
            return std::make_unique<ElfWriter>(arch);
        case ObjFormat::MachO:
            return std::make_unique<MachOWriter>(arch);
        case ObjFormat::COFF:
            return std::make_unique<CoffWriter>(arch);
    }
    // Unhandled format/arch combination: return nullptr per the documented
    // contract so callers' null-checks handle it, rather than throwing.
    return nullptr;
}

} // namespace zanna::codegen::objfile
