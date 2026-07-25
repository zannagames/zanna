//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/TargetAArch64.hpp
// Purpose: Define physical registers, register classes, and target metadata for AArch64.
// Key invariants:
//   - Register enums map 1:1 to hardware registers (no offset encoding).
//   - TargetInfo singletons are immutable after initialization.
//   - AAPCS64 calling convention rules are encoded in TargetInfo fields.
// Ownership/Lifetime:
//   - No heap ownership; darwinTarget()/linuxTarget()/windowsTarget() return
//     references to static singletons that live for the duration of the program.
// Links: src/codegen/aarch64/TargetAArch64.cpp,
//        src/codegen/common/TargetInfoBase.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/TargetInfoBase.hpp"

#include <cstdint>
#include <optional>
#include <vector>

#ifdef _MSC_VER
#include <intrin.h>
#endif

/**
 * @file
 * @brief Defines AArch64 registers, ABI metadata, calling-convention queries, and immediate tests.
 *
 * Target singletons describe the three assembly/object formats supported by
 * the backend while sharing the register convention modeled by Zanna. The
 * header also exposes register classification/name helpers and constexpr
 * predicates used before selecting immediate instruction forms.
 */

namespace zanna::codegen::aarch64 {

/// @brief Physical register identifiers for AArch64.
///
/// Enumerates all 64-bit general purpose registers (X0-X30, SP) and floating-point/
/// SIMD registers (V0-V31) used by the AArch64 instruction set. These correspond
/// directly to the hardware register file.
///
/// ## General Purpose Registers
///
/// | Register | AAPCS64 Usage                                          |
/// |----------|--------------------------------------------------------|
/// | X0-X7    | Argument/result registers (caller-saved)               |
/// | X8       | Indirect result location register (caller-saved)       |
/// | X9-X15   | Temporary registers (caller-saved)                     |
/// | X16-X17  | Intra-procedure-call scratch registers (IP0/IP1)       |
/// | X18      | Platform register (reserved on some OSes, don't use)   |
/// | X19-X28  | Callee-saved registers                                 |
/// | X29      | Frame pointer (FP)                                     |
/// | X30      | Link register (LR, holds return address)               |
/// | SP       | Stack pointer (must be 16-byte aligned)                |
///
/// ## Floating-Point/SIMD Registers
///
/// | Register | AAPCS64 Usage                                          |
/// |----------|--------------------------------------------------------|
/// | V0-V7    | Argument/result registers (caller-saved)               |
/// | V8-V15   | Callee-saved (only lower 64 bits / D-register)         |
/// | V16-V31  | Temporary registers (caller-saved)                     |
///
/// @note We model V registers as their 64-bit D-register aliases since Zanna
///       currently only supports scalar floating-point (f64) values.
///
/// @see RegClass for distinguishing GPR vs FPR
/// @see isGPR(), isFPR() for register class queries
enum class PhysReg {
    X0,
    X1,
    X2,
    X3,
    X4,
    X5,
    X6,
    X7,
    X8, // Indirect result / frame pointer in some ABIs; treated as caller-saved here
    X9,
    X10,
    X11,
    X12,
    X13,
    X14,
    X15,
    X16,
    X17, // intra-proc call registers
    X18, // platform reserved on some OSes; do not allocate
    X19,
    X20,
    X21,
    X22,
    X23,
    X24,
    X25,
    X26,
    X27,
    X28,
    X29, // FP
    X30, // LR
    SP,
    // Floating-point / SIMD 64-bit lanes (we model D-registers)
    V0,
    V1,
    V2,
    V3,
    V4,
    V5,
    V6,
    V7,
    V8,
    V9,
    V10,
    V11,
    V12,
    V13,
    V14,
    V15,
    V16,
    V17,
    V18,
    V19,
    V20,
    V21,
    V22,
    V23,
    V24,
    V25,
    V26,
    V27,
    V28,
    V29,
    V30,
    V31,
};

/// @brief Register class discriminator for AArch64.
///
/// Distinguishes between general-purpose registers (GPR) and floating-point/
/// SIMD registers (FPR). This is essential for register allocation since the
/// two classes have separate register files and different allocation pools.
///
/// @note AArch64 has a clean separation between GPR and FPR - there are no
///       instructions that use both classes in a single operand position.
enum class RegClass {
    GPR, ///< General-purpose registers (X0-X30, SP)
    FPR  ///< Floating-point/SIMD registers (V0-V31 / D0-D31)
};

// =============================================================================
// AArch64 ABI Constants
// =============================================================================

/// @brief Size of a stack slot in bytes (8 bytes for 64-bit values).
inline constexpr int kSlotSizeBytes = 8;

/// @brief Required stack alignment in bytes (16-byte alignment per AAPCS64).
inline constexpr int kStackAlignment = 16;

/// @brief Maximum number of GPR arguments passed in registers (x0-x7).
inline constexpr std::size_t kMaxGPRArgs = 8;

/// @brief Maximum number of FPR arguments passed in registers (v0-v7).
inline constexpr std::size_t kMaxFPRArgs = 8;

/// @brief Primary scratch register for codegen (not used for allocation, safe to clobber).
inline constexpr PhysReg kScratchGPR = PhysReg::X9;

/// @brief Secondary scratch register for post-RA helper sequences.
///
/// Some late emission paths need a second temporary while @c kScratchGPR holds
/// either the base value or a materialized immediate. Reserve IP0 globally so
/// those helpers never clobber an allocated value.
inline constexpr PhysReg kScratchGPR2 = PhysReg::X16;

/// @brief Tertiary scratch register for register allocation emergency reloads.
inline constexpr PhysReg kScratchGPR3 = PhysReg::X17;

/// @brief Scratch FPR register for codegen (not used for allocation).
inline constexpr PhysReg kScratchFPR = PhysReg::V16;

/// @brief Secondary scratch FPR register for register allocation emergency reloads.
inline constexpr PhysReg kScratchFPR2 = PhysReg::V17;

// =============================================================================
// Target Information
// =============================================================================

/// @brief Identifies the target OS ABI for assembly emission.
/// Controls symbol mangling (underscore prefix) and format-specific directives.
///
///  - Darwin:  '_' prefix on symbols; Mach-O format; no ELF directives.
///  - Linux:   No prefix; ELF format; .type/.size directives required.
///  - Windows: No prefix; PE/COFF format; no ELF .type/.size directives.
///             Register conventions are identical to Linux AAPCS64.
enum class ABIFormat {
    Darwin,  ///< macOS/iOS; symbols prefixed with '_', Mach-O format.
    Linux,   ///< Linux ELF; no symbol prefix, .type/.size required.
    Windows, ///< Windows ARM64; no symbol prefix, PE/COFF format (no .type/.size).
};

/**
 * @brief Describes AArch64 register conventions and format-specific codegen features.
 *
 * The inherited base stores caller/callee-saved sets, argument order, return
 * registers, and stack alignment. This derived type selects object-format
 * syntax, hardening instructions, and the variadic-tail rule.
 *
 * @invariant Register lists and return registers belong to the appropriate
 *            GPR/FPR class.
 * @invariant Published singleton instances are immutable after initialization.
 */
struct TargetInfo : zanna::codegen::common::TargetInfoBase<PhysReg, kMaxGPRArgs, kMaxFPRArgs> {
    ABIFormat abiFormat = ABIFormat::Darwin;       ///< Assembly/object ABI format.
    bool emitBranchTargetIdentification{true};    ///< Emit `bti c` at eligible entries.
    bool emitReturnAddressSigning{true};          ///< Emit PAC/AUT return-address protection.
    /// True when anonymous variadic arguments are passed on the stack instead of
    /// continuing through the normal AAPCS64 register banks. This is the Darwin
    /// AArch64 C ABI rule; Linux and Windows keep using their regular argument
    /// locations for variadic calls.
    bool variadicTailOnStack{true};

