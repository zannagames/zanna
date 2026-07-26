//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/PkgMD5.hpp
// Purpose: Self-contained MD5 digest for the packaging library.
//          Ported from src/runtime/text/rt_hash.c with all GC deps removed.
//
// Key invariants:
//   - No runtime (zanna_rt_*) dependencies — fully self-contained.
//   - Produces RFC 1321 compliant 16-byte digests.
//
// Ownership/Lifetime:
//   - Digest is written to caller-provided uint8_t[16] buffer.
//
// Links: src/runtime/text/rt_hash.c (original implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares one-shot RFC 1321 MD5 helpers for package metadata.
/// @details The binary form writes to caller-owned storage; the hexadecimal
///          form returns an owned string. Inputs are never retained.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace zanna::pkg {

/// @brief Compute MD5 digest of input data.
/// @param data Input bytes.
/// @param len Length of input.
/// @param digest Output buffer for 16-byte digest.
/// @throws std::runtime_error If `data` is null for a non-empty input.
void md5(const uint8_t *data, size_t len, uint8_t digest[16]);

/// @brief Compute MD5 digest and return as 32-char lowercase hex string.
/// @param data Input bytes.
/// @param len Length of input.
/// @return Hex-encoded MD5 digest string.
/// @throws std::runtime_error If `data` is null for a non-empty input.
std::string md5hex(const uint8_t *data, size_t len);

} // namespace zanna::pkg
