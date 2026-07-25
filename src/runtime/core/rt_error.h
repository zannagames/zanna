//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_error.h
// Purpose: Structured runtime error system with categorized error codes (Err enum) and an RtError
// record propagated via out-parameters, mapping to BASIC ON ERROR handlers and IL trap
// instructions.
//
// Key invariants:
//   - Err_None == 0 means success; all other values indicate failure.
//   - RtError.kind provides portable classification across platforms.
//   - RtError.code preserves platform-specific detail (e.g., errno).
//   - rt_ok returns true only when kind == Err_None.
//
// Ownership/Lifetime:
//   - RtError is a small value type intended for stack allocation by callers.
//   - RT_ERROR_NONE is a global constant; no heap allocation is involved.
//   - No ownership transfer; callers pass by value or pointer as needed.
//
// Links: src/runtime/core/rt_error.c (implementation)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares structured runtime errors and trap diagnostic metadata.
/// @details The API combines small by-value `RtError` results with thread-local
///   metadata used by language catch handlers and `Zanna.Diagnostics.TrapInfo`.

#pragma once

#include "rt_string.h"
#include <stdint.h>

/// @brief Internal runtime class identifier assigned to TrapInfo snapshots.
#define RT_TRAP_INFO_CLASS_ID INT64_C(-0x440301)

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Canonical runtime trap kinds shared by the native runtime and VM.
typedef enum RtTrapKind {
    /// Integer or numeric division by zero.
    RT_TRAP_KIND_DIVIDE_BY_ZERO = 0,
    /// Checked numeric overflow.
    RT_TRAP_KIND_OVERFLOW = 1,
    /// Invalid runtime type conversion.
    RT_TRAP_KIND_INVALID_CAST = 2,
    /// Value outside an operation's domain.
    RT_TRAP_KIND_DOMAIN_ERROR = 3,
    /// Index or range bounds violation.
    RT_TRAP_KIND_BOUNDS = 4,
    /// Requested filesystem entry was absent.
    RT_TRAP_KIND_FILE_NOT_FOUND = 5,
    /// End of an input stream was reached.
    RT_TRAP_KIND_EOF = 6,
    /// General input/output failure.
    RT_TRAP_KIND_IO_ERROR = 7,
    /// Operation invalid for current state.
    RT_TRAP_KIND_INVALID_OPERATION = 8,
    /// Unclassified runtime failure.
    RT_TRAP_KIND_RUNTIME_ERROR = 9,
    /// Execution interrupted or cancelled.
    RT_TRAP_KIND_INTERRUPT = 10,
    /// Network or protocol failure.
    RT_TRAP_KIND_NETWORK_ERROR = 11
} RtTrapKind;

/// @brief Canonical runtime error codes surfaced by runtime helpers.
enum Err {
    Err_None = 0,             ///< Success.
    Err_FileNotFound = 1,     ///< File could not be located.
    Err_EOF = 2,              ///< Reached end of input.
    Err_IOError = 3,          ///< Generic input/output failure.
    Err_Overflow = 4,         ///< Numeric overflow or underflow occurred.
    Err_InvalidCast = 5,      ///< Requested cast is invalid.
    Err_DomainError = 6,      ///< Input outside valid domain.
    Err_Bounds = 7,           ///< Bounds check failed.
    Err_InvalidOperation = 8, ///< Operation unsupported in current state.
    Err_RuntimeError = 9,     ///< Unclassified runtime error.

    // Network error codes (10–19).
    Err_ConnectionRefused = 10, ///< Remote host actively refused connection.
    Err_HostNotFound = 11,      ///< Hostname could not be resolved.
    Err_ConnectionReset = 12,   ///< Connection reset by remote peer (EPIPE, RST).
    Err_Timeout = 13,           ///< Operation timed out.
    Err_ConnectionClosed = 14,  ///< Operation on a closed connection.
    Err_DnsError = 15,          ///< DNS resolution failed.
    Err_InvalidUrl = 16,        ///< URL is malformed or unparseable.
    Err_TlsError = 17,          ///< TLS handshake or certificate failure.
    Err_NetworkError = 18,      ///< Generic network I/O failure.
    Err_ProtocolError = 19      ///< Protocol-level error (HTTP, WebSocket).
};

