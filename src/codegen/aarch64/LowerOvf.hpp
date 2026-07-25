//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/LowerOvf.hpp
// Purpose: Expand overflow-checked arithmetic pseudo-opcodes for AArch64.
//          AddOvfRRR/SubOvfRRR/AddOvfRI/SubOvfRI/MulOvfRRR are replaced with
//          flag-setting real instructions (ADDS/SUBS, or MUL+SMULH for
//          multiply) followed by a conditional branch to a shared trap block
//          on signed overflow.
// Key invariants:
//   - Runs on MIR before register allocation; expansion is in-place per block.
//   - At most one shared trap block is used per function and reused by every
//     overflow site; it calls rt_trap_ovf, which never returns.
//   - Add/Sub overflow is detected via the V flag (b.vs); multiply overflow
//     via the smulh-vs-asr#63 sign-extension mismatch (b.ne).
// Ownership/Lifetime:
//   - Stateless free function; mutates the caller-owned MFunction and retains
//     no state across calls.
// Links: src/codegen/aarch64/LowerOvf.cpp,
//        src/codegen/aarch64/MachineIR.hpp,
//        src/codegen/aarch64/Noreturn.hpp (trap-call classification)
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/aarch64/MachineIR.hpp"

/**
 * @file
 * @brief Declares expansion of signed-overflow MIR pseudo-instructions.
 *
 * The pass converts checked addition, subtraction, and multiplication into
 * ordinary AArch64 operations plus branches to one shared, non-returning
 * overflow trap block. It must run while operands are still virtual registers
 * because checked multiplication introduces two temporary GPRs.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Expands overflow-checked arithmetic pseudo-opcodes into guarded sequences.
 *
 * Walks the original basic blocks and replaces `AddOvfRRR`, `SubOvfRRR`,
 * `AddOvfRI`, and `SubOvfRI` with flag-setting arithmetic followed by a
 * `b.vs`. A `MulOvfRRR` becomes a low-half multiply, a signed high-half
 * multiply, a sign-extension comparison, and a `b.ne`. Every guard targets
 * the same block, reusing an existing `rt_trap_ovf` block when present.
 *
 * If the function contains no checked-arithmetic pseudo-opcodes, the function
 * is left byte-for-byte unchanged. Otherwise any newly allocated virtual
 * register IDs are greater than every ID observed before expansion.
 *
 * @param[in,out] fn Machine IR function to rewrite in place.
 * @post Each expanded instruction is replaced by ordinary MIR plus an
 *       overflow guard.
 * @post All generated overflow branches target one block that calls
 *       `rt_trap_ovf`.
 */
void lowerOverflowOps(MFunction &fn);

} // namespace zanna::codegen::aarch64
