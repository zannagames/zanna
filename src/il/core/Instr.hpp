//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/core/Instr.hpp
// Purpose: Declares the Instr struct -- a single IL instruction within a basic
//          block. Covers arithmetic, memory, control flow, calls, and switch
//          operations with optional SSA result, operands, branch labels, and
//          call attributes.
// Key invariants:
//   - result is disengaged when the instruction produces no value.
//   - op must be a valid Opcode enumerator.
//   - Branch labels must correspond to basic blocks in the same function.
//   - brArgs outer vector size must match labels vector size.
//   - Source location {0,0} denotes unknown.
// Ownership/Lifetime: BasicBlock owns Instructions in stable storage.
//          Instruction owns all operands, labels, and branch arguments. String
//          fields (callee, labels) use std::string value semantics and may be
//          paired with Module-owned interned symbols.
// Links: docs/il/il-guide.md#reference, il/core/Opcode.hpp, il/core/Type.hpp,
//        il/core/Value.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the complete in-memory representation of an IL instruction.
 *
 * @details `Instr` combines an opcode and optional SSA result with operands,
 *          control-flow edges, direct/indirect call metadata, source location,
 *          and module-owned symbol sidecars. Mutator helpers keep related
 *          string, symbol, signature, and branch-argument fields synchronized;
 *          switch accessors provide checked views of the shared storage layout.
 */

#pragma once

#include "il/core/Opcode.hpp"
#include "il/core/Type.hpp"
#include "il/core/Value.hpp"
#include "support/source_location.hpp"
#include "support/symbol.hpp"
#include <optional>
#include <string>
#include <vector>

namespace il::core {

struct Module;

/// @brief Attribute container for call-like instructions capturing semantic hints.
/// @details Stored on every instruction but only meaningful when
///          @ref Instr::op is a direct or indirect call. Future passes may use these
///          attributes to reason about exception safety and memory effects
///          without re-deriving metadata from callee analysis.
struct CallAttrs {
    /// @brief Call cannot throw an exception.
    bool nothrow = false;

    /// @brief Call may read from memory but performs no writes.
    bool readonly = false;

    /// @brief Call performs no memory access and has no observable side effects.
    bool pure = false;
};

/// @brief Instruction within a basic block.
struct Instr {
    /// @brief Destination temporary id.
    /// Owned by the instruction.
    /// Non-negative; disengaged if the instruction has no result.
    std::optional<unsigned> result;

    /// @brief Operation code selecting semantics.
    /// Owned by the instruction.
    /// Must be set to a valid Opcode enumerator before the instruction is used.
    Opcode op{Opcode::Count};

    /// @brief Result type or void.
    /// Owned by the instruction.
    /// Must be void when result is absent.
    Type type;

    /// @brief General operands.
    /// Vector owns each Value element.
    /// Size and content depend on opcode.
    std::vector<Value> operands;

    /// @brief Callee name for call instructions.
    /// Owned by the instruction.
    /// Must be non-empty when op == Opcode::Call.
    std::string callee;

    /// @brief Branch target labels.
    /// Vector owns each label string.
    /// Each must correspond to a basic block in the same function.
    std::vector<std::string> labels;

    /// @brief Branch arguments per target.
    /// Outer vector matches labels in size; inner vectors own their Value elements.
    /// Types must match the parameters of the corresponding block.
    std::vector<std::vector<Value>> brArgs;

    /// @brief Source location.
    /// Owned by the instruction.
    /// Line and column are >=1 when known; {0,0} denotes unknown.
    il::support::SourceLoc loc;

    /// @brief Semantic attributes describing the behaviour of call-like instructions.
    CallAttrs CallAttr{};

    /// @brief True when a pointer-based call.indirect carries an explicit signature.
    bool hasIndirectSignature = false;

    /// @brief Declared return type for an explicitly-typed call.indirect.
    Type indirectRetType{};

    /// @brief Declared parameter types for an explicitly-typed call.indirect.
    std::vector<Type> indirectParamTypes;

    /// @brief True when an explicitly-typed call.indirect accepts a variadic tail.
    bool indirectIsVarArg = false;

    /// @brief Interned handle for @ref callee within the owning Module.
    /// @details Valid for direct call instructions after Module identifier
    ///          interning. Invalid for non-call instructions and for IR built
    ///          outside a Module interning pass.
    il::support::Symbol calleeSymbol{};

    /// @brief Interned handles for @ref labels within the owning Module.
    /// @details When populated, this vector has the same size and order as
    ///          @ref labels so passes can compare branch targets by Symbol
    ///          instead of hashing strings.
    std::vector<il::support::Symbol> labelSymbols;

    /// @brief Check whether the opcode carries call metadata.
    /// @return True for direct and indirect call opcodes.
    [[nodiscard]] bool isCallLike() const noexcept;

    /// @brief Check whether this is a named direct-call instruction.
    /// @return True only for Opcode::Call with a non-empty callee.
    [[nodiscard]] bool isDirectCall() const noexcept;

    /// @brief Check whether the instruction has branch target metadata.
    /// @return True when at least one successor label is stored.
    [[nodiscard]] bool hasBranchTargets() const noexcept;

    /// @brief Check whether an explicit indirect-call signature is present.
    /// @return True when call.indirect signature fields are active.
    [[nodiscard]] bool hasIndirectCallSignature() const noexcept;

    /// @brief Set a direct callee name and clear its stale symbol sidecar.
    /// @param name Direct callee identifier without an `@` prefix.
    /// @post @ref callee equals @p name and @ref calleeSymbol is invalid.
    void setDirectCallee(std::string name);