/// @brief Structured runtime error record propagated via out-parameters.
typedef struct RtError {
    enum Err kind; ///< High-level error category.
    int32_t code;  ///< Implementation-specific detail code.
} RtError;

/// @brief Helper returning whether @p error encodes success.
/// @param error Error record to inspect.
/// @return 1 when the error represents success, 0 otherwise.
static inline int8_t rt_ok(RtError error) {
    return error.kind == Err_None;
}

/// @brief Constant representing a successful runtime operation.
extern const RtError RT_ERROR_NONE;

/// @brief Store the thrown message string for retrieval by catch handlers.
/// @details Replaces and releases the previous thread-local message.
/// @param msg Borrowed message whose reference is retained, or NULL to clear.
void rt_throw_msg_set(rt_string msg);

/// @brief Clear the last thrown message string.
/// @details Releases the retained thread-local reference, if present.
void rt_throw_msg_clear(void);

/// @brief Retrieve the last thrown message string.
/// @return The message string, or an empty string if none was stored.
///         Caller receives a new reference (must release).
rt_string rt_throw_msg_get(void);

/// @brief Convert a trap kind enum value to a stable user-facing type name.
/// @param kind Canonical `RT_TRAP_KIND_*` value.
/// @return Caller-owned runtime string; unknown values produce `RuntimeError`.
rt_string rt_error_kind_name(int32_t kind);

/// @brief Build a user-facing message for a caught trap.
/// @details Runtime-error traps reuse a retained thrown message when present;
///   other classifications use stable default text.
/// @param kind Canonical `RT_TRAP_KIND_*` value.
/// @param code Secondary error code, currently reserved.
/// @param line Source line number, currently reserved.
/// @return Caller-owned runtime string containing the selected message.
rt_string rt_error_message(int32_t kind, int32_t code, int32_t line);

/// @brief Build a user-facing location string for a caught trap.
/// @param kind Canonical trap kind, currently reserved.
/// @param code Secondary error code, currently reserved.
/// @param line Source line number, or a negative value when unavailable.
/// @return Caller-owned `line N` string, or an empty runtime string when the
///   source line is unavailable.
rt_string rt_error_location(int32_t kind, int32_t code, int32_t line);

/// @brief Store trap classification fields for retrieval by catch handlers.
/// @param kind TrapKind enum value (0=DivByZero, 3=DomainError, 9=RuntimeError, etc.)
/// @param code Secondary error code.
/// @param line Source line number (-1 if unknown).
void rt_trap_fields_set(int32_t kind, int32_t code, int32_t line);

/// @brief Store the native instruction pointer associated with the most recent trap.
/// @details Marks current trap metadata as available without replacing its
///   kind, code, or source line.
/// @param ip Native return address captured at the trap site.
void rt_trap_set_ip(uint64_t ip);

/// @brief Return the current thread's trap snapshot as `Option<TrapInfo>`.
/// @details Returns `None` when no trap metadata has been recorded on the
///          current thread. Otherwise returns `Some(TrapInfo)` containing a
///          read-only copy of the kind, code, instruction pointer, source line,
///          message, kind name, and formatted location currently stored by the
///          trap machinery. Allocation failures raise a trap after temporary
///          references and recovery state are cleaned up.
/// @return Caller-owned opaque `Zanna.Option` object containing
///   `Zanna.Diagnostics.TrapInfo`.
void *rt_diagnostics_current_trap(void);

/// @brief Read the trap kind from a `Zanna.Diagnostics.TrapInfo` snapshot.
/// @param obj Opaque `TrapInfo` object.
/// @return Canonical trap kind integer.
int64_t rt_trap_info_get_kind(void *obj);

/// @brief Read the runtime error code from a `Zanna.Diagnostics.TrapInfo` snapshot.
/// @param obj Opaque `TrapInfo` object.
/// @return Runtime `Err_*` code or zero when none was recorded.
int64_t rt_trap_info_get_code(void *obj);

