//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements trap token helpers for the virtual machine runtime.  The routines
// convert between enum encodings, manage active trap tokens for both VM-owned
// and thread-local fallbacks, and provide helpers for raising and formatting
// runtime traps.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Trap token management utilities for the VM runtime.
/// @details This translation unit exposes helpers that translate between raw
///          integer encodings and @ref TrapKind values, coordinates ownership of
///          active trap tokens between VM instances and thread-local fallbacks,
///          and renders human-readable diagnostics for raised traps.

#include "vm/Trap.hpp"

#include "rt.hpp"
#include "vm/RuntimeBridge.hpp"
#include "vm/TrapInvariants.hpp"
#include "vm/VM.hpp"

#include <string_view>

#ifdef EOF
#pragma push_macro("EOF")
#undef EOF
#define IL_VM_TRAP_CPP_RESTORE_EOF 1
#endif

namespace il::vm {

/// @brief C runtime trap hook implemented by the runtime bridge.
/// @param msg Null-terminated formatted diagnostic.
extern "C" void vm_trap(const char *msg);

// NOTE: toString() and trapKindFromValue() are now defined inline in Trap.hpp
// as constexpr functions for compile-time evaluation

#ifdef IL_VM_TRAP_CPP_RESTORE_EOF
#pragma pop_macro("EOF")
#undef IL_VM_TRAP_CPP_RESTORE_EOF
#endif

namespace {
/// @brief Thread-local fallback error used when no VM is active.
thread_local VmError tlsTrapError{};
/// @brief Thread-local diagnostic paired with @ref tlsTrapError.
thread_local std::string tlsTrapMessage;
/// @brief Whether the thread-local fallback currently contains a live token.
thread_local bool tlsTrapValid = false;

/// @brief Derive a source location from captured frame metadata.
/// @details Prefers the full stored location, then reconstructs line and column
///          fields when only legacy scalar metadata is available.
/// @param frame Frame metadata associated with a trap.
/// @return Best available source location.
il::support::SourceLoc locFromFrame(const FrameInfo &frame) {
    il::support::SourceLoc loc{};
    if (frame.loc.isValid())
        return frame.loc;
    if (frame.line >= 0)
        loc.line = static_cast<uint32_t>(frame.line);
    if (frame.column >= 0)
        loc.column = static_cast<uint32_t>(frame.column);
    return loc;
}
} // namespace

/// @brief Acquire a mutable trap token for recording runtime errors.
///
/// @details When a VM instance is active the function returns a pointer to its
///          owned trap token after clearing any previous data.  Otherwise it
///          falls back to thread-local storage so callers outside the VM can
///          still materialise traps (for example, during unit tests).
///
/// INVARIANT: The returned pointer remains valid until the next call to
///            vm_acquire_trap_token() or until the VM is destroyed.
/// GUARANTEE: Previous trap token data is cleared before returning.
///
/// @return Pointer to a writable trap token associated with the current thread.
VmError *vm_acquire_trap_token() {
    if (auto *vm = VM::activeInstance()) {
        // Clear any previous trap data to prevent stale state
        vm->trapToken.error = {};
        vm->trapToken.message.clear();
        vm->trapToken.valid = true;
        return &vm->trapToken.error;
    }

    // Fallback to thread-local storage when no VM is active
    tlsTrapError = {};
    tlsTrapMessage.clear();
    tlsTrapValid = true;
    return &tlsTrapError;
}

/// @brief Retrieve the currently active trap token when one exists.
///
/// @details Consults the active VM first and validates that the token is marked
///          as valid before returning it.  If the VM is absent the thread-local
///          fallback is checked.  A null pointer indicates no trap is currently
///          armed.
///
/// @return Pointer to the active trap token or nullptr when none is available.
const VmError *vm_current_trap_token() {
    if (auto *vm = VM::activeInstance()) {
        if (!vm->trapToken.valid)
            return nullptr;
        return &vm->trapToken.error;
    }
    if (!tlsTrapValid)
        return nullptr;
    return &tlsTrapError;
}

/// @brief Mark the active trap token as cleared for both VM and thread-local paths.
///
/// @details Resets the validity flag on the VM-owned trap token when a VM is
///          active and clears the thread-local token otherwise.  Callers invoke
///          this once a trap has been processed so subsequent lookups do not
///          observe stale diagnostics.
void vm_clear_trap_token() {
    if (auto *vm = VM::activeInstance())
        vm->trapToken.valid = false;

    tlsTrapValid = false;
    tlsTrapMessage.clear();
}

/// @brief Attach a human-readable message to the active trap token.
///
/// @details Updates the VM-owned token when present, otherwise records the
///          message in thread-local storage.  Marking the token as valid ensures
///          subsequent queries recognise that a trap has been produced.
///
/// @param text Message describing the trap for diagnostics.
void vm_store_trap_token_message(std::string_view text) {
    if (auto *vm = VM::activeInstance()) {
        vm->trapToken.message.assign(text.begin(), text.end());
        vm->trapToken.valid = true;
        return;
    }

    tlsTrapMessage.assign(text.begin(), text.end());
    tlsTrapValid = true;
}

/// @brief Fetch the message associated with the current trap token.
///
/// @details Returns the VM-owned message when a VM is active, otherwise the
///          thread-local fallback message.  Callers typically forward this text
///          to users or logs.
///
/// @return Copy of the currently recorded trap message.
std::string vm_current_trap_message() {
    if (auto *vm = VM::activeInstance())
        return vm->trapToken.message;
    std::string message = tlsTrapMessage;
    vm_clear_trap_token();
    return message;
}

/// @brief Format a trap error and frame information into a printable string.
///
/// @details Consolidates function name, block label, instruction pointer, and
///          source location into a concise diagnostic.  Missing data defaults
///          to placeholder values so the resulting string is still informative.
///
/// Format: "Trap @function:block#ip line N: Kind (code=C): path:line:column"
/// When a path is unavailable, falls back to "file#ID:line:column" after the
/// stable "line N" trap prefix.
///
/// @param error Trap token describing the failure.
/// @param frame Frame metadata captured when the trap surfaced.
/// @return Human-readable description of the trap.
std::string vm_format_error(const VmError &error, const FrameInfo &frame) {
    const std::string_view function =
        frame.function.empty() ? std::string_view("<unknown>") : std::string_view(frame.function);
    const uint64_t ip = error.ip ? error.ip : frame.ip;
    const int32_t line = error.line >= 0 ? error.line : (frame.line >= 0 ? frame.line : -1);
    const auto kindStr = toString(error.kind);

    // Pre-allocate buffer: "Trap @<func>:<block>#<ip> line N: <kind> (code=<code>)"
    // plus optional source detail. Typical overhead ~60 bytes + function + block + numbers.
    std::string result;
    result.reserve(64 + function.size() + frame.block.size());

    result.append("Trap @");
    result.append(function);
    if (!frame.block.empty()) {
        result.push_back(':');
        result.append(frame.block);
    }
    result.push_back('#');
    result.append(std::to_string(ip));
    const int32_t column =
        frame.column >= 0 ? frame.column
                          : (frame.loc.hasColumn() ? static_cast<int32_t>(frame.loc.column) : -1);
    std::string sourceDetail;
    if (!frame.file.empty()) {
        sourceDetail.append(frame.file);
        if (line >= 0) {
            sourceDetail.push_back(':');
            sourceDetail.append(std::to_string(line));
            if (column >= 0) {
                sourceDetail.push_back(':');
                sourceDetail.append(std::to_string(column));
            }
        }
    } else if (frame.loc.hasFile()) {
        sourceDetail.append("file#");
        sourceDetail.append(std::to_string(frame.loc.file_id));
        if (line >= 0) {
            sourceDetail.push_back(':');
            sourceDetail.append(std::to_string(line));
            if (column >= 0) {
                sourceDetail.push_back(':');
                sourceDetail.append(std::to_string(column));
            }
        }
    }
    if (line >= 0) {
        result.append(" line ");
        result.append(std::to_string(line));
    }
    result.append(": ");
    result.append(kindStr);
    result.append(" (code=");
    result.append(std::to_string(error.code));
    result.push_back(')');
    if (!sourceDetail.empty()) {
        result.append(": ");
        result.append(sourceDetail);
    }
    return result;
}

/// @brief Raise a trap using the supplied error description.
///
/// @details Normalises instruction pointer and line metadata against the active
///          VM context, allows the VM to intercept the trap via
///          @ref VM::prepareTrap, and records frame information for later
///          reporting.  When no handler is installed the runtime abort helper is
///          invoked to terminate execution with the formatted message.
///
/// @param input Trap information describing the failure.
void vm_raise_from_error(const VmError &input) {
    VmError error = input;
    FrameInfo frame{};
    std::string message;

    VM *activeVm = VM::activeInstance();
    if (activeVm) {
        const auto ctx = activeVm->currentTrapContext();
        if (error.ip == 0 && ctx.hasInstruction)
            error.ip = static_cast<uint64_t>(ctx.instructionIndex);
        if (error.line < 0 && ctx.loc.hasLine())
            error.line = static_cast<int32_t>(ctx.loc.line);

        if (activeVm->prepareTrap(error))
            return;

        frame = activeVm->buildFrameInfo(error);
        message = activeVm->recordTrap(error, frame);
    } else {
        frame.function = "<unknown>";
        frame.ip = error.ip;
        frame.line = error.line;
        frame.handlerInstalled = false;
        message = vm_format_error(error, frame);
    }

    if (!frame.handlerInstalled) {
        RuntimeBridge::interceptTrap(
            error.kind, error.code, message, locFromFrame(frame), frame.function, frame.block);
        if (activeVm) {
            auto bt = activeVm->buildBacktrace();
            if (bt.size() > 1)
                activeVm->printBacktrace(bt);
        }
        vm_trap(message.c_str());
        return;
    }
}

/// @brief Convenience wrapper that raises a trap from a kind/code pair.
///
/// @details Populates a @ref VmError structure with the provided metadata and
///          enriches it with instruction pointer and line information from the
///          active VM when available.  Control is then delegated to
///          @ref vm_raise_from_error for final processing.
///
///          HIGH-2 optimization: caches the TLS lookup once instead of twice
///          by inlining the vm_raise_from_error logic here.
///
/// @param kind Trap classification to raise.
/// @param code Optional runtime-specific error code.
void vm_raise(TrapKind kind, int32_t code) {
    // HIGH-2: Cache TLS lookup once for both enrichment and final processing
    // This avoids the double TLS access that would occur if we called
    // vm_raise_from_error() which does its own activeInstance() lookup.
    VM *vm = VM::activeInstance();

    VmError error{};
    error.kind = kind;
    error.code = code;
    error.ip = 0;
    error.line = -1;

    FrameInfo frame{};
    std::string message;

    if (vm) {
        const auto ctx = vm->currentTrapContext();
        if (ctx.hasInstruction)
            error.ip = static_cast<uint64_t>(ctx.instructionIndex);
        if (ctx.loc.hasLine())
            error.line = static_cast<int32_t>(ctx.loc.line);

        if (vm->prepareTrap(error))
            return;

        frame = vm->buildFrameInfo(error);
        message = vm->recordTrap(error, frame);
    } else {
        frame.function = "<unknown>";
        frame.ip = error.ip;
        frame.line = error.line;
        frame.handlerInstalled = false;
        message = vm_format_error(error, frame);
    }

    if (!frame.handlerInstalled) {
        RuntimeBridge::interceptTrap(
            error.kind, error.code, message, locFromFrame(frame), frame.function, frame.block);
        if (vm) {
            auto bt = vm->buildBacktrace();
            if (bt.size() > 1)
                vm->printBacktrace(bt);
        }
        vm_trap(message.c_str());
        return;
    }
}

} // namespace il::vm
