//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/CodeSection.hpp
// Purpose: Growable byte buffer that accumulates machine code or read-only data
//          while tracking relocations and symbol definitions.
// Key invariants:
//   - Little-endian only (both x86_64 and AArch64 targets use LE)
//   - Internal branch resolution uses patch32LE(); only external references
//     and cross-section references generate Relocation entries
//   - Separate CodeSection instances for .text and .rodata
// Ownership/Lifetime:
//   - Owned by the binary encoder; passed to ObjectFileWriter for serialization
// Links: codegen/common/objfile/Relocation.hpp
//        codegen/common/objfile/SymbolTable.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file CodeSection.hpp
 * @brief Defines the encoder-side byte, symbol, relocation, and unwind container.
 *
 * A @ref CodeSection owns a growable little-endian payload together with the
 * symbolic metadata needed to serialize it into ELF, Mach-O, or COFF. Logical
 * offsets may carry a dry-run bias, and stable section identities disambiguate
 * cross-section offset relocations when sections are copied or concatenated.
 */

#pragma once

#include "codegen/common/objfile/Relocation.hpp"
#include "codegen/common/objfile/SymbolTable.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace zanna::codegen::objfile {

/// @brief Per-function record serialized into Mach-O `__compact_unwind`.
/// @details Binary encoders collect architecture-specific encodings here; the
///          Mach-O writer expands each logical record to its 32-byte object
///          representation and emits relocations for the function start.
struct CompactUnwindEntry {
    uint32_t symbolIndex{0};    ///< Symbol index of the function start.
    uint32_t functionLength{0}; ///< Length of the function in bytes.
    uint32_t encoding{0};       ///< ARM64/x86_64 compact unwind encoding.
};

/// @brief One logical Win64 unwind operation emitted into PE `.xdata`.
struct Win64UnwindCode {
    /// @brief Supported Windows x64 unwind operation families.
    enum class Kind : uint8_t {
        /// Push a nonvolatile general-purpose register.
        PushNonVol,
        /// Allocate a fixed-size stack frame.
        AllocStack,
        /// Save a nonvolatile GPR at a stack offset.
        SaveNonVol,
        /// Save a nonvolatile 128-bit XMM register.
        SaveXmm128,
    };

    Kind kind{Kind::AllocStack};
    uint8_t codeOffset{0};   ///< End offset of the corresponding prologue instruction.
    uint8_t reg{0};          ///< Win64 register number for save/push operations.
    uint32_t stackOffset{0}; ///< Offset from final RSP after the prologue.
};

/// @brief Per-function Win64 metadata used to construct COFF `.xdata/.pdata`.
struct Win64UnwindEntry {
    uint32_t symbolIndex{0};    ///< Symbol index of the function start.
    uint32_t functionLength{0}; ///< Length of the function in bytes.
    uint8_t prologueSize{0};    ///< Size of the function prologue in bytes.
    std::vector<Win64UnwindCode> codes{};
};

/// @brief Per-function Windows ARM64 metadata for COFF `.xdata/.pdata`.
/// @details The unwind byte stream is already expressed in the Windows ARM64
///          xdata opcode format and is serialized behind an ARM64 pdata entry.
struct WinArm64UnwindEntry {
    uint32_t symbolIndex{0};    ///< Symbol index of the function start.
    uint32_t functionLength{0}; ///< Length of the function in bytes.
    uint8_t prologueSize{0};    ///< Prologue size in bytes, for validation/debugging.
    std::vector<uint8_t> unwindCodes{};
    /// Whether the sole epilog is packed in the header.
    bool packedEpilogInHeader{true};
    /// Bytecode index at which the packed epilog begins.
    uint8_t epilogCodeIndex{0};
};

/// @brief Return the encoded fixup width for a relocation kind.
/// @details Object writers use this information when validating and patching
///          relocation payloads. Relocations may be recorded before their
///          placeholder bytes are emitted, so CodeSection validates the source
///          offset itself and leaves full-width checks to serialization.
/// @param kind Architecture-independent relocation kind.
/// @return Number of bytes occupied by the fixup field.
inline size_t codeSectionRelocationFixupWidth(RelocKind kind) {
    switch (kind) {
        case RelocKind::PCRel32:
        case RelocKind::Branch32:
        case RelocKind::A64Call26:
        case RelocKind::A64Jump26:
        case RelocKind::A64AdrpPage21:
        case RelocKind::A64AddPageOff12:
        case RelocKind::A64LdSt32Off12:
        case RelocKind::A64LdSt64Off12:
        case RelocKind::A64LdSt128Off12:
        case RelocKind::A64CondBr19:
            return 4;
        case RelocKind::Abs64:
            return 8;
    }
    return 0;
}

