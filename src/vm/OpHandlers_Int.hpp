//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/OpHandlers_Int.hpp
// Purpose: Declare integer arithmetic, bitwise, and comparison opcode handlers.
// Key invariants: Handlers implement two's complement semantics and enforce IL traps.
// Ownership/Lifetime: Handlers operate on VM frames without retaining external resources.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Integer opcode handlers for the VM dispatcher.
/// @details Declares handlers for arithmetic, bitwise, comparisons, shifts, and
///          integer casts. Inline helpers implement fast-path versions of basic
///          arithmetic with defined overflow behavior per IL semantics.

#pragma once

#include "zanna/vm/internal/OpHelpers.hpp"
#include "vm/OpHandlerAccess.hpp"
#include "vm/OpHandlerUtils.hpp"
#include "vm/VM.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Instr.hpp"

namespace il::vm::detail::integer {
/// @brief Execution state alias used by integer handlers.
using ExecState = VMAccess::ExecState;

/// @brief Inline implementation for Add with wraparound semantics.
/// @details Evaluates two integer operands and writes the wrapped sum into the
///          destination slot. This mirrors IL's defined two's complement wrap.
/// @param vm Active VM instance.
/// @param state Optional execution state (unused).
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
inline VM::ExecResult handleAddImpl(VM &vm,
                                    ExecState *state,
                                    Frame &fr,
                                    const il::core::Instr &in,
                                    const VM::BlockMap &blocks,
                                    const il::core::BasicBlock *&bb,
                                    size_t &ip) {
    (void)state;
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Add two integer operands with IL wraparound semantics.
    /// @param lhs Left operand.
    /// @param rhs Right operand.
    /// @return Wrapped sum.
    return il::vm::internal::binaryOp<int64_t>(
        vm, fr, in, [](int64_t lhs, int64_t rhs) { return ops::wrap_add(lhs, rhs); });
}

/// @brief Inline implementation for Sub with wraparound semantics.
/// @details Evaluates two integer operands and writes the wrapped difference
///          into the destination slot per IL semantics.
/// @param vm Active VM instance.
/// @param state Optional execution state (unused).
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
inline VM::ExecResult handleSubImpl(VM &vm,
                                    ExecState *state,
                                    Frame &fr,
                                    const il::core::Instr &in,
                                    const VM::BlockMap &blocks,
                                    const il::core::BasicBlock *&bb,
                                    size_t &ip) {
    (void)state;
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Subtract two integer slot values with IL wraparound semantics.
    /// @param[out] out Destination slot.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    return ops::applyBinary(vm, fr, in, [](Slot &out, const Slot &lhsVal, const Slot &rhsVal) {
        // Plain integer sub wraps on overflow per IL semantics.
        out.i64 = ops::wrap_sub(lhsVal.i64, rhsVal.i64);
    });
}

/// @brief Inline implementation for Mul with wraparound semantics.
/// @details Evaluates two integer operands and writes the wrapped product into
///          the destination slot per IL semantics.
/// @param vm Active VM instance.
/// @param state Optional execution state (unused).
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
inline VM::ExecResult handleMulImpl(VM &vm,
                                    ExecState *state,
                                    Frame &fr,
                                    const il::core::Instr &in,
                                    const VM::BlockMap &blocks,
                                    const il::core::BasicBlock *&bb,
                                    size_t &ip) {
    (void)state;
    (void)blocks;
    (void)bb;
    (void)ip;
    /// @brief Multiply two integer slot values with IL wraparound semantics.
    /// @param[out] out Destination slot.
    /// @param lhsVal Left operand slot.
    /// @param rhsVal Right operand slot.
    return ops::applyBinary(vm, fr, in, [](Slot &out, const Slot &lhsVal, const Slot &rhsVal) {
        // Plain integer mul wraps on overflow per IL semantics.
        out.i64 = ops::wrap_mul(lhsVal.i64, rhsVal.i64);
    });
}

/// @brief Execute integer addition (add) with wraparound semantics.
/// @details Evaluates two integer operands, computes the wrapped sum, and
///          stores the result in the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleAdd(VM &vm,
                         Frame &fr,
                         const il::core::Instr &in,
                         const VM::BlockMap &blocks,
                         const il::core::BasicBlock *&bb,
                         size_t &ip);

/// @brief Execute integer subtraction (sub) with wraparound semantics.
/// @details Evaluates two integer operands, computes the wrapped difference,
///          and stores the result in the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSub(VM &vm,
                         Frame &fr,
                         const il::core::Instr &in,
                         const VM::BlockMap &blocks,
                         const il::core::BasicBlock *&bb,
                         size_t &ip);

/// @brief Execute integer multiplication (mul) with wraparound semantics.
/// @details Evaluates two integer operands, computes the wrapped product, and
///          stores the result in the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleMul(VM &vm,
                         Frame &fr,
                         const il::core::Instr &in,
                         const VM::BlockMap &blocks,
                         const il::core::BasicBlock *&bb,
                         size_t &ip);