/// @brief Read the instruction pointer from a `Zanna.Diagnostics.TrapInfo` snapshot.
/// @param obj Opaque `TrapInfo` object.
/// @return Native instruction pointer value or zero when unavailable.
int64_t rt_trap_info_get_ip(void *obj);

/// @brief Read the source line from a `Zanna.Diagnostics.TrapInfo` snapshot.
/// @param obj Opaque `TrapInfo` object.
/// @return Source line number, or -1 when unavailable.
int64_t rt_trap_info_get_line(void *obj);

/// @brief Read the trap kind name from a `Zanna.Diagnostics.TrapInfo` snapshot.
/// @param obj Opaque `TrapInfo` object.
/// @return Owned runtime string containing the stable kind name.
rt_string rt_trap_info_get_kind_name(void *obj);

/// @brief Read the message from a `Zanna.Diagnostics.TrapInfo` snapshot.
/// @param obj Opaque `TrapInfo` object.
/// @return Owned runtime string containing the trap message.
rt_string rt_trap_info_get_message(void *obj);

/// @brief Read the formatted location from a `Zanna.Diagnostics.TrapInfo` snapshot.
/// @param obj Opaque `TrapInfo` object.
/// @return Owned runtime string containing the location, or an empty string.
rt_string rt_trap_info_get_location(void *obj);

/// @brief Retrieve the last trap's kind classification.
/// @return Canonical trap kind stored for the current thread.
int64_t rt_trap_get_kind(void);

/// @brief Retrieve the last trap's error code.
/// @return Secondary runtime error code stored for the current thread.
int64_t rt_trap_get_code(void);

/// @brief Retrieve the native instruction pointer of the last trap.
/// @return Stored native or IL instruction pointer, or zero when unavailable.
int64_t rt_trap_get_ip(void);

/// @brief Retrieve the last trap's source line number.
/// @return Stored source line, or -1 when unavailable.
int64_t rt_trap_get_line(void);

/// @brief Map a legacy runtime Err_* code to the canonical trap-kind integer.
/// @details Network codes map to `RT_TRAP_KIND_NETWORK_ERROR`; unknown codes
///   and `Err_None` map to `RT_TRAP_KIND_RUNTIME_ERROR`.
/// @param code Legacy runtime `Err_*` value.
/// @return Corresponding canonical `RT_TRAP_KIND_*` integer.
int32_t rt_err_to_trap_kind(int32_t code);

/// @brief Construct the current thread's native trap payload and return an opaque token.
/// @param code Legacy Err_* code.
/// @param msg Borrowed runtime string retained as the thrown message.
/// @return Non-owning opaque token encoding the low 32 bits of @p code for
///   native Error-typed results; no object is allocated.
void *rt_trap_error_make(int32_t code, rt_string msg);

/// @brief Raise a trap with explicit trap metadata.
/// @param kind Canonical trap classification.
/// @param code Secondary runtime error code (Err_* or 0).
/// @param line Source line number (-1 if unknown).
/// @param msg Borrowed null-terminated message, or NULL for no message.
/// @note Dispatch may perform a non-local transfer, call the VM trap hook, or
///   return if an embedder's hook permits it.
void rt_trap_raise_kind(int32_t kind, int32_t code, int32_t line, const char *msg);

/// @brief Raise a trap with explicit trap metadata and no message.
/// @param kind Canonical trap classification.
/// @param code Secondary runtime error code (Err_* or 0).
/// @param line Source line number (-1 if unknown).
/// @note Equivalent to @ref rt_trap_raise_kind with a NULL message.
void rt_trap_raise_kind_nomsg(int32_t kind, int32_t code, int32_t line);

/// @brief Raise a trap classified from a legacy Err_* code while preserving @p msg.
/// @param code Legacy Err_* code.
/// @param msg Borrowed null-terminated message, or NULL for no message.
void rt_trap_raise_error_msg(int32_t code, const char *msg);

/// @brief Raise a trap classified from a legacy runtime Err_* code.
/// @param code Legacy Err_* code.
/// @note Dispatches without an explicit user-visible message.
void rt_trap_raise_error(int32_t code);

#ifdef __cplusplus
}
#endif
