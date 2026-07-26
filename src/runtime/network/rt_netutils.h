//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_netutils.h
// Purpose: Static network utility functions — port checking, CIDR matching,
//          interface enumeration, and IP classification.
// Key invariants:
//   - All functions are stateless and thread-safe.
//   - Port checks use a connect+close cycle and apply the timeout per resolved address.
// Ownership/Lifetime:
//   - Pure functions; no state, no heap allocation beyond return values.
// Links: rt_network.h (socket primitives)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Check if a remote port is open (accepts connections).
/// @param host Host name or numeric address to resolve.
/// @param port TCP port in the inclusive range 1 through 65535.
/// @param timeout_ms Shared probe deadline in milliseconds; non-positive values use 1000.
/// @return One when any resolved address accepts a connection before the deadline; zero otherwise.
int8_t rt_netutils_is_port_open(rt_string host, int64_t port, int64_t timeout_ms);

/// @brief Get a free (available) port on the local machine.
/// @return Ephemeral loopback TCP port selected by the OS, or zero on failure.
int64_t rt_netutils_get_free_port(void);

/// @brief Check if an IP address matches a CIDR range (e.g., "10.0.0.0/8").
/// @param ip IPv4 address String.
/// @param cidr IPv4 network with an optional `/0` through `/32` prefix.
/// @return One when @p ip belongs to the range; zero for no match or invalid input.
int8_t rt_netutils_match_cidr(rt_string ip, rt_string cidr);

/// @brief Check if an IP address is in a private range (RFC 1918).
/// @param ip IPv4 address String.
/// @return One for RFC 1918 or loopback addresses; zero otherwise.
int8_t rt_netutils_is_private_ip(rt_string ip);

/// @brief Get the IPv4 source selected for a public UDP route, or loopback on failure.
/// @return Caller-owned IPv4 address String.
rt_string rt_netutils_local_ipv4(void);

#ifdef __cplusplus
}
#endif
