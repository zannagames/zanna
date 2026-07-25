//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lower/Emitter.hpp
// Purpose: Declares the IL emission helper composed by the BASIC lowerer.
// Key invariants: Appends instructions to the active basic block when one is set.
// Ownership/Lifetime: References Lowerer state without owning IR structures.
// Links: docs/internals/codemap.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "frontends/basic/ast/NodeFwd.hpp"
#include "frontends/basic/lower/common/CommonLowering.hpp"

#include "zanna/il/Module.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

/// @file
/// @brief Declares the stateful IL emission facade used by the BASIC lowerer.
/// @details Emitter centralizes instruction construction, runtime ownership
///          cleanup, deferred-temporary release, exception-handler state, and
///          return/trap terminators while borrowing all IR storage from Lowerer.

namespace il::core {
struct BasicBlock;
struct Function;
} // namespace il::core

namespace il::frontends::basic {
class Lowerer;
struct SymbolInfo;
} // namespace il::frontends::basic

namespace il::frontends::basic::lower {

/// @brief Centralizes IL emission primitives for BASIC lowering.
/// @invariant Each helper assumes the caller selected an active basic block.
/// @ownership Borrows Lowerer state; does not own emitted IR or runtime data.
class Emitter {
  public:
    using Type = il::core::Type;
    using Value = il::core::Value;
    using BasicBlock = il::core::BasicBlock;
    using Function = il::core::Function;
    using Opcode = il::core::Opcode;
    using AstType = il::frontends::basic::Type;

    /// @brief Construct an Emitter bound to the given Lowerer.
    /// @param lowerer The owning Lowerer whose IR builder and context are used
    ///                for all emission operations.
    explicit Emitter(Lowerer &lowerer) noexcept;

    /// @brief Get the IL boolean type (i1).
    /// @return The i1 type used for boolean values in the IL.
    Type ilBoolTy() const;

    /// @brief Emit a boolean constant (true or false) as an i1 value.
    /// @param v The boolean value to emit.
    /// @return An i1 IL constant representing @p v.
    Value emitBoolConst(bool v);

    /// @brief Emit a diamond-shaped control flow that produces a boolean result.
    /// @details Creates then/else/join blocks. The @p emitThen and @p emitElse
    ///          callbacks emit code into their respective branches and pass
    ///          their boolean result to the provided Value callback. The join
    ///          block receives a phi of both results.
    /// @param emitThen Callback to emit the "then" branch code.
    /// @param emitElse Callback to emit the "else" branch code.
    /// @param thenLabelBase Base name for the then basic block label.
    /// @param elseLabelBase Base name for the else basic block label.
    /// @param joinLabelBase Base name for the join basic block label.
    /// @return The phi result (i1) in the join block.
    Value emitBoolFromBranches(const std::function<void(Value)> &emitThen,
                               const std::function<void(Value)> &emitElse,
                               std::string_view thenLabelBase,
                               std::string_view elseLabelBase,
                               std::string_view joinLabelBase);

    /// @brief Emit a stack allocation of the given size.
    /// @param bytes Number of bytes to allocate on the stack frame.
    /// @return A pointer value to the allocated stack memory.
    Value emitAlloca(int bytes);

    /// @brief Emit a load instruction reading a value from a memory address.
    /// @param ty The IL type of the value to load.
    /// @param addr Pointer value to load from.
    /// @return The loaded value of type @p ty.
    Value emitLoad(Type ty, Value addr);

    /// @brief Emit a store instruction writing a value to a memory address.
    /// @param ty The IL type of the value being stored.
    /// @param addr Pointer value to store to.
    /// @param val The value to write.
    void emitStore(Type ty, Value addr, Value val);

    /// @brief Emit a binary arithmetic or comparison instruction.
    /// @param op The IL opcode for the binary operation (e.g., iadd, isub, icmp_eq).
    /// @param ty The result type of the operation.
    /// @param lhs Left-hand operand.
    /// @param rhs Right-hand operand.
    /// @return The result value of the binary operation.
    Value emitBinary(Opcode op, Type ty, Value lhs, Value rhs);

    /// @brief Emit a unary instruction (e.g., negation, bitwise not).
    /// @param op The IL opcode for the unary operation.
    /// @param ty The result type of the operation.
    /// @param val The operand value.
    /// @return The result value of the unary operation.
    Value emitUnary(Opcode op, Type ty, Value val);

