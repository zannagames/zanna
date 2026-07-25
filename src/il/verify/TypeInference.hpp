//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// This file declares the TypeInference class, which maintains the type environment
// during IL verification. As the verifier processes instructions sequentially, it
// must track which temporary values are defined and their inferred types to validate
// subsequent uses of those temporaries.
//
// IL uses SSA (Static Single Assignment) form where each temporary is defined
// exactly once. The verifier must ensure that every use of a temporary refers to
// a previously defined value with a compatible type. TypeInference provides the
// mechanism for recording definitions and querying types during verification,
// enabling type-safe validation of data flow.
//
// Key Responsibilities:
// - Track which temporaries are currently defined in the type environment
// - Record the inferred type of each temporary as instructions are verified
// - Provide type queries for operands (literals, temporaries, block parameters)
// - Validate that instruction operands refer to defined temporaries
// - Support temporary injection/removal for block parameter handling
//
// Design Rationale:
// The TypeInference class wraps caller-provided storage (maps and sets) rather
// than owning its data structures. This design enables the FunctionVerifier to
// reuse storage across blocks while TypeInference provides a typed interface for
// verification logic. The separation between definition tracking (set of IDs) and
// type tracking (map from IDs to types) supports efficient def/use validation
// independently from type checking.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares verifier-side temporary type and definition tracking.
/// @details `TypeInference` wraps caller-owned maps and sets, resolves literal
///          and temporary value types, records instruction results, and checks
///          ordinary operands plus branch arguments for unknown or
///          not-yet-defined temporaries.

#pragma once

#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "il/core/fwd.hpp"
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace il::support {
template <class T> class Expected;
}

namespace il::verify {

/// @brief Render an instruction to a short single-line snippet for diagnostics.
/// @param instr Instruction whose textual form is requested.
/// @return Human-readable representation used in error messages.
std::string makeSnippet(const il::core::Instr &instr);

/// @brief Helper providing operand type queries and definition tracking for verification.
class TypeInference {
  public:
    /// @brief Construct a type inference helper backed by caller storage.
    /// @param temps Mapping from temporary id to statically inferred type.
    /// @param defined Set of temporaries considered defined at the current program point.
    TypeInference(std::unordered_map<unsigned, il::core::Type> &temps,
                  std::unordered_set<unsigned> &defined);

    /// @brief Return the static type of a value.
    /// @param value Value to inspect.
    /// @param missing Optional flag set when a temporary has no type-map entry.
    /// @return The inferred type or void when unknown.
    il::core::Type valueType(const il::core::Value &value, bool *missing = nullptr) const;

    /// @brief Compute the size in bytes of a primitive type.
    /// @param kind Type enumerator to inspect.
    /// @return Byte width or zero when the size is unknown.
    static size_t typeSize(il::core::Type::Kind kind);

    /// @brief Register the result of an instruction.
    /// @param instr Instruction whose result is being defined.
    /// @param type Static type assigned to the result temporary.
    void recordResult(const il::core::Instr &instr, il::core::Type type);

    /// @brief Verify that all operands of an instruction are defined.
    /// @param fn Enclosing function used for diagnostics.
    /// @param bb Owning basic block used for diagnostics.
    /// @param instr Instruction whose operands are checked.
    /// @param err Stream receiving diagnostics.
    /// @return True if every operand refers to a known and defined temporary.
    bool ensureOperandsDefined(const il::core::Function &fn,
                               const il::core::BasicBlock &bb,
                               const il::core::Instr &instr,
                               std::ostream &err) const;

    /// @brief Verify operand definitions returning a diagnostic payload on failure.
    /// @param fn Enclosing function used for diagnostics.
    /// @param bb Owning basic block used for diagnostics.
    /// @param instr Instruction whose operands are checked.
    /// @return Empty Expected on success; diagnostic payload when verification fails.
    [[nodiscard]] il::support::Expected<void> ensureOperandsDefined_E(
        const il::core::Function &fn,
        const il::core::BasicBlock &bb,
        const il::core::Instr &instr) const;

    /// @brief Mark a temporary as pre-defined with a given type.
    /// @param id Temporary identifier.
    /// @param type Static type to associate with the id.
    void addTemp(unsigned id, il::core::Type type);

    /// @brief Remove a temporary definition inserted via @ref addTemp.
    /// @param id Temporary identifier to erase.
    void removeTemp(unsigned id);

    /// @brief Check whether a temporary is currently defined.
    /// @param id Temporary identifier.
    /// @return True when @p id is considered defined.
    bool isDefined(unsigned id) const;

  private:
    /// @brief Borrowed map from temporary identifiers to known static types.
    std::unordered_map<unsigned, il::core::Type> &temps_;

    /// @brief Borrowed set of temporaries defined at the current program point.
    std::unordered_set<unsigned> &defined_;
};

} // namespace il::verify
