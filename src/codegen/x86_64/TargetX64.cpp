//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/TargetX64.cpp
// Purpose: Materialise the SysV AMD64 target description used by the Zanna
//          Machine IR pipeline and surface helper queries about register
//          classes and names.
// Key invariants:
//   - The singleton target descriptor is initialised once with ABI-compliant register sets.
//   - All sets and argument orders are immutable after initialisation.
// Ownership/Lifetime:
//   - All resources live in static storage; callers do not own or free them.
// Links: src/codegen/x86_64/TargetX64.hpp,
//        src/codegen/common/TargetInfoBase.hpp
//
//===----------------------------------------------------------------------===//

#include "TargetX64.hpp"

/// @file
/// @brief Defines the x86-64 ABI descriptors and register-query helpers.

namespace zanna::codegen::x64 {

namespace {

/// @brief Construct the SysV AMD64 target description for the backend.
///
/// @details Populates the @ref TargetInfo structure with register save
///          conventions, argument passing order, return registers, and stack
///          alignment information according to the System V ABI.  The helper is
///          evaluated exactly once to initialise the translation unit's static
///          singleton and the resulting object is treated as read-only.
///
/// @return Fully initialised SysV target descriptor.
TargetInfo makeSysVTarget() {
    TargetInfo info{};
    info.callerSavedGPR = {
        PhysReg::RAX,
        PhysReg::RDI,
        PhysReg::RSI,
        PhysReg::RDX,
        PhysReg::RCX,
        PhysReg::R8,
        PhysReg::R9,
        PhysReg::R10,
        PhysReg::R11,
    };
    info.calleeSavedGPR = {
        PhysReg::RBX,
        PhysReg::R12,
        PhysReg::R13,
        PhysReg::R14,
        PhysReg::R15,
        PhysReg::RBP,
    };
    info.callerSavedFPR = {
        PhysReg::XMM0,
        PhysReg::XMM1,
        PhysReg::XMM2,
        PhysReg::XMM3,
        PhysReg::XMM4,
        PhysReg::XMM5,
        PhysReg::XMM6,
        PhysReg::XMM7,
        PhysReg::XMM8,
        PhysReg::XMM9,
        PhysReg::XMM10,
        PhysReg::XMM11,
        PhysReg::XMM12,
        PhysReg::XMM13,
        PhysReg::XMM14,
        PhysReg::XMM15,
    };
    info.calleeSavedFPR = {};
    info.intArgOrder = {
        PhysReg::RDI,
        PhysReg::RSI,
        PhysReg::RDX,
        PhysReg::RCX,
        PhysReg::R8,
        PhysReg::R9,
    };
    info.f64ArgOrder = {
        PhysReg::XMM0,
        PhysReg::XMM1,
        PhysReg::XMM2,
        PhysReg::XMM3,
        PhysReg::XMM4,
        PhysReg::XMM5,
        PhysReg::XMM6,
        PhysReg::XMM7,
    };
    info.intReturnReg = PhysReg::RAX;
    info.f64ReturnReg = PhysReg::XMM0;
    info.stackAlignment = 16U;
    info.hasRedZone = true; // Phase A: do not rely on red zone.
    info.maxGPRArgs = kMaxGPRArgsSysV;
    info.maxFPArgs = kMaxFPArgsSysV;
    info.shadowSpace = 0;
    return info;
}

/// @brief Construct the Windows x64 target description for the backend.
///
/// @details Populates the @ref TargetInfo structure with register save
///          conventions, argument passing order, return registers, and stack
///          alignment information according to the Microsoft x64 ABI.
///
/// @return Fully initialised Windows x64 target descriptor.
TargetInfo makeWin64Target() {
    TargetInfo info{};
    // Windows x64: RAX, RCX, RDX, R8, R9, R10, R11 are caller-saved (volatile)
    info.callerSavedGPR = {
        PhysReg::RAX,
        PhysReg::RCX,
        PhysReg::RDX,
        PhysReg::R8,
        PhysReg::R9,
        PhysReg::R10,
        PhysReg::R11,
    };
    // Windows x64: RBX, RBP, RDI, RSI, R12-R15 are callee-saved (non-volatile)
    info.calleeSavedGPR = {
        PhysReg::RBX,
        PhysReg::RBP,
        PhysReg::RDI,
        PhysReg::RSI,
        PhysReg::R12,
        PhysReg::R13,
        PhysReg::R14,
        PhysReg::R15,
    };
    // Windows x64: XMM0-XMM5 are caller-saved (volatile)
    info.callerSavedFPR = {
        PhysReg::XMM0,
        PhysReg::XMM1,
        PhysReg::XMM2,
        PhysReg::XMM3,
        PhysReg::XMM4,
        PhysReg::XMM5,
    };
    // Windows x64: XMM6-XMM15 are callee-saved (non-volatile)
    info.calleeSavedFPR = {
        PhysReg::XMM6,
        PhysReg::XMM7,
        PhysReg::XMM8,
        PhysReg::XMM9,
        PhysReg::XMM10,
        PhysReg::XMM11,
        PhysReg::XMM12,
        PhysReg::XMM13,
        PhysReg::XMM14,
        PhysReg::XMM15,
    };
    // Windows x64: RCX, RDX, R8, R9 for first 4 integer args
    info.intArgOrder = {
        PhysReg::RCX,
        PhysReg::RDX,
        PhysReg::R8,
        PhysReg::R9,
        PhysReg::RAX, // unused padding (only 4 args in regs)
        PhysReg::RAX, // unused padding
    };
    // Windows x64: XMM0-XMM3 for first 4 float args
    info.f64ArgOrder = {
        PhysReg::XMM0,
        PhysReg::XMM1,
        PhysReg::XMM2,
        PhysReg::XMM3,
        PhysReg::XMM0, // unused padding (only 4 args in regs)
        PhysReg::XMM0, // unused padding
        PhysReg::XMM0, // unused padding
        PhysReg::XMM0, // unused padding
    };
    info.intReturnReg = PhysReg::RAX;
    info.f64ReturnReg = PhysReg::XMM0;
    info.stackAlignment = 16U;
    info.hasRedZone = false; // Windows x64 has no red zone
    info.maxGPRArgs = kMaxGPRArgsWin64;
    info.maxFPArgs = kMaxFPArgsWin64;
    info.shadowSpace = 32; // 32-byte shadow space required
    return info;
}

/// @brief Process-lifetime storage for the immutable SysV ABI descriptor.
TargetInfo sysvTargetInstance = makeSysVTarget();
/// @brief Process-lifetime storage for the immutable Microsoft x64 ABI descriptor.
TargetInfo win64TargetInstance = makeWin64Target();

} // namespace

/// @brief Retrieve the canonical SysV AMD64 target description.
///
/// @details Returns a reference to the statically initialised singleton created
///          by @ref makeSysVTarget(). Callers receive read-only configuration
///          data and do not acquire ownership.
///
/// @return Reference to the global SysV target descriptor.
const TargetInfo &sysvTarget() noexcept {
    return sysvTargetInstance;
}

/// @brief Retrieve the Windows x64 target description.
///
/// @details Returns a reference to the statically initialised singleton created
///          by @ref makeWin64Target().
///
/// @return Reference to the global Windows x64 target descriptor.
const TargetInfo &win64Target() noexcept {
    return win64TargetInstance;
}

/// @brief Retrieve the platform-appropriate target description.
///
/// @details Returns win64Target() on Windows, sysvTarget() on other platforms.
///
/// @return Reference to the appropriate target descriptor for the host.
const TargetInfo &hostTarget() noexcept {
#if defined(_WIN32)
    return win64TargetInstance;
#else
    return sysvTargetInstance;
#endif
}

/// @brief Test whether a physical register is part of the general-purpose set.
///
/// @details Implements a switch over the enumerators recognised by the x86-64
///          backend.  The helper is used by register allocation and frame
///          lowering to discriminate between GPRs and other register classes.
///
/// @param reg Physical register to classify.
/// @return @c true when @p reg identifies a GPR.
bool isGPR(PhysReg reg) noexcept {
    switch (reg) {
        case PhysReg::RAX:
        case PhysReg::RBX:
        case PhysReg::RCX:
        case PhysReg::RDX:
        case PhysReg::RSI:
        case PhysReg::RDI:
        case PhysReg::R8:
        case PhysReg::R9:
        case PhysReg::R10:
        case PhysReg::R11:
        case PhysReg::R12:
        case PhysReg::R13:
        case PhysReg::R14:
        case PhysReg::R15:
        case PhysReg::RBP:
        case PhysReg::RSP:
            return true;
        default:
            return false;
    }
}

/// @brief Determine whether a physical register belongs to the XMM class.
///
/// @details Mirrors @ref isGPR but enumerates the SIMD XMM registers recognised
///          by the backend.  Used in spill slot planning and instruction
///          selection when choosing encodings for floating-point operands.
///
/// @param reg Physical register to classify.
/// @return @c true when @p reg is an XMM register.
bool isXMM(PhysReg reg) noexcept {
    switch (reg) {
        case PhysReg::XMM0:
        case PhysReg::XMM1:
        case PhysReg::XMM2:
        case PhysReg::XMM3:
        case PhysReg::XMM4:
        case PhysReg::XMM5:
        case PhysReg::XMM6:
        case PhysReg::XMM7:
        case PhysReg::XMM8:
        case PhysReg::XMM9:
        case PhysReg::XMM10:
        case PhysReg::XMM11:
        case PhysReg::XMM12:
        case PhysReg::XMM13:
        case PhysReg::XMM14:
        case PhysReg::XMM15:
            return true;
        default:
            return false;
    }
}

/// @brief Map a physical register to its textual assembly representation.
///
/// @details The backend prints registers in AT&T syntax.  This helper covers
///          every register enumerator currently used by the backend and
///          provides a stable string literal suitable for emission into
///          assembly listings or diagnostics.  Unknown registers fall through to
///          the @c default case and return a sentinel string.
///
/// @param reg Physical register whose name is requested.
/// @return Null-terminated string containing the register mnemonic.
const char *regName(PhysReg reg) noexcept {
    switch (reg) {
        case PhysReg::RAX:
            return "%rax";
        case PhysReg::RBX:
            return "%rbx";
        case PhysReg::RCX:
            return "%rcx";
        case PhysReg::RDX:
            return "%rdx";
        case PhysReg::RSI:
            return "%rsi";
        case PhysReg::RDI:
            return "%rdi";
        case PhysReg::R8:
            return "%r8";
        case PhysReg::R9:
            return "%r9";
        case PhysReg::R10:
            return "%r10";
        case PhysReg::R11:
            return "%r11";
        case PhysReg::R12:
            return "%r12";
        case PhysReg::R13:
            return "%r13";
        case PhysReg::R14:
            return "%r14";
        case PhysReg::R15:
            return "%r15";
        case PhysReg::RBP:
            return "%rbp";
        case PhysReg::RSP:
            return "%rsp";
        case PhysReg::XMM0:
            return "%xmm0";
        case PhysReg::XMM1:
            return "%xmm1";
        case PhysReg::XMM2:
            return "%xmm2";
        case PhysReg::XMM3:
            return "%xmm3";
        case PhysReg::XMM4:
            return "%xmm4";
        case PhysReg::XMM5:
            return "%xmm5";
        case PhysReg::XMM6:
            return "%xmm6";
        case PhysReg::XMM7:
            return "%xmm7";
        case PhysReg::XMM8:
            return "%xmm8";
        case PhysReg::XMM9:
            return "%xmm9";
        case PhysReg::XMM10:
            return "%xmm10";
        case PhysReg::XMM11:
            return "%xmm11";
        case PhysReg::XMM12:
            return "%xmm12";
        case PhysReg::XMM13:
            return "%xmm13";
        case PhysReg::XMM14:
            return "%xmm14";
        case PhysReg::XMM15:
            return "%xmm15";
        default:
            return "%unknown";
    }
}

} // namespace zanna::codegen::x64
