//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/aarch64/AsmEmitter.cpp
// Purpose: AArch64 assembly text emission from register-allocated Machine IR.
//          Converts MIR with physical registers to GAS-compatible assembly text,
//          including prologue/epilogue generation and platform symbol mangling.
// Key invariants:
//   - emitFunction() is re-entrant; all per-function state is reset on entry.
//   - All MIR operands must have physical registers (allocation complete).
//   - Late expansion helpers use kScratchGPR/kScratchGPR2/kScratchGPR3; the
//     register allocator reserves all three so emitted scratch sequences cannot
//     clobber allocated values.
// Ownership/Lifetime:
//   - AsmEmitter holds a non-owning pointer to TargetInfo; caller keeps it alive.
//   - currentPlan_ is only valid during a single emitFunction() call.
// Links: src/codegen/aarch64/AsmEmitter.hpp, src/codegen/aarch64/FrameCodegen.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements AArch64 GAS text emission and target-specific symbol syntax.
 *
 * Public instruction methods serialize already allocated MIR. File-local
 * helpers validate register views, legalize immediate/address forms, and
 * normalize symbols. Full-function emission binds a temporary `FramePlan`
 * while return instructions are expanded, then clears that borrowed state.
 */

#include "AsmEmitter.hpp"

#include "A64ImmediateUtils.hpp"
#include "FrameCodegen.hpp"
#include "binenc/A64Encoding.hpp"
#include "codegen/common/ICE.hpp"
#include "codegen/common/LabelUtil.hpp"
#include "il/runtime/RuntimeNameMap.hpp"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Emit primitives — reduce per-method boilerplate for common instruction forms.
// These are file-local helpers; the public method signatures are unchanged.
// ---------------------------------------------------------------------------

namespace {

using zanna::codegen::aarch64::isFPR;
using zanna::codegen::aarch64::PhysReg;
using zanna::codegen::aarch64::regName;

/// @brief Print a floating-point register as dN (64-bit scalar).
/// @details This helper is used by scalar F64 emission paths. Passing a GPR is
///          always a codegen bug, so release builds throw before printing an
///          invalid register spelling.
/// @param os Output stream receiving the register name.
/// @param r Physical register that must be in the FPR register file.
inline void printDReg(std::ostream &os, PhysReg r) {
    if (!isFPR(r)) {
        throw std::runtime_error("AArch64 asm emitter: expected FPR for d-register print");
    }
    const char *name = regName(r);
    os << 'd' << (name + 1);
}

/// @brief Print a GPR through its 32-bit W-register view.
/// @param os Output stream receiving the register name.
/// @param r Physical GPR, stack pointer, or already-compatible register spelling.
inline void printWReg(std::ostream &os, PhysReg r) {
    const char *name = regName(r);
    if (name[0] == 'x')
        os << 'w' << (name + 1);
    else if (name[0] == 's' && name[1] == 'p' && name[2] == '\0')
        os << "wsp";
    else
        os << name;
}

/// @brief Emit a three-register GPR instruction.
/// @param os Output assembly stream.
/// @param mnem Instruction mnemonic.
/// @param d Destination register.
/// @param a First source register.
/// @param b Second source register.
inline void emit3R(std::ostream &os, const char *mnem, PhysReg d, PhysReg a, PhysReg b) {
    os << "  " << mnem << " " << regName(d) << ", " << regName(a) << ", " << regName(b) << "\n";
}

/// @brief Emit a two-register plus immediate GPR instruction.
/// @param os Output assembly stream.
/// @param mnem Instruction mnemonic.
/// @param d Destination register.
/// @param s Source register.
/// @param imm Immediate rendered after `#`.
inline void emit2RI(std::ostream &os, const char *mnem, PhysReg d, PhysReg s, long long imm) {
    os << "  " << mnem << " " << regName(d) << ", " << regName(s) << ", #" << imm << "\n";
}

/// @brief Emit a two-register plus shifted-12-bit immediate instruction.
/// @details Used for add/sub immediates that fit only as `imm12 << 12`.
/// @param os Output assembly stream.
/// @param mnem Instruction mnemonic.
/// @param d Destination register.
/// @param s Source register.
/// @param imm12 Unshifted 12-bit payload.
inline void emit2RIShift12(
    std::ostream &os, const char *mnem, PhysReg d, PhysReg s, uint32_t imm12) {
    os << "  " << mnem << " " << regName(d) << ", " << regName(s) << ", #" << imm12
       << ", lsl #12\n";
}

/// @brief Emit a three-register scalar-double instruction.
/// @param os Output assembly stream.
/// @param mnem Instruction mnemonic.
/// @param d Destination FPR.
/// @param a First source FPR.
/// @param b Second source FPR.
inline void emit3D(std::ostream &os, const char *mnem, PhysReg d, PhysReg a, PhysReg b) {
    os << "  " << mnem << " ";
    printDReg(os, d);
    os << ", ";
    printDReg(os, a);
    os << ", ";
    printDReg(os, b);
    os << "\n";
}

} // namespace

#include <cstring>
#include <iomanip>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace zanna::codegen::aarch64 {

/// @brief Map IL extern names to C runtime symbol names.
/// @details The IL uses namespaced names like `Zanna.Console.PrintI64` but the
///          runtime exports C-style names like `rt_print_i64`. Unknown names
///          pass through unchanged.
/// @param name Canonical IL or already-native symbol.
/// @return Runtime symbol spelling.
static std::string mapRuntimeSymbol(const std::string &name) {
    if (auto mapped = il::runtime::mapCanonicalRuntimeName(name))
        return std::string(*mapped);
    // Not a known runtime symbol, return as-is.
    return name;
}

/// @brief Test whether @p name is a Darwin assembler-local symbol.
/// @details Darwin treats any symbol whose first character is `.` or whose
///          prefix is one of `L.`, `Ltmp`, or `LBB` as a private assembler
///          label that is not visible to the linker. Such labels must not
///          receive the underscore mangling that exported C symbols get.
/// @param name Candidate symbol name to classify.
/// @return True if the name follows the Darwin assembler-local convention.
static bool isDarwinLocalSymbolName(std::string_view name) {
    return !name.empty() && (name.front() == '.' || name.rfind("L.", 0) == 0 ||
                             name.rfind("Ltmp", 0) == 0 || name.rfind("LBB", 0) == 0);
}

/// @brief Mangle a symbol name for the target platform.
/// @details Normalizes frontend entry names such as @c @main, sanitizes
///          assembler-hostile characters, and applies the Darwin underscore
///          prefix for externally visible C-level symbols. Assembler local
///          labels are already controlled by block lowering and are not
///          mangled on Darwin.
/// @param name Raw MIR/runtime symbol name.
/// @param isDarwin True when emitting Mach-O/Darwin assembly syntax.
/// @return Assembly-safe symbol spelling for directives and operands.
static std::string mangleSymbolImpl(const std::string &name, bool isDarwin) {
    const std::string normalized =
        (name == "@main") ? std::string{"main"} : zanna::codegen::common::sanitizeLabel(name);
    if (isDarwin) {
        if (isDarwinLocalSymbolName(name))
            return name;
        return "_" + normalized;
    }
    return normalized;
}

/// @brief Mangle a call target symbol for emission.
/// @details First maps IL runtime names to C runtime names, then applies
///          platform mangling.
/// @param name Raw IL, runtime, or user symbol.
/// @param isDarwin Whether Mach-O underscore conventions apply.
/// @return Assembly-safe call-target spelling.
static std::string mangleCallTargetImpl(const std::string &name, bool isDarwin) {
    return mangleSymbolImpl(mapRuntimeSymbol(name), isDarwin);
}

/// @brief Sanitize a label for assembly output.
/// @details Delegates to the common label sanitizer which replaces hyphens
///          with underscores and handles other illegal assembly characters.
/// @param name Original label identifier.
/// @return Sanitized copy suitable for assembly.
static std::string sanitizeLabel(const std::string &name) {
    std::string sanitized = zanna::codegen::common::sanitizeLabel(name);
    if (sanitized.rfind(".L", 0) == 0)
        sanitized.insert(sanitized.begin(), 'L');
    return sanitized;
}

/// @copydoc AsmEmitter::emitFunctionHeader()
void AsmEmitter::emitFunctionHeader(std::ostream &os, const std::string &name) const {
    const bool darwin = !target_->isLinux() && !target_->isWindows();
    os << ".text\n";
    os << ".align 2\n";

    const std::string sym = mangleSymbolImpl(name, darwin);

    // Darwin:  skip .globl for assembler-local labels.
    // Linux:   always emit .globl + .type (ELF function metadata).
    // Windows: emit .globl only; PE/COFF has no .type/.size directives.
    if (darwin) {
        if (!isDarwinLocalSymbolName(sym) &&
            !(sym.size() > 1 && sym[0] == '_' &&
              isDarwinLocalSymbolName(std::string_view(sym).substr(1)))) {
            os << ".globl " << sym << "\n";
        }
    } else if (target_->isLinux()) {
        os << ".globl " << sym << "\n";
        os << ".type " << sym << ", @function\n";
    } else {
        // Windows ARM64 (PE/COFF): .globl only, no ELF-specific directives.
        os << ".globl " << sym << "\n";
    }
    os << sym << ":\n";
    if (target_->hasBranchTargetIdentification())
        os << "  bti c\n";
}