/// @brief Execute integer subtraction with IL semantics (isub).
/// @details Evaluates operands and computes the result using IL-defined
///          subtraction semantics, which may differ from plain wrap in some
///          opcode variants.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleISub(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip);

/// @brief Execute checked integer addition with overflow trap (iadd.ovf).
/// @details Computes the sum and traps if the result overflows the signed
///          integer range as defined by the IL specification.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleIAddOvf(VM &vm,
                             Frame &fr,
                             const il::core::Instr &in,
                             const VM::BlockMap &blocks,
                             const il::core::BasicBlock *&bb,
                             size_t &ip);

/// @brief Execute checked integer subtraction with overflow trap (isub.ovf).
/// @details Computes the difference and traps if signed overflow occurs per
///          IL overflow-checking semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleISubOvf(VM &vm,
                             Frame &fr,
                             const il::core::Instr &in,
                             const VM::BlockMap &blocks,
                             const il::core::BasicBlock *&bb,
                             size_t &ip);

/// @brief Execute checked integer multiplication with overflow trap (imul.ovf).
/// @details Computes the product and traps if signed overflow occurs per IL
///          overflow-checking semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleIMulOvf(VM &vm,
                             Frame &fr,
                             const il::core::Instr &in,
                             const VM::BlockMap &blocks,
                             const il::core::BasicBlock *&bb,
                             size_t &ip);

/// @brief Execute signed division (sdiv).
/// @details Performs signed division according to IL semantics. Division by
///          zero behavior is defined by the opcode variant; checked versions
///          use explicit trap handlers.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSDiv(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip);

/// @brief Execute unsigned division (udiv).
/// @details Performs unsigned division according to IL semantics. Division by
///          zero behavior is defined by the opcode variant; checked versions
///          use explicit trap handlers.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleUDiv(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip);

/// @brief Execute signed remainder (srem).
/// @details Computes the signed remainder of lhs / rhs per IL semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSRem(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip);

/// @brief Execute unsigned remainder (urem).
/// @details Computes the unsigned remainder of lhs / rhs per IL semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleURem(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip);

/// @brief Execute signed division with divide-by-zero trap (sdiv.chk0).
/// @details Checks for a zero divisor and traps on zero; otherwise performs
///          signed division per IL semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSDivChk0(VM &vm,
                              Frame &fr,
                              const il::core::Instr &in,
                              const VM::BlockMap &blocks,
                              const il::core::BasicBlock *&bb,
                              size_t &ip);

/// @brief Execute unsigned division with divide-by-zero trap (udiv.chk0).
/// @details Checks for a zero divisor and traps on zero; otherwise performs
///          unsigned division per IL semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleUDivChk0(VM &vm,
                              Frame &fr,
                              const il::core::Instr &in,
                              const VM::BlockMap &blocks,
                              const il::core::BasicBlock *&bb,
                              size_t &ip);

/// @brief Execute signed remainder with divide-by-zero trap (srem.chk0).
/// @details Checks for a zero divisor and traps on zero; otherwise computes the
///          signed remainder per IL semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSRemChk0(VM &vm,
                              Frame &fr,
                              const il::core::Instr &in,
                              const VM::BlockMap &blocks,
                              const il::core::BasicBlock *&bb,
                              size_t &ip);

/// @brief Execute unsigned remainder with divide-by-zero trap (urem.chk0).
/// @details Checks for a zero divisor and traps on zero; otherwise computes the
///          unsigned remainder per IL semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleURemChk0(VM &vm,
                              Frame &fr,
                              const il::core::Instr &in,
                              const VM::BlockMap &blocks,
                              const il::core::BasicBlock *&bb,
                              size_t &ip);

/// @brief Execute bounds check for index operations (idxchk).
/// @details Validates an index against a length and traps if out of range,
///          following IL bounds-checking semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleIdxChk(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute checked signed narrowing conversion (casts.narrow.chk).
/// @details Converts a signed integer to a narrower width and traps if the
///          value does not fit in the target range.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleCastSiNarrowChk(VM &vm,
                                     Frame &fr,
                                     const il::core::Instr &in,
                                     const VM::BlockMap &blocks,
                                     const il::core::BasicBlock *&bb,
                                     size_t &ip);

/// @brief Execute checked unsigned narrowing conversion (castu.narrow.chk).
/// @details Converts an unsigned integer to a narrower width and traps if the
///          value does not fit in the target range.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleCastUiNarrowChk(VM &vm,
                                     Frame &fr,
                                     const il::core::Instr &in,
                                     const VM::BlockMap &blocks,
                                     const il::core::BasicBlock *&bb,
                                     size_t &ip);

