//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/fastpaths/FastPathsInternal.hpp
// Purpose: Internal shared declarations for fast-path pattern matching.
// Key invariants:
//   - Each fast-path returns the lowered MFunction if matched, nullopt otherwise.
//   - Fast-path output must be semantically identical to generic lowering.
//   - Parameter registers are accessed via ABI-defined argument order.
// Ownership/Lifetime:
//   - FastPathContext borrows references to externally-owned state for the
//     duration of a single fast-path attempt; does not retain any lifetimes.
// Links: src/codegen/aarch64/FastPaths.hpp,
//        src/codegen/aarch64/fastpaths/FastPaths_Arithmetic.cpp,
//        src/codegen/aarch64/fastpaths/FastPaths_Call.cpp,
//        src/codegen/aarch64/fastpaths/FastPaths_Cast.cpp,
//        src/codegen/aarch64/fastpaths/FastPaths_Memory.cpp,
//        src/codegen/aarch64/fastpaths/FastPaths_Return.cpp
//
// This header is NOT part of the public API; include only from FastPaths_*.cpp.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/FastPaths.hpp"
#include "codegen/aarch64/FrameBuilder.hpp"
#include "codegen/aarch64/InstrLowering.hpp"
#include "codegen/aarch64/MachineIR.hpp"
#include "codegen/aarch64/OpcodeMappings.hpp"
#include "codegen/aarch64/TargetAArch64.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/OpcodeInfo.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @file
 * @brief Defines the private shared context and contracts for AArch64 fast-path lowering.
 *
 * Fast paths recognize narrowly constrained single-block IL shapes and return
 * a complete MIR function only when they can reproduce generic lowering
 * semantics. This header is private to the split fast-path implementation.
 */

namespace zanna::codegen::aarch64::fastpaths {

//===----------------------------------------------------------------------===//
// Common Constants
//===----------------------------------------------------------------------===//

/**
 * @brief Per-thread counter used to uniquify generated fast-path trap labels.
 * @details Defined by the arithmetic fast-path translation unit and isolated
 *          between parallel compilation threads.
 */
extern thread_local unsigned trapLabelCounter;

//===----------------------------------------------------------------------===//
// Context Structure
//===----------------------------------------------------------------------===//

/**
 * @brief Bundles borrowed function-lowering state for one fast-path attempt.
 *
 * @invariant Every reference describes the same function-lowering invocation.
 * @invariant Referenced objects and optional metadata maps outlive the context.
 */
struct FastPathContext {
    /// @brief Source IL function.
    const il::core::Function &fn;
    /// @brief Target ABI/register metadata.
    const TargetInfo &ti;
    /// @brief Caller-owned frame allocator.
    FrameBuilder &fb;
    /// @brief Caller-owned output MIR function.
    MFunction &mf;
    /// @brief Borrowed integer argument-register order.
    const std::array<PhysReg, kMaxGPRArgs> &argOrder;
    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths;
    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts;

    /// @brief Construct a fast-path context from the function being lowered.
    /// @param fn The IL function to lower.
    /// @param ti ABI and register information for the AArch64 target.
    /// @param fb Frame builder for stack slot allocation.
    /// @param mf Output MIR function being constructed.
    /// @param stringLiteralByteLengths Optional literal-length metadata.
    /// @param knownVarArgNamedArgCounts Optional direct-callee vararg metadata.
    FastPathContext(const il::core::Function &fn,
                    const TargetInfo &ti,
                    FrameBuilder &fb,
                    MFunction &mf,
                    const std::unordered_map<std::string, std::size_t> *stringLiteralByteLengths,
                    const std::unordered_map<std::string, std::size_t> *knownVarArgNamedArgCounts)
        : fn(fn), ti(ti), fb(fb), mf(mf), argOrder(ti.intArgOrder),
          stringLiteralByteLengths(stringLiteralByteLengths),
          knownVarArgNamedArgCounts(knownVarArgNamedArgCounts) {}

    /**
     * @brief Retrieves a mutable MIR output block by index.
     * @param idx Index into `mf.blocks`.
     * @return Reference to the indexed output block.
     * @pre `idx < mf.blocks.size()`.
     */
    MBasicBlock &bbOut(std::size_t idx) {
        return mf.blocks[idx];
    }