/// @copydoc AsmEmitter::emitPrologue(std::ostream&)
void AsmEmitter::emitPrologue(std::ostream &os) const {
    if (target_->hasReturnAddressSigning())
        os << "  paciasp\n";
    // stp x29, x30, [sp, #-16]!; mov x29, sp
    os << "  stp x29, x30, [sp, #-16]!\n";
    os << "  mov x29, sp\n";
}

/// @copydoc AsmEmitter::emitEpilogue(std::ostream&)
void AsmEmitter::emitEpilogue(std::ostream &os) const {
    // ldp x29, x30, [sp], #16; ret
    os << "  ldp x29, x30, [sp], #16\n";
    if (target_->hasReturnAddressSigning())
        os << "  autiasp\n";
    os << "  ret\n";
}

/// @copydoc AsmEmitter::emitPrologue(std::ostream&,const FramePlan&)
void AsmEmitter::emitPrologue(std::ostream &os, const FramePlan &plan) const {
    // Step emitter for text output — each step corresponds one-to-one with a
    // call in FrameCodegen.hpp::iteratePrologue.
    struct Steps {
        std::ostream &os;
        const AsmEmitter &self;

        /// @brief Bind text prologue emission state to frame-codegen callbacks.
        /// @param out Destination assembly stream.
        /// @param emitter Emitter that owns register formatting helpers.
        Steps(std::ostream &out, const AsmEmitter &emitter) : os(out), self(emitter) {}

        /// @brief Emit return-address signing at prologue entry.
        void paciasp() const {
            os << "  paciasp\n";
        }

        /// @brief Push frame pointer and link register as a pair.
        void stpFpLrPre() const {
            os << "  stp x29, x30, [sp, #-16]!\n";
        }

        /// @brief Establish `x29` as the current frame pointer.
        void movFpSp() const {
            os << "  mov x29, sp\n";
        }

        /// @brief Allocate @p n bytes of local stack storage.
        /// @param n Positive byte count supplied by frame iteration.
        void subSp(int32_t n) const {
            self.emitSubSp(os, n);
        }

        /// @brief Push a pair of callee-saved GPRs.
        /// @param r0 First register.
        /// @param r1 Second register.
        void stpGprPair(PhysReg r0, PhysReg r1) const {
            os << "  stp " << rn(r0) << ", " << rn(r1) << ", [sp, #-16]!\n";
        }

        /// @brief Push one callee-saved GPR in a 16-byte stack slot.
        /// @param r0 Register to save.
        void strGprSingle(PhysReg r0) const {
            os << "  str " << rn(r0) << ", [sp, #-16]!\n";
        }

        /// @brief Push a pair of callee-saved scalar-double FPRs.
        /// @param r0 First register.
        /// @param r1 Second register.
        void stpFprPair(PhysReg r0, PhysReg r1) const {
            os << "  stp ";
            self.printD(os, r0);
            os << ", ";
            self.printD(os, r1);
            os << ", [sp, #-16]!\n";
        }

        /// @brief Push one callee-saved FPR in a 16-byte stack slot.
        /// @param r0 Register to save.
        void strFprSingle(PhysReg r0) const {
            os << "  str ";
            self.printD(os, r0);
            os << ", [sp, #-16]!\n";
        }
    };

    iteratePrologue(plan.saveGPRs,
                    plan.saveFPRs,
                    plan.localFrameSize,
                    target_->hasReturnAddressSigning(),
                    Steps(os, *this));
}

/// @copydoc AsmEmitter::emitEpilogue(std::ostream&,const FramePlan&)
void AsmEmitter::emitEpilogue(std::ostream &os, const FramePlan &plan) const {
    struct Steps {
        std::ostream &os;
        const AsmEmitter &self;

        /// @brief Bind text epilogue emission state to frame-codegen callbacks.
        /// @param out Destination assembly stream.
        /// @param emitter Emitter that owns register formatting helpers.
        Steps(std::ostream &out, const AsmEmitter &emitter) : os(out), self(emitter) {}

        /// @brief Pop a pair of callee-saved scalar-double FPRs.
        /// @param r0 First register.
        /// @param r1 Second register.
        void ldpFprPair(PhysReg r0, PhysReg r1) const {
            os << "  ldp ";
            self.printD(os, r0);
            os << ", ";
            self.printD(os, r1);
            os << ", [sp], #16\n";
        }

        /// @brief Pop one callee-saved FPR from a 16-byte stack slot.
        /// @param r0 Register to restore.
        void ldrFprSingle(PhysReg r0) const {
            os << "  ldr ";
            self.printD(os, r0);
            os << ", [sp], #16\n";
        }

        /// @brief Pop a pair of callee-saved GPRs.
        /// @param r0 First register.
        /// @param r1 Second register.
        void ldpGprPair(PhysReg r0, PhysReg r1) const {
            os << "  ldp " << rn(r0) << ", " << rn(r1) << ", [sp], #16\n";
        }

        /// @brief Pop one callee-saved GPR from a 16-byte stack slot.
        /// @param r0 Register to restore.
        void ldrGprSingle(PhysReg r0) const {
            os << "  ldr " << rn(r0) << ", [sp], #16\n";
        }

        /// @brief Deallocate @p n bytes of local stack storage.
        /// @param n Positive byte count supplied by frame iteration.
        void addSp(int32_t n) const {
            self.emitAddSp(os, n);
        }

        /// @brief Restore frame pointer and link register as a pair.
        void ldpFpLrPost() const {
            os << "  ldp x29, x30, [sp], #16\n";
        }

        /// @brief Authenticate the saved return address.
        void autiasp() const {
            os << "  autiasp\n";
        }

        /// @brief Emit the final return instruction.
        void ret() const {
            os << "  ret\n";
        }
    };

    iterateEpilogue(plan.saveGPRs,
                    plan.saveFPRs,
                    plan.localFrameSize,
                    target_->hasReturnAddressSigning(),
                    Steps(os, *this));
}

/// @copydoc AsmEmitter::emitMovRR()
void AsmEmitter::emitMovRR(std::ostream &os, PhysReg dst, PhysReg src) const {
    os << "  mov " << rn(dst) << ", " << rn(src) << "\n";
}

/// @copydoc AsmEmitter::emitMovRI()
void AsmEmitter::emitMovRI(std::ostream &os, PhysReg dst, long long imm) const {
    // Use movz/movk sequence for wide immediates that can't be encoded directly
    if (needsWideImmSequence(imm)) {
        emitMovImm64(os, dst, static_cast<unsigned long long>(imm));
    } else {
        os << "  mov " << rn(dst) << ", #" << imm << "\n";
    }
}

/// @copydoc AsmEmitter::emitAddRRR()
void AsmEmitter::emitAddRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "add", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitSubRRR()
void AsmEmitter::emitSubRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "sub", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitMulRRR()
void AsmEmitter::emitMulRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "mul", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitSmulhRRR()
void AsmEmitter::emitSmulhRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "smulh", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitUmulhRRR()
void AsmEmitter::emitUmulhRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "umulh", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitSDivRRR()
void AsmEmitter::emitSDivRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "sdiv", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitUDivRRR()
void AsmEmitter::emitUDivRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "udiv", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitMSubRRRR()
void AsmEmitter::emitMSubRRRR(
    std::ostream &os, PhysReg dst, PhysReg mul1, PhysReg mul2, PhysReg sub) const {
    // AArch64 multiply-subtract: msub xd, xn, xm, xa => xd = xa - xn*xm
    // Used for remainder: rem = dividend - (dividend/divisor)*divisor
    os << "  msub " << rn(dst) << ", " << rn(mul1) << ", " << rn(mul2) << ", " << rn(sub) << "\n";
}

// cppcheck-suppress functionStatic
[[maybe_unused]] void AsmEmitter::emitCbz(std::ostream &os,
                                          PhysReg reg,
                                          const std::string &label) const {
    // AArch64 compare and branch if zero: cbz xn, label
    os << "  cbz " << rn(reg) << ", " << label << "\n";
}