/// @brief Growable encoder buffer with relocation, symbol, and unwind tracking.
/// @details Binary encoders use instances for machine code and read-only data.
///          Logical offsets are bounds checked against emitted bytes, while
///          object writers consume the recorded symbolic fixups and metadata.
///          Copies receive a fresh identity and retarget self-references; moves
///          preserve identity so outstanding exact-section targets remain valid.
class CodeSection {
  public:
    /// @brief Construct an empty section with a unique logical identity.
    CodeSection() : sectionIdentity_(allocateSectionIdentity()) {}

    /// @brief Copy a section while assigning the copy a distinct identity.
    /// @details The source identity is retained as an alias and relocations that
    ///          targeted the source itself are retargeted to the new copy.
    /// @param other Section state to copy.
    CodeSection(const CodeSection &other)
        : bytes_(other.bytes_), relocations_(other.relocations_), symbols_(other.symbols_),
          unwindEntries_(other.unwindEntries_), win64UnwindEntries_(other.win64UnwindEntries_),
          winArm64UnwindEntries_(other.winArm64UnwindEntries_), offsetBias_(other.offsetBias_),
          sectionIdentityAliases_(other.sectionIdentityAliases_),
          sectionIdentity_(allocateSectionIdentity()) {
        addSectionIdentityAlias(other.sectionIdentity_);
        retargetSelfIdentity(other.sectionIdentity_, sectionIdentity_);
    }

    /// @brief Move a section while preserving its logical identity.
    /// @details The moved-from object is reset to an empty logical-offset state
    ///          and assigned a new identity so it can be reused independently.
    /// @param other Section state to transfer.
    CodeSection(CodeSection &&other)
        : bytes_(std::move(other.bytes_)), relocations_(std::move(other.relocations_)),
          symbols_(std::move(other.symbols_)), unwindEntries_(std::move(other.unwindEntries_)),
          win64UnwindEntries_(std::move(other.win64UnwindEntries_)),
          winArm64UnwindEntries_(std::move(other.winArm64UnwindEntries_)),
          offsetBias_(other.offsetBias_),
          sectionIdentityAliases_(std::move(other.sectionIdentityAliases_)),
          sectionIdentity_(other.sectionIdentity_) {
        other.offsetBias_ = 0;
        other.sectionIdentityAliases_.clear();
        other.sectionIdentity_ = allocateSectionIdentity();
    }

    /// @brief Copy-assign section contents with a new destination identity.
    /// @param other Section state to copy.
    /// @return Reference to this section after self-references are retargeted.
    CodeSection &operator=(const CodeSection &other) {
        if (this == &other)
            return *this;
        bytes_ = other.bytes_;
        relocations_ = other.relocations_;
        symbols_ = other.symbols_;
        unwindEntries_ = other.unwindEntries_;
        win64UnwindEntries_ = other.win64UnwindEntries_;
        winArm64UnwindEntries_ = other.winArm64UnwindEntries_;
        offsetBias_ = other.offsetBias_;
        sectionIdentityAliases_ = other.sectionIdentityAliases_;
        sectionIdentity_ = allocateSectionIdentity();
        addSectionIdentityAlias(other.sectionIdentity_);
        retargetSelfIdentity(other.sectionIdentity_, sectionIdentity_);
        return *this;
    }

    /// @brief Move-assign section contents while adopting the source identity.
    /// @param other Section state to transfer and reset.
    /// @return Reference to this section.
    CodeSection &operator=(CodeSection &&other) {
        if (this == &other)
            return *this;
        bytes_ = std::move(other.bytes_);
        relocations_ = std::move(other.relocations_);
        symbols_ = std::move(other.symbols_);
        unwindEntries_ = std::move(other.unwindEntries_);
        win64UnwindEntries_ = std::move(other.win64UnwindEntries_);
        winArm64UnwindEntries_ = std::move(other.winArm64UnwindEntries_);
        offsetBias_ = other.offsetBias_;
        sectionIdentityAliases_ = std::move(other.sectionIdentityAliases_);
        sectionIdentity_ = other.sectionIdentity_;
        other.offsetBias_ = 0;
        other.sectionIdentityAliases_.clear();
        other.sectionIdentity_ = allocateSectionIdentity();
        return *this;
    }

    // === Byte emission ===

    /// @brief Reserve at least a total byte capacity without changing section size.
    /// @param totalBytes Desired payload capacity.
    void reserveBytes(size_t totalBytes) {
        if (bytes_.capacity() < totalBytes)
            bytes_.reserve(totalBytes);
    }

