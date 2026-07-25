//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/objfile/ObjFileWriterUtil.hpp
// Purpose: Shared encoding and padding utilities for ELF, Mach-O, and COFF
//          object file writers. Eliminates triplicated appendLE/alignUp/padTo
//          definitions across the three writer implementations.
// Key invariants:
//   - appendLE* functions append in little-endian byte order
//   - alignUp requires align == 0 or power of two
//   - padTo is a no-op if the buffer already meets or exceeds the target
// Links: ElfWriter.cpp, MachOWriter.cpp, CoffWriter.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ObjFileWriterUtil.hpp
 * @brief Defines shared endian, bounds, arithmetic, alignment, and output helpers.
 *
 * ELF, Mach-O, and COFF writers use these routines to serialize little-endian
 * fields and validate native-size or 64-bit layout calculations consistently.
 * Bool-return helpers emit writer-qualified diagnostics; primitive buffer
 * helpers either operate on caller-validated ranges or throw deterministically.
 */

#pragma once

#include "codegen/common/objfile/CodeSection.hpp"
#include "codegen/common/objfile/SymbolTable.hpp"

#include <cstddef>
#include <cstdint>
#include <ios>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zanna::codegen::objfile {

/// @brief Append a 16-bit value in little-endian byte order.
/// @param out Destination byte vector.
/// @param val Value to serialize.
inline void appendLE16(std::vector<uint8_t> &out, uint16_t val) {
    out.push_back(static_cast<uint8_t>(val));
    out.push_back(static_cast<uint8_t>(val >> 8));
}

/// @brief Append a 32-bit value in little-endian byte order.
/// @param out Destination byte vector.
/// @param val Value to serialize.
inline void appendLE32(std::vector<uint8_t> &out, uint32_t val) {
    out.push_back(static_cast<uint8_t>(val));
    out.push_back(static_cast<uint8_t>(val >> 8));
    out.push_back(static_cast<uint8_t>(val >> 16));
    out.push_back(static_cast<uint8_t>(val >> 24));
}

/// @brief Append a 64-bit value in little-endian byte order.
/// @param out Destination byte vector.
/// @param val Value to serialize.
inline void appendLE64(std::vector<uint8_t> &out, uint64_t val) {
    for (int i = 0; i < 8; ++i)
        out.push_back(static_cast<uint8_t>(val >> (i * 8)));
}

/// @brief Store a 16-bit value at @p p in little-endian byte order.
/// @param p Writable two-byte destination.
/// @param val Value to serialize.
inline void writeLE16(uint8_t *p, uint16_t val) {
    p[0] = static_cast<uint8_t>(val);
    p[1] = static_cast<uint8_t>(val >> 8);
}

/// @brief Store a 32-bit value at @p p in little-endian byte order.
/// @param p Writable four-byte destination.
/// @param val Value to serialize.
inline void writeLE32(uint8_t *p, uint32_t val) {
    p[0] = static_cast<uint8_t>(val);
    p[1] = static_cast<uint8_t>(val >> 8);
    p[2] = static_cast<uint8_t>(val >> 16);
    p[3] = static_cast<uint8_t>(val >> 24);
}

/// @brief Store a 64-bit value at @p p in little-endian byte order.
/// @param p Writable eight-byte destination.
/// @param val Value to serialize.
inline void writeLE64(uint8_t *p, uint64_t val) {
    for (int i = 0; i < 8; ++i)
        p[i] = static_cast<uint8_t>(val >> (i * 8));
}

/// @brief Load a 16-bit value from @p p in little-endian byte order.
/// @param p Readable two-byte source.
/// @return Decoded value.
inline uint16_t readLE16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

/// @brief Load a 32-bit value from @p p in little-endian byte order.
/// @param p Readable four-byte source.
/// @return Decoded value.
inline uint32_t readLE32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// @brief Load a 64-bit value from @p p in little-endian byte order.
/// @param p Readable eight-byte source.
/// @return Decoded value.
inline uint64_t readLE64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<uint64_t>(p[i]) << (i * 8);
    return v;
}

/// @brief Validate a fixed-width patch range before mutating an object buffer.
/// @details The putLE helpers are used after section/file offsets have been
///          computed. A stale offset should therefore fail as a writer bug with
///          a deterministic exception instead of corrupting adjacent bytes.
/// @param buf Destination object-file byte buffer.
/// @param offset First byte to patch.
/// @param width Number of bytes written.
/// @throws std::out_of_range if the patch range does not fit in @p buf.
inline void requirePatchRange(const std::vector<uint8_t> &buf, size_t offset, size_t width) {
    if (offset > buf.size() || width > buf.size() - offset)
        throw std::out_of_range("object writer patch range is out of bounds");
}