/// @brief Pick a reserved scratch GPR for a wide-immediate expansion.
/// @details The register allocator hands the reserved scratch registers
///          (x9/x16/x17) out as one-instruction emergency reload homes, and
///          several fast paths keep a value in x9 across neighbouring
///          instructions. An expansion such as `mov x9, #imm; add dst, lhs, x9`
///          must therefore never pick a scratch that is itself one of the
///          instruction's operands: `add x9, x9, x9` would read the immediate
///          twice. Candidates are tried in preference order so the register
///          chosen for the common (non-conflicting) case is unchanged.
/// @param candidates Reserved scratch registers in preference order.
/// @param blocked Operand registers of the instruction being expanded.
/// @return First candidate absent from @p blocked.
/// @throws std::runtime_error when every candidate is an operand.
static PhysReg pickWideImmScratch(std::initializer_list<PhysReg> candidates,
                                  std::initializer_list<PhysReg> blocked) {
    for (PhysReg candidate : candidates) {
        if (std::find(blocked.begin(), blocked.end(), candidate) == blocked.end())
            return candidate;
    }
    throw std::runtime_error(
        "AArch64 asm emitter: no scratch register for wide immediate expansion");
}

/// @copydoc AsmEmitter::emitAddRI()
void AsmEmitter::emitAddRI(std::ostream &os, PhysReg dst, PhysReg lhs, long long imm) const {
    const uint64_t magnitude = absImmUnsigned(imm);
    if (const auto enc = classifyAddSubImmEncoding(magnitude)) {
        if (imm >= 0) {
            if (enc->shift12)
                emit2RIShift12(os, "add", dst, lhs, enc->imm12);
            else
                emit2RI(os, "add", dst, lhs, static_cast<long long>(magnitude));
        } else if (enc->shift12) {
            emit2RIShift12(os, "sub", dst, lhs, enc->imm12);
        } else {
            emit2RI(os, "sub", dst, lhs, static_cast<long long>(magnitude));
        }
        return;
    }

    const PhysReg scratch =
        pickWideImmScratch({kScratchGPR, kScratchGPR2, kScratchGPR3}, {dst, lhs});
    emitMovImm64(os, scratch, static_cast<unsigned long long>(imm));
    emit3R(os, "add", dst, lhs, scratch);
}

/// @copydoc AsmEmitter::emitSubRI()
void AsmEmitter::emitSubRI(std::ostream &os, PhysReg dst, PhysReg lhs, long long imm) const {
    const uint64_t magnitude = absImmUnsigned(imm);
    if (const auto enc = classifyAddSubImmEncoding(magnitude)) {
        if (imm >= 0) {
            if (enc->shift12)
                emit2RIShift12(os, "sub", dst, lhs, enc->imm12);
            else
                emit2RI(os, "sub", dst, lhs, static_cast<long long>(magnitude));
        } else if (enc->shift12) {
            emit2RIShift12(os, "add", dst, lhs, enc->imm12);
        } else {
            emit2RI(os, "add", dst, lhs, static_cast<long long>(magnitude));
        }
        return;
    }

    const PhysReg scratch =
        pickWideImmScratch({kScratchGPR, kScratchGPR2, kScratchGPR3}, {dst, lhs});
    emitMovImm64(os, scratch, static_cast<unsigned long long>(imm));
    emit3R(os, "sub", dst, lhs, scratch);
}

/// @copydoc AsmEmitter::emitAndRRR()
void AsmEmitter::emitAndRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "and", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitOrrRRR()
void AsmEmitter::emitOrrRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "orr", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitEorRRR()
void AsmEmitter::emitEorRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "eor", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitAndRI()
void AsmEmitter::emitAndRI(std::ostream &os, PhysReg dst, PhysReg src, long long imm) const {
    if (binenc::encodeLogicalImmediate(static_cast<uint64_t>(imm)) >= 0) {
        emit2RI(os, "and", dst, src, imm);
        return;
    }

    const PhysReg scratch =
        pickWideImmScratch({kScratchGPR, kScratchGPR2, kScratchGPR3}, {dst, src});
    emitMovImm64(os, scratch, static_cast<unsigned long long>(imm));
    emit3R(os, "and", dst, src, scratch);
}

/// @copydoc AsmEmitter::emitOrrRI()
void AsmEmitter::emitOrrRI(std::ostream &os, PhysReg dst, PhysReg src, long long imm) const {
    if (binenc::encodeLogicalImmediate(static_cast<uint64_t>(imm)) >= 0) {
        emit2RI(os, "orr", dst, src, imm);
        return;
    }

    const PhysReg scratch =
        pickWideImmScratch({kScratchGPR, kScratchGPR2, kScratchGPR3}, {dst, src});
    emitMovImm64(os, scratch, static_cast<unsigned long long>(imm));
    emit3R(os, "orr", dst, src, scratch);
}

/// @copydoc AsmEmitter::emitEorRI()
void AsmEmitter::emitEorRI(std::ostream &os, PhysReg dst, PhysReg src, long long imm) const {
    if (binenc::encodeLogicalImmediate(static_cast<uint64_t>(imm)) >= 0) {
        emit2RI(os, "eor", dst, src, imm);
        return;
    }

    const PhysReg scratch =
        pickWideImmScratch({kScratchGPR, kScratchGPR2, kScratchGPR3}, {dst, src});
    emitMovImm64(os, scratch, static_cast<unsigned long long>(imm));
    emit3R(os, "eor", dst, src, scratch);
}

/// @copydoc AsmEmitter::emitLslRI()
void AsmEmitter::emitLslRI(std::ostream &os, PhysReg dst, PhysReg lhs, long long sh) const {
    emit2RI(os, "lsl", dst, lhs, sh);
}

/// @copydoc AsmEmitter::emitLsrRI()
void AsmEmitter::emitLsrRI(std::ostream &os, PhysReg dst, PhysReg lhs, long long sh) const {
    emit2RI(os, "lsr", dst, lhs, sh);
}

/// @copydoc AsmEmitter::emitAsrRI()
void AsmEmitter::emitAsrRI(std::ostream &os, PhysReg dst, PhysReg lhs, long long sh) const {
    emit2RI(os, "asr", dst, lhs, sh);
}

