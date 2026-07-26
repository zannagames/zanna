//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/VMConstants.hpp
// Purpose: Centralized constants for VM configuration and limits
// Key invariants: All constants are compile-time evaluable
// Ownership/Lifetime: Static constants with program lifetime
// Links: docs/vm-design.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines compile-time limits, pool capacities, polling defaults, and
///        debugger sentinels shared by the VM implementation.
/// @details Keeping these values in one header ensures dispatch, frame
///          allocation, interruption, and debugging code use identical
///          thresholds without introducing runtime storage.

#pragma once

#include <cstddef>
#include <cstdint>

namespace il::vm {

/// @brief Maximum recursion depth for interpreter.
/// @details Prevents stack overflow from unbounded recursion.
///          This limit is conservative and can be adjusted based on
///          platform stack size and profiling data.
constexpr size_t kMaxRecursionDepth = 1000;

/// @brief Maximum number of instructions to execute before interrupt check.
/// @details Balances responsiveness vs overhead. Can be overridden via
///          ZANNA_INTERRUPT_EVERY_N environment variable.
constexpr uint64_t kDefaultInterruptCheckInterval = 10000;

/// @brief Initial capacity for the execution stack.
/// @details Pre-allocated to avoid reallocation during typical execution.
///          Most programs have call depths < 64. Using inline storage for
///          this capacity eliminates heap allocation in common cases.
constexpr size_t kExecStackInitialCapacity = 64;

/// @brief Maximum pooled stack buffers per VM.
/// @details Pooling stack buffers avoids repeated 64KB allocations during
///          recursive or repeated function calls. 8 buffers covers typical
///          recursive depth while limiting memory overhead to 512KB.
constexpr size_t kStackBufferPoolSize = 8;

/// @brief Maximum pooled register files per VM.
/// @details Register files vary in size but pooling avoids allocation churn
///          for functions with similar SSA counts. Keeps up to 16 buffers.
constexpr size_t kRegisterFilePoolSize = 16;

/// @brief Sentinel value for debug break results indicating a breakpoint hit.
/// @details Returned via Slot::i64 from debug hooks to signal that execution
///          should pause because a breakpoint was reached.
constexpr int64_t kDebugBreakpointSentinel = 10;

/// @brief Sentinel value for debug break results indicating a generic pause.
/// @details Returned via Slot::i64 from debug hooks to signal that execution
///          should pause for non-breakpoint reasons (e.g., step limit exceeded).
constexpr int64_t kDebugPauseSentinel = 1;

} // namespace il::vm