/// @brief Execute signed integer to floating-point conversion.
/// @details Converts a signed integer operand to double precision, following
///          IL conversion semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleCastSiToFp(VM &vm,
                                Frame &fr,
                                const il::core::Instr &in,
                                const VM::BlockMap &blocks,
                                const il::core::BasicBlock *&bb,
                                size_t &ip);

/// @brief Execute unsigned integer to floating-point conversion.
/// @details Converts an unsigned integer operand to double precision, following
///          IL conversion semantics.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleCastUiToFp(VM &vm,
                                Frame &fr,
                                const il::core::Instr &in,
                                const VM::BlockMap &blocks,
                                const il::core::BasicBlock *&bb,
                                size_t &ip);

/// @brief Execute truncation or zero-extension to 1-bit (boolean) values.
/// @details Produces a canonical 0/1 result based on the operand, matching IL
///          semantics for boolean conversions.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleTruncOrZext1(VM &vm,
                                  Frame &fr,
                                  const il::core::Instr &in,
                                  const VM::BlockMap &blocks,
                                  const il::core::BasicBlock *&bb,
                                  size_t &ip);

/// @brief Execute bitwise AND.
/// @details Computes lhs & rhs and writes the result to the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleAnd(VM &vm,
                         Frame &fr,
                         const il::core::Instr &in,
                         const VM::BlockMap &blocks,
                         const il::core::BasicBlock *&bb,
                         size_t &ip);

/// @brief Execute bitwise OR.
/// @details Computes lhs | rhs and writes the result to the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleOr(VM &vm,
                        Frame &fr,
                        const il::core::Instr &in,
                        const VM::BlockMap &blocks,
                        const il::core::BasicBlock *&bb,
                        size_t &ip);

/// @brief Execute bitwise XOR.
/// @details Computes lhs ^ rhs and writes the result to the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleXor(VM &vm,
                         Frame &fr,
                         const il::core::Instr &in,
                         const VM::BlockMap &blocks,
                         const il::core::BasicBlock *&bb,
                         size_t &ip);

/// @brief Execute logical left shift.
/// @details Shifts the left operand by the specified amount, masking the shift
///          count per IL rules, and writes the result to the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleShl(VM &vm,
                         Frame &fr,
                         const il::core::Instr &in,
                         const VM::BlockMap &blocks,
                         const il::core::BasicBlock *&bb,
                         size_t &ip);

/// @brief Execute logical right shift (zero-fill).
/// @details Shifts the left operand right with zero fill, masking the shift
///          count per IL rules, and writes the result to the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleLShr(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip);

/// @brief Execute arithmetic right shift (sign-extended).
/// @details Shifts the left operand right with sign extension, masking the
///          shift count per IL rules, and writes the result to the destination.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleAShr(VM &vm,
                          Frame &fr,
                          const il::core::Instr &in,
                          const VM::BlockMap &blocks,
                          const il::core::BasicBlock *&bb,
                          size_t &ip);

/// @brief Execute integer equality comparison (icmp.eq).
/// @details Compares two integer operands for equality and writes a boolean
///          result to the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleICmpEq(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute integer inequality comparison (icmp.ne).
/// @details Compares two integer operands for inequality and writes a boolean
///          result to the destination slot.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleICmpNe(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute signed greater-than comparison (scmp.gt).
/// @details Compares two signed integers and writes a boolean result.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSCmpGT(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute signed less-than comparison (scmp.lt).
/// @details Compares two signed integers and writes a boolean result.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSCmpLT(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute signed less-or-equal comparison (scmp.le).
/// @details Compares two signed integers and writes a boolean result.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSCmpLE(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute signed greater-or-equal comparison (scmp.ge).
/// @details Compares two signed integers and writes a boolean result.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSCmpGE(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute unsigned less-than comparison (ucmp.lt).
/// @details Compares two unsigned integers and writes a boolean result.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleUCmpLT(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute unsigned less-or-equal comparison (ucmp.le).
/// @details Compares two unsigned integers and writes a boolean result.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleUCmpLE(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute unsigned greater-than comparison (ucmp.gt).
/// @details Compares two unsigned integers and writes a boolean result.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleUCmpGT(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute unsigned greater-or-equal comparison (ucmp.ge).
/// @details Compares two unsigned integers and writes a boolean result.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleUCmpGE(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

/// @brief Execute the @c select opcode choosing between two same-typed values.
/// @param vm Active VM instance.
/// @param fr Current execution frame.
/// @param in Instruction being executed.
/// @param blocks Block map for the current function.
/// @param bb In/out current basic block pointer.
/// @param ip In/out instruction pointer within @p bb.
/// @return Execution result indicating continue or trap status.
VM::ExecResult handleSelect(VM &vm,
                            Frame &fr,
                            const il::core::Instr &in,
                            const VM::BlockMap &blocks,
                            const il::core::BasicBlock *&bb,
                            size_t &ip);

} // namespace il::vm::detail::integer