/// @copydoc AsmEmitter::emitLslvRRR()
void AsmEmitter::emitLslvRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "lslv", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitLsrvRRR()
void AsmEmitter::emitLsrvRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "lsrv", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitAsrvRRR()
void AsmEmitter::emitAsrvRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3R(os, "asrv", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitCmpRR()
void AsmEmitter::emitCmpRR(std::ostream &os, PhysReg lhs, PhysReg rhs) const {
    os << "  cmp " << rn(lhs) << ", " << rn(rhs) << "\n";
}

/// @copydoc AsmEmitter::emitCmpRI()
void AsmEmitter::emitCmpRI(std::ostream &os, PhysReg lhs, long long imm) const {
    // ARM64 cmp immediate range: 0-4095 (12-bit unsigned)
    // ARM64 cmn immediate range: 0-4095 (equivalent to cmp with negated value)
    if (imm >= 0 && imm <= 4095) {
        os << "  cmp " << rn(lhs) << ", #" << imm << "\n";
    } else if (imm >= -4095 && imm < 0) {
        os << "  cmn " << rn(lhs) << ", #" << -imm << "\n";
    } else {
        const PhysReg scratch =
            pickWideImmScratch({kScratchGPR2, kScratchGPR, kScratchGPR3}, {lhs});
        emitMovImm64(os, scratch, imm);
        os << "  cmp " << rn(lhs) << ", " << rn(scratch) << "\n";
    }
}

/// @copydoc AsmEmitter::emitTstRR()
void AsmEmitter::emitTstRR(std::ostream &os, PhysReg lhs, PhysReg rhs) const {
    os << "  tst " << rn(lhs) << ", " << rn(rhs) << "\n";
}

/// @copydoc AsmEmitter::emitCset()
void AsmEmitter::emitCset(std::ostream &os, PhysReg dst, const char *cond) const {
    os << "  cset " << rn(dst) << ", " << cond << "\n";
}

/// @copydoc AsmEmitter::emitSubSp()
void AsmEmitter::emitSubSp(std::ostream &os, long long bytes) const {
    // ARM64 add/sub immediate supports 12-bit unsigned values (0-4095).
    // Prefer the shifted-12 form when possible to avoid thousands of small
    // adjustments for large frames while preserving SP alignment.
    if (bytes < 0) {
        throw std::out_of_range("AArch64 stack subtraction must be non-negative");
    }
    const unsigned long long value = static_cast<unsigned long long>(bytes);
    const unsigned long long hi = value >> 12U;
    const unsigned long long lo = value & 0xFFFULL;
    if (hi > 0 && hi <= 4095ULL) {
        emit2RIShift12(os, "sub", PhysReg::SP, PhysReg::SP, static_cast<uint32_t>(hi));
        if (lo > 0)
            emit2RI(os, "sub", PhysReg::SP, PhysReg::SP, static_cast<long long>(lo));
        return;
    }
    constexpr long long kMaxImm = 4080;
    while (bytes > kMaxImm) {
        emit2RI(os, "sub", PhysReg::SP, PhysReg::SP, kMaxImm);
        bytes -= kMaxImm;
    }
    if (bytes > 0)
        emit2RI(os, "sub", PhysReg::SP, PhysReg::SP, bytes);
}

/// @copydoc AsmEmitter::emitAddSp()
void AsmEmitter::emitAddSp(std::ostream &os, long long bytes) const {
    // ARM64 add/sub immediate supports 12-bit unsigned values (0-4095).
    // Prefer the shifted-12 form when possible to avoid excessive epilogue size.
    if (bytes < 0) {
        throw std::out_of_range("AArch64 stack addition must be non-negative");
    }
    const unsigned long long value = static_cast<unsigned long long>(bytes);
    const unsigned long long hi = value >> 12U;
    const unsigned long long lo = value & 0xFFFULL;
    if (hi > 0 && hi <= 4095ULL) {
        emit2RIShift12(os, "add", PhysReg::SP, PhysReg::SP, static_cast<uint32_t>(hi));
        if (lo > 0)
            emit2RI(os, "add", PhysReg::SP, PhysReg::SP, static_cast<long long>(lo));
        return;
    }
    constexpr long long kMaxImm = 4080;
    while (bytes > kMaxImm) {
        emit2RI(os, "add", PhysReg::SP, PhysReg::SP, kMaxImm);
        bytes -= kMaxImm;
    }
    if (bytes > 0)
        emit2RI(os, "add", PhysReg::SP, PhysReg::SP, bytes);
}

/// @brief Pick a reserved scratch GPR outside the supplied blocked set.
/// @details Prefers kScratchGPR, then kScratchGPR2, then kScratchGPR3. Callers
///          pass every physical register that the helper sequence must not
///          clobber, which keeps large-offset expansions independent of the
///          particular operand shape being emitted.
/// @param blocked Physical GPRs that cannot be used as scratch registers.
/// @return Reserved scratch register not present in @p blocked.
/// @throws std::runtime_error when all reserved scratch registers are blocked.
static PhysReg chooseGprScratch(std::initializer_list<PhysReg> blocked) {
    const PhysReg candidates[] = {kScratchGPR, kScratchGPR2, kScratchGPR3};
    for (PhysReg candidate : candidates) {
        if (std::find(blocked.begin(), blocked.end(), candidate) != blocked.end())
            continue;
        return candidate;
    }
    throw std::runtime_error(
        "AArch64 asm emitter: no scratch register for large offset load/store");
}

/// @brief Pick a scratch GPR that is neither @p base nor optionally @p avoid.
/// @param base Base register used by the addressing sequence.
/// @param avoid Optional transfer register that must not be clobbered.
/// @return Reserved scratch register suitable for the helper sequence.
static PhysReg chooseGprScratch(PhysReg base, std::optional<PhysReg> avoid = std::nullopt) {
    return avoid ? chooseGprScratch({base, *avoid}) : chooseGprScratch({base});
}

/// @copydoc AsmEmitter::emitStrToSp()
void AsmEmitter::emitStrToSp(std::ostream &os, PhysReg src, long long offset) const {
    if (offset >= 0 && (offset % 8) == 0 && (offset / 8) <= 4095) {
        os << "  str " << rn(src) << ", [sp, #" << offset << "]\n";
        return;
    }

    const PhysReg scratch = chooseGprScratch(PhysReg::SP, src);
    os << "  mov " << rn(scratch) << ", sp\n";
    emitAddRI(os, scratch, scratch, offset);
    os << "  str " << rn(src) << ", [" << rn(scratch) << "]\n";
}

/// @copydoc AsmEmitter::emitStrFprToSp()
void AsmEmitter::emitStrFprToSp(std::ostream &os, PhysReg src, long long offset) const {
    if (offset >= 0 && (offset % 8) == 0 && (offset / 8) <= 4095) {
        os << "  str ";
        printD(os, src);
        os << ", [sp, #" << offset << "]\n";
        return;
    }

    const PhysReg scratch = chooseGprScratch(PhysReg::SP);
    os << "  mov " << rn(scratch) << ", sp\n";
    emitAddRI(os, scratch, scratch, offset);
    os << "  str ";
    printD(os, src);
    os << ", [" << rn(scratch) << "]\n";
}

/// @brief Check if offset is in ARM64 signed immediate range for str/ldr instructions.
/// @details The signed unscaled immediate for str/ldr is [-256, 255].
/// @param offset Byte displacement to classify.
/// @return `true` when @p offset fits signed imm9.
static bool isInSignedImmRange(long long offset) {
    return offset >= -256 && offset <= 255;
}

/// @brief Check if offset fits the signed scaled pair form used by LDP/STP.
/// @param offset Byte displacement, which must be eight-byte aligned.
/// @return `true` when the scaled displacement fits signed imm7.
static bool isPairImm7Offset(long long offset) {
    if ((offset % 8) != 0)
        return false;
    const long long scaled = offset / 8;
    return scaled >= -64 && scaled <= 63;
}

/// @brief Add two signed offsets while rejecting overflow.
/// @details Used by pair load/store fallbacks that split an LDP/STP into two
///          scalar accesses at @c offset and @c offset+8. Keeping the check in
///          the text emitter mirrors the binary encoder's guarded fallback.
/// @param lhs Base offset in bytes.
/// @param rhs Delta in bytes.
/// @param context Diagnostic context for any overflow exception.
/// @return Sum of @p lhs and @p rhs.
/// @throws std::overflow_error when the mathematical sum is not representable.
static long long checkedOffsetAdd(long long lhs, long long rhs, const char *context) {
    if ((rhs > 0 && lhs > std::numeric_limits<long long>::max() - rhs) ||
        (rhs < 0 && lhs < std::numeric_limits<long long>::min() - rhs)) {
        throw std::overflow_error(std::string(context) + " offset overflow");
    }
    return lhs + rhs;
}

/// @brief GAS mnemonic for an unscaled narrow load of @p bytes (1/2/4).
/// @details Returns ldurb/ldurh/ldur. The unscaled (ldur*) forms are used so any
///          signed 9-bit FP-relative offset works without scaling constraints.
/// @param bytes Access width: one, two, or four.
/// @return Pointer to a static GAS mnemonic.
/// @throws std::runtime_error if @p bytes is not 1, 2, or 4.
static const char *narrowLoadMnemonic(unsigned bytes) {
    switch (bytes) {
        case 1:
            return "ldurb";
        case 2:
            return "ldurh";
        case 4:
            return "ldur";
        default:
            throw std::runtime_error("AArch64 asm emitter: unsupported narrow load width");
    }
}

/// @brief GAS mnemonic for an unscaled narrow store of @p bytes (1/2/4).
/// @details Returns sturb/sturh/stur — the unscaled counterparts of
///          narrowLoadMnemonic(), chosen for the same offset-flexibility reason.
/// @param bytes Access width: one, two, or four.
/// @return Pointer to a static GAS mnemonic.
/// @throws std::runtime_error if @p bytes is not 1, 2, or 4.
static const char *narrowStoreMnemonic(unsigned bytes) {
    switch (bytes) {
        case 1:
            return "sturb";
        case 2:
            return "sturh";
        case 4:
            return "stur";
        default:
            throw std::runtime_error("AArch64 asm emitter: unsupported narrow store width");
    }
}

/// @brief Emit a narrow GPR load/store using the 32-bit W-register view.
/// @details Writes `  <mnemonic> w<rt>, [<base>, #<offset>]`. The W view is
///          used because sub-word accesses operate on the low 32 bits; the
///          caller selects @p mnemonic via narrow{Load,Store}Mnemonic().
/// @param os       Output assembly stream.
/// @param mnemonic Pre-selected ldur*/stur* mnemonic.
/// @param rt       Transfer register (printed as its W alias).
/// @param base     Base address register.
/// @param offset   Signed byte offset from @p base.
static void emitNarrowGprAccess(
    std::ostream &os, const char *mnemonic, PhysReg rt, PhysReg base, long long offset) {
    os << "  " << mnemonic << " ";
    printWReg(os, rt);
    os << ", [" << regName(base) << ", #" << offset << "]\n";
}

/// @copydoc AsmEmitter::emitLdrFromFp()
void AsmEmitter::emitLdrFromFp(std::ostream &os, PhysReg dst, long long offset) const {
    if (isInSignedImmRange(offset)) {
        os << "  ldr " << rn(dst) << ", [x29, #" << offset << "]\n";
    } else {
        PhysReg scratch = chooseGprScratch(PhysReg::X29);
        emitMovRI(os, scratch, offset);
        os << "  add " << rn(scratch) << ", x29, " << rn(scratch) << "\n";
        os << "  ldr " << rn(dst) << ", [" << rn(scratch) << "]\n";
    }
}

/// @copydoc AsmEmitter::emitStrToFp()
void AsmEmitter::emitStrToFp(std::ostream &os, PhysReg src, long long offset) const {
    if (isInSignedImmRange(offset)) {
        os << "  str " << rn(src) << ", [x29, #" << offset << "]\n";
    } else {
        PhysReg scratch = chooseGprScratch(PhysReg::X29, src);
        emitMovRI(os, scratch, offset);
        os << "  add " << rn(scratch) << ", x29, " << rn(scratch) << "\n";
        os << "  str " << rn(src) << ", [" << rn(scratch) << "]\n";
    }
}

/// @copydoc AsmEmitter::emitLdr8FromFp()
void AsmEmitter::emitLdr8FromFp(std::ostream &os, PhysReg dst, long long offset) const {
    long long resolved;
    PhysReg base = resolveBaseOffset(os, PhysReg::X29, offset, resolved);
    emitNarrowGprAccess(os, narrowLoadMnemonic(1), dst, base, resolved);
}

/// @copydoc AsmEmitter::emitStr8ToFp()
void AsmEmitter::emitStr8ToFp(std::ostream &os, PhysReg src, long long offset) const {
    long long resolved;
    PhysReg base = resolveBaseOffset(os, PhysReg::X29, offset, resolved, src);
    emitNarrowGprAccess(os, narrowStoreMnemonic(1), src, base, resolved);
}

/// @copydoc AsmEmitter::emitLdr16FromFp()
void AsmEmitter::emitLdr16FromFp(std::ostream &os, PhysReg dst, long long offset) const {
    long long resolved;
    PhysReg base = resolveBaseOffset(os, PhysReg::X29, offset, resolved);
    emitNarrowGprAccess(os, narrowLoadMnemonic(2), dst, base, resolved);
}

/// @copydoc AsmEmitter::emitStr16ToFp()
void AsmEmitter::emitStr16ToFp(std::ostream &os, PhysReg src, long long offset) const {
    long long resolved;
    PhysReg base = resolveBaseOffset(os, PhysReg::X29, offset, resolved, src);
    emitNarrowGprAccess(os, narrowStoreMnemonic(2), src, base, resolved);
}

/// @copydoc AsmEmitter::emitLdr32FromFp()
void AsmEmitter::emitLdr32FromFp(std::ostream &os, PhysReg dst, long long offset) const {
    long long resolved;
    PhysReg base = resolveBaseOffset(os, PhysReg::X29, offset, resolved);
    emitNarrowGprAccess(os, narrowLoadMnemonic(4), dst, base, resolved);
}

/// @copydoc AsmEmitter::emitStr32ToFp()
void AsmEmitter::emitStr32ToFp(std::ostream &os, PhysReg src, long long offset) const {
    long long resolved;
    PhysReg base = resolveBaseOffset(os, PhysReg::X29, offset, resolved, src);
    emitNarrowGprAccess(os, narrowStoreMnemonic(4), src, base, resolved);
}

/// @copydoc AsmEmitter::emitLdrFprFromFp()
void AsmEmitter::emitLdrFprFromFp(std::ostream &os, PhysReg dst, long long offset) const {
    if (isInSignedImmRange(offset)) {
        os << "  ldr ";
        printD(os, dst);
        os << ", [x29, #" << offset << "]\n";
    } else {
        PhysReg scratch = chooseGprScratch(PhysReg::X29);
        emitMovRI(os, scratch, offset);
        os << "  add " << rn(scratch) << ", x29, " << rn(scratch) << "\n";
        os << "  ldr ";
        printD(os, dst);
        os << ", [" << rn(scratch) << "]\n";
    }
}

/// @copydoc AsmEmitter::emitStrFprToFp()
void AsmEmitter::emitStrFprToFp(std::ostream &os, PhysReg src, long long offset) const {
    if (isInSignedImmRange(offset)) {
        os << "  str ";
        printD(os, src);
        os << ", [x29, #" << offset << "]\n";
    } else {
        PhysReg scratch = chooseGprScratch(PhysReg::X29);
        emitMovRI(os, scratch, offset);
        os << "  add " << rn(scratch) << ", x29, " << rn(scratch) << "\n";
        os << "  str ";
        printD(os, src);
        os << ", [" << rn(scratch) << "]\n";
    }
}

/// @copydoc AsmEmitter::emitAddFpImm()
void AsmEmitter::emitAddFpImm(std::ostream &os, PhysReg dst, long long offset) const {
    // Compute dst = x29 + offset (where offset is typically negative for locals)
    // ARM64 add/sub immediate can only handle 12-bit unsigned values,
    // so we use sub for negative offsets
    if (offset >= 0 && offset <= 4095) {
        os << "  add " << rn(dst) << ", x29, #" << offset << "\n";
    } else if (offset < 0 && -offset <= 4095) {
        os << "  sub " << rn(dst) << ", x29, #" << -offset << "\n";
    } else {
        // Large offset: use scratch register to load offset, then add
        emitMovRI(os, kScratchGPR, offset);
        os << "  add " << rn(dst) << ", x29, " << rn(kScratchGPR) << "\n";
    }
}

/// @brief Resolve base+offset to an effective base register for load/store.
/// @details When @p offset fits in the signed immediate range, returns @p base
///          unchanged and sets @p resolvedOffset to @p offset. For large offsets
///          that exceed the immediate range, materialises the effective address
///          into a non-conflicting scratch register via movz/movk + add and
///          returns that register with @p resolvedOffset set to 0.
/// @param os Output stream for any scratch-register setup instructions.
/// @param base Original base register.
/// @param offset Byte offset from base.
/// @param[out] resolvedOffset Offset to use with the returned base register.
/// @param avoid Optional live transfer register that scratch selection must not clobber.
/// @return The register to use as the base in the subsequent load/store.
PhysReg AsmEmitter::resolveBaseOffset(std::ostream &os,
                                      PhysReg base,
                                      long long offset,
                                      long long &resolvedOffset,
                                      std::optional<PhysReg> avoid) const {
    if (isInSignedImmRange(offset)) {
        resolvedOffset = offset;
        return base;
    }
    const PhysReg candidates[] = {kScratchGPR, kScratchGPR2, kScratchGPR3};
    std::optional<PhysReg> scratch;
    for (PhysReg candidate : candidates) {
        if (candidate == base)
            continue;
        if (avoid.has_value() && candidate == *avoid)
            continue;
        scratch = candidate;
        break;
    }
    if (!scratch)
        throw std::runtime_error(
            "AArch64 asm emitter: no scratch register for large offset load/store");
    // Large offset: materialise effective address into scratch register.
    emitMovRI(os, *scratch, offset);
    os << "  add " << rn(*scratch) << ", " << rn(base) << ", " << rn(*scratch) << "\n";
    resolvedOffset = 0;
    return *scratch;
}

/// @copydoc AsmEmitter::emitLdrFromBase()
void AsmEmitter::emitLdrFromBase(std::ostream &os,
                                 PhysReg dst,
                                 PhysReg base,
                                 long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved);
    os << "  ldr " << rn(dst) << ", [" << rn(b) << ", #" << resolved << "]\n";
}

