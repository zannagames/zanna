//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/err_bridge.hpp
// Purpose: Defines temporary runtime error bridge mapping legacy codes to TrapKind.
// Key invariants: Mapping remains internal to the VM until runtime emits structured errors.
// Ownership/Lifetime: Header-only helpers; no dynamic state.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares the compatibility mapping from legacy runtime error numbers
///        to structured VM trap categories.
/// @details The bridge isolates historical BASIC/runtime codes while ensuring
///          every value presented to VM diagnostics has a defined TrapKind.

#pragma once

#include "vm/Trap.hpp"

#include <cstdint>

#ifdef EOF
#pragma push_macro("EOF")
#undef EOF
#define IL_VM_ERR_BRIDGE_RESTORE_EOF 1
#endif

namespace il::vm {

/// @brief Legacy runtime error codes forwarded through the VM bridge.
enum class ErrCode : int32_t {
    Err_None = 0,             ///< No runtime error.
    Err_FileNotFound = 1,     ///< Requested file does not exist.
    Err_EOF = 2,              ///< Operation reached end of file.
    Err_IOError = 3,          ///< General input/output failure.
    Err_Overflow = 4,         ///< Arithmetic or conversion overflow.
    Err_InvalidCast = 5,      ///< Value cannot be converted to the requested type.
    Err_DomainError = 6,      ///< Argument lies outside an operation's domain.
    Err_Bounds = 7,           ///< Index or range lies outside valid bounds.
    Err_InvalidOperation = 8, ///< Operation is not valid for the current state.
    Err_RuntimeError = 9,     ///< Unclassified runtime failure.

    // Network error codes (10–19).
    Err_ConnectionRefused = 10, ///< Remote endpoint refused the connection.
    Err_HostNotFound = 11,      ///< Host name could not be resolved.
    Err_ConnectionReset = 12,   ///< Peer reset an established connection.
    Err_Timeout = 13,           ///< Network operation exceeded its time limit.
    Err_ConnectionClosed = 14,  ///< Connection closed before the operation completed.
    Err_DnsError = 15,          ///< Domain-name resolution failed.
    Err_InvalidUrl = 16,        ///< URL syntax or scheme is unsupported.
    Err_TlsError = 17,          ///< TLS negotiation or transport failed.
    Err_NetworkError = 18,      ///< Unclassified network failure.
    Err_ProtocolError = 19,     ///< Peer violated the expected protocol.
};

/// @brief Map a legacy runtime error code to the corresponding trap kind.
/// @param err_code Numeric code reported by the runtime.
/// @return TrapKind describing the semantic category of the error.
TrapKind map_err_to_trap(int err_code);

#ifdef IL_VM_ERR_BRIDGE_RESTORE_EOF
#pragma pop_macro("EOF")
#undef IL_VM_ERR_BRIDGE_RESTORE_EOF
#endif

} // namespace il::vm
