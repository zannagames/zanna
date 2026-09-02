//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/RelocApplier.cpp
// Purpose: Applies relocations to merged output section data.
// Key invariants:
//   - Dispatches by object file format (ELF/Mach-O/COFF) since reloc type
//     numbers collide across formats
//   - All addresses resolved before patching
//   - Range-checked for branch instructions
// Links: codegen/common/linker/RelocApplier.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file RelocApplier.cpp
 * @brief Implements checked, format-aware relocation patching for native links.
 *
 * The relocation applier resolves symbol virtual addresses against the merged
 * output layout, validates each patch site and target encoding, and writes the
 * resulting ELF, Mach-O, or COFF fixup into its output section. It also handles
 * architecture-specific GOT relaxation, AArch64 page and TLS relocations,
 * Mach-O bind/rebase bookkeeping, and Windows unwind-table ordering.
 *
 * All address arithmetic that must be representable is checked before any bytes
 * are modified. Absolute 64-bit relocations are the intentional exception:
 * their object-format semantics define the result as the low 64 bits of
 * symbol-plus-addend.
 */

#include "codegen/common/linker/RelocApplier.hpp"
#include "codegen/common/AArch64RelocUtil.hpp"
#include "codegen/common/linker/NameMangling.hpp"
#include "codegen/common/linker/RelocClassify.hpp"
#include "codegen/common/linker/RelocConstants.hpp"
#include "codegen/common/objfile/ObjFileWriterUtil.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>

namespace zanna::codegen::linker {

using zanna::codegen::objfile::readLE32;
using zanna::codegen::objfile::writeLE16;
using zanna::codegen::objfile::writeLE32;
using zanna::codegen::objfile::writeLE64;

/// @brief Write a signed 32-bit PC-relative relocation after checking its range.
/// @param patch Destination of the four-byte little-endian displacement.
/// @param value Signed displacement to encode.
/// @param obj Object file used to qualify any diagnostic.
/// @param symName Referenced symbol name, or empty for an anonymous target.
/// @param kind Human-readable relocation kind used in diagnostics.
/// @param err Stream that receives an out-of-range diagnostic.
/// @return `true` after writing the displacement; `false` when @p value cannot
///         be represented by a signed 32-bit field.
static bool writeCheckedRel32(uint8_t *patch,
                              int64_t value,
                              const ObjFile &obj,
                              const std::string &symName,
                              const char *kind,
                              std::ostream &err) {
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        err << "error: " << obj.name << ": " << kind << " relocation out of range";
        if (!symName.empty())
            err << " for '" << symName << "'";
        err << "\n";
        return false;
    }
    writeLE32(patch, static_cast<uint32_t>(static_cast<int32_t>(value)));
    return true;
}

/// @brief Require an AArch64 branch displacement to be instruction aligned.
/// @param disp Byte displacement from the branch instruction to its target.
/// @param obj Object file used to qualify any diagnostic.
/// @param symName Referenced symbol name, if available.
/// @param kind Human-readable branch relocation kind.
/// @param err Stream that receives an alignment diagnostic.
/// @return `true` when @p disp is a multiple of four; otherwise `false`.
static bool checkAArch64BranchAlignment(int64_t disp,
                                        const ObjFile &obj,
                                        const std::string &symName,
                                        const char *kind,
                                        std::ostream &err) {
    if ((disp & 0x3) == 0)
        return true;
    err << "error: " << obj.name << ": " << kind << " target is not instruction aligned";
    if (!symName.empty())
        err << " for '" << symName << "'";
    err << "\n";
    return false;
}

/// @brief Check the natural alignment required by a scaled AArch64 page offset.
/// @param pageOff Unscaled offset within a 4 KiB page.
/// @param shift Base-two scale exponent encoded by the load/store instruction.
/// @param obj Object file used to qualify any diagnostic.
/// @param symName Referenced symbol name.
/// @param err Stream that receives an alignment diagnostic.
/// @return `true` when @p pageOff is aligned to `1 << shift` bytes.
static bool checkPageOffsetAlignment(uint32_t pageOff,
                                     uint32_t shift,
                                     const ObjFile &obj,
                                     const std::string &symName,
                                     std::ostream &err) {
    const uint32_t scale = 1u << shift;
    if ((pageOff & (scale - 1u)) == 0)
        return true;
    err << "error: " << obj.name << ": AArch64 page offset for '" << symName
        << "' is not aligned to " << scale << " bytes\n";
    return false;
}

/// @brief Decode the scale of an AArch64 unsigned-offset load/store instruction.
/// @param insn Instruction word to inspect.
/// @param shift Receives the base-two byte-scale exponent on success.
/// @return `true` when @p insn uses the supported unsigned-offset encoding.
static bool aarch64UnsignedLdStOffsetShift(uint32_t insn, uint32_t &shift) {
    return zanna::codegen::a64UnsignedLdStOffsetShift(insn, shift);
}

/// @brief Determine whether an instruction is an AArch64 unsigned-offset load/store.
/// @param insn Instruction word to inspect.
/// @return `true` for a supported unsigned-offset load/store encoding.
[[maybe_unused]] static bool isAArch64UnsignedLdStOffset(uint32_t insn) {
    uint32_t ignored = 0;
    return aarch64UnsignedLdStOffsetShift(insn, ignored);
}

/// @brief Determine whether an instruction is an AArch64 ADD-immediate operation.
/// @param insn Instruction word to inspect.
/// @return `true` when the instruction can accept a 12-bit ADD relocation.
static bool isAArch64AddImmediate(uint32_t insn) {
    return zanna::codegen::isA64AddImmediate(insn);
}

/// @brief Patch the immediate field of an AArch64 ADD-immediate instruction.
/// @param patch Address of the four-byte instruction word to update.
/// @param imm12 Unshifted immediate value to place in instruction bits 21:10.
/// @param obj Object file used to qualify any diagnostic.
/// @param symName Referenced symbol name, if available.
/// @param kind Human-readable relocation kind.
/// @param err Stream that receives opcode or range diagnostics.
/// @return `true` after patching; `false` for an incompatible opcode or an
///         immediate larger than 12 bits.
static bool writeAArch64AddImmediate12(uint8_t *patch,
                                       uint32_t imm12,
                                       const ObjFile &obj,
                                       const std::string &symName,
                                       const char *kind,
                                       std::ostream &err) {
    uint32_t insn = readLE32(patch);
    if (!isAArch64AddImmediate(insn)) {
        err << "error: " << obj.name << ": " << kind << " relocation is not applied to ADD";
        if (!symName.empty())
            err << " for '" << symName << "'";
        err << "\n";
        return false;
    }
    if (imm12 > 0xFFFu) {
        err << "error: " << obj.name << ": " << kind << " immediate out of range";
        if (!symName.empty())
            err << " for '" << symName << "'";
        err << "\n";
        return false;
    }
    insn = (insn & 0xFFC003FFu) | (imm12 << 10);
    writeLE32(patch, insn);
    return true;
}

/// @brief Recognize an AArch64 64-bit LDR with an unsigned immediate offset.
/// @param insn Instruction word to inspect.
/// @return `true` when @p insn has the `LDR X, [X, #imm]` encoding.
static bool isAArch64LdrXUnsignedOffset(uint32_t insn) {
    return (insn & 0xFFC00000u) == 0xF9400000u;
}

/// @brief Determine whether a byte belongs to the x86-64 REX prefix range.
/// @param byte Candidate instruction byte.
/// @return `true` for values from `0x40` through `0x4f`.
static bool isX64RexPrefix(uint8_t byte) {
    return (byte & 0xF0u) == 0x40u;
}

/// @brief Validate the instruction shape required for local GOTPCRELX relaxation.
/// @details The relaxed instruction must be a RIP-relative MOV. The
///          `R_X86_64_REX_GOTPCRELX` form additionally requires the preceding
///          byte to be a REX prefix.
/// @param obj Object file used to qualify any diagnostic.
/// @param rel Relocation whose exact GOTPCRELX form is being validated.
/// @param patch Address of the four-byte displacement field.
/// @param patchOff Offset of @p patch within the merged output section.
/// @param symName Referenced symbol name, if available.
/// @param err Stream that receives malformed-instruction diagnostics.
/// @return `true` when the surrounding instruction can be safely relaxed.
static bool validateGotPCRelXMov(const ObjFile &obj,
                                 const ObjReloc &rel,
                                 const uint8_t *patch,
                                 size_t patchOff,
                                 const std::string &symName,
                                 std::ostream &err) {
    if (patchOff < 2 || patch[-2] != 0x8B || (patch[-1] & 0xC7u) != 0x05u) {
        err << "error: " << obj.name
            << ": local GOTPCRELX relaxation requires MOV r*, disp32(%rip)";
        if (!symName.empty())
            err << " for '" << symName << "'";
        err << "\n";
        return false;
    }
    if (rel.type == elf_x64::kRexGotPcRelX && (patchOff < 3 || !isX64RexPrefix(patch[-3]))) {
        err << "error: " << obj.name << ": REX_GOTPCRELX relaxation requires a REX-prefixed MOV";
        if (!symName.empty())
            err << " for '" << symName << "'";
        err << "\n";
        return false;
    }
    return true;
}

/// @brief Recognize AArch64 unconditional immediate branch opcodes.
/// @param insn Instruction word to inspect.
/// @return `true` for either `B` or `BL`.
static bool isAArch64Branch26Opcode(uint32_t insn) {
    return (insn & 0x7C000000u) == 0x14000000u; // B or BL.
}

/// @brief Recognize an AArch64 ADRP instruction.
/// @param insn Instruction word to inspect.
/// @return `true` when @p insn uses the ADRP encoding.
static bool isAArch64AdrpOpcode(uint32_t insn) {
    return (insn & 0x9F000000u) == 0x90000000u;
}

/// @brief Recognize AArch64 instructions carrying a 19-bit branch displacement.
/// @param insn Instruction word to inspect.
/// @return `true` for `B.cond`, `CBZ`, or `CBNZ`.
static bool isAArch64CondBr19Opcode(uint32_t insn) {
    return ((insn & 0xFF000010u) == 0x54000000u) || // B.cond.
           ((insn & 0x7E000000u) == 0x34000000u);   // CBZ/CBNZ.
}

