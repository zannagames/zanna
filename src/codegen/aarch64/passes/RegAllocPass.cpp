//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/RegAllocPass.cpp
// Purpose: Register allocation pass for the AArch64 modular pipeline.
//          Runs coalescer then linear-scan RA on every MIR function. Functions
//          are processed in parallel when hardware concurrency ≥ 2.
// Key invariants:
//   - Must run after LegalizePass (overflow pseudos must be expanded).
//   - Each function is allocated independently; errors are deferred and
//     reported as a single diagnostic after all workers finish.
//   - After this pass all virtual registers are replaced with physical regs.
// Ownership/Lifetime:
//   - Stateless pass; mutates AArch64Module::mir in place.
// Links: codegen/aarch64/passes/RegAllocPass.hpp,
//        codegen/aarch64/RegAllocLinear.hpp,
//        codegen/aarch64/Coalescer.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/RegAllocPass.hpp"

#include "codegen/aarch64/Coalescer.hpp"
#include "codegen/aarch64/RegAllocLinear.hpp"
#include "codegen/common/Parallelism.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

/// @file
/// @brief Implements copy coalescing and linear-scan allocation for AArch64 MIR.

namespace zanna::codegen::aarch64::passes {

/// @copydoc RegAllocPass::run
bool RegAllocPass::run(AArch64Module &module, Diagnostics &diags) {
    if (!module.ti) {
        diags.error("RegAllocPass: ti must be non-null");
        return false;
    }

    std::string firstError;
    std::mutex errorMutex;

    /// Coalesce and allocate the function at @p index, recording only the first
    /// allocation exception so concurrent failures produce one stable diagnostic.
    auto allocateOne = [&](std::size_t index) {
        auto &fn = module.mir[index];
        try {
            // Coalesce MovRR/FMovRR between virtual registers before register
            // allocation to reduce register pressure and eliminate redundant copies.
            coalesce(fn);
            [[maybe_unused]] auto result = allocate(fn, *module.ti);
        } catch (const std::exception &ex) {
            std::lock_guard<std::mutex> lock(errorMutex);
            if (firstError.empty())
                firstError = "AArch64 register allocation failed for function '" + fn.name +
                             "': " + ex.what();
        }
    };

    const std::size_t functionCount = module.mir.size();
    const std::size_t workerCount = common::codegenWorkerCount(functionCount);
    if (workerCount <= 1) {
        for (std::size_t i = 0; i < functionCount; ++i)
            allocateOne(i);
    } else {
        std::atomic_size_t nextIndex{0};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (std::size_t worker = 0; worker < workerCount; ++worker) {
            /// Claim unallocated function indices until the module is exhausted.
            workers.emplace_back([&]() {
                for (;;) {
                    const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (index >= functionCount)
                        break;
                    allocateOne(index);
                }
            });
        }
        for (auto &worker : workers)
            worker.join();
    }

    if (!firstError.empty()) {
        diags.error("V-CG-AARCH64-REGALLOC", firstError);
        return false;
    }

    return true;
}

} // namespace zanna::codegen::aarch64::passes
