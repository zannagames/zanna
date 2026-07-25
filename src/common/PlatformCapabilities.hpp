//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/common/PlatformCapabilities.hpp
// Purpose: Shared host/build capability vocabulary for C++ code outside the
//          C runtime platform layer.
// Key invariants:
//   - Normal C++ code should prefer these macros/constants over raw host
//     preprocessor probes.
//   - Host OS/compiler and build capability flags are generated once by CMake.
// Ownership/Lifetime:
//   - Header-only constants; no runtime state.
// Links: src/runtime/rt_platform.h, docs/cross-platform/platform-checklist.md
//
//===----------------------------------------------------------------------===//

/**
 * @file PlatformCapabilities.hpp
 * @brief Exposes the generated platform-capability vocabulary to common C++ code.
 *
 * This forwarding header is the policy boundary for host and build capability
 * checks outside the C runtime adapter layer. Consumers should include it
 * instead of repeating compiler- or operating-system-specific probes.
 */

#pragma once

/// Generated host, compiler, architecture, and configured-feature definitions.
#include "zanna/platform/Capabilities.hpp"