    /// @brief Get the register holding a value if it's a parameter.
    /// @param bb The basic block whose parameter list is checked.
    /// @param val The IL value to look up.
    /// @return The physical register if @p val is a parameter within GPR arg limits, nullopt
    /// otherwise.
    [[nodiscard]] std::optional<PhysReg> getValueReg(const il::core::BasicBlock &bb,
                                                     const il::core::Value &val) const {
        if (val.kind == il::core::Value::Kind::Temp) {
            int pIdx = indexOfParam(bb, val.id);
            if (pIdx >= 0 && static_cast<std::size_t>(pIdx) < kMaxGPRArgs) {
                return argOrder[static_cast<size_t>(pIdx)];
            }
        }
        return std::nullopt;
    }
};

//===----------------------------------------------------------------------===//
// Fast-Path Entry Points
//===----------------------------------------------------------------------===//

/// @brief Attempts simple parameter, constant, string, and address return patterns.
/// @param[in,out] ctx Active fast-path state.
/// @return Complete MIR function on a match, otherwise `std::nullopt`.
std::optional<MFunction> tryReturnFastPaths(FastPathContext &ctx);

/// @brief Attempts an alloca/store/load/return memory pattern.
/// @param[in,out] ctx Active fast-path state.
/// @return Complete MIR function on a match, otherwise `std::nullopt`.
std::optional<MFunction> tryMemoryFastPaths(FastPathContext &ctx);

/// @brief Attempts integer arithmetic, bitwise, immediate, and comparison patterns.
/// @param[in,out] ctx Active fast-path state.
/// @return Complete MIR function on a match, otherwise `std::nullopt`.
std::optional<MFunction> tryIntArithmeticFastPaths(FastPathContext &ctx);

/// @brief Attempts binary floating-point arithmetic patterns.
/// @param[in,out] ctx Active fast-path state.
/// @return Complete MIR function on a match, otherwise `std::nullopt`.
std::optional<MFunction> tryFPArithmeticFastPaths(FastPathContext &ctx);

/// @brief Attempts supported boolean and checked signed-narrowing patterns.
/// @param[in,out] ctx Active fast-path state.
/// @return Complete MIR function on a match, otherwise `std::nullopt`.
std::optional<MFunction> tryCastFastPaths(FastPathContext &ctx);

/// @brief Attempts a direct call whose result feeds the function return.
/// @param[in,out] ctx Active fast-path state.
/// @return Complete MIR function on a match, otherwise `std::nullopt`.
std::optional<MFunction> tryCallFastPaths(FastPathContext &ctx);

//===----------------------------------------------------------------------===//
// Utility Functions
//===----------------------------------------------------------------------===//

/// @brief Result of single-block fast-path validation.
/// @details Provides references to the front block and output MIR block if
///          validation succeeds, allowing callers to avoid repeated lookups.
struct SingleBlockFastPathSetup {
    const il::core::BasicBlock &bb; ///< Reference to the single block.
    MBasicBlock &bbMir;             ///< Reference to the output MIR block.

    /// @brief Construct a validated single-block fast-path setup.
    /// @details The references are borrowed from the fast-path context and remain valid for
    ///          the duration of the current lowering attempt. Keeping construction explicit
    ///          prevents accidental default-initialization of the reference members.
    /// @param bb Single IL basic block matched by the fast-path validator.
    /// @param bbMir Corresponding MIR basic block receiving fast-path output.
    SingleBlockFastPathSetup(const il::core::BasicBlock &bb, MBasicBlock &bbMir) noexcept
        : bb(bb), bbMir(bbMir) {}
};

/// @brief Validate context for single-block fast-path patterns.
/// @details Checks that the function has exactly one block with at least
///          minInstrs instructions. Returns references to avoid repeated
///          lookups in calling code.
/// @param ctx Fast-path context to validate.
/// @param minInstrs Minimum required instruction count (default: 2).
/// @param requireParams If true, block must have at least one parameter.
/// @return Setup struct if valid, nullopt otherwise.
[[nodiscard]] inline std::optional<SingleBlockFastPathSetup> validateSingleBlockFastPath(
    FastPathContext &ctx, std::size_t minInstrs = 2, bool requireParams = true) {
    if (ctx.fn.blocks.empty())
        return std::nullopt;
    if (ctx.fn.blocks.size() != 1)
        return std::nullopt;

    const auto &bb = ctx.fn.blocks.front();
    if (bb.instructions.size() < minInstrs)
        return std::nullopt;
    if (requireParams && bb.params.empty())
        return std::nullopt;

    return SingleBlockFastPathSetup{bb, ctx.bbOut(0)};
}

/// @brief Test whether @p bb contains any instruction that prevents fast-path lowering.
/// @details Control-flow terminators (`ret`/`br`/`cbr`) are exempted because every
///          basic block must end in one. For everything else the predicate consults
///          `OpcodeInfo::hasSideEffects` and `memoryEffects` and reports true if
///          either marks the instruction as side-effecting. A side-effecting block
///          can't be reproduced by a fast-path because the generic lowering is the
///          only path that emits the necessary safepoint/trap handling.
/// @param bb Basic block to inspect.
/// @return True if @p bb contains any side-effecting non-terminator instruction.
[[nodiscard]] inline bool hasSideEffects(const il::core::BasicBlock &bb) {
    for (const auto &instr : bb.instructions) {
        switch (instr.op) {
            case il::core::Opcode::Ret:
            case il::core::Opcode::Br:
            case il::core::Opcode::CBr:
                // Control flow terminators don't count as side effects
                continue;
            default:
                break;
        }

        const auto &info = il::core::getOpcodeInfo(instr.op);
        if (info.hasSideEffects)
            return true;

        if (il::core::memoryEffects(instr.op) != il::core::MemoryEffects::None)
            return true;
    }
    return false;
}

} // namespace zanna::codegen::aarch64::fastpaths