    /**
     * @brief Tests whether this target uses Linux ELF syntax.
     * @return `true` exactly when `abiFormat` is `ABIFormat::Linux`.
     */
    [[nodiscard]] bool isLinux() const noexcept {
        return abiFormat == ABIFormat::Linux;
    }

    /**
     * @brief Tests whether this target uses Windows ARM64 PE/COFF syntax.
     * @return `true` exactly when `abiFormat` is `ABIFormat::Windows`.
     */
    [[nodiscard]] bool isWindows() const noexcept {
        return abiFormat == ABIFormat::Windows;
    }

    /**
     * @brief Reports whether anonymous variadic arguments begin on the stack.
     * @return The target's `variadicTailOnStack` policy.
     */
    [[nodiscard]] bool usesStackVariadicTail() const noexcept {
        return variadicTailOnStack;
    }

    /**
     * @brief Reports whether branch-target identification is enabled.
     * @return The target's `emitBranchTargetIdentification` flag.
     */
    [[nodiscard]] bool hasBranchTargetIdentification() const noexcept {
        return emitBranchTargetIdentification;
    }

    /**
     * @brief Reports whether return-address signing is enabled.
     * @return The target's `emitReturnAddressSigning` flag.
     */
    [[nodiscard]] bool hasReturnAddressSigning() const noexcept {
        return emitReturnAddressSigning;
    }
};

// =============================================================================
// Calling Convention Abstraction
// =============================================================================

/**
 * @brief Provides read-only queries over a target's modeled AArch64 calling convention.
 *
 * The object retains a non-owning reference and translates argument-bank
 * indices into registers or stack placement.
 *
 * @invariant The referenced `TargetInfo` outlives this object.
 */
class CallingConvention {
  public:
    /**
     * @brief Binds calling-convention queries to target metadata.
     * @param ti Target description retained by reference.
     */
    explicit CallingConvention(const TargetInfo &ti) noexcept : ti_(ti) {}

