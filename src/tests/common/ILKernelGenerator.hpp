//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/common/ILKernelGenerator.hpp
// Purpose: Seeded generator of small, deterministic IL "kernel" programs
//          shaped like the code the native backends get wrong: checked
//          arithmetic chains, idx.chk results reused across trap branches,
//          loops with phi cycles, calls to leaf helpers, division and
//          remainder by constants, select-style diamonds, and switch dispatch.
//          Every program is a terminating loop that folds its work into one
//          accumulator and returns it (masked to 8 bits) as the exit code, so
//          the VM, the -O0 native build, and the -O2 native build can be
//          byte-compared without any I/O.
// Key invariants:
//   - The same seed always produces the same IL text.
//   - Generated programs verify, never trap, and terminate within a few
//     thousand loop iterations; every checked operation stays in range by
//     construction (operands are masked before they feed a checked op).
//   - Programs use only IL core opcodes (no runtime externs), so they run
//     identically on every backend and host.
// Ownership/Lifetime:
//   - Stateless free functions; the caller owns the returned text.
// Links: src/tests/common/ILGenerator.hpp (random straight-line IL),
//        src/tests/unit/codegen/test_differential_il_kernels.cpp,
//        src/tests/fuzz/fuzz_il_native_diff.cpp,
//        docs/internals/backend-codegen-review-2026-09.md (Phase 2.5)
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// @file
/// @brief Declares the seeded IL kernel program generator.

namespace zanna::tests {

/// @brief Kernel shapes the generator can compose into one loop body.
enum class KernelShape : unsigned {
    CheckedArith = 0, ///< Chain of iadd.ovf/isub.ovf/imul.ovf on masked operands.
    IdxChkReuse,      ///< idx.chk whose result is reused on both sides of a diamond.
    DivRemConst,      ///< sdiv/srem/udiv/urem .chk0 by non-zero constants.
    SwitchDispatch,   ///< cast.si_narrow.chk + switch.i32 with several cases.
    SelectDiamond,    ///< min/clamp diamonds joined through block parameters.
    LeafCall,         ///< Call to a generated leaf helper with its own branches.
    PhiCycleLoop,     ///< Inner loop whose block parameters rotate (phi cycle).
    BitMix,           ///< and/or/xor/shift mixing (never trapping).
    Count             ///< Number of shapes.
};

/// @brief One generated kernel program.
struct KernelProgram {
    std::uint64_t seed{0};           ///< Seed the program was derived from.
    std::string il;                  ///< Complete IL module text (`func @main() -> i64`).
    std::vector<KernelShape> shapes; ///< Shapes composed into the loop body, in order.
    std::uint64_t iterations{0};     ///< Outer loop trip count.
};

/// @brief Name of a shape for diagnostics.
[[nodiscard]] const char *kernelShapeName(KernelShape shape) noexcept;

/// @brief Generate the kernel program for @p seed.
/// @details The seed selects the trip count (64..512), the number of stages
///          (2..4), each stage's shape and constants, and the leaf helper's
///          body when a LeafCall stage is present. The result is a `main`
///          function whose loop threads `(sum, i)` through the stages and
///          returns `sum & 255`.
/// @param seed Any 64-bit value.
/// @return The program and its metadata.
[[nodiscard]] KernelProgram generateKernelProgram(std::uint64_t seed);

} // namespace zanna::tests