    /// @brief Set a direct callee name and intern it in @p module.
    /// @param module Module whose interner owns the callee symbol.
    /// @param name Direct callee identifier without an `@` prefix.
    /// @post @ref callee equals @p name and @ref calleeSymbol is valid when
    ///       @p name is non-empty and interning succeeds.
    void setDirectCallee(Module &module, std::string name);

    /// @brief Remove direct-call metadata from the instruction.
    /// @post @ref callee is empty and @ref calleeSymbol is invalid.
    void clearDirectCallee();

    /// @brief Replace all branch targets and branch argument lists.
    /// @param targets Successor labels in terminator order.
    /// @param args Optional per-target block arguments. Empty means no branch
    ///        argument lists are present; otherwise its size must match
    ///        @p targets.
    /// @throws std::invalid_argument if @p args is non-empty and its size does
    ///         not match @p targets.
    /// @post @ref labels and @ref brArgs mirror the supplied values and
    ///       @ref labelSymbols is cleared.
    void setBranchTargets(std::vector<std::string> targets,
                          std::vector<std::vector<Value>> args = {});

    /// @brief Replace all branch targets and intern them in @p module.
    /// @param module Module whose interner owns target label symbols.
    /// @param targets Successor labels in terminator order.
    /// @param args Optional per-target block arguments. Empty means no branch
    ///        argument lists are present; otherwise its size must match
    ///        @p targets.
    /// @throws std::invalid_argument if @p args is non-empty and its size does
    ///         not match @p targets.
    /// @post @ref labelSymbols has the same size and order as @ref labels.
    void setBranchTargets(Module &module,
                          std::vector<std::string> targets,
                          std::vector<std::vector<Value>> args = {});

    /// @brief Append one branch target and optional argument list.
    /// @param target Successor label to append.
    /// @param args Values passed to the target block parameters.
    /// @post @ref labelSymbols is cleared because the new target has not been
    ///       interned.
    void addBranchTarget(std::string target, std::vector<Value> args = {});

    /// @brief Append one branch target and intern it in @p module.
    /// @param module Module whose interner owns the target label symbol.
    /// @param target Successor label to append.
    /// @param args Values passed to the target block parameters.
    /// @post @ref labelSymbols stays aligned with @ref labels.
    void addBranchTarget(Module &module, std::string target, std::vector<Value> args = {});

    /// @brief Remove all branch target metadata.
    /// @post @ref labels, @ref brArgs, and @ref labelSymbols are empty.
    void clearBranchTargets();

    /// @brief Store an explicit signature for call.indirect.
    /// @param retType Declared return type.
    /// @param paramTypes Declared parameter types.
    /// @param isVarArg True when the signature accepts variadic arguments.
    /// @post Signature fields are active and own the supplied values.
    void setIndirectSignature(Type retType, std::vector<Type> paramTypes, bool isVarArg);

    /// @brief Remove explicit call.indirect signature metadata.
    /// @post Indirect signature fields are reset to their default inactive state.
    void clearIndirectSignature();

    /// @brief Clear every call-only metadata field.
    /// @details Removes direct callee data, call attributes, and indirect-call
    ///          signature data. Operands and result/type fields are untouched.
    void clearCallMetadata();
};

/// @brief Access the scrutinee operand of a switch instruction.
/// @param instr Switch instruction with a non-empty operand vector.
/// @return Reference to the first operand.
/// @throws std::logic_error when @p instr is not a well-formed switch.
const Value &switchScrutinee(const Instr &instr);

/// @brief Retrieve the default branch label for a switch instruction.
/// @param instr Switch instruction with a non-empty label vector.
/// @return Reference to the first label.
/// @throws std::logic_error when @p instr is not a well-formed switch.
const std::string &switchDefaultLabel(const Instr &instr);

/// @brief Retrieve the default branch arguments for a switch instruction.
/// @param instr Switch instruction whose branch-argument vector is aligned.
/// @return Reference to the default edge's argument list.
/// @throws std::logic_error or std::out_of_range for malformed switch metadata.
const std::vector<Value> &switchDefaultArgs(const Instr &instr);

/// @brief Count the number of explicit case arms in a switch instruction.
/// @param instr Switch instruction with a default label.
/// @return Number of labels after the default edge.
/// @throws std::logic_error when @p instr is not a well-formed switch.
size_t switchCaseCount(const Instr &instr);

/// @brief Access the value guarding the @p index-th case arm.
/// @param instr Switch instruction containing case operands.
/// @param index Zero-based explicit-case index.
/// @return Reference to the matching case value.
/// @throws std::logic_error or std::out_of_range for malformed metadata/index.
const Value &switchCaseValue(const Instr &instr, size_t index);

/// @brief Access the branch label for the @p index-th case arm.
/// @param instr Switch instruction containing case targets.
/// @param index Zero-based explicit-case index.
/// @return Reference to the matching target label.
/// @throws std::logic_error or std::out_of_range for malformed metadata/index.
const std::string &switchCaseLabel(const Instr &instr, size_t index);

/// @brief Access the branch arguments for the @p index-th case arm.
/// @param instr Switch instruction whose branch-argument vector is aligned.
/// @param index Zero-based explicit-case index.
/// @return Reference to the matching edge argument list.
/// @throws std::logic_error or std::out_of_range for malformed metadata/index.
const std::vector<Value> &switchCaseArgs(const Instr &instr, size_t index);

} // namespace il::core