/// @brief Patch a 16-bit little-endian value at @p offset within @p buf.
/// @param buf Destination object buffer.
/// @param offset First byte to overwrite.
/// @param val Replacement value.
/// @throws std::out_of_range when the two-byte range is invalid.
inline void putLE16(std::vector<uint8_t> &buf, size_t offset, uint16_t val) {
    requirePatchRange(buf, offset, 2);
    buf[offset] = static_cast<uint8_t>(val);
    buf[offset + 1] = static_cast<uint8_t>(val >> 8);
}

/// @brief Patch a 32-bit little-endian value at @p offset within @p buf.
/// @param buf Destination object buffer.
/// @param offset First byte to overwrite.
/// @param val Replacement value.
/// @throws std::out_of_range when the four-byte range is invalid.
inline void putLE32(std::vector<uint8_t> &buf, size_t offset, uint32_t val) {
    requirePatchRange(buf, offset, 4);
    buf[offset] = static_cast<uint8_t>(val);
    buf[offset + 1] = static_cast<uint8_t>(val >> 8);
    buf[offset + 2] = static_cast<uint8_t>(val >> 16);
    buf[offset + 3] = static_cast<uint8_t>(val >> 24);
}

/// @brief Patch a 64-bit little-endian value at @p offset within @p buf.
/// @param buf Destination object buffer.
/// @param offset First byte to overwrite.
/// @param val Replacement value.
/// @throws std::out_of_range when the eight-byte range is invalid.
inline void putLE64(std::vector<uint8_t> &buf, size_t offset, uint64_t val) {
    requirePatchRange(buf, offset, 8);
    for (int i = 0; i < 8; ++i)
        buf[offset + static_cast<size_t>(i)] = static_cast<uint8_t>(val >> (i * 8));
}

/// @brief Verify that the byte range [@p off, @p off+@p len) fits within @p size.
/// @details Bool-return reader-friendly form (no error context); the writer-oriented
///          variants live above with their writerName / err stream signature.
/// @param off Start offset.
/// @param len Range length.
/// @param size Total buffer size.
/// @return `true` when the half-open range is contained without overflow.
inline bool checkedRange(size_t off, size_t len, size_t size) {
    return off <= size && len <= size - off;
}

/// @brief Add @p lhs + @p rhs into @p out, returning false on size_t overflow.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param out Receives the sum on success.
/// @return `true` when the sum fits in `size_t`.
inline bool checkedAdd(size_t lhs, size_t rhs, size_t &out) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs)
        return false;
    out = lhs + rhs;
    return true;
}

/// @brief Multiply @p lhs * @p rhs into @p out, returning false on size_t overflow.
/// @param lhs Left operand.
/// @param rhs Right operand.
/// @param out Receives the product on success.
/// @return `true` when the product fits in `size_t`.
inline bool checkedMul(size_t lhs, size_t rhs, size_t &out) {
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
        return false;
    out = lhs * rhs;
    return true;
}

/// @brief Convert a Symbol's logical section offset into its physical offset
///        within the section's emitted bytes.
/// @details Both ElfWriter and MachOWriter need to map the Symbol's
///          `logicalOffsetBias()`-relative offset down to the physical position
///          in the emitted byte vector. The writerName prefix lets each caller
///          identify itself in the error message ("ElfWriter:" vs "MachOWriter:").
/// @param section Section supplying logical bias and payload bounds.
/// @param sym Defined symbol to convert.
/// @param sectionName Section name used in diagnostics.
/// @param writerName Writer name used as the diagnostic prefix.
/// @param err Stream that receives bias or bounds diagnostics.
/// @param out Receives the physical byte offset.
/// @return `true` when the symbol lies within emitted section contents.
inline bool physicalSymbolValue(const CodeSection &section,
                                const Symbol &sym,
                                const char *sectionName,
                                const char *writerName,
                                std::ostream &err,
                                uint64_t &out) {
    if (sym.offset < section.logicalOffsetBias()) {
        err << writerName << ": symbol '" << sym.name << "' in " << sectionName
            << " is before the section logical offset bias\n";
        return false;
    }
    const size_t physicalOffset = sym.offset - section.logicalOffsetBias();
    if (physicalOffset > section.bytes().size()) {
        err << writerName << ": symbol '" << sym.name << "' in " << sectionName
            << " is outside section contents\n";
        return false;
    }
    out = static_cast<uint64_t>(physicalOffset);
    return true;
}

