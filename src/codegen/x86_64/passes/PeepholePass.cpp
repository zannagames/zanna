//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/PeepholePass.cpp
// Purpose: Implement the explicit post-RA peephole pass for the x86-64 pipeline.
// Key invariants:
//   - Runs after register allocation on physical-register MIR.
// Ownership/Lifetime:
//   - Stateless; mutates Module::mir in place via Peephole utilities.
// Links: src/codegen/x86_64/passes/PeepholePass.hpp,
//        src/codegen/x86_64/Peephole.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/passes/PeepholePass.hpp"

#include "codegen/common/Parallelism.hpp"
#include "codegen/x86_64/Backend.hpp"
#include "codegen/x86_64/Peephole.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

/// @file
/// @brief Implements parallel post-allocation peephole rewriting and statistics.

namespace zanna::codegen::x64::passes {
namespace {

/// @brief Check whether the codegen-stats environment toggle is active.
/// @details Reads @c ZANNA_CODEGEN_STATS. Reporting is enabled when the value
///          exists and its first character is neither null nor @c '0'.
/// @return @c true when a summary warning should be emitted.
[[nodiscard]] bool codegenStatsEnabled() noexcept {
    if (const char *value = std::getenv("ZANNA_CODEGEN_STATS"))
        return value[0] != '\0' && value[0] != '0';
    return false;
}

} // namespace

/// @brief Run peephole rewrites over every function in @p module.
/// @details Dispatches per-function work either serially (small modules) or
///          through the shared bounded codegen worker policy. Each worker pulls
///          the next function index atomically. Rewriting is skipped at -O0.
///          When statistics are enabled, workers collect locally and merge
///          under a mutex before a single diagnostic summary is emitted.
/// @param module Pipeline state whose @c mir vector is mutated in place.
/// @param diags Diagnostic sink; also receives the optional stats summary.
/// @return True on success or when peepholes are skipped.
bool PeepholePass::run(Module &module, Diagnostics &diags) {
    if (!module.registersAllocated) {
        diags.error("peephole: register allocation must run before backend optimization");
        return false;
    }

    if (module.options.optimizeLevel < 1)
        return true;

    if (module.target == nullptr)
        module.target = &selectTarget(module.options.targetABI);

    const bool collectStats = codegenStatsEnabled();
    std::atomic_size_t total{0};
    const std::size_t workerCount = common::codegenWorkerCount(module.mir.size());
    if (workerCount <= 1) {
        for (auto &fn : module.mir) {
            const std::size_t transformed = runPeepholes(fn, *module.target);
            if (collectStats)
                total.fetch_add(transformed, std::memory_order_relaxed);
        }
    } else {
        std::atomic_size_t nextIndex{0};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            /// @brief Rewrite atomically claimed functions and merge the local count.
            workers.emplace_back([&]() {
                std::size_t localTotal = 0;
                for (;;) {
                    const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (index >= module.mir.size())
                        break;
                    localTotal += runPeepholes(module.mir[index], *module.target);
                }
                if (collectStats)
                    total.fetch_add(localTotal, std::memory_order_relaxed);
            });
        }
        for (auto &worker : workers)
            worker.join();
    }

    // The MIR shape counters live in CodegenStatsPass, which runs on the
    // final MIR at every optimization level.
    if (collectStats)
        diags.warning("x86-64 peephole: " + std::to_string(total.load()) + " transformations");

    return true;
}

} // namespace zanna::codegen::x64::passes
