//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
// File: src/vm/int_ops_cmp.cpp
//
// Summary:
//   Defines the integer comparison opcode handlers for the Zanna virtual
//   machine.  Each handler delegates to the shared comparison helper while
//   supplying the predicate that implements the desired IL semantics.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Integer comparison opcode handlers for the VM.
/// @details Provides thin wrappers around @ref ops::applyCompare that evaluate
///          signed and unsigned predicates over @ref Slot values.  The handlers
///          normalise results to the canonical @c i1 representation expected by
///          the interpreter.

#include "vm/OpHandlers_Int.hpp"

namespace il::vm::detail::integer {

/// @brief Define an integer comparison handler backed by @ref ops::applyCompare.
/// @details Each generated handler evaluates its two operands through the VM,
///          applies @p Pred, stores the normalized boolean result, and leaves
///          block and instruction-pointer arguments unchanged.
/// @param Name Function name emitted for the opcode handler.
/// @param Pred Callable accepting two constant Slot references and returning
///             the comparison result.
#define DEFINE_CMP_HANDLER(Name, Pred)                                                             \
    VM::ExecResult Name(VM &vm,                                                                    \
                        Frame &fr,                                                                 \
                        const il::core::Instr &in,                                                 \
                        const VM::BlockMap &,                                                      \
                        const il::core::BasicBlock *&,                                             \
                        size_t &) {                                                                \
        return ops::applyCompare(vm, fr, in, Pred);                                                \
    }

// Signed / equality comparisons (operate on i64 directly)
/// @brief Compare two integer slot representations for equality.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return `true` when the integer representations are equal.
DEFINE_CMP_HANDLER(handleICmpEq, [](const Slot &a, const Slot &b) { return a.i64 == b.i64; })
/// @brief Compare two integer slot representations for inequality.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return `true` when the integer representations differ.
DEFINE_CMP_HANDLER(handleICmpNe, [](const Slot &a, const Slot &b) { return a.i64 != b.i64; })
/// @brief Test whether the signed left operand is greater than the right operand.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return Signed greater-than result.
DEFINE_CMP_HANDLER(handleSCmpGT, [](const Slot &a, const Slot &b) { return a.i64 > b.i64; })
/// @brief Test whether the signed left operand is less than the right operand.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return Signed less-than result.
DEFINE_CMP_HANDLER(handleSCmpLT, [](const Slot &a, const Slot &b) { return a.i64 < b.i64; })
/// @brief Test whether the signed left operand is less than or equal to the right operand.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return Signed less-than-or-equal result.
DEFINE_CMP_HANDLER(handleSCmpLE, [](const Slot &a, const Slot &b) { return a.i64 <= b.i64; })
/// @brief Test whether the signed left operand is greater than or equal to the right operand.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return Signed greater-than-or-equal result.
DEFINE_CMP_HANDLER(handleSCmpGE, [](const Slot &a, const Slot &b) { return a.i64 >= b.i64; })

// Unsigned comparisons (cast to uint64_t before comparing)
/// @brief Test whether the unsigned left operand is less than the right operand.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return Unsigned less-than result.
DEFINE_CMP_HANDLER(handleUCmpLT, [](const Slot &a, const Slot &b) {
    return static_cast<uint64_t>(a.i64) < static_cast<uint64_t>(b.i64);
})
/// @brief Test whether the unsigned left operand is less than or equal to the right operand.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return Unsigned less-than-or-equal result.
DEFINE_CMP_HANDLER(handleUCmpLE, [](const Slot &a, const Slot &b) {
    return static_cast<uint64_t>(a.i64) <= static_cast<uint64_t>(b.i64);
})
/// @brief Test whether the unsigned left operand is greater than the right operand.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return Unsigned greater-than result.
DEFINE_CMP_HANDLER(handleUCmpGT, [](const Slot &a, const Slot &b) {
    return static_cast<uint64_t>(a.i64) > static_cast<uint64_t>(b.i64);
})
/// @brief Test whether the unsigned left operand is greater than or equal to the right operand.
/// @param a Left operand slot.
/// @param b Right operand slot.
/// @return Unsigned greater-than-or-equal result.
DEFINE_CMP_HANDLER(handleUCmpGE, [](const Slot &a, const Slot &b) {
    return static_cast<uint64_t>(a.i64) >= static_cast<uint64_t>(b.i64);
})

#undef DEFINE_CMP_HANDLER

} // namespace il::vm::detail::integer
