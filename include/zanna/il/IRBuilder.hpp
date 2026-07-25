//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/il/IRBuilder.hpp
// Purpose: Stable façade for constructing IL modules without depending on src paths.
// Key invariants: Mirrors il::build::IRBuilder API; no additional behavior.
// Ownership/Lifetime: IRBuilder lifetime remains managed by callers.
// Links: docs/il/il-guide.md#reference, src/il/build/IRBuilder.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "il/build/IRBuilder.hpp"

/**
 * @file include/zanna/il/IRBuilder.hpp
 * @brief Exposes the supported IL builder interface to downstream clients.
 *
 * @details
 * This forwarding header gives embedders and language frontends access to
 * `il::build::IRBuilder` through the installed `zanna/il` include hierarchy.
 * It introduces no wrapper types or behavior, so insertion-point validity,
 * module mutation, reference stability, and ownership follow the underlying
 * builder declaration.
 */
