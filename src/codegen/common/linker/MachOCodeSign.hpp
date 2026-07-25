//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/MachOCodeSign.hpp
// Purpose: Ad-hoc code signature generation for Mach-O arm64 executables.
//          Builds an Apple-compatible embedded signature SuperBlob with
//          SHA-256 page hashes.
// Key invariants:
//   - Matches Apple's ad-hoc arm64 layout: CodeDirectory + Requirements +
//     empty BlobWrapper
//   - SHA-256 hash computed per target page size
//   - CodeDirectory version 0x20400 (execSegBase/Limit/Flags)
// Ownership/Lifetime:
//   - Stateless builder — returns owned blob
// Links: codegen/common/linker/MachOExeWriter.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file MachOCodeSign.hpp
 * @brief Declares dependency-free Apple ad-hoc signature generation for
 *        Mach-O executables.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace zanna::codegen::linker {

/// @brief Computes the exact embedded SuperBlob size for an executable.
/// @param codeLimit Number of file bytes covered by code-slot hashes.
/// @param identifier NUL-terminated CodeDirectory identifier payload.
/// @param pageSize Nonzero target page size used for code slots.
/// @return Required signature blob size in bytes.
/// @pre @p pageSize is a power of two and all derived counts fit their
///      32-bit CodeDirectory fields.
size_t estimateCodeSignatureSize(size_t codeLimit, const std::string &identifier, size_t pageSize);

/// @brief Builds an Apple-compatible ad-hoc embedded-signature SuperBlob.
/// @details The result contains a version `0x20400` CodeDirectory with the
///          `CS_ADHOC` flag, empty Requirements and BlobWrapper records, a
///          Requirements hash in special slot -2, a zero Info.plist hash in
///          slot -1, and a SHA-256 digest for every code page before
///          @p codeLimit.
/// @param file Complete executable bytes available before the signature blob.
/// @param codeLimit Number of leading bytes covered by code-slot hashes.
/// @param identifier CodeDirectory identifier, written with a trailing NUL.
/// @param textSegFileOff File offset recorded as `execSegBase`.
/// @param textSegFileSize File extent recorded as `execSegLimit`.
/// @param pageSize Nonzero power-of-two target page size.
/// @return Complete owned signature blob ready to append to `__LINKEDIT`.
/// @pre `codeLimit <= file.size()` and all derived CodeDirectory sizes/counts
///      fit their format fields.
std::vector<uint8_t> buildCodeSignature(const std::vector<uint8_t> &file,
                                        size_t codeLimit,
                                        const std::string &identifier,
                                        uint64_t textSegFileOff,
                                        uint64_t textSegFileSize,
                                        size_t pageSize);

} // namespace zanna::codegen::linker
