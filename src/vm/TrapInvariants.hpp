//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/TrapInvariants.hpp
//
// Purpose:
//   Documents trap invariants and provides assertion helpers for the VM trap
//   subsystem. This header centralizes the guarantees made by trap paths and
//   the contracts that callers must satisfy.
//
// ============================================================================
// TRAP SUBSYSTEM INVARIANTS
// ============================================================================
//
// 1. ActiveVMGuard and VM::activeInstance()
// ------------------------------------------
// - ActiveVMGuard is an RAII guard that installs a VM* in thread-local storage.
// - VM::activeInstance() returns the VM* installed by the innermost guard.
// - Guards may be nested; the previous VM is restored when a guard destructs.
// - INVARIANT: When RuntimeBridge::trap() is called with a VM context,
//   VM::activeInstance() MUST return non-null.
// - INVARIANT: When opcode handlers call vm_raise(), an ActiveVMGuard MUST
//   be in scope (via VM::run() or VM::execFunction()).
//
// 2. RuntimeBridge::trap() Contract
// ----------------------------------
// - May be called with or without an active VM.
// - When VM::activeInstance() returns non-null:
//   * vm_raise_from_error() is invoked to deliver the trap through the VM.
//   * The trap may be caught by an installed exception handler.
//   * The VM updates currentContext, runtimeContext, and lastTrap.
// - When VM::activeInstance() returns null:
//   * The trap is formatted and delivered via rt_abort().
//   * rt_abort() terminates the process; execution does not continue.
// - GUARANTEE: In production builds the common path does not return unless an
//   exception handler catches the trap. Test harnesses and embedders may
//   override vm_trap() with an observer that records the trap and returns.
//
// 3. lastTrap Lifetime and Updates
// ---------------------------------
// - VM::lastTrap holds metadata from the most recent trap (if any).
// - lastTrap is updated by VM::recordTrap() when a trap occurs.
// - lastTrap is NOT automatically cleared when execution resumes.
// - INVARIANT: lastTrap.message is non-empty after a trap is recorded.
// - GUARANTEE: After RunStatus::Trapped, lastTrap() returns valid info.
// - CAVEAT: lastTrap may contain stale data from a previous trap if a
//   subsequent successful execution does not clear it. Use clearLastTrap()
//   before running if stale data would cause confusion.
//
// 4. trapToken vs lastTrap
// -------------------------
// - trapToken: Temporary storage for constructing VmError during trap handling.
//   Accessed via vm_acquire_trap_token() and vm_current_trap_token().
//   Cleared by vm_clear_trap_token() after the trap is fully processed.
// - lastTrap: Persistent storage for diagnostic retrieval after execution.
//   Updated when a trap terminates execution or escapes a handler.
// - INVARIANT: trapToken.valid is only true during active trap processing.
// - INVARIANT: After vm_clear_trap_token(), trapToken.valid is false.
//
// 5. Exception Handler Integration
// ---------------------------------
// - When eh.push installs a handler, Frame::ehStack records the handler block.
// - VM::prepareTrap() checks for handlers and routes control appropriately.
// - If a handler is installed:
//   * frame.activeError is populated with the VmError.
//   * Control transfers to the handler block via TrapDispatchSignal.
//   * lastTrap is still updated for diagnostic purposes.
// - If no handler is installed:
//   * lastTrap is updated and rt_abort() is called.
//
// 6. Thread Safety
// -----------------
// - Trap state is thread-local; each thread has its own activeInstance.
// - Multiple VMs may execute on different threads concurrently.
// - A single VM must NOT execute on multiple threads simultaneously.
// - INVARIANT: tlsActiveVM is only modified via ActiveVMGuard.
//
// ============================================================================
// ASSERTION MACROS
// ============================================================================

/// @file
/// @brief Documents VM trap-state invariants and declares trap-path assertion
///        helpers.
/// @details The assertions are intended for cold failure paths: debug builds
///          use the standard assertion mechanism, while release builds emit a
///          diagnostic and abort when a core invariant is violated.

#pragma once

#include <cassert>
#include <cstdio>
#include <cstdlib>

#ifdef NDEBUG
// In release builds, trap invariant violations terminate with a diagnostic
// instead of silently continuing.  These checks run only on trap paths,
// not in hot loops, so the performance cost is negligible.
/// @brief Enforce a trap invariant in release builds.
/// @param condition Expression that must evaluate to true.
/// @param message Diagnostic printed before termination on failure.
#define ZANNA_TRAP_ASSERT(condition, message)                                                      \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "TRAP INVARIANT VIOLATED: %s\n", (message));                      \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)
#else
/// @brief Assert a trap invariant in debug builds.
/// @param condition Expression that must evaluate to true.
/// @param message Diagnostic attached to the assertion.
#define ZANNA_TRAP_ASSERT(condition, message) assert((condition) && (message))

/// @brief Assert that an active VM is installed when expected.
#define ZANNA_TRAP_REQUIRE_ACTIVE_VM()                                                             \
    ZANNA_TRAP_ASSERT(il::vm::VM::activeInstance() != nullptr,                                     \
                      "Trap path requires an active VM via ActiveVMGuard")

/// @brief Assert that no stale trap token exists.
#define ZANNA_TRAP_REQUIRE_NO_STALE_TOKEN()                                                        \
    ZANNA_TRAP_ASSERT(il::vm::vm_current_trap_token() == nullptr,                                  \
                      "Stale trap token exists; call vm_clear_trap_token() first")
#endif

namespace il::vm {

// Forward declarations
class VM;
struct VmError;

// Declaration for vm_current_trap_token (defined in Trap.cpp)
const VmError *vm_current_trap_token();

/// @brief Check if a trap token is currently pending.
/// @return @c true if @ref vm_current_trap_token would return non-null.
inline bool hasPendingTrapToken() {
    return vm_current_trap_token() != nullptr;
}

} // namespace il::vm