    // =========================================================================
    // Argument Passing
    // =========================================================================

    /**
     * @brief Gets the register assigned to an integer-bank argument.
     * @param index Zero-based index within the integer argument bank.
     * @return Physical register, or `std::nullopt` when the argument is stack-passed.
     */
    [[nodiscard]] std::optional<PhysReg> getIntArgReg(std::size_t index) const noexcept {
        if (index < ti_.intArgOrder.size())
            return ti_.intArgOrder[index];
        return std::nullopt;
    }

    /**
     * @brief Gets the register assigned to a floating-point-bank argument.
     * @param index Zero-based index within the FP argument bank.
     * @return Physical register, or `std::nullopt` when the argument is stack-passed.
     */
    [[nodiscard]] std::optional<PhysReg> getFPArgReg(std::size_t index) const noexcept {
        if (index < ti_.f64ArgOrder.size())
            return ti_.f64ArgOrder[index];
        return std::nullopt;
    }

    /**
     * @brief Tests whether an integer-bank argument has a register location.
     * @param index Zero-based integer-bank argument index.
     * @return `true` when @p index is present in `intArgOrder`.
     */
    [[nodiscard]] bool isIntArgInReg(std::size_t index) const noexcept {
        return index < ti_.intArgOrder.size();
    }

    /**
     * @brief Tests whether an FP-bank argument has a register location.
     * @param index Zero-based FP-bank argument index.
     * @return `true` when @p index is present in `f64ArgOrder`.
     */
    [[nodiscard]] bool isFPArgInReg(std::size_t index) const noexcept {
        return index < ti_.f64ArgOrder.size();
    }

    /**
     * @brief Returns the configured integer argument-register capacity.
     * @return Number of entries in `intArgOrder`.
     */
    [[nodiscard]] std::size_t maxIntArgsInRegs() const noexcept {
        return ti_.intArgOrder.size();
    }

    /**
     * @brief Returns the configured FP argument-register capacity.
     * @return Number of entries in `f64ArgOrder`.
     */
    [[nodiscard]] std::size_t maxFPArgsInRegs() const noexcept {
        return ti_.f64ArgOrder.size();
    }