    /// @brief Reserve capacity for additional bytes relative to the current size.
    /// @param additionalBytes Number of future payload bytes anticipated.
    /// @throws std::length_error if current size plus @p additionalBytes overflows.
    void reserveAdditionalBytes(size_t additionalBytes) {
        if (additionalBytes > std::numeric_limits<size_t>::max() - bytes_.size())
            throw std::length_error("CodeSection byte reservation exceeds addressable size");
        reserveBytes(bytes_.size() + additionalBytes);
    }

    /// @brief Set a logical starting offset for dry-run encoding.
    /// @details This lets instruction-size estimators measure code at a nonzero
    ///          offset without materializing padding bytes. Real object emission
    ///          normally leaves the bias at zero.
    /// @param offsetBias Value added to every physical byte index.
    /// @throws std::logic_error if emission or user symbol creation has begun.
    void setLogicalOffsetBias(size_t offsetBias) {
        if (!bytes_.empty() || !relocations_.empty() || symbols_.count() > 1)
            throw std::logic_error("CodeSection logical offset bias must be set before emission");
        offsetBias_ = offsetBias;
    }

    /// @brief Reserve total relocation capacity without adding records.
    /// @param totalRelocs Desired relocation-vector capacity.
    void reserveRelocations(size_t totalRelocs) {
        if (relocations_.capacity() < totalRelocs)
            relocations_.reserve(totalRelocs);
    }

    /// @brief Reserve total symbol-table capacity without defining symbols.
    /// @param totalSymbols Desired symbol capacity, including the null entry.
    [[maybe_unused]] void reserveSymbols(size_t totalSymbols) {
        symbols_.reserve(totalSymbols);
    }

    /// @brief Return the logical write position at the end of emitted bytes.
    /// @return `logicalOffsetBias() + bytes().size()`.
    /// @throws std::length_error if that sum is not representable by `size_t`.
    size_t currentOffset() const {
        if (offsetBias_ > std::numeric_limits<size_t>::max() - bytes_.size())
            throw std::length_error("CodeSection current offset exceeds addressable size");
        return offsetBias_ + bytes_.size();
    }

    /// @brief Append one byte to the section payload.
    /// @param val Byte value to append.
    void emit8(uint8_t val) {
        bytes_.push_back(val);
    }

    /// @brief Append a 16-bit value in little-endian byte order.
    /// @param val Value to serialize.
    [[maybe_unused]] void emit16LE(uint16_t val) {
        bytes_.push_back(static_cast<uint8_t>(val));
        bytes_.push_back(static_cast<uint8_t>(val >> 8));
    }

    /// @brief Append a 32-bit value in little-endian byte order.
    /// @param val Value to serialize.
    void emit32LE(uint32_t val) {
        bytes_.push_back(static_cast<uint8_t>(val));
        bytes_.push_back(static_cast<uint8_t>(val >> 8));
        bytes_.push_back(static_cast<uint8_t>(val >> 16));
        bytes_.push_back(static_cast<uint8_t>(val >> 24));
    }

    /// @brief Append a 64-bit value in little-endian byte order.
    /// @param val Value to serialize.
    void emit64LE(uint64_t val) {
        bytes_.push_back(static_cast<uint8_t>(val));
        bytes_.push_back(static_cast<uint8_t>(val >> 8));
        bytes_.push_back(static_cast<uint8_t>(val >> 16));
        bytes_.push_back(static_cast<uint8_t>(val >> 24));
        bytes_.push_back(static_cast<uint8_t>(val >> 32));
        bytes_.push_back(static_cast<uint8_t>(val >> 40));
        bytes_.push_back(static_cast<uint8_t>(val >> 48));
        bytes_.push_back(static_cast<uint8_t>(val >> 56));
    }

    /// @brief Append an arbitrary contiguous byte range.
    /// @param data Address of the first source byte; may be null only for zero length.
    /// @param len Number of bytes to append.
    /// @throws std::invalid_argument when @p data is null and @p len is nonzero.
    void emitBytes(const void *data, size_t len) {
        if (len == 0)
            return;
        if (data == nullptr)
            throw std::invalid_argument("CodeSection emitBytes requires non-null data");
        auto ptr = static_cast<const uint8_t *>(data);
        bytes_.insert(bytes_.end(), ptr, ptr + len);
    }

    /// @brief Append zero-filled bytes, typically for alignment padding.
    /// @param count Number of zero bytes to append.
    void emitZeros(size_t count) {
        bytes_.insert(bytes_.end(), count, 0);
    }

    // === Alignment ===

    /// @brief Pad with zeros until the logical write position is aligned.
    /// @param alignment Required power-of-two byte alignment; zero and one are no-ops.
    /// @throws std::invalid_argument if @p alignment is not a power of two.
    /// @throws std::length_error if the aligned logical position overflows.
    void alignTo(size_t alignment) {
        if (alignment <= 1)
            return;
        if ((alignment & (alignment - 1)) != 0)
            throw std::invalid_argument("CodeSection alignment must be a power of two");
        const size_t offset = currentOffset();
        const size_t rem = offset % alignment;
        if (rem == 0)
            return;
        const size_t padding = alignment - rem;
        if (padding > std::numeric_limits<size_t>::max() - offset)
            throw std::length_error("CodeSection alignment exceeds addressable size");
        emitZeros(padding);
    }

