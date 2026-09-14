//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/NativeAllocaZeroInit.hpp
// Purpose: Make every `alloca` zero its memory in native code, as the IL
//          specification requires and the VMs already do.
// Key invariants:
//   - Each constant-size alloca is followed, in its own block, by stores that
//     clear exactly its bytes, so the stack slot reads as zero on every execution.
//   - Only in-bounds stores of the allocation's width are emitted, so the module
//     still passes IL verification.
// Ownership/Lifetime:
//   - Rewrites the caller-owned module in place; allocates nothing that outlives it.
// Links: docs/il/il-guide.md (alloca), src/codegen/common/NativeEHLowering.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/core/Module.hpp"

/// @file
/// @brief Declares the native alloca zero-initialisation pass.

namespace zanna::codegen::common {

/// @brief Materialise alloca zero-initialisation for native code generation.
/// @details Native backends map an `alloca` to a frame slot without clearing it,
///          but IL defines alloca memory as zero-initialised. After each alloca
///          with a constant positive size this pass inserts stores of zero that
///          cover the allocation: `i64` stores for whole 8-byte chunks and `i32`,
///          `i16`, and `i1` stores for the remaining bytes. Allocations larger than
///          512 bytes clear their 8-byte chunks with a counted loop instead, which
///          splits the block. The IL optimizer later removes stores that a
///          following store overwrites, and promotes slots whose zero is only an
///          initial value.
/// @param[in,out] module Module whose functions are rewritten.
/// @return `true` when at least one alloca was given zeroing stores.
bool zeroInitAllocas(il::core::Module &module);

} // namespace zanna::codegen::common
