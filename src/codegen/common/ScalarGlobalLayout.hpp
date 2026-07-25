//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ScalarGlobalLayout.hpp
// Purpose: Shared layout rules for writable scalar IL globals emitted into the
//          native `.data`/`__data` section. One source of truth for the
//          IL-type -> {size, float-ness} mapping and initializer bit parsing so
//          the AArch64 and x86-64 backends (binary and assembly paths) cannot
//          silently diverge.
// Key invariants:
//   - Size/float-ness derive solely from the IL scalar type kind.
//   - A zero size means "not a writable scalar" (string/void/error/resumetok).
// Ownership/Lifetime:
//   - Header-only, stateless; no allocation beyond the caller's strings.
// Links: codegen/aarch64/RodataPool.cpp, codegen/x86_64/passes/BinaryEmitPass.cpp,
//        codegen/x86_64/passes/EmitPass.cpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ScalarGlobalLayout.hpp
 * @brief Defines shared native layout and initializer parsing for writable
 *        scalar IL globals.
 *
 * Keeping these rules backend-independent ensures AArch64 and x86-64 binary
 * and assembly emitters agree on storage widths and raw initializer bits.
 */

#pragma once

#include "il/core/Type.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace zanna::codegen::common {

/// @brief Storage layout of a writable scalar global.
struct ScalarGlobalLayout {
    int sizeBytes = 0;    ///< 1/2/4/8; 0 means "not an emittable writable scalar".
    bool isFloat = false; ///< True for f64 (affects the asm directive / parse).
};

/// @brief Maps an IL scalar type kind to its writable-data layout.
/// @param kind IL type kind of the global.
/// @return One byte for `i1`, two for `i16`, four for `i32`, eight for
///         `i64`, pointers, and `f64`; unsupported kinds return a zero-sized
///         non-floating layout.
inline ScalarGlobalLayout scalarGlobalLayout(il::core::Type::Kind kind) noexcept {
    using Kind = il::core::Type::Kind;
    switch (kind) {
        case Kind::I1:
            return {1, false};
        case Kind::I16:
            return {2, false};
        case Kind::I32:
            return {4, false};
        case Kind::I64:
        case Kind::Ptr:
            return {8, false};
        case Kind::F64:
            return {8, true};
        default:
            return {0, false};
    }
}

/// @brief Parse an IL global initializer string into its raw little-endian bits.
/// @details Trims surrounding whitespace, then interprets the literal as a double
///          (when @p isFloat) or a base-0 signed integer. An empty initializer
///          yields zero. Parsing intentionally follows the permissive C
///          conversion routines: conversion stops at the first unrecognized
///          character and no diagnostic is produced here.  Integer consumers
///          truncate the returned 64-bit representation to their layout width.
/// @param init The IL global's initializer text (e.g. `"41"` or `"2.5"`);
///             may be empty or surrounded by ASCII whitespace.
/// @param isFloat Whether to parse as IEEE-754 double bits versus an integer.
/// @return Full 64-bit integer representation or the bit pattern of the
///         parsed IEEE-754 double.
inline std::uint64_t scalarGlobalRawBits(const std::string &init, bool isFloat) {
    std::string trimmed = init;
    const auto b = trimmed.find_first_not_of(" \t\r\n");
    const auto e = trimmed.find_last_not_of(" \t\r\n");
    trimmed = (b == std::string::npos) ? std::string() : trimmed.substr(b, e - b + 1);
    std::uint64_t raw = 0;
    if (!trimmed.empty()) {
        if (isFloat) {
            const double d = std::strtod(trimmed.c_str(), nullptr);
            std::memcpy(&raw, &d, sizeof(raw));
        } else {
            raw = static_cast<std::uint64_t>(std::strtoll(trimmed.c_str(), nullptr, 0));
        }
    }
    return raw;
}

} // namespace zanna::codegen::common