    // =========================================================================
    // Return Values
    // =========================================================================

    /**
     * @brief Returns the integer/pointer result register.
     * @return Target's configured `intReturnReg`.
     */
    [[nodiscard]] PhysReg getIntReturnReg() const noexcept {
        return ti_.intReturnReg;
    }

    /**
     * @brief Returns the scalar floating-point result register.
     * @return Target's configured `f64ReturnReg`.
     */
    [[nodiscard]] PhysReg getFPReturnReg() const noexcept {
        return ti_.f64ReturnReg;
    }

    // =========================================================================
    // Stack Layout
    // =========================================================================

    /**
     * @brief Returns the target's required stack alignment.
     * @return Alignment in bytes.
     */
    [[nodiscard]] unsigned getStackAlignment() const noexcept {
        return ti_.stackAlignment;
    }

    /**
     * @brief Returns the backend's fixed scalar stack-slot size.
     * @return `kSlotSizeBytes`.
     */
    [[nodiscard]] static constexpr int getSlotSize() noexcept {
        return kSlotSizeBytes;
    }

  private:
    const TargetInfo &ti_;
};

/**
 * @brief Returns the immutable Darwin AArch64 target singleton.
 * @return Reference with static storage duration.
 */
[[nodiscard]] const TargetInfo &darwinTarget() noexcept;

/**
 * @brief Returns the immutable Linux AArch64 ELF target singleton.
 * @return Reference with static storage duration.
 */
[[nodiscard]] const TargetInfo &linuxTarget() noexcept;

/**
 * @brief Returns the immutable Windows ARM64 PE/COFF target singleton.
 * @return Reference with static storage duration.
 */
[[nodiscard]] const TargetInfo &windowsTarget() noexcept;

/**
 * @brief Tests whether an enumerator belongs to the GPR register bank.
 * @param reg Physical-register value to classify.
 * @return `true` for `X0` through `X30` and `SP`; otherwise `false`.
 */
[[nodiscard]] bool isGPR(PhysReg reg) noexcept;

/**
 * @brief Tests whether an enumerator belongs to the FP/SIMD register bank.
 * @param reg Physical-register value to classify.
 * @return `true` for `V0` through `V31`; otherwise `false`.
 */
[[nodiscard]] bool isFPR(PhysReg reg) noexcept;

/**
 * @brief Returns the lowercase assembly spelling of a physical register.
 *
 * GPRs use `xN`/`sp`; FP/SIMD registers use `vN`. Operand-size-specific
 * emitters may derive `dN` where scalar binary64 syntax requires it.
 *
 * @param reg Physical-register value to name.
 * @return Pointer to static spelling, or `"unknown"` for an invalid value.
 */
[[nodiscard]] const char *regName(PhysReg reg) noexcept;

// =============================================================================
// Immediate Value Validation
// =============================================================================

/**
 * @brief Tests an unshifted unsigned 12-bit immediate.
 * @param imm Candidate integer value.
 * @return `true` for the inclusive range `[0, 4095]`.
 */
[[nodiscard]] constexpr bool isUImm12(long long imm) noexcept {
    return imm >= 0 && imm <= 4095;
}

/**
 * @brief Tests a signed 9-bit unscaled load/store offset.
 * @param imm Candidate byte offset.
 * @return `true` for the inclusive range `[-256, 255]`.
 */
[[nodiscard]] constexpr bool isSImm9(long long imm) noexcept {
    return imm >= -256 && imm <= 255;
}

/**
 * @brief Tests an unsigned 12-bit scaled load/store byte offset.
 *
 * @param imm Candidate non-negative byte offset.
 * @param scale Positive access-size scale in bytes.
 * @return `true` when @p imm is divisible by @p scale and the quotient is at
 *         most 4095.
 * @pre `scale > 0`.
 */
[[nodiscard]] constexpr bool isScaledUImm12(long long imm, int scale) noexcept {
    if (imm < 0 || imm % scale != 0)
        return false;
    return (imm / scale) <= 4095;
}

/**
 * @brief Tests a 64-bit scalar shift amount.
 * @param imm Candidate shift count.
 * @return `true` for the inclusive range `[0, 63]`.
 */
[[nodiscard]] constexpr bool isValidShiftAmount(long long imm) noexcept {
    return imm >= 0 && imm <= 63;
}

/**
 * @brief Tests the simple, unshifted single-instruction move cases used by this backend.
 *
 * @param imm Candidate signed value.
 * @return `true` for `[0, 65535]` (`MOVZ`) or `[-65536, -1]` (`MOVN`);
 *         otherwise `false`.
 * @note This conservative predicate does not recognize shifted 16-bit chunks
 *       or logical-immediate aliases.
 */
[[nodiscard]] constexpr bool isSimpleMovImm(long long imm) noexcept {
    // Positive values that fit in an unsigned 16-bit immediate (MOVZ Xd, #imm16).
    if (imm >= 0 && imm <= 0xFFFF)
        return true;
    // Negative values where ~imm fits in 16 bits (MOVN Xd, #(~imm)).
    if (imm < 0 && imm >= -0x10000)
        return true;
    return false;
}

/**
 * @brief Tests whether the backend's simple move path rejects an immediate.
 * @param imm Candidate signed value.
 * @return Logical negation of `isSimpleMovImm(imm)`.
 */
[[nodiscard]] constexpr bool needsWideImmSequence(long long imm) noexcept {
    return !isSimpleMovImm(imm);
}

/**
 * @brief Tests whether a 64-bit value is encodable as an AArch64 logical immediate.
 *
 * Each legal element width is tried. The value must replicate one element and
 * that element's circular bit sequence must contain exactly one run of ones.
 * Architectural all-zero and all-one exclusions are enforced.
 *
 * @param imm Unsigned 64-bit value to test.
 * @return `true` when @p imm can be used directly by `AND`/`ORR`/`EOR`.
 */
[[nodiscard]] inline bool isLogicalImmediate(uint64_t imm) noexcept {
    // 0 and ~0 are never valid logical immediates.
    if (imm == 0 || imm == ~uint64_t(0))
        return false;

    // Try each element size from 2 up to 64 bits.
    // For each size N: check whether imm is a consistent replication of an
    // N-bit chunk, and whether that chunk is a contiguous run of 1-bits
    // (possibly rotated to wrap from MSB to LSB).
    for (int N : {2, 4, 8, 16, 32, 64}) {
        const uint64_t mask = (N == 64) ? ~uint64_t(0) : ((uint64_t(1) << N) - 1);
        const uint64_t elem = imm & mask;

        // Skip all-zero elements.
        if (elem == 0)
            continue;

        // Verify all N-bit chunks of imm are identical to elem.
        if (N < 64) {
            uint64_t replicated = elem;
            for (int shift = N; shift < 64; shift += N)
                replicated |= (elem << shift);
            if (replicated != imm)
                continue;
        }

        // elem is the representative N-bit pattern.
        // Check that its bits form a single contiguous run (possibly rotated).
        //
        // Method: rotate elem left by 1 within N bits, XOR with original.
        // A contiguous run has exactly 2 transitions (one 0→1, one 1→0)
        // in the circular bit sequence, so popcount(elem XOR rotate(elem, 1)) == 2.
        //
        // Special case: if elem == mask (all ones in N bits), skip — it would
        // replicate to ~0 which is excluded, but for safety skip anyway.
        if (elem == mask)
            continue;

        const uint64_t rotLeft1 = ((elem << 1) | (elem >> (N - 1))) & mask;
        const uint64_t transitions = elem ^ rotLeft1;
#ifdef _MSC_VER
        if (__popcnt64(transitions) == 2)
#else
        if (__builtin_popcountll(transitions) == 2)
#endif
            return true;
    }
    return false;
}

} // namespace zanna::codegen::aarch64
