//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http_router.h
// Purpose: HTTP URL pattern matching with parameter extraction.
// Key invariants:
//   - Routes are matched in registration order (first match wins).
//   - Pattern parameters use :name syntax (e.g., "/users/:id").
//   - Wildcard *name matches the rest of the path and should be the final segment.
//   - Route registration is exclusive; matching and counting use shared access.
// Ownership/Lifetime:
//   - Router and RouteMatch objects are managed references owned by their callers.
//   - RouteMatch parameter and pattern accessors return owned runtime strings.
// Links: rt_http_server.h (consumer), rt_network.h
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares synchronized HTTP route registration and matching.
 * @details Defines stable managed identities, method-specific and generic
 * route registration, first-match lookup with named and wildcard captures,
 * route counts, and owned RouteMatch parameter and pattern access.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable managed-object class tag for @c Zanna.Network.HttpRouter.
/// @details Runtime entry points validate this tag and the minimum payload size
///          before interpreting an opaque object as router state.
#define RT_HTTP_ROUTER_CLASS_ID INT64_C(-0x720201)

/// @brief Stable managed-object class tag for @c Zanna.Network.RouteMatch.
/// @details Route-match accessors reject wrong-class and undersized handles
///          rather than reading arbitrary managed-object payloads.
#define RT_ROUTE_MATCH_CLASS_ID INT64_C(-0x720202)

/// @brief Create an empty, internally synchronized HTTP router.
/// @details Routes may be registered concurrently with matching. Registration
///          order remains the tie-break order and the first matching route wins.
/// @return Owned managed router reference, or @c NULL after an allocation trap.
void *rt_http_router_new(void);

/// @brief Add a route for a specific HTTP method and pattern.
/// @details The method must be a non-empty HTTP token and is compared
///          case-sensitively. The pattern is parsed and validated before it is
///          atomically published, so a trap never leaves a partial route.
/// @param router Router object.
/// @param method HTTP method (GET, POST, PUT, DELETE, etc.).
/// @param pattern URL pattern (e.g., "/users/:id").
/// @return The original @p router for fluent chaining.
void *rt_http_router_add(void *router, rt_string method, rt_string pattern);

/// @brief Add a GET route.
/// @param router Router object.
/// @param pattern URL pattern to register.
/// @return The original @p router for fluent chaining.
void *rt_http_router_get(void *router, rt_string pattern);

/// @brief Add a POST route.
/// @param router Router object.
/// @param pattern URL pattern to register.
/// @return The original @p router for fluent chaining.
void *rt_http_router_post(void *router, rt_string pattern);

/// @brief Add a PUT route.
/// @param router Router object.
/// @param pattern URL pattern to register.
/// @return The original @p router for fluent chaining.
void *rt_http_router_put(void *router, rt_string pattern);

/// @brief Add a DELETE route.
/// @param router Router object.
/// @param pattern URL pattern to register.
/// @return The original @p router for fluent chaining.
void *rt_http_router_delete(void *router, rt_string pattern);

/// @brief Match a request method and path against registered routes.
/// @details The synchronized search performs no managed allocations. Parameter
///          extraction and RouteMatch construction occur only after the shared
///          lock is released, so allocation traps cannot strand the lock.
/// @param router Router object.
/// @param method HTTP method.
/// @param path Request path.
/// @return Owned RouteMatch object, or @c NULL if no route matches.
void *rt_http_router_match(void *router, rt_string method, rt_string path);

/// @brief Get number of registered routes.
/// @param router Router object; @c NULL reports zero.
/// @return Current route count from one synchronized snapshot.
int64_t rt_http_router_count(void *router);

/// @brief Get a parameter value from a route match.
/// @param match RouteMatch object; @c NULL behaves as an empty match.
/// @param name Capture name to look up.
/// @return Owned captured string, or an owned empty string when absent.
rt_string rt_route_match_param(void *match, rt_string name);

/// @brief Get the route index from a match.
/// @param match RouteMatch object; @c NULL reports @c -1.
/// @return Zero-based registration index of the selected route.
int64_t rt_route_match_index(void *match);

/// @brief Get the matched path pattern.
/// @param match RouteMatch object; @c NULL behaves as an empty match.
/// @return Owned copy of the registered pattern, or an owned empty string.
rt_string rt_route_match_pattern(void *match);

#ifdef __cplusplus
}
#endif
