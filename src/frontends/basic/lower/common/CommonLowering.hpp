//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/common/CommonLowering.hpp
// Purpose: Declare shared IL emission helpers used by BASIC lowering utilities.
// Key invariants: Helpers respect the Lowerer procedure context and only append
//                 instructions to the active block when one is selected.
// Ownership/Lifetime: Borrows Lowerer state; does not own IR objects or AST.
// Links: docs/internals/codemap.md, docs/internals/architecture.md#cpp-overview
//
//===----------------------------------------------------------------------===//

#pragma once

#include "zanna/il/Module.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// @brief Declares primitive IL instruction and control-flow emission shared by
///        BASIC lowering components.

namespace il::core {
struct BasicBlock;
struct Function;
} // namespace il::core

namespace il::frontends::basic {
class Lowerer;
}

namespace il::frontends::basic::lower::common {

/// @brief Provides reusable IL emission helpers shared across BASIC lowering components.
/// @invariant Each helper assumes the caller established an active basic block in the
///            @ref Lowerer procedure context before invocation.
/// @ownership Does not own IR objects; borrows Lowerer state that outlives the helper.
class CommonLowering {
  public:
    using Type = il::core::Type;
    using Value = il::core::Value;
    using BasicBlock = il::core::BasicBlock;
    using Function = il::core::Function;
    using Opcode = il::core::Opcode;

    /// @brief Construct a CommonLowering helper bound to the given Lowerer.
    /// @param lowerer The Lowerer instance whose IL builder and module are used.
    explicit CommonLowering(Lowerer &lowerer) noexcept;

    /// @brief Return the IL type used for BASIC boolean values (i1).
    /// @return The IlType representing a 1-bit boolean.
    [[nodiscard]] Type ilBoolTy() const;

    /// @brief Emit a boolean constant as an IL i1 value.
    /// @param v The boolean constant (true or false).
    /// @return The emitted IL constant value.
    [[nodiscard]] Value emitBoolConst(bool v);

    /// @brief Emit a boolean value computed from two branch bodies.
    /// @details Creates a diamond control-flow pattern where emitThen stores the
    ///          true-path result and emitElse stores the false-path result into a
    ///          shared stack slot, then loads the result at the join point.
    /// @param emitThen Lambda that stores the "true" result into the provided slot.
    /// @param emitElse Lambda that stores the "false" result into the provided slot.
    /// @param thenLabelBase Label prefix for the then-block.
    /// @param elseLabelBase Label prefix for the else-block.
    /// @param joinLabelBase Label prefix for the join-block.
    /// @return The loaded boolean value at the join point.
    [[nodiscard]] Value emitBoolFromBranches(const std::function<void(Value)> &emitThen,
                                             const std::function<void(Value)> &emitElse,
                                             std::string_view thenLabelBase,
                                             std::string_view elseLabelBase,
                                             std::string_view joinLabelBase);

    /// @brief Emit a stack allocation of the given size in bytes.
    /// @param bytes Number of bytes to allocate on the stack frame.
    /// @return A pointer value referencing the allocated stack slot.
    [[nodiscard]] Value emitAlloca(int bytes);

    /// @brief Emit a load instruction from a memory address.
    /// @param ty The IL type of the value being loaded.
    /// @param addr The pointer value to load from.
    /// @return The loaded value.
    [[nodiscard]] Value emitLoad(Type ty, Value addr);

    /// @brief Emit a store instruction writing a value to a memory address.
    /// @param ty The IL type of the value being stored.
    /// @param addr The pointer value to store into.
    /// @param val The value to write.
    void emitStore(Type ty, Value addr, Value val);

    /// @brief Emit a binary arithmetic or comparison instruction.
    /// @param op The IL opcode for the binary operation.
    /// @param ty The IL type of the operands and result.
    /// @param lhs The left-hand operand.
    /// @param rhs The right-hand operand.
    /// @return The result value of the binary operation.
    [[nodiscard]] Value emitBinary(Opcode op, Type ty, Value lhs, Value rhs);