    // === Relocation tracking ===

    /// @brief Record a symbol-index relocation at the current logical offset.
    /// @param kind Encoding and arithmetic of the fixup.
    /// @param symbolIndex Existing target symbol-table index.
    /// @param addend Signed relocation addend.
    /// @param targetSection Optional concrete target-section classification.
    /// @throws std::out_of_range for an invalid symbol or source offset.
    [[maybe_unused]] void addRelocation(
        RelocKind kind,
        uint32_t symbolIndex,
        int64_t addend = 0,
        SymbolSection targetSection = SymbolSection::Undefined) {
        if (symbolIndex >= symbols_.count())
            throw std::out_of_range("CodeSection relocation symbol index is out of range");
        validateRelocationSourceRange(currentOffset(), kind);
        relocations_.push_back(
            Relocation{currentOffset(), kind, symbolIndex, addend, targetSection});
    }

    /// @brief Record a symbol-index relocation at a specific logical offset.
    /// @param offset Logical offset of the fixup field.
    /// @param kind Encoding and arithmetic of the fixup.
    /// @param symbolIndex Existing target symbol-table index.
    /// @param addend Signed relocation addend.
    /// @param targetSection Optional concrete target-section classification.
    /// @throws std::out_of_range for an invalid offset or symbol index.
    void addRelocationAt(size_t offset,
                         RelocKind kind,
                         uint32_t symbolIndex,
                         int64_t addend = 0,
                         SymbolSection targetSection = SymbolSection::Undefined) {
        if (offset < offsetBias_ || offset - offsetBias_ > bytes_.size())
            throw std::out_of_range("CodeSection relocation offset is out of range");
        if (symbolIndex >= symbols_.count())
            throw std::out_of_range("CodeSection relocation symbol index is out of range");
        validateRelocationSourceRange(offset, kind);
        relocations_.push_back(Relocation{offset, kind, symbolIndex, addend, targetSection});
    }

    /// @brief Record a current-position relocation to a concrete section offset.
    /// @details Object writers serialize this through a section-anchor symbol
    ///          plus addend, avoiding ambiguous name lookup for duplicate locals.
    /// @param kind Encoding and arithmetic of the fixup.
    /// @param targetSection Logical target-section classification.
    /// @param targetOffset Logical byte offset within that target section.
    /// @param addend Additional signed relocation addend.
    /// @throws std::invalid_argument for an undefined target section.
    void addSectionOffsetRelocation(RelocKind kind,
                                    SymbolSection targetSection,
                                    size_t targetOffset,
                                    int64_t addend = 0) {
        if (targetSection == SymbolSection::Undefined)
            throw std::invalid_argument(
                "CodeSection section-offset relocation requires a target section");
        validateRelocationSourceRange(currentOffset(), kind);
        Relocation rel{currentOffset(), kind, 0, addend, targetSection};
        rel.targetOffsetValid = true;
        rel.targetOffset = targetOffset;
        relocations_.push_back(rel);
    }

    /// @brief Record a current-position relocation to an exact section instance.
    /// @param kind Encoding and arithmetic of the fixup.
    /// @param target Concrete target section whose identity is recorded.
    /// @param targetSection Logical target-section classification.
    /// @param targetOffset Logical byte offset within @p target.
    /// @param addend Additional signed relocation addend.
    /// @throws std::invalid_argument for an undefined target classification.
    /// @throws std::out_of_range when @p targetOffset is outside @p target.
    void addSectionOffsetRelocation(RelocKind kind,
                                    const CodeSection &target,
                                    SymbolSection targetSection,
                                    size_t targetOffset,
                                    int64_t addend = 0) {
        if (targetSection == SymbolSection::Undefined)
            throw std::invalid_argument(
                "CodeSection section-offset relocation requires a target section");
        if (!target.containsOffsetRange(targetOffset, 0))
            throw std::out_of_range("CodeSection relocation target offset is out of range");
        validateRelocationSourceRange(currentOffset(), kind);
        Relocation rel{currentOffset(), kind, 0, addend, targetSection};
        rel.targetOffsetValid = true;
        rel.targetOffset = targetOffset;
        rel.targetSectionIdentityValid = true;
        rel.targetSectionIdentity = target.sectionIdentity();
        relocations_.push_back(rel);
    }

