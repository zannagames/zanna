//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/Parallelism.hpp
// Purpose: Shared worker-count policy for codegen passes.
// Key invariants:
//   - Codegen parallel loops use one bounded policy rather than each pass
//     spawning up to hardware_concurrency() threads independently.
//   - ZANNA_CODEGEN_THREADS can lower or raise the cap for CI and debugging.
//   - The helper never returns zero for non-empty work.
// Ownership/Lifetime:
//   - Header-only utility; no persistent state.
// Links: codegen/x86_64/Backend.cpp, codegen/aarch64/passes/BinaryEmitPass.cpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <limits>
#include <thread>

/// @file
/// @brief Defines the shared bounded worker-count policy for native codegen.

namespace zanna::codegen::common {

/// @brief Default upper bound for codegen worker threads.
/// @details Codegen passes are often memory-bandwidth heavy and may be invoked
///          in build systems that already run multiple compiler processes.
///          Keeping the default cap modest avoids oversubscription while still
///          allowing substantial parallelism on large modules.
inline constexpr std::size_t kDefaultCodegenWorkerCap = 8;

/// @brief Parse an unsigned thread-count limit from an environment string.
/// @details Accepts decimal ASCII digits only. Invalid, empty, or overflowing
///          values return zero, which callers treat as "no usable override".
/// @param raw Environment variable contents, or nullptr when unset.
/// @return Parsed positive value, or zero when no valid value is present.
[[nodiscard]] inline std::size_t parseThreadLimit(const char *raw) noexcept {
    if (raw == nullptr || *raw == '\0') {
        return 0;
    }
    std::size_t value = 0;
    for (const char *cur = raw; *cur != '\0'; ++cur) {
        if (*cur < '0' || *cur > '9') {
            return 0;
        }
        const std::size_t digit = static_cast<std::size_t>(*cur - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
            return 0;
        }
        value = value * 10U + digit;
    }
    return value;
}

/// @brief Compute a bounded worker count for a codegen parallel loop.
/// @details The result is the minimum of the item count, detected hardware
///          concurrency, and a cap. The cap defaults to
///          @ref kDefaultCodegenWorkerCap but can be overridden by
///          @c ZANNA_CODEGEN_THREADS. For non-empty work the return value is
///          always at least one, so callers can use it directly to choose
///          sequential versus parallel execution.
/// @param itemCount Number of independent work items.
/// @param defaultCap Cap used when @c ZANNA_CODEGEN_THREADS is unset or invalid.
/// @return Worker count in the range [0, itemCount].
[[nodiscard]] inline std::size_t codegenWorkerCount(
    std::size_t itemCount, std::size_t defaultCap = kDefaultCodegenWorkerCap) noexcept {
    if (itemCount == 0) {
        return 0;
    }
    const std::size_t hw =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::thread::hardware_concurrency()));
    const std::size_t envCap = parseThreadLimit(std::getenv("ZANNA_CODEGEN_THREADS"));
    const std::size_t effectiveCap = envCap != 0 ? envCap : std::max<std::size_t>(1, defaultCap);
    return std::min(itemCount, std::min(hw, effectiveCap));
}

} // namespace zanna::codegen::common