/// @brief Round @p val up to the next multiple of @p align.
/// @param val Value to align.
/// @param align Power-of-two alignment, or zero for no change.
/// @return The aligned value.
/// @throws std::invalid_argument when nonzero @p align is not a power of two.
/// @throws std::length_error when the rounded value overflows `size_t`.
inline size_t alignUp(size_t val, size_t align) {
    if (align == 0)
        return val;
    if ((align & (align - 1)) != 0)
        throw std::invalid_argument("alignUp: alignment must be a power of two");
    if (val > std::numeric_limits<size_t>::max() - (align - 1))
        throw std::length_error("alignUp: aligned value exceeds addressable size");
    return (val + align - 1) & ~(align - 1);
}

/// @brief Add @p a + @p b into @p out, writing a writer-prefixed error on overflow.
/// @details The convention is shared by every writer so a "size_t overflow"
///          message identifies which writer (ELF/Mach-O/COFF) caught it and
///          which conceptual quantity (@p what) overflowed.
/// @param a Left operand.
/// @param b Right operand.
/// @param writerName Diagnostic prefix.
/// @param what Human-readable quantity being calculated.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the sum.
/// @return `true` when the sum fits in `size_t`.
inline bool checkedAddSize(
    size_t a, size_t b, const char *writerName, const char *what, std::ostream &err, size_t &out) {
    if (a > std::numeric_limits<size_t>::max() - b) {
        err << writerName << ": " << what << " exceeds addressable size\n";
        return false;
    }
    out = a + b;
    return true;
}

/// @brief Multiply @p a * @p b into @p out with the writer-prefixed overflow error.
/// @param a Left operand.
/// @param b Right operand.
/// @param writerName Diagnostic prefix.
/// @param what Human-readable quantity being calculated.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the product.
/// @return `true` when the product fits in `size_t`.
inline bool checkedMulSize(
    size_t a, size_t b, const char *writerName, const char *what, std::ostream &err, size_t &out) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a) {
        err << writerName << ": " << what << " exceeds addressable size\n";
        return false;
    }
    out = a * b;
    return true;
}

/// @brief Align-up @p val to @p align with the writer-prefixed error on overflow
///        or non-power-of-two alignment.
/// @param val Value to align.
/// @param align Power-of-two alignment, or zero for no change.
/// @param writerName Diagnostic prefix.
/// @param what Human-readable quantity being calculated.
/// @param err Stream that receives alignment or overflow diagnostics.
/// @param out Receives the aligned value.
/// @return `true` when alignment is valid and representable by `size_t`.
inline bool checkedAlignUpSize(size_t val,
                               size_t align,
                               const char *writerName,
                               const char *what,
                               std::ostream &err,
                               size_t &out) {
    if (align == 0) {
        out = val;
        return true;
    }
    if ((align & (align - 1)) != 0) {
        err << writerName << ": " << what << " alignment is not a power of two\n";
        return false;
    }
    if (val > std::numeric_limits<size_t>::max() - (align - 1)) {
        err << writerName << ": " << what << " exceeds addressable size after alignment\n";
        return false;
    }
    out = (val + align - 1) & ~(align - 1);
    return true;
}

/// @brief 64-bit overflow-checked addition for object-file fields like Mach-O VAs.
/// @param a Left operand.
/// @param b Right operand.
/// @param writerName Diagnostic prefix.
/// @param what Human-readable quantity being calculated.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the sum.
/// @return `true` when the sum fits in `uint64_t`.
inline bool checkedAddU64(uint64_t a,
                          uint64_t b,
                          const char *writerName,
                          const char *what,
                          std::ostream &err,
                          uint64_t &out) {
    if (a > std::numeric_limits<uint64_t>::max() - b) {
        err << writerName << ": " << what << " exceeds 64-bit object-file range\n";
        return false;
    }
    out = a + b;
    return true;
}

/// @brief Multiply two 64-bit object-file quantities without wrapping.
/// @param a Left operand.
/// @param b Right operand.
/// @param writerName Diagnostic prefix.
/// @param what Human-readable quantity being calculated.
/// @param err Stream that receives an overflow diagnostic.
/// @param out Receives the product.
/// @return `true` when the product fits in `uint64_t`.
inline bool checkedMulU64(uint64_t a,
                          uint64_t b,
                          const char *writerName,
                          const char *what,
                          std::ostream &err,
                          uint64_t &out) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
        err << writerName << ": " << what << " exceeds 64-bit object-file range\n";
        return false;
    }
    out = a * b;
    return true;
}