    /// @brief Record a section-offset relocation at a specific source offset.
    /// @param offset Logical source offset of the fixup.
    /// @param kind Encoding and arithmetic of the fixup.
    /// @param targetSection Logical target-section classification.
    /// @param targetOffset Logical offset within the target section class.
    /// @param addend Additional signed relocation addend.
    /// @throws std::invalid_argument for an undefined target section.
    /// @throws std::out_of_range when @p offset is outside this section.
    void addSectionOffsetRelocationAt(size_t offset,
                                      RelocKind kind,
                                      SymbolSection targetSection,
                                      size_t targetOffset,
                                      int64_t addend = 0) {
        if (targetSection == SymbolSection::Undefined)
            throw std::invalid_argument(
                "CodeSection section-offset relocation requires a target section");
        if (offset < offsetBias_ || offset - offsetBias_ > bytes_.size())
            throw std::out_of_range("CodeSection relocation offset is out of range");
        validateRelocationSourceRange(offset, kind);
        Relocation rel{offset, kind, 0, addend, targetSection};
        rel.targetOffsetValid = true;
        rel.targetOffset = targetOffset;
        relocations_.push_back(rel);
    }

    /// @brief Record an exact-section relocation at a specific source offset.
    /// @param offset Logical source offset of the fixup.
    /// @param kind Encoding and arithmetic of the fixup.
    /// @param target Concrete target section whose identity is recorded.
    /// @param targetSection Logical target-section classification.
    /// @param targetOffset Logical byte offset within @p target.
    /// @param addend Additional signed relocation addend.
    /// @throws std::invalid_argument for an undefined target classification.
    /// @throws std::out_of_range for an invalid source or target offset.
    void addSectionOffsetRelocationAt(size_t offset,
                                      RelocKind kind,
                                      const CodeSection &target,
                                      SymbolSection targetSection,
                                      size_t targetOffset,
                                      int64_t addend = 0) {
        if (targetSection == SymbolSection::Undefined)
            throw std::invalid_argument(
                "CodeSection section-offset relocation requires a target section");
        if (offset < offsetBias_ || offset - offsetBias_ > bytes_.size())
            throw std::out_of_range("CodeSection relocation offset is out of range");
        if (!target.containsOffsetRange(targetOffset, 0))
            throw std::out_of_range("CodeSection relocation target offset is out of range");
        validateRelocationSourceRange(offset, kind);
        Relocation rel{offset, kind, 0, addend, targetSection};
        rel.targetOffsetValid = true;
        rel.targetOffset = targetOffset;
        rel.targetSectionIdentityValid = true;
        rel.targetSectionIdentity = target.sectionIdentity();
        relocations_.push_back(rel);
    }

    // === Symbol management ===

    /// @brief Define a non-external symbol at the current logical offset.
    /// @param name Object symbol name.
    /// @param binding Local, global, or weak binding.
    /// @param section Concrete logical section containing the definition.
    /// @return Index of the new definition in this section's symbol table.
    /// @throws std::invalid_argument for external binding or undefined section.
    uint32_t defineSymbol(const std::string &name, SymbolBinding binding, SymbolSection section) {
        if (binding == SymbolBinding::External)
            throw std::invalid_argument("CodeSection defineSymbol cannot define External symbols");
        if (section == SymbolSection::Undefined)
            throw std::invalid_argument("CodeSection defineSymbol requires a concrete section");
        return symbols_.add(Symbol{name, binding, section, currentOffset(), 0});
    }

    /// @brief Find or declare an external, undefined symbol.
    /// @param name Symbol name referenced by a future relocation.
    /// @return Existing or newly allocated symbol-table index.
    uint32_t declareExternal(const std::string &name) {
        return symbols_.findOrAdd(name);
    }

    /// @brief Find any existing symbol by name or declare it as external.
    /// @param name Symbol name to intern.
    /// @return Existing or newly allocated symbol-table index.
    uint32_t findOrDeclareSymbol(const std::string &name) {
        return symbols_.findOrAdd(name);
    }

    // === Patch (for resolved internal branches) ===

    /// @brief Overwrite four emitted bytes with a little-endian value.
    /// @param offset Logical offset of the first destination byte.
    /// @param val Replacement 32-bit value.
    /// @throws std::out_of_range when the four-byte range is not emitted.
    void patch32LE(size_t offset, uint32_t val) {
        if (!containsOffsetRange(offset, 4))
            throw std::out_of_range("CodeSection patch32LE offset is out of range");
        const size_t physicalOffset = offset - offsetBias_;
        bytes_[physicalOffset] = static_cast<uint8_t>(val);
        bytes_[physicalOffset + 1] = static_cast<uint8_t>(val >> 8);
        bytes_[physicalOffset + 2] = static_cast<uint8_t>(val >> 16);
        bytes_[physicalOffset + 3] = static_cast<uint8_t>(val >> 24);
    }

