//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/FunctionExecCache.cpp
// Purpose: Build and cache pre-resolved operand arrays per (function, block).
//
// Key invariants:
//   - IL is immutable after construction; caches never become stale.
//   - Cache building is idempotent: a second call for the same function
//     returns immediately without rebuilding.
//   - ResolvedOp::Cold is used for ConstStr/GlobalAddr/NullPtr so the fallback
//     path through VM::eval() still handles those correctly.
//
// Ownership/Lifetime:
//   - Caches are owned by VM::fnExecCache_ and destroyed with the VM.
//   - BlockExecCache* pointers handed to ExecState::blockCache are valid for
//     the lifetime of the VM (as long as IL is not deallocated).
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements lazy per-function caches of pre-resolved VM operands.
/// @details Cache construction converts common IL operands into compact
///          register or immediate representations while retaining a cold-path
///          marker for values that still require general evaluation.

#include "vm/VM.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Value.hpp"

#include <bit>
#include <cassert>
#include <limits>
#include <stdexcept>

namespace il::vm {
namespace {

/// @brief Convert one IL value to its compact @c ResolvedOp representation.
/// @details The three hot-path kinds (Temp → Reg, ConstInt → ImmI64,
///          ConstFloat → ImmF64) are handled directly.  All other kinds
///          produce a @c Cold entry that tells the evaluator to call
///          @c VM::eval() with the original @c Value.
/// @param v Source IL value.
/// @return Compact resolved operand.
ResolvedOp resolveValue(const il::core::Value &v) noexcept {
    ResolvedOp op{};
    switch (v.kind) {
        case il::core::Value::Kind::Temp:
            op.kind = ResolvedOp::Kind::Reg;
            op.regId = v.id;
            break;
        case il::core::Value::Kind::ConstInt:
            op.kind = ResolvedOp::Kind::ImmI64;
            op.numVal = static_cast<int64_t>(v.i64);
            break;
        case il::core::Value::Kind::ConstFloat:
            op.kind = ResolvedOp::Kind::ImmF64;
            // Store f64 as bit-identical int64 so the cache entry is POD-trivial.
            op.numVal = std::bit_cast<int64_t>(v.f64);
            break;
        default:
            // ConstStr, GlobalAddr, NullPtr — handled by VM::eval() cold path
            op.kind = ResolvedOp::Kind::Cold;
            break;
    }
    return op;
}

/// @brief Build the @c BlockExecCache for one basic block.
/// @details Iterates instructions in order and fills a flat @c resolvedOps
///          array, recording the starting offset of each instruction's
///          operands in @c instrOpOffset.
/// @param block Source basic block (IL, immutable after construction).
/// @return Populated @c BlockExecCache.
/// @throws std::overflow_error If accumulated operand offsets exceed the
///         representable @c size_t range.
BlockExecCache buildBlockExecCacheFor(const il::core::BasicBlock &block) {
    BlockExecCache bc;
    bc.instrOpOffset.reserve(block.instructions.size());
    size_t offset = 0;
    for (const auto &instr : block.instructions) {
        bc.instrOpOffset.push_back(offset);
        for (const auto &op : instr.operands)
            bc.resolvedOps.push_back(resolveValue(op));
        if (instr.operands.size() > std::numeric_limits<size_t>::max() - offset)
            throw std::overflow_error("VM operand cache offset overflow");
        offset += instr.operands.size();
    }
    return bc;
}

} // namespace

/// @brief Obtain or lazily build the pre-resolved operand cache for @p bb.
/// @details On the first call for a given function the cache is built for
///          every block in that function (the cost is amortised across all
///          subsequent block entries).  Subsequent calls are O(1) map
///          lookups.
/// @param fn  Function that contains @p bb; used as the outer cache key.
/// @param bb  Basic block whose @c BlockExecCache is requested.
/// @return Pointer to the immutable @c BlockExecCache, or @c nullptr when
///         either argument is null.
/// @throws std::overflow_error If a block's operand offsets overflow while the
///         containing function cache is built.
const BlockExecCache *VM::getOrBuildBlockCache(const il::core::Function *fn,
                                               const il::core::BasicBlock *bb) {
    if (!fn || !bb)
        return nullptr;

    auto &blockMap = fnExecCache_[fn];
    if (blockMap.empty()) {
        // Build entries for every block in the function in one pass.
        blockMap.reserve(fn->blocks.size());
        for (const auto &block : fn->blocks)
            blockMap.emplace(&block, buildBlockExecCacheFor(block));
    }

    auto it = blockMap.find(bb);
    return it != blockMap.end() ? &it->second : nullptr;
}

} // namespace il::vm
