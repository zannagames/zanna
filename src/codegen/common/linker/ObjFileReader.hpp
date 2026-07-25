//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ObjFileReader.hpp
// Purpose: Unified object file representation and reader interface for the
//          native linker. Provides ObjFile/ObjSection/ObjSymbol/ObjReloc
//          types and format-specific readers for ELF, Mach-O, and COFF.
// Key invariants:
//   - Auto-detects format from magic bytes
//   - Symbol names are unmangled (Mach-O '_' prefix stripped)
//   - Relocations carry addend uniformly (extracted from instructions for
//     Mach-O and COFF which lack explicit addend fields)
// Ownership/Lifetime:
//   - ObjFile owns all parsed data including section bytes
// Links: codegen/common/linker/ArchiveReader.hpp
//        codegen/common/objfile/Relocation.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ObjFileReader.hpp
 * @brief Defines the neutral parsed-object model and format-dispatch interface.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Neutralized duplicate-selection policy for COMDAT sections.
enum class ComdatSelection : uint8_t {
    None,
    NoDuplicates,
    Any,
    SameSize,
    ExactMatch,
    Associative,
    Largest,
};

/// @brief Format-neutral relocation record retaining its native type number.
struct ObjReloc {
    size_t offset = 0;            ///< Byte offset within the section.
    uint32_t type = 0;            ///< Format-native relocation type (e.g., R_X86_64_PLT32).
    uint32_t symIndex = 0;        ///< Index into ObjFile::symbols.
    int64_t addend = 0;           ///< Addend (explicit for ELF; extracted for Mach-O/COFF).
    bool pcrel = false;           ///< Mach-O r_pcrel bit; unused by ELF/COFF readers.
    uint8_t length = 0;           ///< Mach-O r_length field (0=byte, 1=word, 2=long, 3=quad).
    bool sectionRelative = false; ///< Reader-internal: raw reloc targeted a section ordinal.
};

/// @brief Normalized object symbol and its definition/ownership metadata.
struct ObjSymbol {
    std::string name;

    enum Binding : uint8_t { Local, Global, Weak, Undefined } binding = Undefined;

    uint32_t sectionIndex = 0; ///< Index into ObjFile::sections (0 = undefined).
    size_t offset = 0;         ///< Byte offset within section.
    size_t size = 0;
    bool absolute = false;       ///< Symbol value is absolute, not section-relative.
    bool weakExternal = false;   ///< Undefined weak external that may resolve to zero/default.
    std::string weakDefaultName; ///< Optional COFF weak-external fallback symbol.
    uint32_t weakExternalCharacteristics = 0; ///< COFF IMAGE_WEAK_EXTERN_SEARCH_* value.
    bool common = false;                      ///< Tentative/common symbol, coalesced by linker.
    size_t commonAlignment = 1;               ///< Required alignment for common storage.
    bool altEntry = false; ///< Mach-O N_ALT_ENTRY: alternate entry inside an atom.
    bool noDeadStrip = false; ///< Mach-O N_NO_DEAD_STRIP / __attribute__((used)); keep its section.
};

/// @brief Owns one parsed input section, its bytes, attributes, and relocations.
struct ObjSection {
    std::string name;
    std::vector<uint8_t> data;
    size_t memSize = 0; ///< Logical in-memory size. For non-zero-fill sections this is data.size().
    std::vector<ObjReloc> relocs;
    uint32_t alignment = 1;
    bool executable = false;
    bool writable = false;
    bool alloc = true;               ///< Section contributes to memory image.
    bool tls = false;                ///< Thread-local storage section.
    bool zeroFill = false;           ///< Section occupies memory but has no file bytes.
    bool dataSegment = false;        ///< Must be emitted in a data segment even if final read-only.
    bool isCStringSection = false;   ///< Section contains NUL-terminated C strings only.
    uint32_t associativeSection = 0; ///< COFF associative COMDAT parent section, if any.
    ComdatSelection comdatSelection = ComdatSelection::None; ///< Duplicate-selection policy.
    std::string comdatKey; ///< COFF/ELF COMDAT group key/signature.
    bool stripped = false; ///< Dead-strip removed this section explicitly.
    bool noDeadStrip = false; ///< ELF SHF_GNU_RETAIN / Mach-O S_ATTR_NO_DEAD_STRIP: keep alive.
};

/// @brief Returns an input section's logical in-memory footprint.
/// @param sec Parsed input section.
/// @return Declared zero-fill size when applicable, otherwise materialized byte count.
inline size_t objSectionMemSize(const ObjSection &sec) {
    return sec.zeroFill ? (sec.memSize != 0 ? sec.memSize : sec.data.size()) : sec.data.size();
}

