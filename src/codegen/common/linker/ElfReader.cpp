//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ElfReader.cpp
// Purpose: ELF 64-bit relocatable object file reader.
// Key invariants:
//   - Handles both x86_64 (EM_X86_64=62) and AArch64 (EM_AARCH64=183)
//   - Uses explicit addends from .rela and decodes implicit addends from .rel
//   - Section name from .shstrtab, symbol names from .strtab
// Links: codegen/common/linker/ObjFileReader.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ElfReader.cpp
 * @brief Implements bounded ELF64 relocatable-object parsing for x86-64 and AArch64.
 *
 * The reader handles extended section indices, multiple symbol tables, COMDAT
 * groups, `SHT_NOBITS`, common/absolute/weak symbols, and both RELA and REL
 * relocation encodings without depending on host ELF headers.
 */

#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/RelocConstants.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"

#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace zanna::codegen::linker {

using zanna::codegen::objfile::checkedRange;
using zanna::codegen::objfile::readLE32;
using zanna::codegen::objfile::readLE64;

// ELF64 structures — defined inline to avoid system header dependencies.
namespace elf {
[[maybe_unused]] static constexpr uint16_t ET_REL = 1;
static constexpr uint16_t EM_X86_64 = 62;
static constexpr uint16_t EM_AARCH64 = 183;

[[maybe_unused]] static constexpr uint32_t SHT_PROGBITS = 1;
static constexpr uint32_t SHT_SYMTAB = 2;
static constexpr uint32_t SHT_STRTAB = 3;
static constexpr uint32_t SHT_RELA = 4;
static constexpr uint32_t SHT_REL = 9;
static constexpr uint32_t SHT_NOBITS = 8;
static constexpr uint32_t SHT_GROUP = 17;
static constexpr uint32_t SHT_SYMTAB_SHNDX = 18;

static constexpr uint32_t SHF_WRITE = 0x1;
static constexpr uint32_t SHF_ALLOC = 0x2;
static constexpr uint32_t SHF_EXECINSTR = 0x4;
static constexpr uint32_t SHF_TLS = 0x400;
static constexpr uint32_t GRP_COMDAT = 0x1;

static constexpr uint8_t STB_LOCAL = 0;
[[maybe_unused]] static constexpr uint8_t STB_GLOBAL = 1;
static constexpr uint8_t STB_WEAK = 2;

static constexpr uint16_t SHN_UNDEF = 0;
static constexpr uint16_t SHN_XINDEX = 0xFFFF;
static constexpr uint16_t SHN_ABS = 0xFFF1;
static constexpr uint16_t SHN_COMMON = 0xFFF2;

/// @brief Host-independent in-memory spelling of an ELF64 file header.
struct Elf64_Ehdr {
    uint8_t e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/// @brief Host-independent in-memory spelling of an ELF64 section header.
struct Elf64_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};

/// @brief Host-independent in-memory spelling of an ELF64 symbol record.
struct Elf64_Sym {
    uint32_t st_name;
    uint8_t st_info;
    uint8_t st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};

/// @brief ELF64 relocation record carrying an explicit addend.
struct Elf64_Rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t r_addend;
};

/// @brief ELF64 relocation record whose addend remains at the patch site.
struct Elf64_Rel {
    uint64_t r_offset;
    uint64_t r_info;
};

} // namespace elf

/// @brief Bounds-checked struct copy at @p offset within a byte buffer.
/// @tparam T Trivially copyable record type.
/// @param data Base of the input buffer.
/// @param size Total input-buffer size.
/// @param offset Byte offset at which the record begins.
/// @return Copied record, or `std::nullopt` when `sizeof(T)` bytes would exceed
///         @p size.
template <typename T>
static std::optional<T> readStruct(const uint8_t *data, size_t size, size_t offset) {
    if (offset > size || sizeof(T) > size - offset)
        return std::nullopt;
    T value{};
    std::memcpy(&value, data + offset, sizeof(T));
    return value;
}

/// @brief Tests ELF's valid section-alignment domain.
/// @param value Alignment field to validate.
/// @return `true` when the value is zero or an exact power of two.
static bool isPowerOfTwoOrZero(uint64_t value) {
    return value == 0 || (value & (value - 1)) == 0;
}