/// @brief Add two unsigned 64-bit values without wrapping.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param out Receives the sum on success and is unchanged on overflow.
/// @return `true` when the mathematical sum fits in `uint64_t`.
static bool checkedAddU64(uint64_t lhs, uint64_t rhs, uint64_t &out) {
    if (lhs > std::numeric_limits<uint64_t>::max() - rhs)
        return false;
    out = lhs + rhs;
    return true;
}

/// @brief Compute the unsigned magnitude of a signed 64-bit value.
/// @details The formulation is valid for `INT64_MIN`, whose positive magnitude
///          cannot be represented by `int64_t`.
/// @param value Signed value to measure.
/// @return The magnitude of @p value as an unsigned integer.
static uint64_t int64Magnitude(int64_t value) {
    if (value >= 0)
        return static_cast<uint64_t>(value);
    return static_cast<uint64_t>(-(value + 1)) + 1;
}

/// @brief Add a signed relocation addend to an unsigned address without wrapping.
/// @param base Unsigned base address.
/// @param addend Signed addend to apply.
/// @param out Receives the mathematical result on success.
/// @return `true` when the result lies in the `uint64_t` range.
static bool checkedAddSignedU64(uint64_t base, int64_t addend, uint64_t &out) {
    if (addend >= 0)
        return checkedAddU64(base, static_cast<uint64_t>(addend), out);

    const uint64_t mag = int64Magnitude(addend);
    if (base < mag)
        return false;
    out = base - mag;
    return true;
}