/// @copydoc AsmEmitter::emitStrToBase()
void AsmEmitter::emitStrToBase(std::ostream &os,
                               PhysReg src,
                               PhysReg base,
                               long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved, src);
    os << "  str " << rn(src) << ", [" << rn(b) << ", #" << resolved << "]\n";
}

/// @copydoc AsmEmitter::emitLdr8FromBase()
void AsmEmitter::emitLdr8FromBase(std::ostream &os,
                                  PhysReg dst,
                                  PhysReg base,
                                  long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved);
    emitNarrowGprAccess(os, narrowLoadMnemonic(1), dst, b, resolved);
}

/// @copydoc AsmEmitter::emitStr8ToBase()
void AsmEmitter::emitStr8ToBase(std::ostream &os,
                                PhysReg src,
                                PhysReg base,
                                long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved, src);
    emitNarrowGprAccess(os, narrowStoreMnemonic(1), src, b, resolved);
}

/// @copydoc AsmEmitter::emitLdr16FromBase()
void AsmEmitter::emitLdr16FromBase(std::ostream &os,
                                   PhysReg dst,
                                   PhysReg base,
                                   long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved);
    emitNarrowGprAccess(os, narrowLoadMnemonic(2), dst, b, resolved);
}

/// @copydoc AsmEmitter::emitStr16ToBase()
void AsmEmitter::emitStr16ToBase(std::ostream &os,
                                 PhysReg src,
                                 PhysReg base,
                                 long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved, src);
    emitNarrowGprAccess(os, narrowStoreMnemonic(2), src, b, resolved);
}

/// @copydoc AsmEmitter::emitLdr32FromBase()
void AsmEmitter::emitLdr32FromBase(std::ostream &os,
                                   PhysReg dst,
                                   PhysReg base,
                                   long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved);
    emitNarrowGprAccess(os, narrowLoadMnemonic(4), dst, b, resolved);
}

/// @copydoc AsmEmitter::emitStr32ToBase()
void AsmEmitter::emitStr32ToBase(std::ostream &os,
                                 PhysReg src,
                                 PhysReg base,
                                 long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved, src);
    emitNarrowGprAccess(os, narrowStoreMnemonic(4), src, b, resolved);
}

