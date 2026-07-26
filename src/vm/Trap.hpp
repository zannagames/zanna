//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/Trap.hpp
// Purpose: Defines trap classification for VM diagnostics.
// Key invariants: Enum values map directly to trap categories used in diagnostics.
// Ownership/Lifetime: Not applicable.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares VM trap categories, structured diagnostics, token storage,
///        formatting, and escalation helpers.
/// @details Trap tokens use VM-owned storage during interpreted execution and a
///          thread-local fallback otherwise.  Raising a trap enriches its error
///          record with frame context before transferring to an installed
///          handler or the runtime failure hook.

#pragma once

#include "support/source_location.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace il::vm {

/// @brief Categorises runtime traps for diagnostic reporting.
#ifdef EOF
#pragma push_macro("EOF")
#undef EOF
#define IL_VM_TRAP_RESTORE_EOF 1
#endif
enum class TrapKind : int32_t {
    DivideByZero = 0,     ///< Integer division or remainder by zero.
    Overflow = 1,         ///< Arithmetic or conversion overflow.
    InvalidCast = 2,      ///< Invalid cast or conversion semantics.
    DomainError = 3,      ///< Semantic domain violation or user trap.
    Bounds = 4,           ///< Bounds check failure.
    FileNotFound = 5,     ///< File system open on a path that does not exist.
    EOF = 6,              ///< End-of-file reached while input still expected.
    IOError = 7,          ///< Generic I/O failure.
    InvalidOperation = 8, ///< Operation outside the allowed state machine.
    RuntimeError = 9,     ///< Catch-all for unexpected runtime failures.
    Interrupt = 10,       ///< Program interrupted by Ctrl-C or requestInterrupt().
    NetworkError = 11,    ///< Network I/O failure (connection, DNS, TLS, etc.).
};

/// @brief Structured representation of a VM error record.
struct VmError {
    TrapKind kind = TrapKind::RuntimeError; ///< Trap classification.
    int32_t code = 0;                       ///< Secondary error code.
    uint64_t ip = 0;                        ///< Instruction pointer within block.
    int32_t line = -1;                      ///< Source line, or -1 when unknown.
};

/// @brief Execution context metadata used for trap formatting.
struct FrameInfo {
    std::string function;          ///< Function in which the trap occurred.
    std::string block;             ///< Block label where the trap occurred.
    uint64_t ip = 0;               ///< Instruction pointer of the trap.
    il::support::SourceLoc loc{};  ///< Full source location when known.
    std::string file;              ///< Resolved source file path when known.
    int32_t line = -1;             ///< Source line for diagnostics (-1 = unknown).
    int32_t column = -1;           ///< Source column for diagnostics (-1 = unknown).
    bool handlerInstalled = false; ///< Whether an error handler is active.
};

/// @brief Convert trap kind to canonical diagnostic string.
/// @param kind Enumerated trap kind.
/// @return Stable string view naming the trap category.
/// @note Constexpr for compile-time string resolution when possible.
constexpr std::string_view toString(TrapKind kind) noexcept {
    switch (kind) {
        case TrapKind::DivideByZero:
            return "DivideByZero";
        case TrapKind::Overflow:
            return "Overflow";
        case TrapKind::InvalidCast:
            return "InvalidCast";
        case TrapKind::DomainError:
            return "DomainError";
        case TrapKind::Bounds:
            return "Bounds";
        case TrapKind::FileNotFound:
            return "FileNotFound";
        case TrapKind::EOF:
            return "EOF";
        case TrapKind::IOError:
            return "IOError";
        case TrapKind::InvalidOperation:
            return "InvalidOperation";
        case TrapKind::RuntimeError:
            return "RuntimeError";
        case TrapKind::Interrupt:
            return "Interrupt";
        case TrapKind::NetworkError:
            return "NetworkError";
    }
    return "RuntimeError";
}

