//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/il/Verify.hpp
// Purpose: Stable façade exposing IL verifier entry points.
// Key invariants: Mirrors il::verify::Verifier public API only.
// Ownership/Lifetime: Caller retains ownership of modules and diagnostics.
// Links: docs/il/il-guide.md#reference, src/il/verify/Verifier.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/verify/Verifier.hpp"

/**
 * @file include/zanna/il/Verify.hpp
 * @brief Exposes the supported IL verification interface to downstream code.
 *
 * @details
 * This forwarding header makes the verifier API available through the
 * installed `zanna/il` include hierarchy. It adds no adapters or state:
 * callers retain ownership of the module being checked and receive diagnostics
 * according to the contracts on `il::verify::Verifier`.
 *
 * Verifier acceptance rules must remain aligned with the normative IL
 * reference linked in the source-file header.
 */
