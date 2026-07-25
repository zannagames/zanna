//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/TargetX64.hpp
// Purpose: Define physical registers, register classes, and SysV ABI metadata.
// Key invariants:
//   - sysvTarget() returns a singleton initialised once on first call.
//   - Caller/callee-saved sets and argument order are immutable after initialisation.
//   - kStackAlignment is always 16 bytes.
// Ownership/Lifetime:
//   - No heap ownership beyond the singleton TargetInfo; containers live for the process duration.
// Links: src/codegen/x86_64/TargetX64.cpp,
//        src/codegen/common/TargetInfoBase.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/TargetInfoBase.hpp"

#include <array>
#include <vector>

/// @file
/// @brief Declares the x86-64 register model and ABI target descriptions.
///
/// @details The declarations in this file form the target contract consumed by
///          x86-64 instruction selection, register allocation, call lowering,
///          and frame lowering. The two ABI descriptors share a physical
///          register namespace but expose different volatile-register sets,
///          argument orders, red-zone policies, and outgoing shadow-space
///          requirements.

namespace zanna::codegen::x64 {

/// @brief Enumerates the physical registers recognised by the x86-64 backend.
/// @details Covers the sixteen 64-bit general-purpose registers followed by the
///          sixteen XMM registers used for scalar floating-point and vector
///          operands. Enumerator values are backend identifiers, not hardware
///          encoding numbers; use the binary encoder's register mapping when
///          constructing instruction encodings.
enum class PhysReg {
    /// @brief Integer accumulator and integer return register.
    RAX,
    /// @brief Callee-saved general-purpose base register.
    RBX,
    /// @brief Count register and first Win64 integer argument register.
    RCX,
    /// @brief Data register used by division and argument passing.
    RDX,
    /// @brief Source-index register and second SysV integer argument register.
    RSI,
    /// @brief Destination-index register and first SysV integer argument register.
    RDI,
    /// @brief Extended general-purpose registers.
    R8,
    R9,
    R10,
    R11,
    R12,
    R13,
    R14,
    R15,
    /// @brief Frame-pointer register.
    RBP,
    /// @brief Stack-pointer register.
    RSP,
    /// @brief SIMD registers; XMM0 is the scalar floating-point return register.
    XMM0,
    XMM1,
    XMM2,
    XMM3,
    XMM4,
    XMM5,
    XMM6,
    XMM7,
    XMM8,
    XMM9,
    XMM10,
    XMM11,
    XMM12,
    XMM13,
    XMM14,
    XMM15
};

/// @brief Register classification used by the allocator and instruction selector.
/// @details @c GPR represents the integer register bank and @c XMM represents
///          the SIMD register bank. The class determines allocation pools,
///          spill-slot ranges, operand legality, and emitted instruction forms.
enum class RegClass { GPR, XMM };

// -----------------------------------------------------------------------------
// Architecture constants
// -----------------------------------------------------------------------------

/// @brief Size of a single spill slot or stack slot in bytes.
/// @details All stack-based values (spills, outgoing arguments, etc.) are
///          allocated in 8-byte increments to match the pointer size and
///          maintain natural alignment for scalar values.
inline constexpr int kSlotSizeBytes = 8;

/// @brief Required stack alignment at function call boundaries (bytes).
/// @details Both supported x86-64 ABIs require 16-byte stack alignment before
///          a CALL instruction executes. This constant is used by
///          frame lowering and call lowering to enforce alignment.
inline constexpr int kStackAlignment = 16;

/// @brief Page size used by stack-probing logic (bytes).
/// @details When allocating stack frames larger than this threshold, the code
///          generator emits stack probing code to ensure the guard page is
///          touched and stack overflow is detected properly.
inline constexpr int kPageSize = 4096;

/// @brief Disjoint placeholder index ranges for frame-slot classification.
/// @details Before frame lowering, RBP-relative frame slots are encoded as
///          negative 8-byte-stepped placeholder displacements. Three kinds of
///          slot share that encoding and are told apart purely by index range,
///          so the ranges must never overlap:
///            - alloca slots:    [0, kSpillSlotOffsetGPR)  (from SSA result ids)
///            - GPR spill slots: [kSpillSlotOffsetGPR, kSpillSlotOffsetXMM)
///            - XMM spill slots: [kSpillSlotOffsetXMM, kMaxFramePlaceholderIndex]
///          The previous scheme used a single +1000 offset for both spill
///          classes — disambiguated by guessing the class from a neighbouring
///          register operand — and silently collided with alloca slots past
///          index 999. The upper bound keeps placeholder displacements
///          representable: (index + 1) * kSlotSizeBytes must fit in int32.
inline constexpr int kSpillSlotOffsetGPR = 100'000'000;
/// @brief First placeholder index reserved for XMM-register spill slots.
inline constexpr int kSpillSlotOffsetXMM = 200'000'000;
/// @brief Largest placeholder index whose byte displacement fits in @c int32_t.
inline constexpr int kMaxFramePlaceholderIndex = 2147483647 / kSlotSizeBytes - 1;

/// @brief Maximum number of integer/pointer arguments passed in registers (SysV).
/// @details The SysV AMD64 ABI allows up to 6 integer arguments in registers
///          (RDI, RSI, RDX, RCX, R8, R9). Additional arguments go on the stack.
inline constexpr std::size_t kMaxGPRArgsSysV = 6;

/// @brief Maximum number of floating-point arguments passed in registers (SysV).
/// @details The SysV AMD64 ABI allows up to 8 floating-point arguments in
///          XMM registers (XMM0-XMM7). Additional arguments go on the stack.
inline constexpr std::size_t kMaxFPArgsSysV = 8;

/// @brief Maximum number of integer/pointer arguments passed in registers (Windows).
/// @details The Windows x64 ABI allows up to 4 integer arguments in registers
///          (RCX, RDX, R8, R9). Additional arguments go on the stack.
inline constexpr std::size_t kMaxGPRArgsWin64 = 4;

/// @brief Maximum number of floating-point arguments passed in registers (Windows).
/// @details The Windows x64 ABI allows up to 4 floating-point arguments in
///          XMM registers (XMM0-XMM3). Additional arguments go on the stack.
inline constexpr std::size_t kMaxFPArgsWin64 = 4;

#if defined(_WIN32)
/// @brief Host ABI's maximum number of register-passed integer arguments.
inline constexpr std::size_t kMaxGPRArgs = kMaxGPRArgsWin64;
/// @brief Host ABI's maximum number of register-passed floating-point arguments.
inline constexpr std::size_t kMaxFPArgs = kMaxFPArgsWin64;
#else
/// @brief Host ABI's maximum number of register-passed integer arguments.
inline constexpr std::size_t kMaxGPRArgs = kMaxGPRArgsSysV;
/// @brief Host ABI's maximum number of register-passed floating-point arguments.
inline constexpr std::size_t kMaxFPArgs = kMaxFPArgsSysV;
#endif

/// @brief Captures the architectural contract for an x86-64 ABI.
/// @details Extends the common register-save and argument-order metadata with
///          x86-64 ABI properties used to size frames and lower calls.
/// @invariant The published singleton instances are fully populated during
///            static initialization and are subsequently exposed as const data.
struct TargetInfo : zanna::codegen::common::TargetInfoBase<PhysReg, 6, 8> {
    /// @brief Whether the ABI specifies stack storage below the current stack pointer.
    /// @details Records ABI capability only; the current backend does not rely
    ///          on the SysV red zone when laying out frames.
    bool hasRedZone{true};
    /// @brief Number of meaningful entries at the start of @c intArgOrder.
    std::size_t maxGPRArgs{6};
    /// @brief Number of meaningful entries at the start of @c f64ArgOrder.
    std::size_t maxFPArgs{8};
    /// @brief Bytes of caller-provided home space required at each call site.
    /// @details Zero for SysV and 32 for Win64.
    std::size_t shadowSpace{0};
};

/// @brief Returns the singleton SysV AMD64 target description.
/// @return Const reference valid for the lifetime of the process.
[[nodiscard]] const TargetInfo &sysvTarget() noexcept;

/// @brief Returns the singleton Microsoft x64 target description.
/// @return Const reference valid for the lifetime of the process.
[[nodiscard]] const TargetInfo &win64Target() noexcept;

/// @brief Returns the platform-appropriate target description.
/// @details Selects the Win64 descriptor when compiling for Windows and the
///          SysV descriptor on all other supported hosts.
/// @return Const reference to one of the process-lifetime ABI singletons.
[[nodiscard]] const TargetInfo &hostTarget() noexcept;

/// @brief Determines if a physical register belongs to the general-purpose class.
/// @param reg Physical-register identifier to classify.
/// @return @c true for RAX through RSP; otherwise @c false.
[[nodiscard]] bool isGPR(PhysReg reg) noexcept;

/// @brief Determines if a physical register belongs to the XMM class.
/// @param reg Physical-register identifier to classify.
/// @return @c true for XMM0 through XMM15; otherwise @c false.
[[nodiscard]] bool isXMM(PhysReg reg) noexcept;

/// @brief Provides the canonical AT&T-syntax name of a physical register.
/// @param reg Physical-register identifier to render.
/// @return Process-lifetime string literal beginning with @c %, or
///         @c "%unknown" when @p reg is outside the declared enumeration.
[[nodiscard]] const char *regName(PhysReg reg) noexcept;

} // namespace zanna::codegen::x64
