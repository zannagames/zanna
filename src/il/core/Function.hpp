//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Function.hpp
// Purpose: Declares the Function struct -- an IL function definition with
//          parameters, basic blocks, SSA value names, and semantic attributes.
//          Functions are the primary unit of code organisation in Zanna IL.
// Key invariants:
//   - Functions must contain at least one basic block.
//   - Block labels must be unique within the function.
//   - Parameter types and count must match the function signature.
//   - All control flow paths must terminate with proper terminators.
// Ownership/Lifetime: Module owns Functions by value in a std::vector.
//          Function owns BasicBlocks in stable storage plus Params and metadata.
//          Functions can be moved efficiently but are expensive to copy (deep
//          copy of all blocks).
// Links: docs/il/il-guide.md#reference, il/core/BasicBlock.hpp,
//        il/core/Param.hpp, il/core/Type.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares IL function definitions and calling-convention metadata.
 *
 * @details A `Function` owns its signature, stable basic-block storage,
 *          diagnostic SSA names, linkage, and semantic attributes. Its textual
 *          function identity may be accompanied by an interned module symbol
 *          for fast lookup, while the owned string remains canonical.
 */

#pragma once

#include "il/core/BasicBlock.hpp"
#include "il/core/EffectAttrs.hpp"
#include "il/core/Linkage.hpp"
#include "il/core/Param.hpp"
#include "il/core/Type.hpp"
#include "support/symbol.hpp"
#include <string>
#include <vector>

namespace il::core {

/// @brief Calling convention used when lowering an IL function to native code.
/// @details The current IL grammar exposes only the default platform calling
///          convention. Keeping this as explicit function metadata prevents the
///          parser from accepting and then dropping convention annotations when
///          additional conventions are added.
enum class CallingConv {
    Default, ///< Standard platform calling convention.
};

/// @brief Definition of an IL function with parameters and basic blocks.
/// @see docs/il/il-guide.md#reference
struct Function {
    /// @brief Human-readable identifier for the function.
    /// @ownership Stored by the containing Module; immutable after insertion.
    /// @constraint Unique within its Module.
    std::string name;

    /// @brief Return type declared for the function.
    /// @ownership Value copied by Function.
    /// @constraint Must match verifier rules and caller expectations.
    Type retType;

    /// @brief Ordered list of parameters.
    /// @ownership Function owns the container and its Param elements.
    /// @constraint Size and types must match the function type.
    std::vector<Param> params;

    /// @brief True when the function accepts a C-style variadic argument tail.
    bool isVarArg = false;

    /// @brief Calling convention used by calls to this function.
    CallingConv callingConv = CallingConv::Default;

    /// @brief True when the linker must invoke this function before the entry point.
    /// Serialized as the `[module_init]` function attribute.
    bool moduleInitializer = false;

    /// @brief Basic blocks comprising the function body.
    /// @ownership Function owns all blocks; each block's parent is this function.
    /// @constraint Contains at least one block; labels unique within the function.
    StableList<BasicBlock> blocks;

    /// @brief Mapping from SSA value IDs to their original names for diagnostics.
    /// @ownership Function owns this vector.
    /// @constraint Index aligns with SSA value numbering; entries may be empty.
    std::vector<std::string> valueNames;

    /// @brief Cross-module visibility for the IL linker.
    /// @details Defaults to Internal for backwards compatibility. Export makes
    ///          the function callable from other modules; Import declares a
    ///          function defined in another module (no body required).
    /// @see ADR-0003, il/core/Linkage.hpp
    Linkage linkage = Linkage::Internal;

    /// @brief Mutable attribute bundle describing semantic hints for the function.
    FunctionAttrs Attrs{};

    /// @brief Access mutable attribute bundle for the function.
    /// @return Reference to the owned attribute container.
    [[nodiscard]] FunctionAttrs &attrs() {
        return Attrs;
    }

    /// @brief Access read-only function attributes.
    /// @return Const reference to the attribute container.
    [[nodiscard]] const FunctionAttrs &attrs() const {
        return Attrs;
    }

    /// @brief Interned handle for @ref name within the owning Module.
    /// @details Invalid until populated by a Module helper or by construction
    ///          paths that have access to the parent Module.
    il::support::Symbol nameSymbol{};
};

} // namespace il::core
