//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/EffectAttrs.hpp
// Purpose: Shared semantic effect attributes for IL call targets.
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares semantic side-effect attributes shared by IL call targets.
 *
 * @details These independently represented guarantees describe exception and
 *          memory behavior for functions and external declarations. Analysis
 *          and optimization code may rely only on flags explicitly set by the
 *          producer or validated by the verifier.
 */

#pragma once

namespace il::core {

/// @brief Semantic effect attributes attached to callable IL declarations.
struct EffectAttrs {
    /// @brief Callable is guaranteed not to throw.
    bool nothrow = false;

    /// @brief Callable may read memory but performs no writes.
    bool readonly = false;

    /// @brief Callable is free of observable side effects and memory access.
    bool pure = false;
};

/// @brief Backwards-compatible name used by Function.
using FunctionAttrs = EffectAttrs;

} // namespace il::core
