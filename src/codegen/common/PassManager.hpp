//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/PassManager.hpp
// Purpose: Target-independent pass manager templated on backend Module type.
// Key invariants: Passes run sequentially, short-circuiting on failure;
//                 diagnostics are checked after every pass to catch silent errors.
// Ownership/Lifetime: PassManager takes unique_ptr ownership of all registered
//                     passes and retains them for the manager's lifetime.
// Links: docs/internals/architecture.md, codegen/common/Diagnostics.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/Diagnostics.hpp"

#include <chrono>
#include <exception>
#include <iosfwd>
#include <memory>
#include <ostream>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>

/// @file
/// @brief Defines the sequential, diagnostic-aware native codegen pass manager.

namespace zanna::codegen::common {

/// @brief Abstract interface implemented by individual pipeline passes.
/// @tparam ModuleT The backend-specific module state type.
template <typename ModuleT> class Pass {
  public:
    /// @brief Destroy a backend pass through the common interface.
    virtual ~Pass() = default;
    /// @brief Execute the pass over @p module, emitting diagnostics to @p diags.
    /// @param module The backend-specific module state to transform.
    /// @param diags  Diagnostic sink for warnings and errors encountered during the pass.
    /// @return True if the pass completed successfully, false on failure.
    virtual bool run(ModuleT &module, Diagnostics &diags) = 0;
};

/// @brief Container sequencing registered passes for execution.
/// @tparam ModuleT The backend-specific module state type.
template <typename ModuleT> class PassManager {
  public:
    /// @brief Add a pass to the manager; ownership is transferred.
    /// @param pass The pass to register; the manager takes unique ownership.
    void addPass(std::unique_ptr<Pass<ModuleT>> pass) {
        passes_.push_back(std::move(pass));
    }

    /// @brief Enable or disable per-pass timing diagnostics.
    /// @param stream Destination for timing lines, or null to disable timing.
    /// @param prefix Optional backend/pipeline component inserted into timing keys.
    void setTimingStream(std::ostream *stream, std::string prefix = {}) {
        timingStream_ = stream;
        timingPrefix_ = std::move(prefix);
    }

    /// @brief Execute all registered passes in order.
    /// @details Converts standard and non-standard pass exceptions into
    ///          `V-CG-PASS-EXCEPTION`, emits timing after successful invocation,
    ///          and stops when a pass returns false or records an error.
    /// @param module The backend-specific module state to transform.
    /// @param diags  Diagnostic sink checked after each pass for errors.
    /// @return False when a pass signals failure or diagnostics contain errors.
    bool run(ModuleT &module, Diagnostics &diags) const {
        for (const auto &pass : passes_) {
            bool ok = false;
            const auto start = std::chrono::steady_clock::now();
            try {
                ok = pass->run(module, diags);
            } catch (const std::exception &ex) {
                diags.error("V-CG-PASS-EXCEPTION",
                            std::string("codegen pass '") + typeid(*pass).name() +
                                "' threw exception: " + ex.what());
                return false;
            } catch (...) {
                diags.error("V-CG-PASS-EXCEPTION",
                            std::string("codegen pass '") + typeid(*pass).name() +
                                "' threw non-standard exception");
                return false;
            }
            if (timingStream_) {
                const auto elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start);
                *timingStream_ << "[time-compile] codegen";
                if (!timingPrefix_.empty())
                    *timingStream_ << "." << timingPrefix_;
                *timingStream_ << "." << typeid(*pass).name() << " " << elapsed.count() << "ms\n";
            }
            if (!ok) {
                return false;
            }
            // A pass may report errors via Diagnostics but still return true.
            // Catch that case to avoid silent miscompilation.
            if (diags.hasErrors()) {
                return false;
            }
        }
        return true;
    }

  private:
    /// Owned passes in execution order.
    std::vector<std::unique_ptr<Pass<ModuleT>>> passes_{};

    /// Optional non-owning timing destination.
    std::ostream *timingStream_{nullptr};

    /// Optional timing-key component.
    std::string timingPrefix_{};
};

} // namespace zanna::codegen::common