/// @brief Supported relocatable object container format.
enum class ObjFileFormat : uint8_t {
    ELF,
    MachO,
    COFF,
    Unknown,
};

/// @brief Owns a complete normalized relocatable object.
struct ObjFile {
    std::string name; ///< Source file name or archive member name.
    ObjFileFormat format = ObjFileFormat::Unknown;
    bool is64bit = true;
    bool isLittleEndian = true;
    bool synthetic = false; ///< True for linker-created helper objects, never user/archive input.
    uint16_t machine = 0;   ///< Machine type (EM_X86_64, EM_AARCH64, etc.)

    std::vector<ObjSection> sections; ///< Index 0 is reserved (null section).
    std::vector<ObjSymbol> symbols;   ///< Index 0 is reserved (null symbol).
};

/// Maximum number of sections/symbols accepted from an object file.
/// Guards against corrupt or malicious inputs causing unbounded allocation.
inline constexpr uint32_t kMaxObjSections = 1 << 20;
inline constexpr uint32_t kMaxObjSymbols = 1 << 20; // 1M symbols.
inline constexpr size_t kMaxObjSectionBytes = 2ULL * 1024 * 1024 * 1024;
inline constexpr size_t kMaxObjMaterializedBytes = 4ULL * 1024 * 1024 * 1024;

/// @brief Detects a supported object format from leading bytes and COFF fields.
/// @param data Pointer to at least @p size bytes.
/// @param size Available byte count.
/// @return ELF, Mach-O, COFF, or Unknown.
/// @pre @p data is non-null when @p size is nonzero.
ObjFileFormat detectFormat(const uint8_t *data, size_t size);

/// @brief Detect a COFF short import-library member.
/// @details MSVC import archives can contain pseudo-members that describe DLL
///          imports rather than relocatable object code. They begin like a COFF
///          BigObj header (`0, 0xffff`) but must not be passed to the COFF
///          object reader.
/// @param data Candidate archive-member bytes; may be null.
/// @param size Available byte count.
/// @return `true` only for a structurally plausible AMD64/AArch64 import header.
bool isCoffImportLibraryMember(const uint8_t *data, size_t size);

/// @brief Detects and parses an object file from caller-owned bytes.
/// @details Parsing is transactional: @p obj is replaced only after the
///          selected reader succeeds.
/// @param data Raw object-file bytes.
/// @param size Byte count.
/// @param name Display name for diagnostics and the resulting object.
/// @param obj Destination parsed object.
/// @param err Diagnostic output stream.
/// @return `true` on successful complete parsing.
bool readObjFile(
    const uint8_t *data, size_t size, const std::string &name, ObjFile &obj, std::ostream &err);

/// @brief Reads an object file from a UTF-8 path and dispatches its parser.
/// @param path UTF-8 object-file path.
/// @param obj Destination parsed object, replaced only on parse success.
/// @param err Diagnostic output stream.
/// @return `true` after successful I/O and parsing.
bool readObjFile(const std::string &path, ObjFile &obj, std::ostream &err);

// Format-specific readers (called by readObjFile after detectFormat).
// All three share the readObjFile() signature; the dispatcher picks one based
// on the magic bytes.

/// @brief Parses an ELF relocatable object (`ET_REL`) into @p obj.
/// @param data Complete object bytes.
/// @param size Byte count.
/// @param name Diagnostic display name.
/// @param obj Destination object representation.
/// @param err Diagnostic output stream.
/// @return `true` when supported and valid.
bool readElfObj(
    const uint8_t *data, size_t size, const std::string &name, ObjFile &obj, std::ostream &err);

/// @brief Parse a Mach-O object file (MH_OBJECT) into @p obj.
/// @param data Complete object bytes.
/// @param size Byte count.
/// @param name Diagnostic display name.
/// @param obj Destination object representation.
/// @param err Diagnostic output stream.
/// @return `true` when supported and valid.
bool readMachOObj(
    const uint8_t *data, size_t size, const std::string &name, ObjFile &obj, std::ostream &err);

/// @brief Parse a COFF object (Microsoft x64 / ARM64) into @p obj.
/// @param data Complete object bytes.
/// @param size Byte count.
/// @param name Diagnostic display name.
/// @param obj Destination object representation.
/// @param err Diagnostic output stream.
/// @return `true` when supported and valid.
bool readCoffObj(
    const uint8_t *data, size_t size, const std::string &name, ObjFile &obj, std::ostream &err);

} // namespace zanna::codegen::linker