    /// @brief Emit a 64-bit integer constant.
    /// @param v The integer value to materialize.
    /// @return An i64 IL constant representing @p v.
    Value emitConstI64(std::int64_t v);

    /// @brief Emit a zero-extension from i1 to i64.
    /// @details Used to convert boolean results to BASIC's integer representation
    ///          where -1 = true and 0 = false (after subsequent negation).
    /// @param val An i1 boolean value to extend.
    /// @return An i64 value (0 or 1).
    Value emitZext1ToI64(Value val);

    /// @brief Emit an integer subtraction (lhs - rhs).
    /// @param lhs Left-hand i64 operand.
    /// @param rhs Right-hand i64 operand.
    /// @return The i64 difference.
    Value emitISub(Value lhs, Value rhs);

    /// @brief Convert a BASIC logical i64 value to an IL i1 boolean.
    /// @details In BASIC, any non-zero integer is truthy. This emits an
    ///          `icmp_ne(b1, 0)` to produce a proper i1 boolean.
    /// @param b1 An i64 value representing a BASIC boolean (-1 or 0).
    /// @return An i1 boolean (true if b1 != 0).
    Value emitBasicLogicalI64(Value b1);

    /// @brief Emit a checked negation that traps on overflow (e.g., MIN_INT).
    /// @param ty The integer type of the value.
    /// @param val The value to negate.
    /// @return The negated value.
    Value emitCheckedNeg(Type ty, Value val);

    /// @brief Emit an unconditional branch to the target basic block.
    /// @param target The basic block to jump to.
    void emitBr(BasicBlock *target);

    /// @brief Emit a conditional branch based on an i1 condition.
    /// @param cond The i1 boolean condition value.
    /// @param t The basic block to jump to if @p cond is true.
    /// @param f The basic block to jump to if @p cond is false.
    void emitCBr(Value cond, BasicBlock *t, BasicBlock *f);

    /// @brief Emit a direct function call that returns a value.
    /// @param ty The expected return type.
    /// @param callee The name of the function to call.
    /// @param args The argument values to pass.
    /// @return The return value of the call.
    Value emitCallRet(Type ty, const std::string &callee, const std::vector<Value> &args);

    /// @brief Emit a direct function call with no return value (void).
    /// @param callee The name of the function to call.
    /// @param args The argument values to pass.
    void emitCall(const std::string &callee, const std::vector<Value> &args);

    /// @brief Emit an indirect call through a function pointer, with a return value.
    /// @param ty The expected return type.
    /// @param callee The function pointer value to call through.
    /// @param args The argument values to pass.
    /// @return The return value of the indirect call.
    Value emitCallIndirectRet(Type ty, Value callee, const std::vector<Value> &args);

    /// @brief Emit an indirect call through a function pointer, with no return value.
    /// @param callee The function pointer value to call through.
    /// @param args The argument values to pass.
    void emitCallIndirect(Value callee, const std::vector<Value> &args);

    /// @brief Emit a reference to a string literal global constant.
    /// @param globalName The IL global name for the string constant.
    /// @return A pointer value referencing the string data.
    Value emitConstStr(const std::string &globalName);

    /// @brief Store a value into an array slot, requesting the appropriate
    ///        runtime helper for the element type.
    /// @param slot The array handle or pointer to the target slot.
    /// @param value The value to store into the array.
    /// @param elementType The BASIC type of the array element (default: I64).
    /// @param isObjectArray True if the array holds object references.
    void storeArray(Value slot,
                    Value value,
                    AstType elementType = AstType::I64,
                    bool isObjectArray = false);

    /// @brief Emit release calls for all local array variables at function exit.
    /// @param paramNames Set of parameter names to exclude (they are not locals).
    void releaseArrayLocals(const std::unordered_set<std::string> &paramNames);

    /// @brief Emit release calls for array parameters at function exit.
    /// @param paramNames Set of parameter names to include for release.
    void releaseArrayParams(const std::unordered_set<std::string> &paramNames);

    /// @brief Emit destructor calls for all local object variables at function exit.
    /// @param paramNames Set of parameter names to exclude (they are not locals).
    void releaseObjectLocals(const std::unordered_set<std::string> &paramNames);