    /// @brief Emit a unary instruction (negation, bitwise NOT, etc.).
    /// @param op The IL opcode for the unary operation.
    /// @param ty The IL type of the operand and result.
    /// @param val The operand value.
    /// @return The result value of the unary operation.
    [[nodiscard]] Value emitUnary(Opcode op, Type ty, Value val);

    /// @brief Emit a 64-bit integer constant.
    /// @param v The integer constant value.
    /// @return The emitted IL constant value.
    [[nodiscard]] Value emitConstI64(std::int64_t v) const;

    /// @brief Emit a zero-extension from i1 to i64.
    /// @param val The i1 value to extend.
    /// @return The zero-extended i64 value.
    [[nodiscard]] Value emitZext1ToI64(Value val);

    /// @brief Emit an integer subtraction (lhs - rhs).
    /// @param lhs The left-hand operand.
    /// @param rhs The right-hand operand.
    /// @return The difference as an i64 value.
    [[nodiscard]] Value emitISub(Value lhs, Value rhs);

    /// @brief Convert a BASIC logical value to canonical i64 form.
    /// @details BASIC uses -1 for TRUE and 0 for FALSE in logical operations.
    /// @param b1 The input i64 value to normalize.
    /// @return The canonical BASIC logical value (-1 or 0).
    [[nodiscard]] Value emitBasicLogicalI64(Value b1);

    /// @brief Emit a checked negation with overflow detection.
    /// @details Negates the value and traps on overflow (e.g. -INT_MIN).
    /// @param ty The IL type of the value.
    /// @param val The value to negate.
    /// @return The negated value (if no overflow).
    [[nodiscard]] Value emitCheckedNeg(Type ty, Value val);

    /// @brief Emit an unconditional branch to a target basic block.
    /// @param target The basic block to branch to.
    void emitBr(BasicBlock *target);

    /// @brief Emit a conditional branch based on a boolean condition.
    /// @param cond The i1 condition value.
    /// @param t The basic block to branch to if cond is true.
    /// @param f The basic block to branch to if cond is false.
    void emitCBr(Value cond, BasicBlock *t, BasicBlock *f);

    /// @brief Emit a direct function call that returns a value.
    /// @param ty The IL return type of the called function.
    /// @param callee The name of the function to call.
    /// @param args The argument values to pass.
    /// @return The return value of the call.
    [[nodiscard]] Value emitCallRet(Type ty,
                                    const std::string &callee,
                                    const std::vector<Value> &args);

    /// @brief Emit a direct function call that returns void.
    /// @param callee The name of the function to call.
    /// @param args The argument values to pass.
    void emitCall(const std::string &callee, const std::vector<Value> &args);

    /// @brief Emit an indirect call that returns a value.
    /// @details Appends a `CallIndirect` instruction with the callee operand followed by args.
    /// @param ty The IL return type.
    /// @param callee The function pointer value to call through.
    /// @param args The argument values to pass.
    /// @return The return value of the indirect call.
    [[nodiscard]] Value emitCallIndirectRet(Type ty, Value callee, const std::vector<Value> &args);

    /// @brief Emit an indirect call that does not return a value.
    /// @details Appends a void-typed `CallIndirect` instruction with operands.
    /// @param callee The function pointer value to call through.
    /// @param args The argument values to pass.
    void emitCallIndirect(Value callee, const std::vector<Value> &args);

    /// @brief Emit a reference to a string constant in the global data section.
    /// @param globalName The name of the global string constant.
    /// @return A pointer value referencing the string data.
    [[nodiscard]] Value emitConstStr(const std::string &globalName);

    /// @brief Generate a unique block label with the given base prefix.
    /// @param base The prefix for the block label.
    /// @return A unique block label string incorporating the prefix and a counter.
    [[nodiscard]] std::string makeBlockLabel(std::string_view base) const;

  private:
    /// Borrowed lowerer supplying the active block, builder, location, and IDs.
    Lowerer *lowerer_;
};

} // namespace il::frontends::basic::lower::common
