//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Param.hpp
// Purpose: Declares the Param struct -- parameters for both functions and
//          basic blocks in Zanna IL. Serves as the IL's equivalent of phi
//          nodes in traditional SSA form, with optional semantic attributes
//          (noalias, nocapture, nonnull) for optimisation hints.
// Key invariants:
//   - Param id is unique within its parent function or block.
//   - Param type must match the containing function or block signature.
//   - Attributes are hints only; they do not alter IL operational semantics.
// Ownership/Lifetime: Functions and BasicBlocks own Param structs by value
//          in std::vector. Each Param owns its name string and type. Parameter
//          lifetime matches the containing function or block.
// Links: docs/il/il-guide.md#reference, il/core/Type.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares typed IL function and basic-block parameters.
 *
 * @details Parameters combine an SSA identifier, diagnostic name, static type,
 *          and optional alias/lifetime guarantees. Function parameters describe
 *          call signatures; basic-block parameters receive values from
 *          predecessor-edge argument lists and serve the role of SSA phis.
 */

#pragma once

#include "il/core/Type.hpp"
#include <string>

namespace il::core {

/// @brief Attribute container associated with a parameter value.
/// @details Attributes convey aliasing and lifetime guarantees that future
///          optimisation passes may exploit when reasoning about calls and
///          aggregate construction.
struct ParamAttrs {
    /// @brief Parameter is guaranteed not to alias any other pointer argument.
    bool noalias = false;

    /// @brief Parameter value is not captured beyond the callee.
    bool nocapture = false;

    /// @brief Parameter is guaranteed never to be null.
    bool nonnull = false;
};

/// @brief Describes a function or basic block parameter.
/// Holds the metadata required to reference and type-check a parameter.
struct Param {
    /// @brief Name used for diagnostics and debugging.
    /// Owned by the Param and stored by value; may be empty for unnamed parameters.
    std::string name;

    /// @brief Static type of the parameter.
    /// Owned by the Param and stored by value.
    /// Must match the containing function or block signature.
    Type type;

    /// @brief Ordinal identifier assigned during IR construction.
    /// Unique within its parent function or block; defaults to 0 before assignment.
    unsigned id = 0;

    /// @brief Attribute bundle communicating aliasing and lifetime hints.
    ParamAttrs Attrs{};

    /// @brief Mark whether the parameter is @c noalias.
    /// @param value True if the parameter cannot alias other pointer arguments.
    void setNoAlias(bool value) {
        Attrs.noalias = value;
    }

    /// @brief Query whether the parameter carries the @c noalias attribute.
    /// @return True when @ref setNoAlias enabled the attribute.
    [[nodiscard]] bool isNoAlias() const {
        return Attrs.noalias;
    }

    /// @brief Mark whether the parameter is @c nocapture.
    /// @param value True when the parameter will not be captured beyond the callee.
    void setNoCapture(bool value) {
        Attrs.nocapture = value;
    }

    /// @brief Query whether the parameter carries the @c nocapture attribute.
    /// @return True when @ref setNoCapture enabled the attribute.
    [[nodiscard]] bool isNoCapture() const {
        return Attrs.nocapture;
    }

    /// @brief Mark whether the parameter is @c nonnull.
    /// @param value True if the parameter value is guaranteed non-null.
    void setNonNull(bool value) {
        Attrs.nonnull = value;
    }

    /// @brief Query whether the parameter carries the @c nonnull attribute.
    /// @return True when @ref setNonNull enabled the attribute.
    [[nodiscard]] bool isNonNull() const {
        return Attrs.nonnull;
    }
};

} // namespace il::core
