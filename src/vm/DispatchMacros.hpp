//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/DispatchMacros.hpp
//
// Purpose:
//   Centralizes definitions of macros used for VM opcode dispatch. This header
//   documents the relationship between Opcode.def entries and VM handler
//   generation across all dispatch strategies (function table, switch, threaded).
//
// ============================================================================
// HOW VM DISPATCH WORKS
// ============================================================================
//
// The VM supports three dispatch strategies for executing IL instructions:
//
// 1. FUNCTION TABLE (FnTable):
//    - Uses an array of function pointers indexed by opcode value
//    - Handlers are declared as: VM::ExecResult handle<Name>(VM&, Frame&, ...)
//    - Generated in: HandlerTable.hpp (static table)
//    - Entry point: VM::executeOpcode()
//
// 2. SWITCH DISPATCH:
//    - Uses a switch statement on instr.op
//    - Calls inline_handle_<Name>(state) for each case
//    - Generated in: SwitchDispatchImpl.inc
//    - Entry point: VM::dispatchOpcodeSwitch()
//
// 3. THREADED DISPATCH (computed goto):
//    - Uses goto *label_table[opcode] for direct threading
//    - Labels are: LBL_<Name>
//    - Generated in: ThreadedLabels.inc, ThreadedCases.inc
//    - Entry point: ThreadedDispatchDriver::run()
//
// ============================================================================
// MACRO LAYER OVERVIEW
// ============================================================================
//
// IL_OPCODE(Name, Mnemonic, ..., DispatchSpec, ...)
//   - Defined in: il/core/Opcode.def
//   - Expands to: enum value, metadata, and dispatch binding
//   - DispatchSpec argument determines VM handler mapping
//
// VM_DISPATCH(Name)
//   - Maps opcode Name to handler handle<Name>
//   - Default: VM_DISPATCH_IMPL(Name, Name)
//
// VM_DISPATCH_ALT(DispatchName, Handler)
//   - Maps opcode to a different handler function
//   - Example: VM_DISPATCH_ALT(TrapKindRead, handleTrapKind)
//
// VM_DISPATCH_IMPL(DispatchName, Handler)
//   - Low-level: specifies dispatch table entry and handler
//   - Default expands to: VMDispatch::DispatchName
//
// ============================================================================
// ADDING A NEW OPCODE - STEP BY STEP
// ============================================================================
//
// Step 1: Add the opcode to Opcode.def
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   In src/il/core/Opcode.def, add a new IL_OPCODE entry:
//
//   IL_OPCODE(MyOp,              // Name (becomes Opcode::MyOp enum)
//             "my.op",           // Mnemonic (for parser/serializer)
//             ResultArity::One,  // Result arity
//             TypeCategory::I64, // Result type category
//             2, 2,              // Min/max operand count
//             TypeCategory::I64, // Operand 0 type
//             TypeCategory::I64, // Operand 1 type
//             TypeCategory::None,// Operand 2 type (None if unused)
//             false,             // May trap
//             0,                 // Branch target count
//             false,             // Is terminator
//             VM_DISPATCH(MyOp), // <-- Dispatch specification
//             makeParseSpec(...),// Parse specs...
//             ...)
//
// Step 2: Implement the handler function
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   Create the handler in the appropriate OpHandlers_*.hpp file:
//
//   // In OpHandlers_Int.hpp (or appropriate category file)
//   namespace il::vm::detail::integer {
//   VM::ExecResult handleMyOp(VM &vm, Frame &fr, const Instr &in,
//                             const VM::BlockMap &blocks,
//                             const BasicBlock *&bb, size_t &ip);
//   }
//
//   Implement in OpHandlers_Int.cpp:
//   VM::ExecResult handleMyOp(...) {
//       // Implementation
//       Slot result;
//       result.i64 = /* computation */;
//       fr.regs[*in.result] = result;
//       return {}; // No control flow change
//   }
//
// Step 3: Export the handler in OpHandlers.hpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   Add the using declaration:
//
//   namespace il::vm::detail {
//   using integer::handleMyOp;
//   }
//
// Step 4: Add to the handler table
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   In src/vm/ops/generated/HandlerTable.hpp, add the entry:
//
//   static const VM::OpcodeHandlerTable table = {
//       // ... existing entries ...
//       &il::vm::detail::handleMyOp,  // At position matching Opcode::MyOp
//   };
//
// Step 5: Add inline handler for switch/threaded dispatch
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   In src/vm/ops/generated/InlineHandlersImpl.inc:
//
//   void VM::inline_handle_MyOp(ExecState &st) {
//       const Instr *instr = st.currentInstr;
//       if (!instr) {
//           trapUnimplemented(Opcode::MyOp);
//       }
//       ExecResult exec = il::vm::detail::handleMyOp(
//           *this, st.fr, *instr, st.blocks, st.bb, st.ip);
//       handleInlineResult(st, exec);
//   }
//
//   In src/vm/ops/generated/InlineHandlersDecl.inc:
//   void inline_handle_MyOp(ExecState &st);
//
// Step 6: Update switch dispatch
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   In src/vm/ops/generated/SwitchDispatchImpl.inc:
//
//   case il::core::Opcode::MyOp: {
//       traceInstruction(instr, state.fr);
//       inline_handle_MyOp(state);
//       break;
//   }
//
// Step 7: Update threaded dispatch
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   In src/vm/ops/generated/ThreadedLabels.inc (add label pointer):
//   &&LBL_MyOp,
//
//   In src/vm/ops/generated/ThreadedCases.inc (add case):
//   LBL_MyOp: {
//       vm.traceInstruction(*currentInstr, state.fr);
//       vm.inline_handle_MyOp(state);
//       if (state.exitRequested) return true;
//       opcode = fetchNext();
//       if (state.exitRequested) return true;
//       ZANNA_VM_DISPATCH_BEFORE(context, opcode);
//       DISPATCH_TO(opcode);
//   }
//
// Step 8: Add verifier support (if needed)
// ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//   Update src/il/verify/InstructionChecker*.cpp with validation rules.
//
// Step 9: Run tests
// ~~~~~~~~~~~~~~~~~~
//   cmake --build build -j
//   ctest --test-dir build --output-on-failure
//
// ============================================================================
// COMPILE-TIME SAFETY
// ============================================================================
//
// To ensure all opcodes have handlers, this header provides:
//
// 1. ZANNA_HANDLER_COUNT - Total handlers in the table
// 2. ZANNA_ASSERT_HANDLER_COVERAGE - static_assert for table size
//
// The handler table size must equal il::core::kNumOpcodes to ensure every
// opcode has a corresponding handler (or explicit stub).
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines VM opcode-dispatch metadata, generation macros, and
///        compile-time handler coverage checks.
/// @details The declarations connect opcode registry entries to function-table,
///          switch, and computed-goto dispatch implementations.  Validation
///          helpers allow generated tables to prove complete coverage at compile
///          time or inspect handler availability in constant expressions.