/// @brief Align a 64-bit object-file quantity with checked arithmetic.
/// @param val Value to align.
/// @param align Power-of-two alignment, or zero for no change.
/// @param writerName Diagnostic prefix.
/// @param what Human-readable quantity being calculated.
/// @param err Stream that receives alignment or overflow diagnostics.
/// @param out Receives the aligned value.
/// @return `true` when alignment is valid and representable by `uint64_t`.
inline bool checkedAlignUpU64(uint64_t val,
                              uint64_t align,
                              const char *writerName,
                              const char *what,
                              std::ostream &err,
                              uint64_t &out) {
    if (align == 0) {
        out = val;
        return true;
    }
    if ((align & (align - 1)) != 0) {
        err << writerName << ": " << what << " alignment is not a power of two\n";
        return false;
    }
    if (val > std::numeric_limits<uint64_t>::max() - (align - 1)) {
        err << writerName << ": " << what << " exceeds 64-bit range after alignment\n";
        return false;
    }
    out = (val + align - 1) & ~(align - 1);
    return true;
}

/// @brief Narrow a 64-bit object-file size to the host's `size_t`.
/// @param value Value to narrow.
/// @param writerName Diagnostic prefix.
/// @param what Human-readable quantity being converted.
/// @param err Stream that receives a range diagnostic.
/// @param out Receives the native-size value.
/// @return `true` when @p value fits in `size_t`.
inline bool checkedSizeTFromU64(
    uint64_t value, const char *writerName, const char *what, std::ostream &err, size_t &out) {
    if (value > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        err << writerName << ": " << what << " exceeds addressable size\n";
        return false;
    }
    out = static_cast<size_t>(value);
    return true;
}

/// @brief Add a signed relocation addend to an unsigned section offset.
/// @param addend Signed encoder addend.
/// @param targetOffset Concrete target-section byte offset.
/// @param writerName Diagnostic prefix.
/// @param sectionName Source section containing the relocation.
/// @param relocOffset Source offset used to identify the relocation.
/// @param err Stream that receives a signed-range diagnostic.
/// @param out Receives the combined signed addend.
/// @return `true` when both conversion and addition fit in `int64_t`.
inline bool checkedSectionOffsetAddend(int64_t addend,
                                       size_t targetOffset,
                                       const char *writerName,
                                       const char *sectionName,
                                       size_t relocOffset,
                                       std::ostream &err,
                                       int64_t &out) {
    if (targetOffset > static_cast<size_t>(std::numeric_limits<int64_t>::max())) {
        err << writerName << ": relocation in " << sectionName << " at offset " << relocOffset
            << " has a section-offset addend outside int64 range\n";
        return false;
    }
    const int64_t signedOffset = static_cast<int64_t>(targetOffset);
    if ((addend > 0 && signedOffset > std::numeric_limits<int64_t>::max() - addend) ||
        (addend < 0 && signedOffset < std::numeric_limits<int64_t>::min() - addend)) {
        err << writerName << ": relocation in " << sectionName << " at offset " << relocOffset
            << " has a section-offset addend outside int64 range\n";
        return false;
    }
    out = signedOffset + addend;
    return true;
}

/// @brief Write a complete byte buffer after checking streamsize can represent it.
/// @param os Open destination stream.
/// @param data Complete object-file bytes.
/// @param writerName Diagnostic prefix.
/// @param path Destination path used in diagnostics.
/// @param err Stream that receives size or write-failure diagnostics.
/// @return `true` when the entire requested stream write succeeds.
inline bool checkedWriteAll(std::ostream &os,
                            const std::vector<uint8_t> &data,
                            const char *writerName,
                            const std::string &path,
                            std::ostream &err) {
    if (data.size() > static_cast<size_t>(std::numeric_limits<std::streamsize>::max())) {
        err << writerName << ": output file '" << path << "' exceeds stream write size limit\n";
        return false;
    }
    if (!data.empty())
        os.write(reinterpret_cast<const char *>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    if (!os) {
        err << writerName << ": write failed for " << path << "\n";
        return false;
    }
    return true;
}

/// @brief Pad @p out with zeros until it reaches @p target bytes.
/// @param out Destination byte vector.
/// @param target Minimum resulting vector size.
inline void padTo(std::vector<uint8_t> &out, size_t target) {
    if (out.size() < target)
        out.resize(target, 0);
}

} // namespace zanna::codegen::objfile