/// @brief Narrow an ELF 64-bit file offset/size to the host address size.
/// @details The ELF64 format can represent offsets and lengths that a 32-bit
///          host process cannot address. Reader code uses this helper before
///          passing format fields to size_t-based range checks.
/// @param value ELF64 offset or size.
/// @param out Receives the host-sized value when representable.
/// @return `true` on successful narrowing.
static bool checkedU64ToSize(uint64_t value, size_t &out) {
    if (value > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        return false;
    out = static_cast<size_t>(value);
    return true;
}

/// @brief Multiply two host-size values for table byte counts.
/// @details Section, symbol, and relocation table spans are derived from
///          format counts. This helper prevents wraparound before checkedRange()
///          validates the resulting byte range.
/// @param lhs Left factor.
/// @param rhs Right factor.
/// @param out Receives the product when representable.
/// @return `true` on success; `false` on overflow.
static bool checkedMulSize(size_t lhs, size_t rhs, size_t &out) {
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
        return false;
    out = lhs * rhs;
    return true;
}

/// @brief Sign-extend the low @p bits of @p value to a 64-bit signed integer.
/// @param value Unsigned bit pattern containing the field.
/// @param bits Width of the signed field.
/// @return Sign-extended field value.
static int64_t signExtend(uint64_t value, unsigned bits) {
    const uint64_t signBit = uint64_t{1} << (bits - 1);
    const uint64_t mask = (uint64_t{1} << bits) - 1;
    value &= mask;
    return static_cast<int64_t>((value ^ signBit) - signBit);
}

/// @brief Recover the implicit addend embedded in instruction bytes for SHT_REL inputs.
/// @details ELF .rel sections (rare) lack the explicit r_addend field that .rela
///          sections carry; this helper decodes the addend from the live
///          instruction at the relocation site, mirroring what the AArch64 ABI
///          documents for each relocation type. ELF x86_64 uses a literal
///          32/64-bit slot read; AArch64 BR/B.cond/ADRP/ADD-imm decode their
///          immediate fields and rescale by their natural shift.
/// @param machine ELF machine identifier.
/// @param relocType Architecture-specific relocation type.
/// @param sectionData Bytes of the relocation's target section.
/// @param offset Patch-site offset within @p sectionData.
/// @param out Receives the decoded signed addend.
/// @return `true` when the type is supported and its complete operand is in bounds.
static bool extractRelAddend(uint16_t machine,
                             uint32_t relocType,
                             const std::vector<uint8_t> &sectionData,
                             size_t offset,
                             int64_t &out) {
    out = 0;
    if (machine == elf::EM_X86_64) {
        if (relocType == elf_x64::kAbs64) {
            if (!checkedRange(offset, 8, sectionData.size()))
                return false;
            out = static_cast<int64_t>(readLE64(sectionData.data() + offset));
            return true;
        }
        switch (relocType) {
            case elf_x64::kPC32:
            case elf_x64::kPLT32:
            case elf_x64::kGotPcRel:
            case elf_x64::kAbs32:
            case elf_x64::kTpoff32:
            case elf_x64::kGotPcRelX:
            case elf_x64::kRexGotPcRelX:
                if (!checkedRange(offset, 4, sectionData.size()))
                    return false;
                {
                    int32_t val = 0;
                    std::memcpy(&val, sectionData.data() + offset, 4);
                    out = val;
                }
                return true;
            default:
                return false;
        }
    }

    if (machine != elf::EM_AARCH64)
        return false;

    if (relocType == elf_a64::kAbs64) {
        if (!checkedRange(offset, 8, sectionData.size()))
            return false;
        out = static_cast<int64_t>(readLE64(sectionData.data() + offset));
        return true;
    }

    const bool needsInsn =
        relocType == elf_a64::kCall26 || relocType == elf_a64::kJump26 ||
        relocType == elf_a64::kCondBr19 || relocType == elf_a64::kAdrPrelPgHi21 ||
        relocType == elf_a64::kAdrGotPage || relocType == elf_a64::kAddAbsLo12Nc ||
        relocType == elf_a64::kLdSt32Lo12Nc || relocType == elf_a64::kLdSt64Lo12Nc ||
        relocType == elf_a64::kLd64GotLo12Nc || relocType == elf_a64::kLdSt128Lo12Nc ||
        relocType == elf_a64::kTlsLeAddTprelHi12 || relocType == elf_a64::kTlsLeAddTprelLo12 ||
        relocType == elf_a64::kTlsLeAddTprelLo12Nc;
    if (!needsInsn)
        return false;
    if (!checkedRange(offset, 4, sectionData.size()))
        return false;

    const uint32_t insn = readLE32(sectionData.data() + offset);
    switch (relocType) {
        case elf_a64::kCall26:
        case elf_a64::kJump26:
            out = signExtend(insn & 0x03FFFFFFu, 26) << 2;
            return true;
        case elf_a64::kCondBr19:
            out = signExtend((insn >> 5) & 0x7FFFFu, 19) << 2;
            return true;
        case elf_a64::kAdrPrelPgHi21:
        case elf_a64::kAdrGotPage: {
            const uint32_t immlo = (insn >> 29) & 0x3u;
            const uint32_t immhi = (insn >> 5) & 0x7FFFFu;
            out = signExtend((immhi << 2) | immlo, 21) << 12;
            return true;
        }
        case elf_a64::kAddAbsLo12Nc:
        case elf_a64::kTlsLeAddTprelLo12:
        case elf_a64::kTlsLeAddTprelLo12Nc:
            out = static_cast<int64_t>((insn >> 10) & 0xFFFu);
            return true;
        case elf_a64::kTlsLeAddTprelHi12:
            out = static_cast<int64_t>(((insn >> 10) & 0xFFFu) << 12);
            return true;
        case elf_a64::kLdSt32Lo12Nc:
            out = static_cast<int64_t>(((insn >> 10) & 0xFFFu) << 2);
            return true;
        case elf_a64::kLdSt64Lo12Nc:
        case elf_a64::kLd64GotLo12Nc:
            out = static_cast<int64_t>(((insn >> 10) & 0xFFFu) << 3);
            return true;
        case elf_a64::kLdSt128Lo12Nc:
            out = static_cast<int64_t>(((insn >> 10) & 0xFFFu) << 4);
            return true;
        default:
            return true;
    }
}

/// @brief Read a NUL-terminated string from an ELF string table (SHT_STRTAB).
/// @param data Base of the object-file bytes.
/// @param size Size of the object-file buffer.
/// @param strTabOff Byte offset of the string table.
/// @param strTabSize String-table size in bytes.
/// @param nameOff Offset of the requested string within the table.
/// @return The decoded string, or `std::nullopt` when the table/range is
///         invalid or no terminator appears before its boundary.
static std::optional<std::string> readStringOpt(
    const uint8_t *data, size_t size, size_t strTabOff, size_t strTabSize, uint32_t nameOff) {
    if (!checkedRange(strTabOff, strTabSize, size) || nameOff >= strTabSize)
        return std::nullopt;
    size_t pos = strTabOff + nameOff;
    if (pos < strTabOff || pos >= strTabOff + strTabSize)
        return std::nullopt;
    const uint8_t *begin = data + pos;
    const uint8_t *end = data + strTabOff + strTabSize;
    const void *nul = std::memchr(begin, '\0', static_cast<size_t>(end - begin));
    if (!nul)
        return std::nullopt;
    return std::string(reinterpret_cast<const char *>(begin), static_cast<const char *>(nul));
}

/// @brief Parses an ELF64 relocatable object into the neutral linker model.
/// @details Validates header/table ranges and resource limits, materializes
///          supported sections, translates every referenced symbol table, and
///          attaches decoded RELA/REL relocations to their target sections.
///          The destination may contain partial results after failure.
/// @param data Pointer to complete object-file bytes.
/// @param size Size of @p data.
/// @param name Display name used in diagnostics and stored in @p obj.
/// @param obj Destination object representation.
/// @param err Stream that receives the first hard parse diagnostic.
/// @return `true` when the object is supported and fully parsed.
bool readElfObj(
    const uint8_t *data, size_t size, const std::string &name, ObjFile &obj, std::ostream &err) {
    const auto ehdrValue = readStruct<elf::Elf64_Ehdr>(data, size, 0);
    if (!ehdrValue) {
        err << "error: " << name << ": truncated ELF header\n";
        return false;
    }
    const auto *ehdr = &*ehdrValue;

    obj.format = ObjFileFormat::ELF;
    obj.is64bit = true;
    obj.isLittleEndian = (ehdr->e_ident[5] == 1);
    obj.machine = ehdr->e_machine;
    obj.name = name;
    obj.symbols.assign(1, ObjSymbol{});

    if (ehdr->e_ident[4] != 2 || ehdr->e_ident[5] != 1 || ehdr->e_ident[6] != 1 ||
        ehdr->e_type != 1 || ehdr->e_shentsize != sizeof(elf::Elf64_Shdr)) {
        err << "error: " << name << ": unsupported ELF object format\n";
        return false;
    }
    if (ehdr->e_machine != elf::EM_X86_64 && ehdr->e_machine != elf::EM_AARCH64) {
        err << "error: " << name << ": unsupported ELF machine\n";
        return false;
    }

    size_t sectionHeaderOff = 0;
    if (!checkedU64ToSize(ehdr->e_shoff, sectionHeaderOff)) {
        err << "error: " << name << ": ELF section header offset exceeds addressable size\n";
        return false;
    }
    const auto sh0Value = readStruct<elf::Elf64_Shdr>(data, size, sectionHeaderOff);
    if (!sh0Value) {
        err << "error: " << name << ": missing ELF section header 0\n";
        return false;
    }
    const auto *sh0 = &*sh0Value;

    size_t shnum = ehdr->e_shnum;
    if (shnum == 0 && !checkedU64ToSize(sh0->sh_size, shnum)) {
        err << "error: " << name << ": extended ELF section count exceeds addressable size\n";
        return false;
    }
    size_t shstrndx = ehdr->e_shstrndx;
    if (shstrndx == elf::SHN_XINDEX)
        shstrndx = sh0->sh_link;
    if (shnum == 0) {
        obj.sections.assign(1, ObjSection{});
        return true;
    }
    if (shnum > kMaxObjSections) {
        err << "error: " << name << ": section count " << shnum << " exceeds limit\n";
        return false;
    }

    // Read section headers.
    std::vector<elf::Elf64_Shdr> shdrs(shnum);
    size_t sectionHeaderBytes = 0;
    if (!checkedMulSize(shnum, ehdr->e_shentsize, sectionHeaderBytes) ||
        !checkedRange(sectionHeaderOff, sectionHeaderBytes, size)) {
        err << "error: " << name << ": section header table is out of bounds\n";
        return false;
    }
    for (size_t i = 0; i < shnum; ++i) {
        size_t entryOff = 0;
        if (!checkedMulSize(i, ehdr->e_shentsize, entryOff) ||
            entryOff > std::numeric_limits<size_t>::max() - sectionHeaderOff) {
            err << "error: " << name << ": section header offset overflows addressable size\n";
            return false;
        }
        auto shdrValue = readStruct<elf::Elf64_Shdr>(data, size, sectionHeaderOff + entryOff);
        if (!shdrValue) {
            err << "error: " << name << ": truncated section header " << i << "\n";
            return false;
        }
        shdrs[i] = *shdrValue;
    }

    // Locate .shstrtab for section names.
    size_t shstrOff = 0, shstrSize = 0;
    if (shstrndx < shnum) {
        if (!checkedU64ToSize(shdrs[shstrndx].sh_offset, shstrOff) ||
            !checkedU64ToSize(shdrs[shstrndx].sh_size, shstrSize)) {
            err << "error: " << name
                << ": ELF section-name string table exceeds addressable size\n";
            return false;
        }
    }

    /// @brief Temporarily records an ELF COMDAT signature and member indices.
    struct ElfComdatGroup {
        std::string signature;
        std::vector<uint32_t> members;
    };

    /// Reads one signature name through a specified symbol table and its string table.
    auto readSymbolNameFromTable = [&](uint32_t symtabIndex,
                                       uint32_t symIndex) -> std::optional<std::string> {
        if (symtabIndex >= shnum)
            return std::nullopt;
        const auto &symtab = shdrs[symtabIndex];
        if (symtab.sh_type != elf::SHT_SYMTAB || symtab.sh_link >= shnum ||
            shdrs[symtab.sh_link].sh_type != elf::SHT_STRTAB ||
            (symtab.sh_entsize != 0 && symtab.sh_entsize != sizeof(elf::Elf64_Sym)) ||
            (symtab.sh_size % sizeof(elf::Elf64_Sym)) != 0)
            return std::nullopt;
        const uint64_t symCount = symtab.sh_size / sizeof(elf::Elf64_Sym);
        size_t symtabOff = 0, symtabSize = 0;
        if (symIndex >= symCount || !checkedU64ToSize(symtab.sh_offset, symtabOff) ||
            !checkedU64ToSize(symtab.sh_size, symtabSize) ||
            !checkedRange(symtabOff, symtabSize, size))
            return std::nullopt;
        size_t symOff = 0;
        if (!checkedMulSize(symIndex, sizeof(elf::Elf64_Sym), symOff) ||
            symOff > std::numeric_limits<size_t>::max() - symtabOff)
            return std::nullopt;
        const auto sym = readStruct<elf::Elf64_Sym>(data, size, symtabOff + symOff);
        if (!sym)
            return std::nullopt;
        const auto &strtab = shdrs[symtab.sh_link];
        size_t strtabOff = 0, strtabSize = 0;
        if (!checkedU64ToSize(strtab.sh_offset, strtabOff) ||
            !checkedU64ToSize(strtab.sh_size, strtabSize))
            return std::nullopt;
        return readStringOpt(data, size, strtabOff, strtabSize, sym->st_name);
    };

    std::vector<ElfComdatGroup> comdatGroups;
    for (size_t i = 1; i < shnum; ++i) {
        const auto *sh = &shdrs[i];
        if (sh->sh_type != elf::SHT_GROUP)
            continue;
        size_t groupOff = 0, groupSize = 0;
        if (sh->sh_size < 4 || (sh->sh_size % 4) != 0 ||
            !checkedU64ToSize(sh->sh_offset, groupOff) ||
            !checkedU64ToSize(sh->sh_size, groupSize) || !checkedRange(groupOff, groupSize, size)) {
            err << "error: " << name << ": malformed ELF section group\n";
            return false;
        }
        const uint32_t flags = readLE32(data + groupOff);
        if ((flags & elf::GRP_COMDAT) == 0)
            continue;
        auto signature = readSymbolNameFromTable(sh->sh_link, sh->sh_info);
        if (!signature || signature->empty()) {
            err << "error: " << name << ": ELF COMDAT group has invalid signature symbol\n";
            return false;
        }
        std::vector<uint32_t> members;
        const size_t count = groupSize / 4;
        for (size_t entry = 1; entry < count; ++entry) {
            const uint32_t member = readLE32(data + groupOff + entry * 4);
            if (member >= shnum) {
                err << "error: " << name << ": ELF section group references invalid section "
                    << member << "\n";
                return false;
            }
            members.push_back(member);
        }
        if (!members.empty())
            comdatGroups.push_back(ElfComdatGroup{std::move(*signature), std::move(members)});
    }

    // Build sections (index 0 = null).
    // Map from ELF section index → ObjFile section index.
    std::vector<uint32_t> secMap(shnum, 0);
    obj.sections.resize(1); // Null section at index 0.
    obj.sections[0].name = "";
    size_t materializedBytes = 0;

    for (size_t i = 1; i < shnum; ++i) {
        const auto *sh = &shdrs[i];
        if (sh->sh_type == elf::SHT_SYMTAB || sh->sh_type == elf::SHT_STRTAB ||
            sh->sh_type == elf::SHT_RELA || sh->sh_type == elf::SHT_REL ||
            sh->sh_type == elf::SHT_GROUP || sh->sh_type == elf::SHT_SYMTAB_SHNDX)
            continue;

        ObjSection sec;
        auto secName = readStringOpt(data, size, shstrOff, shstrSize, sh->sh_name);
        if (!secName && sh->sh_name != 0) {
            err << "error: " << name << ": ELF section name offset " << sh->sh_name
                << " is invalid\n";
            return false;
        }
        sec.name = secName.value_or("");
        if (!isPowerOfTwoOrZero(sh->sh_addralign) ||
            sh->sh_addralign > std::numeric_limits<uint32_t>::max()) {
            err << "error: " << name << ": ELF section '" << sec.name
                << "' has unsupported alignment " << sh->sh_addralign << "\n";
            return false;
        }
        sec.alignment = static_cast<uint32_t>(sh->sh_addralign);
        sec.executable = (sh->sh_flags & elf::SHF_EXECINSTR) != 0;
        sec.writable = (sh->sh_flags & elf::SHF_WRITE) != 0;
        sec.alloc = (sh->sh_flags & elf::SHF_ALLOC) != 0;
        sec.tls = (sh->sh_flags & elf::SHF_TLS) != 0;
        // SHF_GNU_RETAIN marks a section the linker must keep even under
        // --gc-sections (e.g. __attribute__((retain))).
        constexpr uint64_t SHF_GNU_RETAIN = 0x200000;
        sec.noDeadStrip = (sh->sh_flags & SHF_GNU_RETAIN) != 0;

        // ELF sections with SHF_STRINGS + SHF_MERGE contain NUL-terminated
        // string literals suitable for cross-module deduplication.
        // Also recognize by name: .rodata.str* sections are GCC/Clang-generated
        // mergeable string sections even when flag inspection is impractical.
        constexpr uint32_t SHF_MERGE = 0x10;
        constexpr uint32_t SHF_STRINGS = 0x20;
        sec.isCStringSection =
            sec.alloc && (((sh->sh_flags & SHF_MERGE) != 0 && (sh->sh_flags & SHF_STRINGS) != 0) ||
                          sec.name.rfind(".rodata.str", 0) == 0);

        if (sh->sh_type == elf::SHT_NOBITS) {
            sec.zeroFill = true;
            size_t noBitsSize = 0;
            if (!checkedU64ToSize(sh->sh_size, noBitsSize) || noBitsSize > kMaxObjSectionBytes) {
                err << "error: " << name << ": ELF section '" << sec.name << "' is too large\n";
                return false;
            }
            sec.memSize = noBitsSize;
        } else {
            size_t off = 0, sz = 0;
            if (!checkedU64ToSize(sh->sh_offset, off) || !checkedU64ToSize(sh->sh_size, sz)) {
                err << "error: " << name << ": ELF section '" << sec.name
                    << "' contents exceed addressable size\n";
                return false;
            }
            if (sz > 0 && !checkedRange(off, sz, size)) {
                err << "error: " << name << ": ELF section '" << sec.name
                    << "' contents are out of bounds\n";
                return false;
            }
            if (sz == 0) {
                sec.memSize = 0;
            } else {
                if (sz > kMaxObjSectionBytes) {
                    err << "error: " << name << ": ELF section '" << sec.name << "' is too large\n";
                    return false;
                }
                if (sz > kMaxObjMaterializedBytes - materializedBytes) {
                    err << "error: " << name << ": ELF materialized section data exceeds limit\n";
                    return false;
                }
                materializedBytes += sz;
                sec.data.assign(data + off, data + off + sz);
                sec.memSize = sec.data.size();
            }
        }

        secMap[i] = static_cast<uint32_t>(obj.sections.size());
        obj.sections.push_back(std::move(sec));
    }

    for (const auto &group : comdatGroups) {
        uint32_t leader = 0;
        for (uint32_t elfSec : group.members) {
            if (elfSec < secMap.size() && secMap[elfSec] != 0) {
                leader = secMap[elfSec];
                break;
            }
        }
        if (leader == 0)
            continue;
        for (uint32_t elfSec : group.members) {
            if (elfSec >= secMap.size() || secMap[elfSec] == 0)
                continue;
            auto &member = obj.sections[secMap[elfSec]];
            member.comdatSelection = ComdatSelection::Any;
            member.comdatKey = group.signature;
            if (secMap[elfSec] != leader)
                member.associativeSection = leader;
        }
    }

    // Read all symbol tables. Relocation sections use sh_link to select the
    // symbol table they reference, so keeping only the first SHT_SYMTAB can
    // misresolve otherwise valid objects.
    std::vector<std::vector<uint32_t>> symMapsBySection(shnum);
    /// Parses one symbol table and records its raw-to-neutral index mapping.
    auto parseSymtab = [&](size_t symShIndex) -> bool {
        const auto *symSh = &shdrs[symShIndex];
        if (symSh->sh_link >= shnum || shdrs[symSh->sh_link].sh_type != elf::SHT_STRTAB) {
            err << "error: " << name << ": ELF symbol table has invalid string table link\n";
            return false;
        }
        size_t symtabOff = 0, symtabSize = 0;
        if (!checkedU64ToSize(symSh->sh_offset, symtabOff) ||
            !checkedU64ToSize(symSh->sh_size, symtabSize) ||
            !checkedRange(symtabOff, symtabSize, size)) {
            err << "error: " << name << ": symbol table is out of bounds\n";
            return false;
        }
        if (symSh->sh_entsize != 0 && symSh->sh_entsize != sizeof(elf::Elf64_Sym)) {
            err << "error: " << name << ": unsupported ELF symbol entry size\n";
            return false;
        }
        if ((symSh->sh_size % sizeof(elf::Elf64_Sym)) != 0) {
            err << "error: " << name << ": malformed ELF symbol table size\n";
            return false;
        }
        const uint64_t rawSymCount = symSh->sh_size / sizeof(elf::Elf64_Sym);
        if (rawSymCount > std::numeric_limits<uint32_t>::max()) {
            err << "error: " << name << ": symbol count exceeds 32-bit reader limit\n";
            return false;
        }
        const uint32_t symCount = static_cast<uint32_t>(rawSymCount);
        if (symCount > kMaxObjSymbols) {
            err << "error: " << name << ": symbol count " << symCount << " exceeds limit\n";
            return false;
        }

        auto &symMap = symMapsBySection[symShIndex];
        symMap.assign(symCount, 0);
        const auto &strSh = shdrs[symSh->sh_link];
        size_t strOff = 0, strSize = 0;
        if (!checkedU64ToSize(strSh.sh_offset, strOff) ||
            !checkedU64ToSize(strSh.sh_size, strSize)) {
            err << "error: " << name << ": ELF symbol string table exceeds addressable size\n";
            return false;
        }
        if (!checkedRange(strOff, strSize, size)) {
            err << "error: " << name << ": ELF symbol string table is out of bounds\n";
            return false;
        }

        std::vector<uint32_t> extendedSectionIndexes;
        for (size_t si = 1; si < shnum; ++si) {
            const auto &candidate = shdrs[si];
            if (candidate.sh_type != elf::SHT_SYMTAB_SHNDX || candidate.sh_link != symShIndex)
                continue;
            if (candidate.sh_entsize != 0 && candidate.sh_entsize != sizeof(uint32_t)) {
                err << "error: " << name << ": unsupported ELF symbol section-index entry size\n";
                return false;
            }
            size_t minShndxBytes = 0, shndxOff = 0, shndxSize = 0;
            if (!checkedMulSize(symCount, sizeof(uint32_t), minShndxBytes) ||
                !checkedU64ToSize(candidate.sh_offset, shndxOff) ||
                !checkedU64ToSize(candidate.sh_size, shndxSize) || shndxSize < minShndxBytes ||
                !checkedRange(shndxOff, shndxSize, size)) {
                err << "error: " << name << ": ELF symbol section-index table is malformed\n";
                return false;
            }
            extendedSectionIndexes.resize(symCount, 0);
            for (uint32_t idx = 0; idx < symCount; ++idx) {
                extendedSectionIndexes[idx] =
                    readLE32(data + shndxOff + static_cast<size_t>(idx) * sizeof(uint32_t));
            }
            break;
        }

        for (uint32_t i = 1; i < symCount; ++i) {
            const auto symValue = readStruct<elf::Elf64_Sym>(
                data, size, symtabOff + static_cast<size_t>(i) * sizeof(elf::Elf64_Sym));
            if (!symValue)
                break;
            const auto *sym = &*symValue;

            ObjSymbol os;
            auto symName = readStringOpt(data, size, strOff, strSize, sym->st_name);
            if (!symName && sym->st_name != 0) {
                err << "error: " << name << ": ELF symbol name offset " << sym->st_name
                    << " is invalid\n";
                return false;
            }
            os.name = symName.value_or("");
            uint32_t effectiveShndx = sym->st_shndx;
            if (sym->st_shndx == elf::SHN_XINDEX) {
                if (i >= extendedSectionIndexes.size() || extendedSectionIndexes[i] == 0) {
                    err << "error: " << name
                        << ": ELF symbol uses SHN_XINDEX without section-index table\n";
                    return false;
                }
                effectiveShndx = extendedSectionIndexes[i];
            }

            const uint8_t bind = sym->st_info >> 4;
            if (effectiveShndx == elf::SHN_UNDEF) {
                os.binding = ObjSymbol::Undefined;
                os.weakExternal = (bind == elf::STB_WEAK);
            } else if (bind == elf::STB_LOCAL)
                os.binding = ObjSymbol::Local;
            else if (bind == elf::STB_WEAK)
                os.binding = ObjSymbol::Weak;
            else
                os.binding = ObjSymbol::Global;

            if (effectiveShndx == elf::SHN_ABS) {
                os.absolute = true;
            } else if (effectiveShndx == elf::SHN_COMMON) {
                size_t alignment = 0;
                if (!checkedU64ToSize(sym->st_value, alignment)) {
                    err << "error: " << name
                        << ": ELF common symbol alignment exceeds addressable size\n";
                    return false;
                }
                if (alignment != 0 && ((alignment & (alignment - 1)) != 0 ||
                                       alignment > std::numeric_limits<uint32_t>::max())) {
                    err << "error: " << name << ": ELF common symbol has unsupported alignment\n";
                    return false;
                }
                os.common = true;
                os.commonAlignment = alignment == 0 ? 1 : alignment;
                os.sectionIndex = 0;
            } else if (effectiveShndx < shnum && effectiveShndx != elf::SHN_UNDEF) {
                os.sectionIndex = secMap[effectiveShndx];
                if (os.sectionIndex == 0) {
                    err << "error: " << name << ": ELF symbol '" << os.name
                        << "' references unsupported section index " << effectiveShndx << "\n";
                    return false;
                }
            } else if (effectiveShndx != elf::SHN_UNDEF) {
                err << "error: " << name << ": ELF symbol '" << os.name
                    << "' uses unsupported section index " << effectiveShndx << "\n";
                return false;
            }

            if (sym->st_value > static_cast<uint64_t>(SIZE_MAX) ||
                sym->st_size > static_cast<uint64_t>(SIZE_MAX)) {
                err << "error: " << name << ": ELF symbol '" << os.name
                    << "' offset/size exceeds addressable size\n";
                return false;
            }
            os.offset = effectiveShndx == elf::SHN_COMMON ? 0 : static_cast<size_t>(sym->st_value);
            os.size = static_cast<size_t>(sym->st_size);
            if (effectiveShndx != elf::SHN_COMMON && os.sectionIndex > 0 &&
                os.sectionIndex < obj.sections.size()) {
                const size_t secSize = objSectionMemSize(obj.sections[os.sectionIndex]);
                if (os.offset > secSize || os.size > secSize - os.offset) {
                    err << "error: " << name << ": ELF symbol '" << os.name
                        << "' extends beyond section '" << obj.sections[os.sectionIndex].name
                        << "'\n";
                    return false;
                }
            }

            if (obj.symbols.size() >= kMaxObjSymbols) {
                err << "error: " << name << ": combined ELF symbol count exceeds limit\n";
                return false;
            }
            symMap[i] = static_cast<uint32_t>(obj.symbols.size());
            obj.symbols.push_back(std::move(os));
        }
        return true;
    };

    for (size_t i = 1; i < shnum; ++i) {
        if (shdrs[i].sh_type == elf::SHT_SYMTAB && !parseSymtab(i))
            return false;
    }

    // Read relocations from .rela/.rel sections.
    for (size_t i = 1; i < shnum; ++i) {
        if (shdrs[i].sh_type != elf::SHT_RELA && shdrs[i].sh_type != elf::SHT_REL)
            continue;

        // sh_info points to the section these relocs apply to.
        const uint32_t targetSecElf = shdrs[i].sh_info;
        if (targetSecElf >= shnum) {
            err << "error: " << name
                << ": ELF relocation section references invalid target section " << targetSecElf
                << "\n";
            return false;
        }
        if (secMap[targetSecElf] == 0) {
            err << "error: " << name << ": ELF relocation section targets unsupported section "
                << targetSecElf << "\n";
            return false;
        }

        auto &targetSec = obj.sections[secMap[targetSecElf]];
        size_t relocTableOff = 0, relocTableSize = 0;
        if (!checkedU64ToSize(shdrs[i].sh_offset, relocTableOff) ||
            !checkedU64ToSize(shdrs[i].sh_size, relocTableSize) ||
            !checkedRange(relocTableOff, relocTableSize, size)) {
            err << "error: " << name << ": relocation table is out of bounds\n";
            return false;
        }
        const bool isRela = shdrs[i].sh_type == elf::SHT_RELA;
        const size_t relEntSize = isRela ? sizeof(elf::Elf64_Rela) : sizeof(elf::Elf64_Rel);
        if (shdrs[i].sh_entsize != 0 && shdrs[i].sh_entsize != relEntSize) {
            err << "error: " << name << ": unsupported ELF relocation entry size\n";
            return false;
        }
        if ((shdrs[i].sh_size % relEntSize) != 0) {
            err << "error: " << name << ": malformed ELF relocation table size\n";
            return false;
        }
        const uint64_t rawRelCount = shdrs[i].sh_size / relEntSize;
        if (rawRelCount > std::numeric_limits<uint32_t>::max()) {
            err << "error: " << name << ": relocation count exceeds 32-bit reader limit\n";
            return false;
        }
        const uint32_t relCount = static_cast<uint32_t>(rawRelCount);

        for (uint32_t r = 0; r < relCount; ++r) {
            ObjReloc rel;
            uint64_t rInfo = 0;
            size_t relEntryOff = 0;
            if (!checkedMulSize(static_cast<size_t>(r), relEntSize, relEntryOff) ||
                relEntryOff > std::numeric_limits<size_t>::max() - relocTableOff) {
                err << "error: " << name << ": ELF relocation entry offset overflows\n";
                return false;
            }
            if (isRela) {
                const auto relaValue =
                    readStruct<elf::Elf64_Rela>(data, size, relocTableOff + relEntryOff);
                if (!relaValue)
                    break;
                const auto *rela = &*relaValue;
                if (!checkedU64ToSize(rela->r_offset, rel.offset)) {
                    err << "error: " << name
                        << ": ELF relocation offset exceeds addressable size\n";
                    return false;
                }
                rInfo = rela->r_info;
                rel.addend = rela->r_addend;
            } else {
                const auto relNoAddendValue =
                    readStruct<elf::Elf64_Rel>(data, size, relocTableOff + relEntryOff);
                if (!relNoAddendValue)
                    break;
                const auto *relNoAddend = &*relNoAddendValue;
                if (!checkedU64ToSize(relNoAddend->r_offset, rel.offset)) {
                    err << "error: " << name
                        << ": ELF relocation offset exceeds addressable size\n";
                    return false;
                }
                rInfo = relNoAddend->r_info;
            }

            rel.type = static_cast<uint32_t>(rInfo & 0xFFFFFFFF);
            const uint32_t elfSymIdx = static_cast<uint32_t>(rInfo >> 32);
            if (shdrs[i].sh_link >= shnum || shdrs[shdrs[i].sh_link].sh_type != elf::SHT_SYMTAB ||
                symMapsBySection[shdrs[i].sh_link].empty()) {
                err << "error: " << name
                    << ": ELF relocation section has invalid symbol table link\n";
                return false;
            }
            const auto &symMap = symMapsBySection[shdrs[i].sh_link];
            if (elfSymIdx >= symMap.size()) {
                err << "error: " << name << ": relocation references invalid symbol index "
                    << elfSymIdx << "\n";
                return false;
            }
            rel.symIndex = symMap[elfSymIdx];
            if (!isRela && !extractRelAddend(
                               ehdr->e_machine, rel.type, targetSec.data, rel.offset, rel.addend)) {
                err << "error: " << name << ": ELF REL relocation addend at offset " << rel.offset
                    << " is out of bounds in section '" << targetSec.name << "'\n";
                return false;
            }

            targetSec.relocs.push_back(rel);
        }
    }

    return true;
}

} // namespace zanna::codegen::linker