    /// @brief Overwrite one emitted byte.
    /// @param offset Logical offset of the destination byte.
    /// @param val Replacement byte value.
    /// @throws std::out_of_range when @p offset is not emitted.
    [[maybe_unused]] void patch8(size_t offset, uint8_t val) {
        if (!containsOffsetRange(offset, 1))
            throw std::out_of_range("CodeSection patch8 offset is out of range");
        bytes_[offset - offsetBias_] = val;
    }

    /// @brief Replace an existing logical byte range without resizing the section.
    /// @details This constrained patch API is intended for writer-side addend
    ///          embedding and late fixups that already know their exact byte
    ///          payload. It preserves relocation and symbol offsets by rejecting
    ///          any range that would extend beyond emitted bytes.
    /// @param offset Logical section offset at which @p data should be written.
    /// @param data Replacement bytes. Empty ranges are accepted as no-ops.
    /// @throws std::out_of_range when the replacement range is not fully emitted.
    [[maybe_unused]] void patchBytes(size_t offset, const std::vector<uint8_t> &data) {
        if (data.empty())
            return;
        if (!containsOffsetRange(offset, data.size()))
            throw std::out_of_range("CodeSection patchBytes range is out of range");
        const size_t physicalOffset = offset - offsetBias_;
        std::copy(
            data.begin(), data.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(physicalOffset));
    }

    /// @brief Read four emitted bytes as a little-endian 32-bit value.
    /// @param offset Logical offset of the first source byte.
    /// @return Decoded unsigned value.
    /// @throws std::out_of_range when the four-byte range is not emitted.
    uint32_t read32LE(size_t offset) const {
        if (!containsOffsetRange(offset, 4))
            throw std::out_of_range("CodeSection read32LE offset is out of range");
        const size_t physicalOffset = offset - offsetBias_;
        return static_cast<uint32_t>(bytes_[physicalOffset]) |
               (static_cast<uint32_t>(bytes_[physicalOffset + 1]) << 8) |
               (static_cast<uint32_t>(bytes_[physicalOffset + 2]) << 16) |
               (static_cast<uint32_t>(bytes_[physicalOffset + 3]) << 24);
    }

    // === Accessors ===

    /// @brief Access the immutable physical byte payload.
    /// @return Reference to the payload vector; logical bias is not represented.
    const std::vector<uint8_t> &bytes() const {
        return bytes_;
    }

    /// @brief Access the mutable physical payload as a compatibility escape hatch.
    /// @details Prefer bounded patch APIs for updates that must not resize the
    ///          section and invalidate recorded symbol or relocation offsets.
    /// @return Mutable reference to the backing byte vector.
    std::vector<uint8_t> &mutableBytes() {
        return bytes_;
    }

    /// @brief Return the bias added to physical indexes to form logical offsets.
    /// @return Current logical offset bias.
    size_t logicalOffsetBias() const {
        return offsetBias_;
    }

    /// @brief Return the stable identity used by exact section-offset relocations.
    /// @return Nonzero section identity token.
    uint64_t sectionIdentity() const {
        return sectionIdentity_;
    }

    /// @brief Test the current identity and all identities inherited by copying.
    /// @param identity Identity token recorded in a relocation target.
    /// @return `true` when this section is the target or a copy-derived alias.
    bool matchesSectionIdentity(uint64_t identity) const {
        if (identity == sectionIdentity_)
            return true;
        return std::find(sectionIdentityAliases_.begin(),
                         sectionIdentityAliases_.end(),
                         identity) != sectionIdentityAliases_.end();
    }

    /// @brief Test whether a logical half-open range lies within emitted bytes.
    /// @param offset Logical start offset.
    /// @param width Number of bytes in the range; zero permits the end position.
    /// @return `true` when `[offset, offset + width)` maps into the payload.
    bool containsOffsetRange(size_t offset, size_t width) const {
        if (offset < offsetBias_)
            return false;
        const size_t physicalOffset = offset - offsetBias_;
        return physicalOffset <= bytes_.size() && width <= bytes_.size() - physicalOffset;
    }

    /// @brief Access all recorded relocations in insertion order.
    /// @return Immutable relocation vector.
    const std::vector<Relocation> &relocations() const {
        return relocations_;
    }

    /// @brief Access the section's symbol table.
    /// @return Immutable symbol table.
    const SymbolTable &symbols() const {
        return symbols_;
    }

    /// @brief Access the mutable symbol table for encoder-side updates.
    /// @return Mutable symbol table.
    SymbolTable &symbols() {
        return symbols_;
    }