#pragma once

#include "il/core/Opcode.hpp"
#include <cstddef>

namespace il::vm::dispatch {

//===----------------------------------------------------------------------===//
// Handler Registry Types
//===----------------------------------------------------------------------===//

/// @brief Marker type for VM dispatch entries.
/// @details Used in Opcode.def to bind opcodes to handlers at compile time.
///          The VMDispatch enum mirrors Opcode names for dispatch metadata.
enum class VMDispatch {
#define IL_OPCODE(NAME, ...) NAME,
#include "il/core/Opcode.def"
    Count
};

/// @brief Number of dispatch entries (should match opcode count).
constexpr size_t kDispatchCount = static_cast<size_t>(VMDispatch::Count);

//===----------------------------------------------------------------------===//
// Compile-Time Safety Assertions
//===----------------------------------------------------------------------===//

/// @brief Assert that the handler table covers all opcodes.
/// @details Use this macro in handler table definitions to catch mismatches
///          at compile time rather than runtime.
/// @param TABLE Fixed-size handler table expression checked with @c std::size.
#define ZANNA_ASSERT_HANDLER_TABLE_SIZE(TABLE)                                                     \
    static_assert(std::size(TABLE) == il::core::kNumOpcodes,                                       \
                  "Handler table size mismatch: missing or extra opcode handlers")

/// @brief Assert that a specific table has the expected size.
/// @details For arrays where std::size() isn't available.
/// @param COUNT Constant expression containing the number of handlers.
#define ZANNA_ASSERT_HANDLER_COUNT(COUNT)                                                          \
    static_assert((COUNT) == il::core::kNumOpcodes,                                                \
                  "Handler count mismatch: missing or extra opcode handlers")

//===----------------------------------------------------------------------===//
// Dispatch Table Generation Helpers
//===----------------------------------------------------------------------===//

/// @brief Generate a handler table entry using the opcode name.
/// @details Expands to the handler function address for TABLE dispatch.
/// @param NAME Opcode suffix appended to the @c handle function prefix.
#define ZANNA_HANDLER_ENTRY(NAME) &il::vm::detail::handle##NAME

/// @brief Generate a switch case for an opcode.
/// @details Expands to a case statement calling the inline handler.
/// @param NAME Opcode enumerator and inline-handler suffix.
#define ZANNA_SWITCH_CASE(NAME)                                                                    \
    case il::core::Opcode::NAME:                                                                   \
        inline_handle_##NAME(state);                                                               \
        break

/// @brief Generate a threaded dispatch label.
/// @details Expands to a computed goto label address.
/// @param NAME Opcode suffix appended to the threaded label prefix.
#define ZANNA_THREADED_LABEL(NAME) &&LBL_##NAME

//===----------------------------------------------------------------------===//
// Runtime Handler Verification
//===----------------------------------------------------------------------===//

/// @brief Check if an opcode has a valid handler at runtime.
/// @tparam Table Indexable fixed-size handler-table type supported by
///         @c std::size.
/// @param op Opcode to check.
/// @param table Handler table to search.
/// @return @c true if @p op indexes a non-null handler.
template <typename Table> constexpr bool hasHandler(il::core::Opcode op, const Table &table) {
    const size_t index = static_cast<size_t>(op);
    return index < std::size(table) && table[index] != nullptr;
}

/// @brief Verify all opcodes have handlers (for debug builds).
/// @tparam Table Indexable fixed-size handler-table type supported by
///         @c std::size.
/// @param table Handler table to verify.
/// @return @c true if every registered opcode has a non-null table entry.
template <typename Table> constexpr bool verifyAllHandlers(const Table &table) {
    for (size_t i = 0; i < il::core::kNumOpcodes; ++i) {
        if (i >= std::size(table) || table[i] == nullptr)
            return false;
    }
    return true;
}

} // namespace il::vm::dispatch

//===----------------------------------------------------------------------===//
// Legacy Macro Compatibility
//===----------------------------------------------------------------------===//
// These macros are used by Opcode.def and should not be redefined elsewhere.
// They are documented here for reference:
//
// VM_DISPATCH(NAME)
//   - Default: Binds Opcode::NAME to handler handleNAME
//   - Used for most opcodes where name matches handler
//
// VM_DISPATCH_ALT(DISPATCH, HANDLER)
//   - Alternative: Binds opcode to a differently-named handler
//   - Used when multiple opcodes share a handler
//
// VM_DISPATCH_IMPL(DISPATCH, HANDLER)
//   - Implementation: Low-level dispatch binding
//   - Rarely used directly
//
// These macros are defined with defaults in Opcode.def and automatically
// cleaned up after inclusion, so includers can redefine them as needed.
//===----------------------------------------------------------------------===//