/// @brief Compute a signed difference between two unsigned addresses.
/// @param lhs Address from which @p rhs is subtracted.
/// @param rhs Address to subtract.
/// @param out Receives `lhs - rhs` on success.
/// @return `true` when the difference is representable by `int64_t`.
static bool checkedAddressDelta(uint64_t lhs, uint64_t rhs, int64_t &out) {
    if (lhs >= rhs) {
        const uint64_t delta = lhs - rhs;
        if (delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return false;
        out = static_cast<int64_t>(delta);
        return true;
    }

    const uint64_t delta = rhs - lhs;
    const uint64_t minMag = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
    if (delta > minMag)
        return false;
    out = (delta == minMag) ? std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(delta);
    return true;
}

/// @brief Compute the checked relocation target address `S + A`.
/// @param S Resolved symbol address.
/// @param A Signed relocation addend.
/// @param obj Object file used to qualify any diagnostic.
/// @param symName Referenced symbol name, if available.
/// @param kind Human-readable relocation kind.
/// @param err Stream that receives an address-overflow diagnostic.
/// @param target Receives the target address on success.
/// @return `true` when `S + A` is representable by `uint64_t`.
static bool checkedRelocTarget(uint64_t S,
                               int64_t A,
                               const ObjFile &obj,
                               const std::string &symName,
                               const char *kind,
                               std::ostream &err,
                               uint64_t &target) {
    if (checkedAddSignedU64(S, A, target))
        return true;
    err << "error: " << obj.name << ": " << kind << " relocation address overflow";
    if (!symName.empty())
        err << " for '" << symName << "'";
    err << "\n";
    return false;
}

/// @brief Compute the checked PC-relative relocation delta `target - P`.
/// @param target Fully resolved relocation target.
/// @param P Virtual address of the relocation place.
/// @param obj Object file used to qualify any diagnostic.
/// @param symName Referenced symbol name, if available.
/// @param kind Human-readable relocation kind.
/// @param err Stream that receives a range diagnostic.
/// @param delta Receives the signed displacement on success.
/// @return `true` when the displacement is representable by `int64_t`.
static bool checkedRelocDelta(uint64_t target,
                              uint64_t P,
                              const ObjFile &obj,
                              const std::string &symName,
                              const char *kind,
                              std::ostream &err,
                              int64_t &delta) {
    if (checkedAddressDelta(target, P, delta))
        return true;
    err << "error: " << obj.name << ": " << kind << " relocation delta out of range";
    if (!symName.empty())
        err << " for '" << symName << "'";
    err << "\n";
    return false;
}

/// @brief Compute a 64-bit absolute relocation value with object-format wrap semantics.
/// @details ELF, Mach-O, and COFF 64-bit absolute relocations write the low
///          64 bits of S + A. Converting the signed addend to uint64_t makes
///          high-bit addends deterministic and avoids undefined signed overflow.
/// @param S Resolved symbol address.
/// @param A Signed relocation addend.
/// @return The low 64 bits of `S + A`.
static uint64_t wrappingAbs64Target(uint64_t S, int64_t A) noexcept {
    return S + static_cast<uint64_t>(A);
}

/// @brief Validate and convert a relocation result to an unsigned 32-bit value.
/// @param value Signed intermediate value to convert.
/// @param obj Object file used to qualify any diagnostic.
/// @param symName Referenced symbol name, if available.
/// @param kind Human-readable relocation kind.
/// @param err Stream that receives a range diagnostic.
/// @param out Receives the converted value on success.
/// @return `true` when @p value lies in the inclusive `uint32_t` range.
static bool checkedU32Value(int64_t value,
                            const ObjFile &obj,
                            const std::string &symName,
                            const char *kind,
                            std::ostream &err,
                            uint32_t &out) {
    if (value >= 0 && value <= std::numeric_limits<uint32_t>::max()) {
        out = static_cast<uint32_t>(value);
        return true;
    }
    err << "error: " << obj.name << ": " << kind << " relocation out of 32-bit range";
    if (!symName.empty())
        err << " for '" << symName << "'";
    err << "\n";
    return false;
}

/// @brief Validate the patch site for one relocation and compute the place address.
/// @details Extracted from the per-relocation iteration in @ref applyRelocations so
///          the format-dispatch core can read as "compute patch site, then dispatch"
///          instead of inlining ~25 LOC of overflow + bounds + zero-fill checks per
///          relocation. Outputs the place virtual address @p P and the patch file
///          offset @p patchOff on success.
/// @param outSec Merged output section containing the input chunk.
/// @param obj Object file that owns the relocation.
/// @param rel Relocation whose patch location is required.
/// @param chunkBase Byte offset of the input section in @p outSec.
/// @param secVA Virtual address of @p outSec.
/// @param err Stream that receives overflow, zero-fill, or bounds diagnostics.
/// @param P Receives the relocation place virtual address.
/// @param patchOff Receives the byte offset of the patch in @p outSec.
/// @return `true` when the relocation begins at a valid file-backed byte.
static bool computeRelocPatchSite(const OutputSection &outSec,
                                  const ObjFile &obj,
                                  const ObjReloc &rel,
                                  size_t chunkBase,
                                  uint64_t secVA,
                                  std::ostream &err,
                                  uint64_t &P,
                                  size_t &patchOff) {
    uint64_t secChunkVA = 0;
    if (!checkedAddU64(secVA, static_cast<uint64_t>(chunkBase), secChunkVA) ||
        !checkedAddU64(secChunkVA, static_cast<uint64_t>(rel.offset), P)) {
        err << "error: " << obj.name << ": relocation place address overflow in '" << outSec.name
            << "'\n";
        return false;
    }
    if (rel.offset > std::numeric_limits<size_t>::max() - chunkBase) {
        err << "error: " << obj.name << ": relocation file offset overflow in '" << outSec.name
            << "'\n";
        return false;
    }
    patchOff = chunkBase + rel.offset;
    if (outSec.zeroFill) {
        err << "error: relocation in zero-fill output section '" << outSec.name
            << "' has no file-backed bytes to patch\n";
        return false;
    }
    if (patchOff >= outSec.data.size()) {
        err << "error: relocation at offset " << patchOff << " out of bounds in '" << outSec.name
            << "' (size=" << outSec.data.size() << ")\n";
        return false;
    }
    return true;
}

/// @brief Sort Windows unwind metadata records by function RVA.
/// @details PE/COFF exception tables must be ordered for the OS unwinder. The
///          record width differs by target architecture, so this helper validates
///          the section size against the expected record size before applying a
///          stable byte-preserving sort.
/// @param layout Link layout containing any merged `.pdata` section.
/// @param arch   Target architecture selecting the unwind record size.
/// @param err    Receives diagnostics for malformed record spans.
/// @return false when `.pdata` has a non-integral record count.
static bool sortWindowsPdata(LinkLayout &layout, LinkArch arch, std::ostream &err) {
    const size_t recordSize = (arch == LinkArch::AArch64) ? 8 : 12;

    for (auto &sec : layout.sections) {
        if (sec.name != ".pdata" || sec.data.size() < recordSize)
            continue;
        if ((sec.data.size() % recordSize) != 0) {
            err << "error: .pdata section size " << sec.data.size()
                << " is not a multiple of unwind record size " << recordSize << "\n";
            return false;
        }

        const size_t recordCount = sec.data.size() / recordSize;
        std::vector<std::vector<uint8_t>> records(recordCount, std::vector<uint8_t>(recordSize));
        for (size_t i = 0; i < recordCount; ++i)
            std::memcpy(records[i].data(), sec.data.data() + i * recordSize, recordSize);

        /// Compare unwind records by the little-endian function-start RVA in
        /// their first four bytes; stability preserves the order of equal keys.
        std::stable_sort(records.begin(), records.end(), [](const auto &a, const auto &b) {
            return readLE32(a.data()) < readLE32(b.data());
        });

        for (size_t i = 0; i < recordCount; ++i)
            std::memcpy(sec.data.data() + i * recordSize, records[i].data(), recordSize);
    }
    return true;
}

/// @brief Placement of one input section within a merged output section.
/// @details Entries form the values of the reverse location index built once by
///          @ref buildLocationMap, replacing repeated linear layout scans with
///          amortized constant-time lookups during relocation.
struct OutputLocation {
    ///< Index of the containing output section in `LinkLayout::sections`.
    size_t outSecIdx = 0;
    ///< Byte offset at which the input section's merged chunk begins.
    size_t outputOffset = 0;
    ///< Original input-section memory size used for symbol bounds checks.
    size_t inputSize = 0;
};

/// @brief Reverse index from an object/section pair to its output placement.
using LocationMap = std::unordered_map<InputSectionKey, OutputLocation, InputSectionKeyHash>;

/// @brief Virtual-address extent of the statically allocated TLS image.
/// @details Thread-local variable descriptor sections are deliberately excluded;
///          only sections that contribute bytes to the per-thread image expand
///          this half-open range.
struct TlsImageInfo {
    ///< Whether at least one qualifying TLS section was encountered.
    bool present = false;
    ///< Lowest virtual address occupied by the TLS image.
    uint64_t startVA = 0;
    ///< One-past-the-highest virtual address occupied by the TLS image.
    uint64_t endVA = 0;
};

/// @brief Build a reverse index for all non-synthetic input-section chunks.
/// @param layout Completed section layout whose chunk placements are indexed.
/// @param map Receives mappings from `(object, section)` keys to placements.
/// @param err Stream that receives a duplicate-placement diagnostic.
/// @return `true` when every input section has at most one output placement.
static bool buildLocationMap(const LinkLayout &layout, LocationMap &map, std::ostream &err) {
    for (size_t si = 0; si < layout.sections.size(); ++si) {
        for (const auto &chunk : layout.sections[si].chunks) {
            if (chunk.synthetic)
                continue;
            InputSectionKey key{chunk.inputObjIndex, chunk.inputSecIndex};
            if (map.find(key) != map.end()) {
                err << "error: input section (" << chunk.inputObjIndex << ", "
                    << chunk.inputSecIndex << ") appears in multiple output chunks\n";
                return false;
            }
            map[key] = OutputLocation{si, chunk.outputOffset, chunk.size};
        }
    }
    return true;
}

/// @brief Find the output offset of the ADRP paired with a local GOT page-offset relocation.
/// @details GOT relaxation rewrites an ADRP/LDR-through-GOT pair into ADRP/ADD-direct. The
///          page-offset relocation may be separated from the page relocation by scheduler-inserted
///          instructions, so pairing is based on relocation metadata plus the ADRP destination
///          register used as the LDR base, not raw instruction adjacency.
/// @param obj Object containing the candidate relocation pair.
/// @param arch Target architecture used to classify candidate relocations.
/// @param sec Input section whose relocations are searched.
/// @param outSec Merged output section containing @p sec.
/// @param chunkBase Offset of @p sec within @p outSec.
/// @param pageOffRel GOT page-offset relocation needing its paired ADRP.
/// @param ldrBaseReg Register used as the base of the LDR being relaxed.
/// @return The nearest eligible preceding ADRP patch offset, or `std::nullopt`
///         when no metadata- and register-matching relocation exists.
static std::optional<size_t> findMatchingAArch64GotPageRelocOffset(const ObjFile &obj,
                                                                   LinkArch arch,
                                                                   const ObjSection &sec,
                                                                   const OutputSection &outSec,
                                                                   size_t chunkBase,
                                                                   const ObjReloc &pageOffRel,
                                                                   uint32_t ldrBaseReg) {
    std::optional<size_t> bestPatchOff;
    for (const auto &candidate : sec.relocs) {
        if (candidate.offset >= pageOffRel.offset)
            continue;
        if (candidate.symIndex != pageOffRel.symIndex || candidate.addend != pageOffRel.addend)
            continue;
        const bool isGotPage =
            classifyReloc(obj.format, arch, candidate.type) == RelocAction::GotPage21;
        const bool isMachOTlvpPage = obj.format == ObjFileFormat::MachO &&
                                     pageOffRel.type == macho_a64::kTlvpLoadPageOff12 &&
                                     candidate.type == macho_a64::kTlvpLoadPage21;
        if (!isGotPage && !isMachOTlvpPage)
            continue;

        if (candidate.offset > std::numeric_limits<size_t>::max() - chunkBase)
            continue;
        const size_t candidatePatchOff = chunkBase + candidate.offset;
        if (candidatePatchOff > outSec.data.size() || outSec.data.size() - candidatePatchOff < 4)
            continue;

        const uint32_t adrpInsn = readLE32(outSec.data.data() + candidatePatchOff);
        if (!isAArch64AdrpOpcode(adrpInsn) || (adrpInsn & 0x1F) != ldrBaseReg)
            continue;

        if (!bestPatchOff || candidatePatchOff > *bestPatchOff)
            bestPatchOff = candidatePatchOff;
    }
    return bestPatchOff;
}

/// @brief Look up the output placement of an input section.
/// @param locMap Reverse placement index to query.
/// @param objIdx Index of the owning object file.
/// @param secIdx One-based input-section index within the object.
/// @param outSecIdx Receives the containing output-section index.
/// @param outOffset Receives the input chunk's byte offset in that section.
/// @param inputSize Optionally receives the original input-section size.
/// @return `true` when the input section was placed in the output layout.
static bool findOutputLocation(const LocationMap &locMap,
                               size_t objIdx,
                               uint32_t secIdx,
                               size_t &outSecIdx,
                               size_t &outOffset,
                               size_t *inputSize = nullptr) {
    auto it = locMap.find(InputSectionKey{objIdx, secIdx});
    if (it == locMap.end())
        return false;
    outSecIdx = it->second.outSecIdx;
    outOffset = it->second.outputOffset;
    if (inputSize)
        *inputSize = it->second.inputSize;
    return true;
}

/// @brief Compute the address range occupied by the static TLS image.
/// @details Only allocated TLS data sections contribute to the range; Mach-O
///          thread-variable descriptor sections do not contain per-thread data
///          and are skipped.
/// @param layout Link layout whose output sections are examined.
/// @param info Receives the union extent of all qualifying TLS sections.
/// @param err Stream that receives an address-overflow diagnostic.
/// @return `true` when every TLS section end address is representable.
static bool computeTlsImageInfo(const LinkLayout &layout, TlsImageInfo &info, std::ostream &err) {
    for (const auto &sec : layout.sections) {
        if (!sec.alloc || !sec.tls || sec.tlvDescriptors)
            continue;

        uint64_t secEnd = 0;
        if (!checkedAddU64(
                sec.virtualAddr, static_cast<uint64_t>(outputSectionMemSize(sec)), secEnd)) {
            err << "error: TLS section '" << sec.name << "' exceeds 64-bit address range\n";
            return false;
        }

        if (!info.present) {
            info.present = true;
            info.startVA = sec.virtualAddr;
            info.endVA = secEnd;
            continue;
        }

        info.startVA = std::min(info.startVA, sec.virtualAddr);
        info.endVA = std::max(info.endVA, secEnd);
    }

    return true;
}

/// @brief Compute an AArch64 TLS offset relative to the static TLS image start.
/// @param S Resolved address of the thread-local symbol.
/// @param A Signed relocation addend.
/// @param obj Object file used to qualify any diagnostic.
/// @param symName Referenced symbol name, if available.
/// @param kind Human-readable TLS relocation kind.
/// @param tlsImage Address extent of the static TLS image.
/// @param err Stream that receives missing-image, overflow, or bounds diagnostics.
/// @param tprel Receives `(S + A) - tlsImage.startVA` on success.
/// @return `true` when a TLS image exists and the target is within or after its start.
static bool computeAArch64TlsTprel(uint64_t S,
                                   int64_t A,
                                   const ObjFile &obj,
                                   const std::string &symName,
                                   const char *kind,
                                   const TlsImageInfo &tlsImage,
                                   std::ostream &err,
                                   uint64_t &tprel) {
    if (!tlsImage.present) {
        err << "error: " << obj.name << ": " << kind << " relocation requires a TLS image";
        if (!symName.empty())
            err << " for '" << symName << "'";
        err << "\n";
        return false;
    }

    uint64_t target = 0;
    if (!checkedRelocTarget(S, A, obj, symName, kind, err, target))
        return false;
    if (target < tlsImage.startVA) {
        err << "error: " << obj.name << ": " << kind << " target precedes TLS image";
        if (!symName.empty())
            err << " for '" << symName << "'";
        err << "\n";
        return false;
    }
    tprel = target - tlsImage.startVA;
    return true;
}

/// @brief Resolve a local or absolute object symbol to its final virtual address.
/// @details Section-relative symbols are resolved through the reverse placement
///          map and rejected when their offsets exceed the original input
///          section bounds.
/// @param sym Symbol-table entry to resolve.
/// @param objIdx Index of the symbol's object in the link input vector.
/// @param locMap Reverse input-to-output placement index.
/// @param layout Final output layout supplying section virtual addresses.
/// @param addr Receives the resolved virtual address.
/// @param resolvedOutSecIdx Optionally receives the containing output-section index.
/// @return `true` when the symbol is absolute or has a valid placed definition.
static bool resolveLocalSymAddr(const ObjSymbol &sym,
                                size_t objIdx,
                                const LocationMap &locMap,
                                const LinkLayout &layout,
                                uint64_t &addr,
                                size_t *resolvedOutSecIdx = nullptr) {
    if (sym.absolute) {
        addr = static_cast<uint64_t>(sym.offset);
        return true;
    }
    if (sym.sectionIndex == 0)
        return false;
    size_t outSecIdx = 0;
    size_t chunkOff = 0;
    size_t inputSize = 0;
    if (!findOutputLocation(locMap, objIdx, sym.sectionIndex, outSecIdx, chunkOff, &inputSize))
        return false;
    const auto &outSec = layout.sections[outSecIdx];
    if (chunkOff > outputSectionMemSize(outSec) || sym.offset > inputSize)
        return false; // Symbol offset exceeds section bounds (malformed .o).
    uint64_t withChunk = 0;
    if (!checkedAddU64(outSec.virtualAddr, static_cast<uint64_t>(chunkOff), withChunk) ||
        !checkedAddU64(withChunk, static_cast<uint64_t>(sym.offset), addr))
        return false;
    if (resolvedOutSecIdx)
        *resolvedOutSecIdx = outSecIdx;
    return true;
}

/// @brief Resolve a global symbol name and optionally identify its output section.
/// @details Name lookup honors platform spelling fallbacks. Definitions backed
///          by placed input sections are recomputed from the layout; absolute
///          and already-resolved synthetic or dynamic entries are returned
///          directly.
/// @param symName Name requested by the relocation.
/// @param globalSyms Global symbol table to search.
/// @param locMap Reverse input-to-output placement index.
/// @param layout Final output layout supplying section virtual addresses.
/// @param platform Target platform controlling alternate symbol spellings.
/// @param addr Receives the resolved address.
/// @param resolvedOutSecIdx Optionally receives a concrete output-section index.
/// @return `true` when a usable definition or pre-resolved address is found.
static bool resolveGlobalSymLocation(
    const std::string &symName,
    const std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
    const LocationMap &locMap,
    const LinkLayout &layout,
    LinkPlatform platform,
    uint64_t &addr,
    size_t *resolvedOutSecIdx = nullptr) {
    auto it = findWithPlatformFallback(globalSyms, symName, platform);
    if (it == globalSyms.end())
        return false;

    const auto &entry = it->second;
    if (entry.absolute) {
        addr = static_cast<uint64_t>(entry.offset);
        return true;
    }

    if (entry.secIndex > 0) {
        size_t outSecIdx = 0;
        size_t chunkOff = 0;
        size_t inputSize = 0;
        if (findOutputLocation(
                locMap, entry.objIndex, entry.secIndex, outSecIdx, chunkOff, &inputSize)) {
            const auto &outSec = layout.sections[outSecIdx];
            if (chunkOff > outputSectionMemSize(outSec) || entry.offset > inputSize)
                return false;
            uint64_t withChunk = 0;
            if (!checkedAddU64(outSec.virtualAddr, static_cast<uint64_t>(chunkOff), withChunk) ||
                !checkedAddU64(withChunk, static_cast<uint64_t>(entry.offset), addr))
                return false;
            if (resolvedOutSecIdx)
                *resolvedOutSecIdx = outSecIdx;
            return true;
        }
    }

    if (!entry.resolvedAddrValid && entry.resolvedAddr == 0)
        return false;
    addr = entry.resolvedAddr;
    return true;
}

/// @brief Find the synthetic GOT entry corresponding to a symbol.
/// @details The canonical lookup uses the `__got_` prefix. On macOS the helper
///          also tries the opposite leading-underscore spelling so Mach-O input
///          names and synthetic linker names can meet at the same entry.
/// @param globalSyms Mutable global symbol table containing synthetic GOT entries.
/// @param symName Referenced symbol's input spelling.
/// @param platform Target platform controlling Mach-O name fallback.
/// @return An iterator to the GOT symbol, or `globalSyms.end()` if absent.
static auto findGotSymbol(std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                          const std::string &symName,
                          LinkPlatform platform) -> decltype(globalSyms.find(symName)) {
    if (symName.empty())
        return globalSyms.end();

    auto it = globalSyms.find("__got_" + symName);
    if (it != globalSyms.end() || platform != LinkPlatform::macOS)
        return it;

    if (symName[0] == '_')
        it = globalSyms.find("__got_" + symName.substr(1));
    else
        it = globalSyms.find("__got_" + machoMangle(symName));
    return it;
}

/// @brief Test whether a symbol has an addressable synthetic GOT entry.
/// @param globalSyms Global symbol table containing synthetic GOT entries.
/// @param symName Referenced symbol's input spelling.
/// @param platform Target platform controlling Mach-O name fallback.
/// @return `true` when the corresponding GOT entry exists and has a final address.
static bool hasResolvedGotSymbol(std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                                 const std::string &symName,
                                 LinkPlatform platform) {
    auto it = findGotSymbol(globalSyms, symName, platform);
    return it != globalSyms.end() && it->second.resolvedAddrValid;
}

// Relocation classification (RelocAction, classifyReloc) is in RelocClassify.hpp.

/// @brief Resolve every defined global symbol before relocation patching begins.
/// @details Absolute symbols are copied directly, placed definitions are
///          computed from output section/chunk offsets, and previously resolved
///          synthetic definitions are retained. A defined symbol whose input
///          section was not placed is treated as a layout error.
/// @param layout Link layout whose global symbol entries are updated in place.
/// @param locMap Reverse input-to-output placement index.
/// @param err Stream that receives placement, bounds, or overflow diagnostics.
/// @return `true` when every non-dynamic definition has a valid final address.
static bool resolveGlobalSymbolAddresses(LinkLayout &layout,
                                         const LocationMap &locMap,
                                         std::ostream &err) {
    for (auto &[name, entry] : layout.globalSyms) {
        if (entry.binding == GlobalSymEntry::Undefined || entry.binding == GlobalSymEntry::Dynamic)
            continue;
        if (entry.absolute) {
            entry.resolvedAddr = static_cast<uint64_t>(entry.offset);
            entry.resolvedAddrValid = true;
            continue;
        }

        size_t outSecIdx = 0;
        size_t chunkOffset = 0;
        size_t inputSize = 0;
        if (findOutputLocation(
                locMap, entry.objIndex, entry.secIndex, outSecIdx, chunkOffset, &inputSize)) {
            const auto &outSec = layout.sections[outSecIdx];
            if (chunkOffset > outputSectionMemSize(outSec) || entry.offset > inputSize) {
                err << "error: symbol '" << name << "' is outside its input section bounds\n";
                return false;
            }
            uint64_t withChunk = 0;
            if (!checkedAddU64(outSec.virtualAddr, static_cast<uint64_t>(chunkOffset), withChunk) ||
                !checkedAddU64(
                    withChunk, static_cast<uint64_t>(entry.offset), entry.resolvedAddr)) {
                err << "error: symbol address overflow while resolving '" << name << "'\n";
                return false;
            }
            entry.resolvedAddrValid = true;
        } else {
            if (entry.resolvedAddrValid || entry.resolvedAddr != 0)
                continue;
            err << "error: defined symbol '" << name
                << "' references an input section that was not placed in the output layout\n";
            return false;
        }
    }
    return true;
}

/// @copydoc zanna::codegen::linker::applyRelocations
bool applyRelocations(const std::vector<ObjFile> &objects,
                      LinkLayout &layout,
                      const std::unordered_set<std::string> &dynamicSyms,
                      LinkPlatform platform,
                      LinkArch arch,
                      std::ostream &err) {
    // Build reverse-index map once: (objIdx, secIdx) → (outSecIdx, outputOffset).
    // This replaces the previous O(S×C) linear scan per lookup with O(1) amortized.
    LocationMap locMap;
    if (!buildLocationMap(layout, locMap, err))
        return false;
    TlsImageInfo tlsImage;
    if (!computeTlsImageInfo(layout, tlsImage, err))
        return false;

    // First pass: resolve all symbol addresses.
    if (!resolveGlobalSymbolAddresses(layout, locMap, err))
        return false;

    // Second pass: apply relocations.
    for (size_t oi = 0; oi < objects.size(); ++oi) {
        const auto &obj = objects[oi];
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const auto &sec = obj.sections[si];
            if (sec.relocs.empty())
                continue;

            size_t outSecIdx = 0;
            size_t chunkBase = 0;
            if (!findOutputLocation(locMap, oi, static_cast<uint32_t>(si), outSecIdx, chunkBase)) {
                if (sec.stripped || !sec.alloc)
                    continue;
                err << "error: " << obj.name << ": live section '" << sec.name
                    << "' has relocations but was not placed in the output layout\n";
                return false;
            }

            auto &outSec = layout.sections[outSecIdx];
            const uint64_t secVA = outSec.virtualAddr;

            for (const auto &rel : sec.relocs) {
                if (rel.offset >= objSectionMemSize(sec)) {
                    err << "error: relocation offset " << rel.offset << " exceeds section size "
                        << objSectionMemSize(sec) << " in '" << obj.name << "'\n";
                    return false;
                }

                if (rel.symIndex >= obj.symbols.size()) {
                    err << "error: " << obj.name << ": relocation references invalid symbol index "
                        << rel.symIndex << "\n";
                    return false;
                }

                const ObjSymbol &targetSym = obj.symbols[rel.symIndex];
                const std::string &symName = targetSym.name;
                const std::string targetDisplay =
                    symName.empty() ? std::string("<anonymous section symbol>") : symName;

                uint64_t S = 0;
                bool symResolved = false;
                bool weakResolvedToZero = false;
                size_t symOutSecIdx = SIZE_MAX;
                bool hasSymOutputSection = false;
                const bool preferGlobal = !symName.empty() && targetSym.binding != ObjSymbol::Local;
                if (preferGlobal) {
                    symResolved = resolveGlobalSymLocation(
                        symName, layout.globalSyms, locMap, layout, platform, S, &symOutSecIdx);
                    if (symResolved && symOutSecIdx != SIZE_MAX)
                        hasSymOutputSection = true;
                }
                if (!symResolved && (targetSym.sectionIndex > 0 || targetSym.absolute))
                    symResolved =
                        resolveLocalSymAddr(targetSym, oi, locMap, layout, S, &symOutSecIdx);
                if (symResolved && symOutSecIdx != SIZE_MAX)
                    hasSymOutputSection = true;
                if (!symResolved && !symName.empty()) {
                    symResolved = resolveGlobalSymLocation(
                        symName, layout.globalSyms, locMap, layout, platform, S, &symOutSecIdx);
                    if (symResolved && symOutSecIdx != SIZE_MAX)
                        hasSymOutputSection = true;
                }
                if (!symResolved && !symName.empty() && targetSym.weakExternal) {
                    const std::string &fallback = targetSym.weakDefaultName;
                    if (!fallback.empty()) {
                        symResolved = resolveGlobalSymLocation(fallback,
                                                               layout.globalSyms,
                                                               locMap,
                                                               layout,
                                                               platform,
                                                               S,
                                                               &symOutSecIdx);
                        if (symResolved && symOutSecIdx != SIZE_MAX)
                            hasSymOutputSection = true;
                    }
                    if (!symResolved) {
                        S = 0;
                        symResolved = true;
                        weakResolvedToZero = true;
                    }
                }
                if (!symResolved && !symName.empty()) {
                    if (platform == LinkPlatform::Windows && symName == "__ImageBase") {
                        S = defaultImageBaseForPlatform(LinkPlatform::Windows);
                        symResolved = true;
                    } else if (platform == LinkPlatform::Windows && symName == "vm_trap") {
                        symResolved = resolveGlobalSymLocation("vm_trap_default",
                                                               layout.globalSyms,
                                                               locMap,
                                                               layout,
                                                               platform,
                                                               S,
                                                               &symOutSecIdx) ||
                                      resolveGlobalSymLocation("rt_abort",
                                                               layout.globalSyms,
                                                               locMap,
                                                               layout,
                                                               platform,
                                                               S,
                                                               &symOutSecIdx);
                        if (symResolved && symOutSecIdx != SIZE_MAX)
                            hasSymOutputSection = true;
                    }
                }
                if (!symResolved) {
                    err << "error: " << obj.name << ": undefined symbol '" << targetDisplay
                        << "'\n";
                    return false;
                }

                const int64_t A = rel.addend;
                uint64_t P = 0;
                size_t patchOff = 0;
                if (!computeRelocPatchSite(outSec, obj, rel, chunkBase, secVA, err, P, patchOff))
                    return false;
                uint8_t *patch = outSec.data.data() + patchOff;

                /// Validate that a relocation field fits both its original
                /// input chunk and the file-backed merged output bytes.
                auto requirePatchBytes = [&](size_t width, const char *kind) -> bool {
                    // The fixup must fit within the *input chunk*, not merely within the
                    // merged output section. Input sections are concatenated (with
                    // alignment padding) into one output section, so a fixup near a chunk
                    // boundary that only satisfies the output-section bound would patch
                    // bytes belonging to the adjacent chunk. rel.offset < chunkSize is
                    // guaranteed by the section-size check above, so the subtraction is
                    // safe.
                    const size_t chunkSize = objSectionMemSize(sec);
                    if (width <= chunkSize - rel.offset && width <= outSec.data.size() - patchOff)
                        return true;
                    err << "error: " << kind << " relocation at offset " << patchOff
                        << " out of bounds in '" << outSec.name << "' (size=" << outSec.data.size()
                        << ")\n";
                    return false;
                };

                /// Require a relocation target to belong to a concrete output
                /// section, as needed by section-relative COFF encodings.
                auto requireTargetOutputSection = [&](const char *kind) -> bool {
                    if (hasSymOutputSection)
                        return true;
                    err << "error: " << obj.name << ": " << kind << " target '" << targetDisplay
                        << "' has no output section\n";
                    return false;
                };

                if (obj.format == ObjFileFormat::COFF && arch == LinkArch::X86_64) {
                    if (rel.type == coff_x64::kSecRel) {
                        if (!requirePatchBytes(4, "SECREL"))
                            return false;
                        if (!requireTargetOutputSection("SECREL"))
                            return false;
                        uint64_t target = 0;
                        int64_t secRel = 0;
                        uint32_t val = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "SECREL", err, target) ||
                            !checkedRelocDelta(target,
                                               layout.sections[symOutSecIdx].virtualAddr,
                                               obj,
                                               symName,
                                               "SECREL",
                                               err,
                                               secRel) ||
                            !checkedU32Value(secRel, obj, symName, "SECREL", err, val))
                            return false;
                        writeLE32(patch, val);
                        continue;
                    }
                    if (rel.type == coff_x64::kSection) {
                        if (!requirePatchBytes(2, "SECTION"))
                            return false;
                        if (!requireTargetOutputSection("SECTION"))
                            return false;
                        if (symOutSecIdx + 1 > std::numeric_limits<uint16_t>::max()) {
                            err << "error: " << obj.name << ": SECTION relocation target index "
                                << (symOutSecIdx + 1) << " exceeds 16-bit COFF limit\n";
                            return false;
                        }
                        writeLE16(patch, static_cast<uint16_t>(symOutSecIdx + 1));
                        continue;
                    }
                    if (rel.type == coff_x64::kAddr32Nb) {
                        if (!requirePatchBytes(4, "ADDR32NB"))
                            return false;
                        // Pull the active image base from the layout, not the
                        // platform default constant. The two agree today, but
                        // if a configurable image base is ever wired through
                        // NativeLinkerOptions, the writer-side RVA computation
                        // and the applier here will only stay aligned if they
                        // share the same source of truth.
                        const uint64_t imageBase = layout.imageBase != 0
                                                       ? layout.imageBase
                                                       : defaultImageBaseForPlatform(platform);
                        uint64_t target = 0;
                        int64_t rva = 0;
                        uint32_t val = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "ADDR32NB", err, target) ||
                            !checkedRelocDelta(
                                target, imageBase, obj, symName, "ADDR32NB", err, rva) ||
                            !checkedU32Value(rva, obj, symName, "ADDR32NB", err, val))
                            return false;
                        writeLE32(patch, val);
                        continue;
                    }
                    if (rel.type == coff_x64::kRel32 ||
                        (rel.type >= coff_x64::kRel32_1 && rel.type <= coff_x64::kRel32_5)) {
                        if (!requirePatchBytes(4, "COFF REL32"))
                            return false;
                        // COFF AMD64 REL32 relocations are relative to the byte
                        // following the relocated field, not the field address.
                        // REL32_n variants apply the same base bias plus an
                        // additional n-byte adjustment for instructions whose
                        // displacement isn't immediately followed by the next
                        // instruction boundary.
                        const int64_t extraBias =
                            (rel.type == coff_x64::kRel32)
                                ? 0
                                : static_cast<int64_t>(rel.type - coff_x64::kRel32);
                        uint64_t target = 0;
                        int64_t delta = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "COFF REL32", err, target) ||
                            !checkedRelocDelta(target, P, obj, symName, "COFF REL32", err, delta))
                            return false;
                        const int64_t val = delta - 4 - extraBias;
                        if (!writeCheckedRel32(patch, val, obj, symName, "COFF REL32", err))
                            return false;
                        continue;
                    }
                }

                if (obj.format == ObjFileFormat::COFF && arch == LinkArch::AArch64) {
                    if (rel.type == coff_a64::kSecRel) {
                        if (!requirePatchBytes(4, "SECREL"))
                            return false;
                        if (!requireTargetOutputSection("SECREL"))
                            return false;
                        uint64_t target = 0;
                        int64_t secRel = 0;
                        uint32_t val = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "SECREL", err, target) ||
                            !checkedRelocDelta(target,
                                               layout.sections[symOutSecIdx].virtualAddr,
                                               obj,
                                               symName,
                                               "SECREL",
                                               err,
                                               secRel) ||
                            !checkedU32Value(secRel, obj, symName, "SECREL", err, val))
                            return false;
                        writeLE32(patch, val);
                        continue;
                    }
                    if (rel.type == coff_a64::kSection) {
                        if (!requirePatchBytes(2, "SECTION"))
                            return false;
                        if (!requireTargetOutputSection("SECTION"))
                            return false;
                        if (symOutSecIdx + 1 > std::numeric_limits<uint16_t>::max()) {
                            err << "error: " << obj.name << ": SECTION relocation target index "
                                << (symOutSecIdx + 1) << " exceeds 16-bit COFF limit\n";
                            return false;
                        }
                        writeLE16(patch, static_cast<uint16_t>(symOutSecIdx + 1));
                        continue;
                    }
                    if (rel.type == coff_a64::kAddr32Nb) {
                        if (!requirePatchBytes(4, "ADDR32NB"))
                            return false;
                        // See the x86_64 ADDR32NB path above for why the
                        // image base must come from the layout.
                        const uint64_t imageBase = layout.imageBase != 0
                                                       ? layout.imageBase
                                                       : defaultImageBaseForPlatform(platform);
                        uint64_t target = 0;
                        int64_t rva = 0;
                        uint32_t val = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "ADDR32NB", err, target) ||
                            !checkedRelocDelta(
                                target, imageBase, obj, symName, "ADDR32NB", err, rva) ||
                            !checkedU32Value(rva, obj, symName, "ADDR32NB", err, val))
                            return false;
                        writeLE32(patch, val);
                        continue;
                    }
                    if (rel.type == coff_a64::kSecRelLow12A ||
                        rel.type == coff_a64::kSecRelHigh12A ||
                        rel.type == coff_a64::kSecRelLow12L) {
                        if (!requirePatchBytes(4, "SECREL_LOW/HIGH"))
                            return false;
                        if (!requireTargetOutputSection("SECREL"))
                            return false;
                        uint64_t target = 0;
                        int64_t secRel = 0;
                        uint32_t value = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "SECREL", err, target) ||
                            !checkedRelocDelta(target,
                                               layout.sections[symOutSecIdx].virtualAddr,
                                               obj,
                                               symName,
                                               "SECREL",
                                               err,
                                               secRel) ||
                            !checkedU32Value(secRel, obj, symName, "SECREL", err, value))
                            return false;
                        if (rel.type == coff_a64::kSecRelHigh12A) {
                            if ((value >> 12) > 0xFFFu) {
                                err << "error: " << obj.name
                                    << ": SECREL_HIGH12A relocation out of 24-bit range";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                            value >>= 12;
                        } else {
                            value &= 0xFFF;
                        }

                        uint32_t insn = readLE32(patch);
                        if (rel.type == coff_a64::kSecRelLow12A ||
                            rel.type == coff_a64::kSecRelHigh12A) {
                            if (!isAArch64AddImmediate(insn)) {
                                err << "error: " << obj.name
                                    << ": SECREL_LOW/HIGH12A relocation is not applied to an "
                                       "AArch64 ADD immediate instruction";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                        }
                        if (rel.type == coff_a64::kSecRelLow12L) {
                            uint32_t shift = 0;
                            if (!aarch64UnsignedLdStOffsetShift(insn, shift)) {
                                err << "error: " << obj.name
                                    << ": SECREL_LOW12L relocation is not applied to an "
                                       "AArch64 unsigned-offset load/store";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                            if (!checkPageOffsetAlignment(value, shift, obj, symName, err))
                                return false;
                            value >>= shift;
                        }
                        insn = (insn & 0xFFC003FF) | ((value & 0xFFF) << 10);
                        writeLE32(patch, insn);
                        continue;
                    }
                }

                if (rel.subtract) {
                    // Mach-O SUBTRACTOR + UNSIGNED pair: a link-time symbol difference
                    // S(target) + A - S(subtrahend). The value is a delta, so it is
                    // position-independent by construction and takes no rebase or bind
                    // bookkeeping. Emitted for __eh_frame FDE pointers in optimized C
                    // objects, C++ exception/typeinfo tables, and jump tables.
                    if (obj.format != ObjFileFormat::MachO ||
                        rel.subSymIndex >= obj.symbols.size()) {
                        err << "error: " << obj.name
                            << ": malformed symbol-difference relocation for '" << targetDisplay
                            << "'\n";
                        return false;
                    }
                    if (weakResolvedToZero) {
                        err << "error: " << obj.name << ": weak undefined symbol '"
                            << targetDisplay << "' cannot anchor a symbol-difference relocation\n";
                        return false;
                    }
                    const ObjSymbol &subSym = obj.symbols[rel.subSymIndex];
                    const std::string subDisplay = subSym.name.empty()
                                                       ? std::string("<anonymous section symbol>")
                                                       : subSym.name;
                    uint64_t subAddr = 0;
                    bool subResolved = false;
                    size_t subOutSecIdx = SIZE_MAX;
                    if (!subSym.name.empty() && subSym.binding != ObjSymbol::Local)
                        subResolved = resolveGlobalSymLocation(subSym.name,
                                                               layout.globalSyms,
                                                               locMap,
                                                               layout,
                                                               platform,
                                                               subAddr,
                                                               &subOutSecIdx);
                    if (!subResolved && (subSym.sectionIndex > 0 || subSym.absolute))
                        subResolved =
                            resolveLocalSymAddr(subSym, oi, locMap, layout, subAddr, &subOutSecIdx);
                    if (!subResolved && !subSym.name.empty())
                        subResolved = resolveGlobalSymLocation(subSym.name,
                                                               layout.globalSyms,
                                                               locMap,
                                                               layout,
                                                               platform,
                                                               subAddr,
                                                               &subOutSecIdx);
                    if (!subResolved) {
                        err << "error: " << obj.name << ": undefined subtrahend symbol '"
                            << subDisplay << "' in symbol-difference relocation\n";
                        return false;
                    }
                    const uint64_t minuend = wrappingAbs64Target(S, A);
                    const int64_t diff = static_cast<int64_t>(minuend - subAddr);
                    if (rel.length == 3) {
                        if (!requirePatchBytes(8, "symbol difference"))
                            return false;
                        writeLE64(patch, static_cast<uint64_t>(diff));
                    } else {
                        if (!requirePatchBytes(4, "symbol difference"))
                            return false;
                        if (diff < std::numeric_limits<int32_t>::min() ||
                            diff > std::numeric_limits<int32_t>::max()) {
                            err << "error: " << obj.name
                                << ": 32-bit symbol-difference relocation out of range for '"
                                << targetDisplay << "' - '" << subDisplay << "'\n";
                            return false;
                        }
                        writeLE32(patch, static_cast<uint32_t>(static_cast<int32_t>(diff)));
                    }
                    continue;
                }

                RelocAction action = classifyReloc(obj.format, arch, rel.type);
                if (obj.format == ObjFileFormat::MachO &&
                    ((arch == LinkArch::X86_64 && rel.type == macho_x64::kUnsigned) ||
                     (arch == LinkArch::AArch64 && rel.type == macho_a64::kUnsigned))) {
                    if (rel.length == 2) {
                        action = RelocAction::Abs32;
                    } else if (rel.length == 3) {
                        action = RelocAction::Abs64;
                    } else if (rel.length == 0 && action == RelocAction::Abs64) {
                        // Synthetic unit-test objects historically left Mach-O's
                        // length exponent unset. Real Mach-O input is validated and
                        // normalized by MachOReader before reaching the applier.
                    } else {
                        err << "error: " << obj.name
                            << ": unsupported Mach-O unsigned relocation length "
                            << static_cast<unsigned>(rel.length);
                        if (!symName.empty())
                            err << " for '" << symName << "'";
                        err << "\n";
                        return false;
                    }
                }
                if (!outSec.alloc && hasSymOutputSection && layout.sections[symOutSecIdx].alloc &&
                    (action == RelocAction::PCRel32 || action == RelocAction::PCRel64 ||
                     action == RelocAction::Branch26 || action == RelocAction::Page21 ||
                     action == RelocAction::PageOff12 || action == RelocAction::PageOff12A ||
                     action == RelocAction::PageOff12L || action == RelocAction::LdSt32Off ||
                     action == RelocAction::LdSt64Off || action == RelocAction::LdSt128Off ||
                     action == RelocAction::CondBr19 || action == RelocAction::GotPCRel32 ||
                     action == RelocAction::GotPage21 || action == RelocAction::GotPageOff12 ||
                     action == RelocAction::GotPointer)) {
                    err << "error: " << obj.name << ": non-alloc section '" << outSec.name
                        << "' contains runtime PC/page relocation against alloc symbol '"
                        << targetDisplay << "'\n";
                    return false;
                }
                if (weakResolvedToZero && action != RelocAction::Abs64 &&
                    action != RelocAction::Abs32) {
                    err << "error: " << obj.name << ": weak undefined symbol '" << targetDisplay
                        << "' cannot resolve to address zero for this relocation\n";
                    return false;
                }

                if (obj.format == ObjFileFormat::MachO && arch == LinkArch::X86_64) {
                    if (rel.type == macho_x64::kSigned || rel.type == macho_x64::kSigned1 ||
                        rel.type == macho_x64::kSigned2 || rel.type == macho_x64::kSigned4) {
                        if (!requirePatchBytes(4, "Mach-O signed"))
                            return false;
                        const int64_t extraBias =
                            rel.type == macho_x64::kSigned1
                                ? 1
                                : (rel.type == macho_x64::kSigned2
                                       ? 2
                                       : (rel.type == macho_x64::kSigned4 ? 4 : 0));
                        uint64_t target = 0;
                        int64_t delta = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "Mach-O signed", err, target) ||
                            !checkedRelocDelta(
                                target, P, obj, symName, "Mach-O signed", err, delta))
                            return false;
                        const int64_t val = delta - extraBias;
                        if (!writeCheckedRel32(patch, val, obj, symName, "Mach-O signed", err))
                            return false;
                        continue;
                    }
                }

                switch (action) {
                    case RelocAction::PCRel32: {
                        if (!requirePatchBytes(4, "PC-relative"))
                            return false;
                        uint64_t target = 0;
                        int64_t val = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "PC-relative", err, target) ||
                            !checkedRelocDelta(target, P, obj, symName, "PC-relative", err, val))
                            return false;
                        if (!writeCheckedRel32(patch, val, obj, symName, "PC-relative", err))
                            return false;
                        break;
                    }
                    case RelocAction::PCRel64: {
                        if (!requirePatchBytes(8, "64-bit PC-relative"))
                            return false;
                        uint64_t target = 0;
                        int64_t val = 0;
                        if (!checkedRelocTarget(
                                S, A, obj, symName, "64-bit PC-relative", err, target) ||
                            !checkedRelocDelta(
                                target, P, obj, symName, "64-bit PC-relative", err, val))
                            return false;
                        writeLE64(patch, static_cast<uint64_t>(val));
                        break;
                    }
                    case RelocAction::Abs64: {
                        if (!requirePatchBytes(8, "64-bit absolute"))
                            return false;
                        uint64_t val = wrappingAbs64Target(S, A);

                        const bool isDynamicSym =
                            !symName.empty() &&
                            findWithPlatformFallback(dynamicSyms, symName, platform) !=
                                dynamicSyms.end();
                        const bool hasDynamicGot =
                            hasResolvedGotSymbol(layout.globalSyms, symName, platform);

                        // Linux x86_64: imported data/function pointers are emitted as
                        // runtime-loader relocations instead of being resolved to the
                        // local jump stub address.
                        if (platform == LinkPlatform::Linux && (isDynamicSym || hasDynamicGot)) {
                            writeLE64(patch, 0);
                            layout.bindEntries.push_back({symName, outSecIdx, patchOff});
                            break;
                        }

                        // Mach-O: writable data pointers to imported symbols are left
                        // for dyld bind opcodes. Code branches use the synthetic stubs.
                        if ((outSec.writable || outSec.dataSegment) &&
                            (isDynamicSym || hasDynamicGot)) {
                            writeLE64(patch, 0);
                            layout.bindEntries.push_back({symName, outSecIdx, patchOff});
                            break;
                        }

                        // Mach-O TLV descriptor fixups (in __thread_vars/.tdata):
                        if (outSec.tls && outSec.name == ".tdata") {
                            // Thunk field (_tlv_bootstrap): write 0 — dyld fills this via
                            // bind opcodes and sets up the TLS infrastructure. If we
                            // statically resolve it, dyld never discovers the TLV
                            // descriptors and _tlv_bootstrap (a fail-stub) aborts.
                            if (symName.find("tlv_bootstrap") != std::string::npos) {
                                writeLE64(patch, 0);
                                break;
                            }

                            // Offset field ($tlv$init symbols): convert absolute VA to
                            // TLS-relative offset.  _tlv_bootstrap expects offsets
                            // relative to the START of the TLS template (first
                            // S_THREAD_LOCAL_REGULAR section), not per-section VAs.
                            // The template spans all non-.tdata TLS sections including
                            // any alignment gaps between them.
                            uint64_t templateStartVA = 0;
                            bool templateStartValid = false;
                            for (const auto &ls : layout.sections) {
                                if (ls.tls && ls.name != ".tdata") {
                                    templateStartVA = ls.virtualAddr;
                                    templateStartValid = true;
                                    break;
                                }
                            }
                            bool tlvMatch = false;
                            if (templateStartValid) {
                                for (const auto &ls : layout.sections) {
                                    if (!ls.tls || ls.name == ".tdata")
                                        continue; // Skip the descriptor section itself.
                                    uint64_t tlsEnd = 0;
                                    if (!checkedAddU64(
                                            ls.virtualAddr,
                                            static_cast<uint64_t>(outputSectionMemSize(ls)),
                                            tlsEnd))
                                        return false;
                                    if (val >= ls.virtualAddr && val < tlsEnd) {
                                        val -= templateStartVA;
                                        tlvMatch = true;
                                        break;
                                    }
                                }
                            }
                            if (!tlvMatch && val != 0) {
                                err << "error: TLV offset for '" << symName
                                    << "' could not be converted to TLS-relative\n";
                                return false;
                            }
                        }

                        writeLE64(patch, val);

                        // Record image-base rebase entries for executable writers.
                        // Windows ARM64 requires a base relocation directory instead of
                        // fixed images; PE x64 can consume the same DIR64 fixups. Mach-O
                        // keeps its narrower writable-data rule because TLS and bind
                        // entries use separate dyld mechanisms there.
                        if (platform == LinkPlatform::Windows && outSec.alloc && val != 0)
                            layout.rebaseEntries.push_back({outSecIdx, patchOff});
                        else if (platform == LinkPlatform::macOS &&
                                 (outSec.writable || outSec.dataSegment) && !outSec.tls && val != 0)
                            layout.rebaseEntries.push_back({outSecIdx, patchOff});

                        break;
                    }
                    case RelocAction::Abs32: {
                        if (!requirePatchBytes(4, "32-bit absolute"))
                            return false;
                        uint64_t target = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "32-bit absolute", err, target))
                            return false;
                        if (target > std::numeric_limits<uint32_t>::max()) {
                            err << "error: " << obj.name
                                << ": 32-bit absolute relocation out of range";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        writeLE32(patch, static_cast<uint32_t>(target));
                        break;
                    }
                    case RelocAction::TlsOffset32: {
                        if (!requirePatchBytes(4, "TLS local-exec"))
                            return false;
                        if (!tlsImage.present) {
                            err << "error: " << obj.name
                                << ": TLS local-exec relocation requires a TLS image";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        if (!requireTargetOutputSection("TLS local-exec"))
                            return false;
                        if (!layout.sections[symOutSecIdx].tls) {
                            err << "error: " << obj.name << ": TLS local-exec target '"
                                << targetDisplay << "' is not in a TLS section\n";
                            return false;
                        }
                        uint64_t target = 0;
                        int64_t tpoff = 0;
                        if (!checkedRelocTarget(
                                S, A, obj, symName, "TLS local-exec", err, target) ||
                            !checkedRelocDelta(
                                target, tlsImage.endVA, obj, symName, "TLS local-exec", err, tpoff))
                            return false;
                        if (!writeCheckedRel32(patch, tpoff, obj, symName, "TLS local-exec", err))
                            return false;
                        break;
                    }
                    case RelocAction::TlsA64AddTprelHi12: {
                        if (!requirePatchBytes(4, "AArch64 TLS local-exec HI12"))
                            return false;
                        if (!requireTargetOutputSection("AArch64 TLS local-exec HI12"))
                            return false;
                        if (!layout.sections[symOutSecIdx].tls) {
                            err << "error: " << obj.name << ": AArch64 TLS local-exec target '"
                                << targetDisplay << "' is not in a TLS section\n";
                            return false;
                        }
                        uint64_t tprel = 0;
                        if (!computeAArch64TlsTprel(S,
                                                    A,
                                                    obj,
                                                    symName,
                                                    "AArch64 TLS local-exec HI12",
                                                    tlsImage,
                                                    err,
                                                    tprel))
                            return false;
                        if (tprel > 0xFFFFFFu) {
                            err << "error: " << obj.name
                                << ": AArch64 TLS local-exec HI12 relocation out of range";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        if (!writeAArch64AddImmediate12(
                                patch,
                                static_cast<uint32_t>((tprel >> 12) & 0xFFFu),
                                obj,
                                symName,
                                "AArch64 TLS local-exec HI12",
                                err))
                            return false;
                        break;
                    }
                    case RelocAction::TlsA64AddTprelLo12:
                    case RelocAction::TlsA64AddTprelLo12Nc: {
                        if (!requirePatchBytes(4, "AArch64 TLS local-exec LO12"))
                            return false;
                        if (!requireTargetOutputSection("AArch64 TLS local-exec LO12"))
                            return false;
                        if (!layout.sections[symOutSecIdx].tls) {
                            err << "error: " << obj.name << ": AArch64 TLS local-exec target '"
                                << targetDisplay << "' is not in a TLS section\n";
                            return false;
                        }
                        uint64_t tprel = 0;
                        if (!computeAArch64TlsTprel(S,
                                                    A,
                                                    obj,
                                                    symName,
                                                    "AArch64 TLS local-exec LO12",
                                                    tlsImage,
                                                    err,
                                                    tprel))
                            return false;
                        if (action == RelocAction::TlsA64AddTprelLo12 && tprel > 0xFFFu) {
                            err << "error: " << obj.name
                                << ": AArch64 TLS local-exec LO12 relocation out of range";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        if (!writeAArch64AddImmediate12(patch,
                                                        static_cast<uint32_t>(tprel & 0xFFFu),
                                                        obj,
                                                        symName,
                                                        "AArch64 TLS local-exec LO12",
                                                        err))
                            return false;
                        break;
                    }
                    case RelocAction::GotPCRel32: {
                        if (!requirePatchBytes(4, "GOT PC-relative"))
                            return false;

                        uint64_t base = S;
                        auto git = findGotSymbol(layout.globalSyms, symName, platform);
                        if (git != layout.globalSyms.end()) {
                            if (!git->second.resolvedAddrValid) {
                                err << "error: " << obj.name << ": unresolved GOT entry for '"
                                    << targetDisplay << "'\n";
                                return false;
                            }
                            base = git->second.resolvedAddr;
                        } else if (rel.type == elf_x64::kGotPcRelX ||
                                   rel.type == elf_x64::kRexGotPcRelX) {
                            if (!validateGotPCRelXMov(obj, rel, patch, patchOff, symName, err))
                                return false;
                            patch[-2] =
                                0x8D; // Relax mov foo@GOTPCRELX(%rip), %reg -> lea foo(%rip), %reg
                        } else {
                            err << "error: " << obj.name << ": missing GOT entry for '"
                                << targetDisplay << "'\n";
                            return false;
                        }

                        uint64_t target = 0;
                        int64_t delta = 0;
                        if (!checkedRelocTarget(
                                base, A, obj, symName, "GOT PC-relative", err, target) ||
                            !checkedRelocDelta(
                                target, P, obj, symName, "GOT PC-relative", err, delta))
                            return false;
                        if (!writeCheckedRel32(patch, delta, obj, symName, "GOT PC-relative", err))
                            return false;
                        break;
                    }
                    case RelocAction::Branch26: {
                        if (!requirePatchBytes(4, "branch"))
                            return false;
                        uint32_t insn = readLE32(patch);
                        if (!isAArch64Branch26Opcode(insn)) {
                            err << "error: " << obj.name
                                << ": AArch64 branch relocation is not applied to B/BL";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        uint64_t target = 0;
                        int64_t disp = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "branch", err, target) ||
                            !checkedRelocDelta(target, P, obj, symName, "branch", err, disp))
                            return false;
                        if (!checkAArch64BranchAlignment(disp, obj, symName, "branch", err))
                            return false;
                        int64_t imm26 = disp >> 2;
                        if (imm26 > 0x1FFFFFF || imm26 < -0x2000000) {
                            err << "error: " << obj.name << ": branch out of range for '" << symName
                                << "'\n";
                            return false;
                        }
                        insn = (insn & 0xFC000000) | (static_cast<uint32_t>(imm26) & 0x03FFFFFF);
                        writeLE32(patch, insn);
                        break;
                    }
                    case RelocAction::Page21: {
                        if (!requirePatchBytes(4, "ADRP"))
                            return false;
                        uint32_t insn = readLE32(patch);
                        if (!isAArch64AdrpOpcode(insn)) {
                            err << "error: " << obj.name
                                << ": AArch64 page relocation is not applied to ADRP";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        uint64_t target = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "ADRP", err, target))
                            return false;
                        uint64_t pageS = target & ~0xFFFULL;
                        uint64_t pageP = P & ~0xFFFULL;
                        int64_t pageDelta = 0;
                        if (!checkedRelocDelta(pageS, pageP, obj, symName, "ADRP", err, pageDelta))
                            return false;
                        int64_t immHiLo = pageDelta >> 12;
                        if (immHiLo > 0xFFFFF || immHiLo < -0x100000) {
                            err << "error: " << obj.name << ": ADRP page offset out of range for '"
                                << symName << "'\n";
                            return false;
                        }
                        uint32_t immlo = static_cast<uint32_t>(immHiLo) & 0x3;
                        uint32_t immhi = (static_cast<uint32_t>(immHiLo) >> 2) & 0x7FFFF;
                        insn = (insn & 0x9F00001F) | (immlo << 29) | (immhi << 5);
                        writeLE32(patch, insn);
                        break;
                    }
                    case RelocAction::PageOff12:
                    case RelocAction::PageOff12A:
                    case RelocAction::PageOff12L: {
                        if (!requirePatchBytes(4, "page offset"))
                            return false;
                        uint32_t insn = readLE32(patch);
                        uint64_t target = 0;
                        if (!checkedRelocTarget(S, A, obj, symName, "page offset", err, target))
                            return false;
                        uint32_t pageOff = static_cast<uint32_t>(target) & 0xFFF;
                        const bool requireArithmetic = action == RelocAction::PageOff12A;
                        const bool requireLdSt = action == RelocAction::PageOff12L;

                        // Mach-O ARM64_RELOC_PAGEOFF12 is used for both ADD (unscaled)
                        // and LDR/STR (scaled by access size). The linker must inspect the
                        // instruction to determine the correct scale factor.
                        //
                        // LDR/STR unsigned offset encoding: bits [31:30] = size,
                        // bits [29:24] = 11100x. Test: (insn & 0x3B000000) == 0x39000000.
                        // Scale = 1 << size, except 128-bit SIMD where scale = 16.
                        uint32_t ldStShift = 0;
                        if (aarch64UnsignedLdStOffsetShift(insn, ldStShift)) {
                            if (requireArithmetic) {
                                err << "error: " << obj.name
                                    << ": COFF PAGEOFFSET_12A relocation targets a load/store "
                                       "instruction";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                            if (!checkPageOffsetAlignment(pageOff, ldStShift, obj, symName, err))
                                return false;
                            pageOff >>= ldStShift;
                        } else if (isAArch64AddImmediate(insn)) {
                            if (requireLdSt) {
                                err << "error: " << obj.name
                                    << ": COFF PAGEOFFSET_12L relocation targets an ADD "
                                       "instruction";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                        } else {
                            err << "error: " << obj.name << ": AArch64 page offset relocation for '"
                                << symName << "' targets an unsupported instruction\n";
                            return false;
                        }

                        insn = (insn & 0xFFC003FF) | (pageOff << 10);
                        writeLE32(patch, insn);
                        break;
                    }
                    case RelocAction::LdSt64Off: {
                        if (!requirePatchBytes(4, "load/store page offset"))
                            return false;
                        uint32_t insn = readLE32(patch);
                        uint32_t shift = 0;
                        if (!aarch64UnsignedLdStOffsetShift(insn, shift) || shift != 3) {
                            err << "error: " << obj.name
                                << ": AArch64 64-bit load/store page offset relocation targets "
                                   "an incompatible instruction";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        uint64_t target = 0;
                        if (!checkedRelocTarget(
                                S, A, obj, symName, "load/store page offset", err, target))
                            return false;
                        uint32_t pageOff = static_cast<uint32_t>(target) & 0xFFF;
                        if (!checkPageOffsetAlignment(pageOff, 3, obj, symName, err))
                            return false;
                        pageOff >>= 3;
                        insn = (insn & 0xFFC003FF) | (pageOff << 10);
                        writeLE32(patch, insn);
                        break;
                    }
                    case RelocAction::LdSt32Off: {
                        if (!requirePatchBytes(4, "load/store page offset"))
                            return false;
                        uint32_t insn = readLE32(patch);
                        uint32_t shift = 0;
                        if (!aarch64UnsignedLdStOffsetShift(insn, shift) || shift != 2) {
                            err << "error: " << obj.name
                                << ": AArch64 32-bit load/store page offset relocation targets "
                                   "an incompatible instruction";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        uint64_t target = 0;
                        if (!checkedRelocTarget(
                                S, A, obj, symName, "load/store page offset", err, target))
                            return false;
                        uint32_t pageOff = static_cast<uint32_t>(target) & 0xFFF;
                        if (!checkPageOffsetAlignment(pageOff, 2, obj, symName, err))
                            return false;
                        pageOff >>= 2;
                        insn = (insn & 0xFFC003FF) | (pageOff << 10);
                        writeLE32(patch, insn);
                        break;
                    }
                    case RelocAction::LdSt128Off: {
                        if (!requirePatchBytes(4, "load/store page offset"))
                            return false;
                        uint32_t insn = readLE32(patch);
                        uint32_t shift = 0;
                        if (!aarch64UnsignedLdStOffsetShift(insn, shift) || shift != 4) {
                            err << "error: " << obj.name
                                << ": AArch64 128-bit load/store page offset relocation targets "
                                   "an incompatible instruction";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        uint64_t target = 0;
                        if (!checkedRelocTarget(
                                S, A, obj, symName, "load/store page offset", err, target))
                            return false;
                        uint32_t pageOff = static_cast<uint32_t>(target) & 0xFFF;
                        if (!checkPageOffsetAlignment(pageOff, 4, obj, symName, err))
                            return false;
                        pageOff >>= 4;
                        insn = (insn & 0xFFC003FF) | (pageOff << 10);
                        writeLE32(patch, insn);
                        break;
                    }
                    case RelocAction::CondBr19: {
                        if (!requirePatchBytes(4, "conditional branch"))
                            return false;
                        uint32_t insn = readLE32(patch);
                        if (!isAArch64CondBr19Opcode(insn)) {
                            err << "error: " << obj.name
                                << ": AArch64 conditional branch relocation is not applied to "
                                   "B.cond/CBZ/CBNZ";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        uint64_t target = 0;
                        int64_t disp = 0;
                        if (!checkedRelocTarget(
                                S, A, obj, symName, "conditional branch", err, target) ||
                            !checkedRelocDelta(
                                target, P, obj, symName, "conditional branch", err, disp))
                            return false;
                        if (!checkAArch64BranchAlignment(
                                disp, obj, symName, "conditional branch", err))
                            return false;
                        int64_t imm19 = disp >> 2;
                        if (imm19 > 0x3FFFF || imm19 < -0x40000) {
                            err << "error: " << obj.name
                                << ": conditional branch out of range for '" << symName << "'\n";
                            return false;
                        }
                        insn =
                            (insn & 0xFF00001F) | ((static_cast<uint32_t>(imm19) & 0x7FFFF) << 5);
                        writeLE32(patch, insn);
                        break;
                    }
                    case RelocAction::GotPage21: {
                        if (!requirePatchBytes(4, "GOT ADRP"))
                            return false;
                        // GOT ADRP: if symbol is dynamic, point at its GOT entry;
                        // otherwise, GOT relaxation → point directly at symbol.
                        uint64_t target = S;
                        auto git = findGotSymbol(layout.globalSyms, symName, platform);
                        if (git != layout.globalSyms.end()) {
                            if (!git->second.resolvedAddrValid) {
                                err << "error: " << obj.name << ": unresolved GOT entry for '"
                                    << targetDisplay << "'\n";
                                return false;
                            }
                            target = git->second.resolvedAddr; // Use GOT entry for dynamic sym.
                        }

                        uint32_t insn = readLE32(patch);
                        if (!isAArch64AdrpOpcode(insn)) {
                            err << "error: " << obj.name
                                << ": AArch64 GOT page relocation is not applied to ADRP";
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        uint64_t targetWithAddend = 0;
                        if (!checkedRelocTarget(
                                target, A, obj, symName, "GOT ADRP", err, targetWithAddend))
                            return false;
                        uint64_t pageT = targetWithAddend & ~0xFFFULL;
                        uint64_t pageP = P & ~0xFFFULL;
                        int64_t pageDelta = 0;
                        if (!checkedRelocDelta(
                                pageT, pageP, obj, symName, "GOT ADRP", err, pageDelta))
                            return false;
                        int64_t immHiLo = pageDelta >> 12;
                        if (immHiLo > 0xFFFFF || immHiLo < -0x100000) {
                            err << "error: " << obj.name
                                << ": GOT ADRP page offset out of range for '" << symName << "'\n";
                            return false;
                        }
                        uint32_t immlo = static_cast<uint32_t>(immHiLo) & 0x3;
                        uint32_t immhi = (static_cast<uint32_t>(immHiLo) >> 2) & 0x7FFFF;
                        insn = (insn & 0x9F00001F) | (immlo << 29) | (immhi << 5);
                        writeLE32(patch, insn);
                        break;
                    }
                    case RelocAction::GotPageOff12: {
                        if (!requirePatchBytes(4, "GOT page offset"))
                            return false;
                        // GOT LDR pageoff: if symbol is dynamic, keep LDR with GOT entry offset;
                        // otherwise, GOT relaxation → rewrite LDR as ADD.
                        auto git = findGotSymbol(layout.globalSyms, symName, platform);
                        if (git != layout.globalSyms.end() && !git->second.resolvedAddrValid) {
                            err << "error: " << obj.name << ": unresolved GOT entry for '"
                                << targetDisplay << "'\n";
                            return false;
                        }
                        if (git != layout.globalSyms.end()) {
                            // Dynamic: LDR from GOT entry (8-byte scaled).
                            uint32_t insn = readLE32(patch);
                            if (!isAArch64LdrXUnsignedOffset(insn)) {
                                err << "error: " << obj.name
                                    << ": GOT page-offset relocation is not applied to LDR X";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                            uint64_t gotTarget = 0;
                            if (!checkedRelocTarget(git->second.resolvedAddr,
                                                    A,
                                                    obj,
                                                    symName,
                                                    "GOT page offset",
                                                    err,
                                                    gotTarget))
                                return false;
                            uint32_t pageOff = static_cast<uint32_t>(gotTarget) & 0xFFF;
                            if (!checkPageOffsetAlignment(pageOff, 3, obj, symName, err))
                                return false;
                            pageOff >>= 3; // 8-byte scale for LDR X
                            insn = (insn & 0xFFC003FF) | (pageOff << 10);
                            writeLE32(patch, insn);
                        } else {
                            // Local: GOT relaxation — rewrite LDR to ADD.
                            uint32_t insn = readLE32(patch);
                            if (!isAArch64LdrXUnsignedOffset(insn)) {
                                err << "error: " << obj.name
                                    << ": local GOT relaxation requires LDR X";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                            uint32_t rd = insn & 0x1F;
                            uint32_t rn = (insn >> 5) & 0x1F;
                            const auto adrpPatchOff = findMatchingAArch64GotPageRelocOffset(
                                obj, arch, sec, outSec, chunkBase, rel, rn);
                            if (!adrpPatchOff) {
                                err << "error: " << obj.name
                                    << ": local GOT relaxation requires a preceding matching ADRP "
                                       "relocation using the LDR base register";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                            uint64_t target = 0;
                            if (!checkedRelocTarget(
                                    S, A, obj, symName, "GOT page offset", err, target))
                                return false;
                            uint32_t pageOff = static_cast<uint32_t>(target) & 0xFFF;
                            // ADD Xd, Xn, #imm12 = 0x91000000 | (imm12 << 10) | (Rn << 5) | Rd
                            insn = 0x91000000 | (pageOff << 10) | (rn << 5) | rd;
                            writeLE32(patch, insn);
                        }
                        break;
                    }
                    case RelocAction::GotPointer: {
                        auto git = findGotSymbol(layout.globalSyms, symName, platform);
                        if (git == layout.globalSyms.end() || !git->second.resolvedAddrValid) {
                            err << "error: " << obj.name << ": missing GOT entry for '"
                                << targetDisplay << "'\n";
                            return false;
                        }

                        uint64_t target = 0;
                        if (!checkedRelocTarget(git->second.resolvedAddr,
                                                A,
                                                obj,
                                                symName,
                                                "GOT pointer",
                                                err,
                                                target))
                            return false;

                        if (rel.pcrel) {
                            if (rel.length != 2) {
                                err << "error: " << obj.name
                                    << ": unsupported PC-relative GOT pointer relocation length "
                                    << static_cast<unsigned>(rel.length);
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                            if (!requirePatchBytes(4, "GOT pointer"))
                                return false;
                            int64_t delta = 0;
                            if (!checkedRelocDelta(
                                    target, P, obj, symName, "GOT pointer", err, delta))
                                return false;
                            if (!writeCheckedRel32(patch, delta, obj, symName, "GOT pointer", err))
                                return false;
                            break;
                        }

                        if (rel.length == 3) {
                            if (!requirePatchBytes(8, "GOT pointer"))
                                return false;
                            writeLE64(patch, target);
                            if (platform == LinkPlatform::macOS &&
                                (outSec.writable || outSec.dataSegment) && target != 0)
                                layout.rebaseEntries.push_back({outSecIdx, patchOff});
                        } else if (rel.length == 2) {
                            if (!requirePatchBytes(4, "GOT pointer"))
                                return false;
                            if (target > std::numeric_limits<uint32_t>::max()) {
                                err << "error: " << obj.name
                                    << ": GOT pointer relocation out of 32-bit range";
                                if (!symName.empty())
                                    err << " for '" << symName << "'";
                                err << "\n";
                                return false;
                            }
                            writeLE32(patch, static_cast<uint32_t>(target));
                        } else {
                            err << "error: " << obj.name
                                << ": unsupported GOT pointer relocation length "
                                << static_cast<unsigned>(rel.length);
                            if (!symName.empty())
                                err << " for '" << symName << "'";
                            err << "\n";
                            return false;
                        }
                        break;
                    }
                    case RelocAction::Unknown:
                        err << "error: " << obj.name << ": unknown reloc type " << rel.type
                            << " (format=" << static_cast<int>(obj.format) << ") for symbol '"
                            << symName << "'\n";
                        return false;
                }
            }
        }
    }

    if (platform == LinkPlatform::Windows && !sortWindowsPdata(layout, arch, err))
        return false;

    return true;
}

} // namespace zanna::codegen::linker