    /// @brief Concatenate another section and rebase all of its metadata.
    /// @details Defined symbols are copied at rebased offsets, externals are
    ///          interned, relocation symbol indices are remapped, exact
    ///          self-targets are retargeted to this section, and unwind records
    ///          receive the corresponding new function-symbol indices.
    /// @param other Section to append; it remains unchanged.
    /// @throws std::out_of_range for malformed source metadata.
    /// @throws std::length_error if rebased sizes or offsets overflow.
    /// @throws std::logic_error for an offset relocation lacking exact identity.
    void appendSection(const CodeSection &other) {
        const size_t offsetBias = currentOffset();
        addSectionIdentityAlias(other.sectionIdentity());
        for (uint64_t alias : other.sectionIdentityAliases_)
            addSectionIdentityAlias(alias);
        reserveAdditionalBytes(other.bytes().size());
        if (other.relocations().size() > std::numeric_limits<size_t>::max() - relocations_.size())
            throw std::length_error("CodeSection relocation reservation exceeds addressable size");
        reserveRelocations(relocations_.size() + other.relocations().size());
        std::vector<uint32_t> symbolRemap(other.symbols().count(), 0);

        /// Convert a source logical offset to its concatenated destination
        /// position while checking source bounds and destination overflow.
        auto rebaseLogicalOffset = [&](size_t logicalOffset) -> size_t {
            if (logicalOffset < other.offsetBias_)
                throw std::out_of_range("CodeSection append source offset is before logical bias");
            const size_t physicalOffset = logicalOffset - other.offsetBias_;
            if (physicalOffset > other.bytes().size())
                throw std::out_of_range("CodeSection append source offset is out of range");
            if (physicalOffset > std::numeric_limits<size_t>::max() - offsetBias)
                throw std::length_error("CodeSection append offset exceeds addressable size");
            return offsetBias + physicalOffset;
        };

        for (uint32_t i = 1; i < other.symbols().count(); ++i) {
            const Symbol &sym = other.symbols().at(i);
            uint32_t mappedIndex = 0;

            if (sym.binding == SymbolBinding::External || sym.section == SymbolSection::Undefined) {
                mappedIndex = findOrDeclareSymbol(sym.name);
            } else {
                const size_t rebasedOffset = rebaseLogicalOffset(sym.offset);
                mappedIndex = symbols_.add(
                    Symbol{sym.name, sym.binding, sym.section, rebasedOffset, sym.size});
                Symbol &dst = symbols_.at(mappedIndex);
                dst.binding = sym.binding;
                dst.section = sym.section;
                dst.offset = rebasedOffset;
                dst.size = sym.size;
            }

            symbolRemap[i] = mappedIndex;
        }

        if (!other.bytes().empty())
            emitBytes(other.bytes().data(), other.bytes().size());

        for (const auto &reloc : other.relocations()) {
            if (reloc.symbolIndex >= symbolRemap.size())
                throw std::out_of_range(
                    "CodeSection append relocation symbol index is out of range");
            Relocation rebased = reloc;
            rebased.offset = rebaseLogicalOffset(reloc.offset);
            rebased.symbolIndex = symbolRemap[reloc.symbolIndex];
            const bool targetsAppendedIdentity =
                rebased.targetSectionIdentityValid &&
                other.matchesSectionIdentity(rebased.targetSectionIdentity);
            if (rebased.targetOffsetValid && !rebased.targetSectionIdentityValid) {
                throw std::logic_error(
                    "CodeSection append requires section identity for section-offset relocation");
            }
            if (rebased.targetOffsetValid && targetsAppendedIdentity) {
                rebased.targetOffset = rebaseLogicalOffset(reloc.targetOffset);
                rebased.targetSectionIdentityValid = true;
                rebased.targetSectionIdentity = sectionIdentity_;
            }
            relocations_.push_back(rebased);
        }

        for (const auto &entry : other.unwindEntries()) {
            if (entry.symbolIndex >= symbolRemap.size())
                throw std::out_of_range(
                    "CodeSection append compact unwind symbol index is out of range");
            CompactUnwindEntry rebased = entry;
            rebased.symbolIndex = symbolRemap[entry.symbolIndex];
            unwindEntries_.push_back(rebased);
        }

        for (const auto &entry : other.win64UnwindEntries()) {
            if (entry.symbolIndex >= symbolRemap.size())
                throw std::out_of_range(
                    "CodeSection append Win64 unwind symbol index is out of range");
            Win64UnwindEntry rebased = entry;
            rebased.symbolIndex = symbolRemap[entry.symbolIndex];
            win64UnwindEntries_.push_back(std::move(rebased));
        }

        for (const auto &entry : other.winArm64UnwindEntries()) {
            if (entry.symbolIndex >= symbolRemap.size())
                throw std::out_of_range(
                    "CodeSection append Windows ARM64 unwind symbol index is out of range");
            WinArm64UnwindEntry rebased = entry;
            rebased.symbolIndex = symbolRemap[entry.symbolIndex];
            winArm64UnwindEntries_.push_back(std::move(rebased));
        }
    }

