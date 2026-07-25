//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/EmitPass.cpp
// Purpose: Assembly-text emission pass for the AArch64 modular pipeline.
//          Emits rodata globals, then each MIR function via AsmEmitter;
//          appends platform-specific module-level directives at the end.
// Key invariants:
//   - Requires AArch64Module::ti to be non-null.
//   - Requires register allocation to have completed (operates on physical regs).
//   - .subsections_via_symbols emitted on Mach-O for dead-strip support.
//   - .note.GNU-stack emitted on Linux ELF to mark non-executable stack.
// Ownership/Lifetime:
//   - Stateless pass; writes into AArch64Module::assembly string.
// Links: src/codegen/aarch64/passes/EmitPass.hpp,
//        src/codegen/aarch64/AsmEmitter.hpp,
//        src/codegen/aarch64/RodataPool.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/EmitPass.hpp"

#include "codegen/aarch64/AsmEmitter.hpp"

#include <sstream>

/**
 * @file
 * @brief Implements complete target-specific AArch64 assembly serialization.
 *
 * Read-only and writable globals precede functions. A final object-format
 * directive enables Mach-O subsection dead stripping or marks the ELF stack
 * non-executable; PE/COFF needs neither trailer.
 */

namespace zanna::codegen::aarch64::passes {

/**
 * @brief Emits globals, functions, and format-specific trailer directives.
 * @param[in,out] module Module whose `assembly` string is replaced on success.
 * @param[in,out] diags Sink receiving a missing-target diagnostic.
 * @return `false` only when `module.ti` is null; otherwise `true`.
 */
bool EmitPass::run(AArch64Module &module, Diagnostics &diags) {
    if (!module.ti) {
        diags.error("EmitPass: ti must be non-null");
        return false;
    }

    std::ostringstream os;

    // Emit rodata globals (string literals etc.) before function bodies.
    // NOTE: EarlyCSE/GVN now handle ConstStr via ValueKey (line 149 of
    // ValueKey.cpp).  Residual duplicate adrp+add pairs may still appear if
    // SCCP constant-propagates const_str values after CSE has already run.
    // Monitor whether pass ordering changes eliminate the remaining cases.
    module.rodataPool.emit(os, *module.ti);
    // Emit writable scalar globals (.data) so gaddr references resolve.
    module.rodataPool.emitData(os, *module.ti);

    AsmEmitter emitter{*module.ti};
    for (const auto &fn : module.mir) {
        emitter.emitFunction(os, fn);
        os << "\n";
    }

    // Emit platform-specific directives at end of module.
    // .subsections_via_symbols enables function-level dead stripping on macOS
    // and prevents the linker from setting MH_ALLOW_STACK_EXECUTION.
    // .note.GNU-stack marks the stack as non-executable on Linux ELF.
    if (module.ti->isLinux())
        os << ".section .note.GNU-stack,\"\",@progbits\n";
    else if (!module.ti->isWindows())
        os << ".subsections_via_symbols\n";

    module.assembly = os.str();
    return true;
}

} // namespace zanna::codegen::aarch64::passes
