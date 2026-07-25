//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
/// @file Options.cpp
/// @brief Defines the BASIC frontend's process-wide feature flags.
/// @details Implements the storage and atomic accessors declared in
///          `Options.hpp`. Each flag is backed by a relaxed atomic so tools can
///          toggle behavior without introducing data races. The intended usage
///          is to configure these flags before compilation threads start.
//
//===----------------------------------------------------------------------===//

#include "frontends/basic/Options.hpp"

#include <atomic>

namespace il::frontends::basic {

/// @brief Allow USING Zanna.* imports and references to runtime namespaces.
static std::atomic<bool> g_enableRuntimeNamespaces{true};
/// @brief Allow lowering of selected runtime type constructors.
static std::atomic<bool> g_enableRuntimeTypeBridging{true};
/// @brief Accept CONST/CHR$ expressions as SELECT CASE labels.
static std::atomic<bool> g_enableSelectCaseConstLabels{true};

/// @brief Return whether runtime namespaces are enabled.
/// @details Uses relaxed atomic load; see `Options.hpp` for the threading model
///          and recommended usage pattern.
/// @return Current process-wide runtime-namespace flag.
bool FrontendOptions::enableRuntimeNamespaces() {
    return g_enableRuntimeNamespaces.load(std::memory_order_relaxed);
}

/// @brief Enable or disable runtime namespaces for the current process.
/// @details Uses relaxed atomic store; callers should configure this before
///          launching compilation threads for consistent behavior.
/// @param on New process-wide value.
void FrontendOptions::setEnableRuntimeNamespaces(bool on) {
    g_enableRuntimeNamespaces.store(on, std::memory_order_relaxed);
}

/// @brief Return whether runtime type bridging is enabled.
/// @details Uses relaxed atomic load; see `Options.hpp` for the threading model.
/// @return Current process-wide runtime-type-bridging flag.
bool FrontendOptions::enableRuntimeTypeBridging() {
    return g_enableRuntimeTypeBridging.load(std::memory_order_relaxed);
}

/// @brief Enable or disable runtime type bridging for this process.
/// @details Uses relaxed atomic store; should be set during configuration.
/// @param on New process-wide value.
void FrontendOptions::setEnableRuntimeTypeBridging(bool on) {
    g_enableRuntimeTypeBridging.store(on, std::memory_order_relaxed);
}

/// @brief Return whether CONST/CHR$ labels are allowed in SELECT CASE.
/// @details Uses relaxed atomic load; see `Options.hpp` for full semantics.
/// @return Current process-wide SELECT CASE constant-label flag.
bool FrontendOptions::enableSelectCaseConstLabels() {
    return g_enableSelectCaseConstLabels.load(std::memory_order_relaxed);
}

/// @brief Enable or disable CONST/CHR$ labels in SELECT CASE.
/// @details Uses relaxed atomic store; set before parsing for consistency.
/// @param on New process-wide value.
void FrontendOptions::setEnableSelectCaseConstLabels(bool on) {
    g_enableSelectCaseConstLabels.store(on, std::memory_order_relaxed);
}

} // namespace il::frontends::basic