/// @brief Translate an integer payload into a TrapKind value.
/// @param value Integer supplied by IL operands.
/// @return Enumerated trap kind, defaulting to RuntimeError for unknown values.
/// @note Constexpr for compile-time value resolution when possible.
constexpr TrapKind trapKindFromValue(int32_t value) noexcept {
    switch (value) {
        case static_cast<int32_t>(TrapKind::DivideByZero):
            return TrapKind::DivideByZero;
        case static_cast<int32_t>(TrapKind::Overflow):
            return TrapKind::Overflow;
        case static_cast<int32_t>(TrapKind::InvalidCast):
            return TrapKind::InvalidCast;
        case static_cast<int32_t>(TrapKind::DomainError):
            return TrapKind::DomainError;
        case static_cast<int32_t>(TrapKind::Bounds):
            return TrapKind::Bounds;
        case static_cast<int32_t>(TrapKind::FileNotFound):
            return TrapKind::FileNotFound;
        case static_cast<int32_t>(TrapKind::EOF):
            return TrapKind::EOF;
        case static_cast<int32_t>(TrapKind::IOError):
            return TrapKind::IOError;
        case static_cast<int32_t>(TrapKind::InvalidOperation):
            return TrapKind::InvalidOperation;
        case static_cast<int32_t>(TrapKind::RuntimeError):
            return TrapKind::RuntimeError;
        case static_cast<int32_t>(TrapKind::Interrupt):
            return TrapKind::Interrupt;
        case static_cast<int32_t>(TrapKind::NetworkError):
            return TrapKind::NetworkError;
    }
    return TrapKind::RuntimeError;
}

/// @brief Obtain writable storage for constructing a trap token in the active VM.
/// @return Pointer to a VmError slot owned by the VM or thread-local fallback when no VM is active.
VmError *vm_acquire_trap_token();

/// @brief Access the most recently written trap token for the active VM.
/// @return Pointer to the stored VmError when available, otherwise nullptr.
const VmError *vm_current_trap_token();

/// @brief Clear the active trap token so future lookups observe no pending trap.
/// @details Resets both the VM-owned and thread-local trap token validity flags
///          after a trap has been fully handled. Callers use this helper to
///          prevent stale trap tokens from persisting beyond their intended
///          lifetime.
void vm_clear_trap_token();

/// @brief Store the diagnostic message associated with the current trap token.
/// @param text Human-readable message text to retain alongside the token.
void vm_store_trap_token_message(std::string_view text);

/// @brief Retrieve the diagnostic message associated with the current trap token.
/// @return Copy of the stored message text.
std::string vm_current_trap_message();

/// @brief Raise a trap with the specified kind and optional error code.
/// @details Populates and stores a trap token that can be queried via
///          vm_current_trap_token(). If an error handler is installed in the
///          active VM, control transfers to the handler; otherwise execution
///          terminates with a diagnostic message.
/// @param kind Classification of the trap condition (e.g., DivideByZero).
/// @param code Secondary error code providing additional context (default 0).
void vm_raise(TrapKind kind, int32_t code = 0);

/// @brief Raise a trap from an existing VmError record.
/// @details Enriches the provided error with active instruction metadata,
///          offers it to any installed VM handler, records frame information,
///          and otherwise reports it through the runtime trap hook.
/// @param error Fully populated error record to propagate.
void vm_raise_from_error(const VmError &error);

/// @brief Format a human-readable diagnostic message for a trap.
/// @details Combines the error classification, code, and frame context into a
///          single-line string suitable for display to users or logging.
/// @param error Error record containing trap kind, code, and location info.
/// @param frame Execution context (function name, block label, source line).
/// @return Formatted diagnostic message string.
std::string vm_format_error(const VmError &error, const FrameInfo &frame);

#ifdef IL_VM_TRAP_RESTORE_EOF
#pragma pop_macro("EOF")
#undef IL_VM_TRAP_RESTORE_EOF
#endif

} // namespace il::vm