    /// @brief Test whether the physical payload contains no bytes.
    /// @return `true` when the byte vector is empty, regardless of metadata.
    bool empty() const {
        return bytes_.empty();
    }

    // === Compact unwind tracking ===

    /// @brief Record Mach-O compact-unwind metadata for one function.
    /// @param entry Entry whose symbol index refers to this section's table.
    void addUnwindEntry(const CompactUnwindEntry &entry) {
        unwindEntries_.push_back(entry);
    }

    /// @brief Access all Mach-O compact-unwind entries.
    /// @return Immutable entries in insertion order.
    const std::vector<CompactUnwindEntry> &unwindEntries() const {
        return unwindEntries_;
    }

    // === Win64 unwind tracking ===

    /// @brief Record Windows x64 unwind metadata for one function.
    /// @param entry Entry moved into this section's metadata list.
    void addWin64UnwindEntry(Win64UnwindEntry entry) {
        win64UnwindEntries_.push_back(std::move(entry));
    }

    /// @brief Access all Windows x64 unwind entries.
    /// @return Immutable entries in insertion order.
    const std::vector<Win64UnwindEntry> &win64UnwindEntries() const {
        return win64UnwindEntries_;
    }

    /// @brief Record Windows ARM64 unwind metadata for one function.
    /// @param entry Entry moved into this section's metadata list.
    void addWinArm64UnwindEntry(WinArm64UnwindEntry entry) {
        winArm64UnwindEntries_.push_back(std::move(entry));
    }

    /// @brief Access all Windows ARM64 unwind entries.
    /// @return Immutable entries in insertion order.
    const std::vector<WinArm64UnwindEntry> &winArm64UnwindEntries() const {
        return winArm64UnwindEntries_;
    }

  private:
    /// @brief Remember an equivalent identity inherited through copy or append.
    /// @param identity Nonzero identity to add unless already represented.
    void addSectionIdentityAlias(uint64_t identity) {
        if (identity == 0 || identity == sectionIdentity_)
            return;
        if (std::find(sectionIdentityAliases_.begin(), sectionIdentityAliases_.end(), identity) ==
            sectionIdentityAliases_.end())
            sectionIdentityAliases_.push_back(identity);
    }

    /// @brief Retarget relocations that refer to this section's former identity.
    /// @param oldIdentity Identity token to replace.
    /// @param newIdentity Identity token to record in matching relocations.
    void retargetSelfIdentity(uint64_t oldIdentity, uint64_t newIdentity) {
        for (auto &rel : relocations_) {
            if (rel.targetSectionIdentityValid && rel.targetSectionIdentity == oldIdentity)
                rel.targetSectionIdentity = newIdentity;
        }
    }

    /// @brief Validate that a relocation source offset is within this section.
    /// @details Relocation offsets are recorded as logical section offsets, which
    ///          may include an offset bias during dry-run encoding. Relocations
    ///          are often registered before the placeholder bytes are emitted, so
    ///          this check intentionally accepts the current end offset and leaves
    ///          architecture-specific fixup-width validation to object writers.
    /// @param offset Logical section offset of the fixup field.
    /// @param kind Relocation kind; retained for call-site clarity and future diagnostics.
    /// @throws std::out_of_range if the source offset is outside emitted bytes.
    void validateRelocationSourceRange(size_t offset, RelocKind kind) const {
        (void)kind;
        if (offset < offsetBias_ || offset - offsetBias_ > bytes_.size())
            throw std::out_of_range("CodeSection relocation source range is out of bounds");
    }

    /// @brief Allocate the next process-unique section identity token.
    /// @return A nonzero monotonically allocated identity.
    /// @throws std::length_error if the atomic identity counter wraps to zero.
    static uint64_t allocateSectionIdentity() {
        const uint64_t id = nextSectionIdentity_.fetch_add(1, std::memory_order_relaxed);
        if (id == 0)
            throw std::length_error("CodeSection identity counter wrapped");
        return id;
    }

    std::vector<uint8_t> bytes_;
    std::vector<Relocation> relocations_;
    SymbolTable symbols_;
    std::vector<CompactUnwindEntry> unwindEntries_;
    std::vector<Win64UnwindEntry> win64UnwindEntries_;
    std::vector<WinArm64UnwindEntry> winArm64UnwindEntries_;
    size_t offsetBias_ = 0;
    std::vector<uint64_t> sectionIdentityAliases_;
    uint64_t sectionIdentity_ = 0;
    inline static std::atomic<uint64_t> nextSectionIdentity_{1};
};

} // namespace zanna::codegen::objfile
