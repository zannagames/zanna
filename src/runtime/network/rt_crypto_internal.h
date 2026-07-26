//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_crypto_internal.h
// Purpose: Shared internal crypto helpers used across the crypto translation
//   units (rt_crypto.c primitives + rt_x25519.c).
//
// Key invariants:
//   - Wiping is performed through volatile byte stores so ordinary compiler
//     dead-store elimination cannot remove it.
//
// Ownership/Lifetime:
//   - The helper borrows caller-owned writable storage and never allocates or
//     transfers ownership.
//
// Links: src/runtime/network/rt_crypto.c (implementation),
//        src/runtime/network/rt_x25519.c (key-material cleanup consumer).
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stddef.h>

/// @brief Best-effort constant-time memory wipe (defined in rt_crypto.c).
/// @param ptr Writable buffer whose bytes will be overwritten.
/// @param len Number of bytes to clear.
void rt_secure_zero(void *ptr, size_t len);