/// @copydoc AsmEmitter::emitLdrFprFromBase()
void AsmEmitter::emitLdrFprFromBase(std::ostream &os,
                                    PhysReg dst,
                                    PhysReg base,
                                    long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved);
    os << "  ldr ";
    printD(os, dst);
    os << ", [" << rn(b) << ", #" << resolved << "]\n";
}

/// @copydoc AsmEmitter::emitStrFprToBase()
void AsmEmitter::emitStrFprToBase(std::ostream &os,
                                  PhysReg src,
                                  PhysReg base,
                                  long long offset) const {
    long long resolved;
    PhysReg b = resolveBaseOffset(os, base, offset, resolved);
    os << "  str ";
    printD(os, src);
    os << ", [" << rn(b) << ", #" << resolved << "]\n";
}

/// @brief Emit a MOVZ (move wide with zero) instruction.
/// @param os Output stream for assembly text.
/// @param dst Destination GPR.
/// @param imm16 16-bit immediate value.
/// @param lsl Left-shift amount (0, 16, 32, or 48).
void AsmEmitter::emitMovZ(std::ostream &os, PhysReg dst, unsigned imm16, unsigned lsl) const {
    os << "  movz " << rn(dst) << ", #" << imm16;
    if (lsl)
        os << ", lsl #" << lsl;
    os << "\n";
}

/// @brief Emit a MOVN (move wide with NOT) instruction.
/// @param os Output stream for assembly text.
/// @param dst Destination GPR.
/// @param imm16 16-bit immediate value.
/// @param lsl Left-shift amount (0, 16, 32, or 48).
void AsmEmitter::emitMovN(std::ostream &os, PhysReg dst, unsigned imm16, unsigned lsl) const {
    os << "  movn " << rn(dst) << ", #" << imm16;
    if (lsl)
        os << ", lsl #" << lsl;
    os << "\n";
}

/// @brief Emit a MOVK (move wide with keep) instruction.
/// @param os Output stream for assembly text.
/// @param dst Destination GPR (other bits preserved).
/// @param imm16 16-bit immediate value to insert.
/// @param lsl Lane position (0, 16, 32, or 48).
void AsmEmitter::emitMovK(std::ostream &os, PhysReg dst, unsigned imm16, unsigned lsl) const {
    os << "  movk " << rn(dst) << ", #" << imm16;
    if (lsl)
        os << ", lsl #" << lsl;
    os << "\n";
}

/// @brief Emit a full 64-bit immediate load using movz + up to three movk.
/// @param os Output stream for assembly text.
/// @param dst Destination GPR.
/// @param value 64-bit immediate to materialise.
void AsmEmitter::emitMovImm64(std::ostream &os, PhysReg dst, unsigned long long value) const {
    /// @brief Emit one instruction from the decomposed move-wide sequence.
    /// @param inst Move-wide opcode, immediate lane, and shift.
    forEachMoveWideInst(value, [&](const MoveWideInst &inst) {
        switch (inst.opcode) {
            case MoveWideOpcode::MovZ:
                emitMovZ(os, dst, inst.imm16, inst.shift);
                break;
            case MoveWideOpcode::MovN:
                emitMovN(os, dst, inst.imm16, inst.shift);
                break;
            case MoveWideOpcode::MovK:
                emitMovK(os, dst, inst.imm16, inst.shift);
                break;
        }
    });
}

// cppcheck-suppress functionStatic
[[maybe_unused]] void AsmEmitter::emitRet(std::ostream &os) const {
    os << "  ret\n";
}

/// @copydoc AsmEmitter::emitFMovRR()
void AsmEmitter::emitFMovRR(std::ostream &os, PhysReg dst, PhysReg src) const {
    os << "  fmov ";
    printD(os, dst);
    os << ", ";
    printD(os, src);
    os << "\n";
}

/// @copydoc AsmEmitter::emitFMovRI()
void AsmEmitter::emitFMovRI(std::ostream &os, PhysReg dst, double imm) const {
    if (binenc::encodeFP8Immediate(imm) >= 0) {
        const auto savedFlags = os.flags();
        os << std::fixed;
        os << "  fmov ";
        printD(os, dst);
        os << ", #" << imm << "\n";
        os.flags(savedFlags);
        return;
    }

    unsigned long long bits = 0;
    static_assert(sizeof(bits) == sizeof(imm), "unexpected f64 size");
    std::memcpy(&bits, &imm, sizeof(bits));
    emitMovImm64(os, kScratchGPR2, bits);
    emitFMovGR(os, dst, kScratchGPR2);
}

/// @copydoc AsmEmitter::emitFMovGR()
void AsmEmitter::emitFMovGR(std::ostream &os, PhysReg dst, PhysReg src) const {
    // fmov dN, xM - transfer bits from GPR to FPR without conversion
    os << "  fmov ";
    printD(os, dst);
    os << ", " << rn(src) << "\n";
}