    /// @brief Emit destructor calls for object parameters at function exit.
    /// @param paramNames Set of parameter names to include for release.
    void releaseObjectParams(const std::unordered_set<std::string> &paramNames);

    /// @name Temporary Lifetime Management
    /// @brief Track and release temporary values whose lifetime ends at statement boundary.
    /// @{

    /// @brief Defer releasing a temporary string value until releaseDeferredTemps().
    /// @param v The string pointer value to release later.
    void deferReleaseStr(Value v);

    /// @brief Defer releasing a temporary object value until releaseDeferredTemps().
    /// @param v The object pointer value to release later.
    /// @param className Optional class name for destructor dispatch.
    void deferReleaseObj(Value v, const std::string &className = {});

    /// @brief Emit release calls for all deferred temporaries, then clear the list.
    void releaseDeferredTemps();

    /// @brief Discard all deferred temporaries without emitting release calls.
    void clearDeferredTemps();
    /// @}

    /// @brief Emit an unconditional trap (program abort).
    void emitTrap();

    /// @brief Emit a trap that propagates an error code from a runtime call.
    /// @param errCode The i64 error code value to propagate.
    void emitTrapFromErr(Value errCode);

    /// @brief Push an exception handler block onto the EH stack.
    /// @param handler The basic block to branch to on error.
    void emitEhPush(BasicBlock *handler);

    /// @brief Pop the topmost exception handler from the EH stack.
    void emitEhPop();

    /// @brief Pop the EH stack as part of a function return (cleans up all frames).
    void emitEhPopForReturn();

    /// @brief Clear the currently active error handler tracking state.
    void clearActiveErrorHandler();

    /// @brief Get or create the error handler basic block for a given BASIC line.
    /// @param targetLine The BASIC source line number for the ON ERROR GOTO target.
    /// @return Pointer to the error handler basic block.
    BasicBlock *ensureErrorHandlerBlock(int targetLine);

    /// @brief Emit a return instruction with a value.
    /// @param v The value to return from the current function.
    void emitRet(Value v);

    /// @brief Emit a void return instruction (no value).
    void emitRetVoid();

    /// @brief True when @p v is already scheduled for a deferred release.
    /// @details Lets a consumer (e.g. PRINT) tell an OWNED temporary — such as
    ///          a runtime property getter's string result — apart from a
    ///          BORROWED lvalue load, so it does not emit a second release that
    ///          would double-free (VDOC-180).
    [[nodiscard]] bool isDeferredReleaseTemp(Value v) const noexcept {
        if (v.kind != Value::Kind::Temp)
            return false;
        for (const auto &t : deferredTemps_)
            if (t.v.kind == Value::Kind::Temp && t.v.id == v.id)
                return true;
        return false;
    }

  private:
    /// Lowerer whose procedure, symbol, runtime, and builder state is mutated.
    Lowerer &lowerer_;
    /// Shared primitive-emission helper bound to @ref lowerer_.
    common::CommonLowering common_;

    /// @brief Release a single object slot, emitting destructor call if needed.
    /// @param info Mutable symbol metadata describing the object slot.
    void releaseObjectSlot(SymbolInfo &info);

    /// @brief State tracking for array release runtime helper requests.
    struct ArrayReleaseState {
        bool requestedI64{false};
        bool requestedF64{false};
        bool requestedStr{false};
        bool requestedObj{false};
    };

    /// @brief Emit the appropriate array release call based on element type.
    /// @param handle Array handle value.
    /// @param info Symbol info describing the array.
    /// @param state Tracks which runtime helpers have been requested.
    /// @param skipObjectArrays If true, return false for object arrays without emitting.
    /// @return True if release was emitted, false if skipped.
    bool emitArrayRelease(Value handle,
                          const SymbolInfo &info,
                          ArrayReleaseState &state,
                          bool skipObjectArrays);

    /// @brief One deferred ownership release scheduled at statement boundary.
    struct TempRelease {
        /// Runtime handle to release.
        Value v;
        /// True for string handles; false for object handles.
        bool isString{false};
        std::string className; // optional, for object destructors
    };

    /// Deferred releases retained in scheduling order.
    std::vector<TempRelease> deferredTemps_;
};

} // namespace il::frontends::basic::lower