/// @copydoc AsmEmitter::emitFAddRRR()
void AsmEmitter::emitFAddRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3D(os, "fadd", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitFSubRRR()
void AsmEmitter::emitFSubRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3D(os, "fsub", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitFMulRRR()
void AsmEmitter::emitFMulRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3D(os, "fmul", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitFDivRRR()
void AsmEmitter::emitFDivRRR(std::ostream &os, PhysReg dst, PhysReg lhs, PhysReg rhs) const {
    emit3D(os, "fdiv", dst, lhs, rhs);
}

/// @copydoc AsmEmitter::emitFCmpRR()
void AsmEmitter::emitFCmpRR(std::ostream &os, PhysReg lhs, PhysReg rhs) const {
    os << "  fcmp ";
    printD(os, lhs);
    os << ", ";
    printD(os, rhs);
    os << "\n";
}

/// @copydoc AsmEmitter::emitSCvtF()
void AsmEmitter::emitSCvtF(std::ostream &os, PhysReg dstFPR, PhysReg srcGPR) const {
    os << "  scvtf ";
    printD(os, dstFPR);
    os << ", " << rn(srcGPR) << "\n";
}

/// @copydoc AsmEmitter::emitFCvtZS()
void AsmEmitter::emitFCvtZS(std::ostream &os, PhysReg dstGPR, PhysReg srcFPR) const {
    os << "  fcvtzs " << rn(dstGPR) << ", ";
    printD(os, srcFPR);
    os << "\n";
}

/// @copydoc AsmEmitter::emitUCvtF()
void AsmEmitter::emitUCvtF(std::ostream &os, PhysReg dstFPR, PhysReg srcGPR) const {
    os << "  ucvtf ";
    printD(os, dstFPR);
    os << ", " << rn(srcGPR) << "\n";
}

/// @copydoc AsmEmitter::emitFCvtZU()
void AsmEmitter::emitFCvtZU(std::ostream &os, PhysReg dstGPR, PhysReg srcFPR) const {
    os << "  fcvtzu " << rn(dstGPR) << ", ";
    printD(os, srcFPR);
    os << "\n";
}

/// @copydoc AsmEmitter::emitFRintN()
void AsmEmitter::emitFRintN(std::ostream &os, PhysReg dstFPR, PhysReg srcFPR) const {
    os << "  frintn ";
    printD(os, dstFPR);
    os << ", ";
    printD(os, srcFPR);
    os << "\n";
}

/// @copydoc AsmEmitter::emitFunction()
void AsmEmitter::emitFunction(std::ostream &os, const MFunction &fn) const {
    emitFunctionHeader(os, fn.name);

    // Leaf function optimization: skip frame setup entirely when the function
    // makes no calls, uses no callee-saved registers, and has no local frame.
    // LegalizePass inserts any backend-required calls before emission, so
    // fn.isLeaf already reflects the emitted instruction stream.
    const bool skipFrame =
        fn.isLeaf && fn.savedGPRs.empty() && fn.savedFPRs.empty() && fn.localFrameSize == 0;

    const bool usePlan = !fn.savedGPRs.empty() || !fn.savedFPRs.empty() || fn.localFrameSize > 0;
    FramePlan plan;
    if (usePlan) {
        plan.saveGPRs = fn.savedGPRs;
        plan.saveFPRs = fn.savedFPRs;
        plan.localFrameSize = fn.localFrameSize;
    }
    if (!skipFrame) {
        if (usePlan)
            emitPrologue(os, plan);
        else
            emitPrologue(os);
    }

    // Store the plan for use by Ret instructions
    currentPlan_ = &plan;
    currentPlanValid_ = usePlan;
    skipFrame_ = skipFrame;

    for (const auto &bb : fn.blocks)
        emitBlock(os, bb);

    currentPlan_ = nullptr;
    currentPlanValid_ = false;
    skipFrame_ = false;

    // On Linux ELF, emit .size after the function body so the linker and
    // profilers can determine the function's byte extent.
    // Windows PE/COFF does not use .size directives.
    if (target_->isLinux()) {
        const std::string sym = mangleSymbolImpl(fn.name, /*isDarwin=*/false);
        os << ".size " << sym << ", .-" << sym << "\n";
    }
    // Note: epilogue is emitted by each Ret instruction, not here
}

/// @copydoc AsmEmitter::emitBlock()
void AsmEmitter::emitBlock(std::ostream &os, const MBasicBlock &bb) const {
    if (!bb.name.empty())
        os << sanitizeLabel(bb.name) << ":\n";
    for (const auto &mi : bb.instrs)
        emitInstruction(os, mi);
}

/// @copydoc AsmEmitter::emitInstruction()
void AsmEmitter::emitInstruction(std::ostream &os, const MInstr &mi) const {
    // Provide single-argument wrappers used by the generated OpcodeDispatch.inc,
    // which was generated without knowledge of the isDarwin parameter.
    // These lambdas shadow the free-function names within this scope.
    const bool kDarwin_ = !target_->isLinux() && !target_->isWindows();
    /// @brief Map and mangle one data symbol for the active object format.
    /// @param n Unmangled symbol name.
    /// @return Target-specific assembly symbol spelling.
    [[maybe_unused]] auto mangleSymbol = [kDarwin_](const std::string &n) -> std::string {
        return mangleSymbolImpl(mapRuntimeSymbol(n), kDarwin_);
    };
    /// @brief Mangle one call target for the active object format.
    /// @param n Unmangled call-target name.
    /// @return Target-specific assembly symbol spelling.
    [[maybe_unused]] auto mangleCallTarget = [kDarwin_](const std::string &n) -> std::string {
        return mangleCallTargetImpl(n, kDarwin_);
    };

    // Handle Ret specially since it needs the epilogue
    if (mi.opc == MOpcode::Ret) {
        if (skipFrame_) {
            // Leaf function with no frame: just emit ret, no epilogue needed.
            os << "  ret\n";
        } else if (currentPlanValid_ && currentPlan_)
            emitEpilogue(os, *currentPlan_);
        else
            emitEpilogue(os);
        return;
    }

    // Fallback handling for opcodes that may be missing from the generated
    // dispatch table due to generator drift. Keep these early and return to
    // avoid duplicate emission when present in the generated switch.
    /// @brief Extract a physical register from one MIR operand.
    /// @param op Operand to validate and decode.
    /// @return Physical register identifier.
    /// @pre `op` must encode a physical register.
    auto getReg = [](const MOperand &op) -> PhysReg {
        if (op.kind != MOperand::Kind::Reg)
            ZANNA_ICE("expected register operand in AArch64 asm emitter");
        if (!op.reg.isPhys)
            ZANNA_ICE("virtual register v" + std::to_string(op.reg.idOrPhys) +
                      " reached AArch64 asm emitter (register allocation bug)");
        return static_cast<PhysReg>(op.reg.idOrPhys);
    };
    /// @brief Extract an immediate value from one MIR operand.
    /// @param op Operand to validate and decode.
    /// @return Signed immediate value.
    /// @pre `op` must encode an immediate.
    auto getImm = [](const MOperand &op) -> long long {
        if (op.kind != MOperand::Kind::Imm)
            ZANNA_ICE("expected immediate operand in AArch64 asm emitter");
        return op.imm;
    };
    /// @brief Emit target-specific ADRP syntax for one symbol page.
    /// @param dst Destination register.
    /// @param label Unmangled symbol label.
    const auto emitAdrPage = [&](PhysReg dst, const std::string &label) {
        const std::string sym = mangleSymbol(label);
        if (target_->isLinux() || target_->isWindows())
            os << "  adrp " << rn(dst) << ", " << sym << "\n";
        else
            os << "  adrp " << rn(dst) << ", " << sym << "@PAGE\n";
    };
    /// @brief Emit target-specific page-offset addition syntax.
    /// @param dst Destination register.
    /// @param base Register holding the symbol page.
    /// @param label Unmangled symbol label.
    const auto emitAddPageOff = [&](PhysReg dst, PhysReg base, const std::string &label) {
        const std::string sym = mangleSymbol(label);
        if (target_->isLinux() || target_->isWindows())
            os << "  add " << rn(dst) << ", " << rn(base) << ", :lo12:" << sym << "\n";
        else
            os << "  add " << rn(dst) << ", " << rn(base) << ", " << sym << "@PAGEOFF\n";
    };
    switch (mi.opc) {
        case MOpcode::SDivRRR:
            emitSDivRRR(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getReg(mi.ops[2]));
            return;
        case MOpcode::SmulhRRR:
            emitSmulhRRR(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getReg(mi.ops[2]));
            return;
        case MOpcode::UmulhRRR:
            emitUmulhRRR(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getReg(mi.ops[2]));
            return;
        case MOpcode::UDivRRR:
            emitUDivRRR(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getReg(mi.ops[2]));
            return;
        case MOpcode::MSubRRRR:
            emitMSubRRRR(
                os, getReg(mi.ops[0]), getReg(mi.ops[1]), getReg(mi.ops[2]), getReg(mi.ops[3]));
            return;
        case MOpcode::AddFpImm:
            emitAddFpImm(os, getReg(mi.ops[0]), getImm(mi.ops[1]));
            return;
        case MOpcode::Ldr8RegFpImm:
            emitLdr8FromFp(os, getReg(mi.ops[0]), getImm(mi.ops[1]));
            return;
        case MOpcode::Str8RegFpImm:
            emitStr8ToFp(os, getReg(mi.ops[0]), getImm(mi.ops[1]));
            return;
        case MOpcode::Ldr16RegFpImm:
            emitLdr16FromFp(os, getReg(mi.ops[0]), getImm(mi.ops[1]));
            return;
        case MOpcode::Str16RegFpImm:
            emitStr16ToFp(os, getReg(mi.ops[0]), getImm(mi.ops[1]));
            return;
        case MOpcode::Ldr32RegFpImm:
            emitLdr32FromFp(os, getReg(mi.ops[0]), getImm(mi.ops[1]));
            return;
        case MOpcode::Str32RegFpImm:
            emitStr32ToFp(os, getReg(mi.ops[0]), getImm(mi.ops[1]));
            return;
        case MOpcode::Ldr8RegBaseImm:
            emitLdr8FromBase(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::Str8RegBaseImm:
            emitStr8ToBase(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::Ldr16RegBaseImm:
            emitLdr16FromBase(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::Str16RegBaseImm:
            emitStr16ToBase(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::Ldr32RegBaseImm:
            emitLdr32FromBase(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::Str32RegBaseImm:
            emitStr32ToBase(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::LdrFprBaseImm:
            emitLdrFprFromBase(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::StrFprBaseImm:
            emitStrFprToBase(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::LdrRegBaseRegLsl:
        case MOpcode::StrRegBaseRegLsl:
        case MOpcode::Ldr32RegBaseRegLsl:
        case MOpcode::Str32RegBaseRegLsl:
        case MOpcode::LdrFprBaseRegLsl:
        case MOpcode::StrFprBaseRegLsl: {
            // ldr/str Rt, [Xn, Xm, lsl #k] — scaled register-offset form.
            const bool isLoad = mi.opc == MOpcode::LdrRegBaseRegLsl ||
                                mi.opc == MOpcode::Ldr32RegBaseRegLsl ||
                                mi.opc == MOpcode::LdrFprBaseRegLsl;
            const bool is32 =
                mi.opc == MOpcode::Ldr32RegBaseRegLsl || mi.opc == MOpcode::Str32RegBaseRegLsl;
            const bool isFpr =
                mi.opc == MOpcode::LdrFprBaseRegLsl || mi.opc == MOpcode::StrFprBaseRegLsl;
            const long long shift = getImm(mi.ops[3]);
            os << (isLoad ? "  ldr " : "  str ");
            if (isFpr) {
                printD(os, getReg(mi.ops[0]));
            } else if (is32) {
                const char *xn = rn(getReg(mi.ops[0]));
                os << 'w' << (xn + 1);
            } else {
                os << rn(getReg(mi.ops[0]));
            }
            os << ", [" << rn(getReg(mi.ops[1])) << ", " << rn(getReg(mi.ops[2]));
            if (shift != 0)
                os << ", lsl #" << shift;
            os << "]\n";
            return;
        }
        case MOpcode::AddRRRLsl:
        case MOpcode::SubRRRLsl:
        case MOpcode::AndRRRLsl:
        case MOpcode::OrrRRRLsl:
        case MOpcode::EorRRRLsl: {
            const char *mnemonic = mi.opc == MOpcode::AddRRRLsl   ? "add"
                                   : mi.opc == MOpcode::SubRRRLsl ? "sub"
                                   : mi.opc == MOpcode::AndRRRLsl ? "and"
                                   : mi.opc == MOpcode::OrrRRRLsl ? "orr"
                                                                  : "eor";
            os << "  " << mnemonic << ' ' << rn(getReg(mi.ops[0])) << ", " << rn(getReg(mi.ops[1]))
               << ", " << rn(getReg(mi.ops[2])) << ", lsl #" << getImm(mi.ops[3]) << "\n";
            return;
        }
        case MOpcode::TstRR:
            emitTstRR(os, getReg(mi.ops[0]), getReg(mi.ops[1]));
            return;
        case MOpcode::Bl:
            // Direct call to named symbol - apply runtime name mapping and mangling.
            // Uses the local mangleCallTarget lambda (captures kDarwin_).
            os << "  bl " << mangleCallTarget(mi.ops[0].label) << "\n";
            return;
        case MOpcode::Blr:
            os << "  blr " << rn(getReg(mi.ops[0])) << "\n";
            return;
        case MOpcode::AdrPage:
            emitAdrPage(getReg(mi.ops[0]), mi.ops[1].label);
            return;
        case MOpcode::AddPageOff:
            emitAddPageOff(getReg(mi.ops[0]), getReg(mi.ops[1]), mi.ops[2].label);
            return;
        case MOpcode::Cbz:
            os << "  cbz " << rn(getReg(mi.ops[0])) << ", " << sanitizeLabel(mi.ops[1].label)
               << "\n";
            return;
        case MOpcode::Tbz:
            os << "  tbz " << rn(getReg(mi.ops[0])) << ", #" << mi.ops[2].imm << ", "
               << sanitizeLabel(mi.ops[1].label) << "\n";
            return;
        case MOpcode::Tbnz:
            os << "  tbnz " << rn(getReg(mi.ops[0])) << ", #" << mi.ops[2].imm << ", "
               << sanitizeLabel(mi.ops[1].label) << "\n";
            return;
        case MOpcode::JumpTable: {
            // [0]=index, [1]=table name, [2..]=case labels. The dispatch tail
            // runs in the reserved X16/X17 scratch registers; the table is
            // emitted inline after the br with entries anchored to its start.
            const std::string tbl = sanitizeLabel(mi.ops[1].label);
            const std::string idx = rn(getReg(mi.ops[0]));
            os << "  adr x16, " << tbl << "\n";
            os << "  ldrsw x17, [x16, " << idx << ", lsl #2]\n";
            os << "  add x17, x17, x16\n";
            os << "  br x17\n";
            os << "  .p2align 2\n";
            os << tbl << ":\n";
            for (std::size_t i = 2; i < mi.ops.size(); ++i) {
                os << "  .long " << sanitizeLabel(mi.ops[i].label) << " - " << tbl << "\n";
            }
            return;
        }
        case MOpcode::LslvRRR:
            emitLslvRRR(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getReg(mi.ops[2]));
            return;
        case MOpcode::LsrvRRR:
            emitLsrvRRR(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getReg(mi.ops[2]));
            return;
        case MOpcode::AsrvRRR:
            emitAsrvRRR(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getReg(mi.ops[2]));
            return;
        case MOpcode::FMovGR:
            emitFMovGR(os, getReg(mi.ops[0]), getReg(mi.ops[1]));
            return;
        case MOpcode::FRintN:
            emitFRintN(os, getReg(mi.ops[0]), getReg(mi.ops[1]));
            return;
        case MOpcode::Br:
            os << "  b " << sanitizeLabel(mi.ops[0].label) << "\n";
            return;
        case MOpcode::BCond:
            os << "  b." << mi.ops[0].cond << " " << sanitizeLabel(mi.ops[1].label) << "\n";
            return;
        case MOpcode::Cbnz:
            os << "  cbnz " << rn(getReg(mi.ops[0])) << ", " << sanitizeLabel(mi.ops[1].label)
               << "\n";
            return;
        case MOpcode::AddsRRR:
            os << "  adds " << rn(getReg(mi.ops[0])) << ", " << rn(getReg(mi.ops[1])) << ", "
               << rn(getReg(mi.ops[2])) << "\n";
            return;
        case MOpcode::SubsRRR:
            os << "  subs " << rn(getReg(mi.ops[0])) << ", " << rn(getReg(mi.ops[1])) << ", "
               << rn(getReg(mi.ops[2])) << "\n";
            return;
        case MOpcode::AddsRI:
        case MOpcode::SubsRI: {
            const PhysReg dst = getReg(mi.ops[0]);
            const PhysReg lhs = getReg(mi.ops[1]);
            const long long imm = getImm(mi.ops[2]);
            const uint64_t magnitude = absImmUnsigned(imm);
            const bool isAdds = (mi.opc == MOpcode::AddsRI);
            if (const auto enc = classifyAddSubImmEncoding(magnitude)) {
                const bool emitAddImm = imm >= 0 ? isAdds : !isAdds;
                os << "  " << (emitAddImm ? "adds " : "subs ") << rn(dst) << ", " << rn(lhs)
                   << ", #" << enc->imm12;
                if (enc->shift12)
                    os << ", lsl #12";
                os << "\n";
            } else {
                const PhysReg scratch =
                    pickWideImmScratch({kScratchGPR2, kScratchGPR, kScratchGPR3}, {dst, lhs});
                emitMovImm64(os, scratch, static_cast<unsigned long long>(imm));
                os << "  " << (isAdds ? "adds " : "subs ") << rn(dst) << ", " << rn(lhs) << ", "
                   << rn(scratch) << "\n";
            }
            return;
        }
        case MOpcode::MAddRRRR:
            os << "  madd " << rn(getReg(mi.ops[0])) << ", " << rn(getReg(mi.ops[1])) << ", "
               << rn(getReg(mi.ops[2])) << ", " << rn(getReg(mi.ops[3])) << "\n";
            return;
        case MOpcode::AndRI:
            emitAndRI(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::OrrRI:
            emitOrrRI(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::EorRI:
            emitEorRI(os, getReg(mi.ops[0]), getReg(mi.ops[1]), getImm(mi.ops[2]));
            return;
        case MOpcode::Csel:
            os << "  csel " << rn(getReg(mi.ops[0])) << ", " << rn(getReg(mi.ops[1])) << ", "
               << rn(getReg(mi.ops[2])) << ", " << mi.ops[3].cond << "\n";
            return;
        case MOpcode::FCsel:
            os << "  fcsel ";
            printDReg(os, getReg(mi.ops[0]));
            os << ", ";
            printDReg(os, getReg(mi.ops[1]));
            os << ", ";
            printDReg(os, getReg(mi.ops[2]));
            os << ", " << mi.ops[3].cond << "\n";
            return;
        case MOpcode::LdpRegFpImm:
            if (!isPairImm7Offset(getImm(mi.ops[2]))) {
                emitLdrFromFp(os, getReg(mi.ops[0]), getImm(mi.ops[2]));
                emitLdrFromFp(os,
                              getReg(mi.ops[1]),
                              checkedOffsetAdd(getImm(mi.ops[2]), 8, "AArch64 ldp fallback"));
                return;
            }
            os << "  ldp " << rn(getReg(mi.ops[0])) << ", " << rn(getReg(mi.ops[1])) << ", [x29, #"
               << getImm(mi.ops[2]) << "]\n";
            return;
        case MOpcode::StpRegFpImm:
            if (!isPairImm7Offset(getImm(mi.ops[2]))) {
                emitStrToFp(os, getReg(mi.ops[0]), getImm(mi.ops[2]));
                emitStrToFp(os,
                            getReg(mi.ops[1]),
                            checkedOffsetAdd(getImm(mi.ops[2]), 8, "AArch64 stp fallback"));
                return;
            }
            os << "  stp " << rn(getReg(mi.ops[0])) << ", " << rn(getReg(mi.ops[1])) << ", [x29, #"
               << getImm(mi.ops[2]) << "]\n";
            return;
        case MOpcode::LdpFprFpImm: {
            if (!isPairImm7Offset(getImm(mi.ops[2]))) {
                emitLdrFprFromFp(os, getReg(mi.ops[0]), getImm(mi.ops[2]));
                emitLdrFprFromFp(
                    os,
                    getReg(mi.ops[1]),
                    checkedOffsetAdd(getImm(mi.ops[2]), 8, "AArch64 ldp fpr fallback"));
                return;
            }
            os << "  ldp ";
            printD(os, getReg(mi.ops[0]));
            os << ", ";
            printD(os, getReg(mi.ops[1]));
            os << ", [x29, #" << getImm(mi.ops[2]) << "]\n";
            return;
        }
        case MOpcode::StpFprFpImm: {
            if (!isPairImm7Offset(getImm(mi.ops[2]))) {
                emitStrFprToFp(os, getReg(mi.ops[0]), getImm(mi.ops[2]));
                emitStrFprToFp(os,
                               getReg(mi.ops[1]),
                               checkedOffsetAdd(getImm(mi.ops[2]), 8, "AArch64 stp fpr fallback"));
                return;
            }
            os << "  stp ";
            printD(os, getReg(mi.ops[0]));
            os << ", ";
            printD(os, getReg(mi.ops[1]));
            os << ", [x29, #" << getImm(mi.ops[2]) << "]\n";
            return;
        }
        default:
            break;
    }

#include "generated/OpcodeDispatch.inc"
}

} // namespace zanna::codegen::aarch64
