//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_http_router.c
// Purpose: HTTP URL pattern matching with parameter extraction.
// Key invariants:
//   - Routes matched in registration order (first match wins).
//   - :name captures a single path segment; *name captures the rest.
//   - Internally synchronized: Add takes a write lock, Match/Count take a
//     read lock, so concurrent registration and matching are safe.
// Ownership/Lifetime:
//   - Router and match objects are GC-managed.
// Links: rt_http_router.h (API), rt_http_server.c (consumer)
//
//===----------------------------------------------------------------------===//

#include "rt_http_router.h"

#include "rt_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_string.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "rt_trap.h"

//=============================================================================
// Internal Structures
//=============================================================================

#define INITIAL_ROUTE_CAPACITY 16
#define INITIAL_ROUTE_SEGMENT_CAPACITY 8

typedef enum {
    SEG_LITERAL, // Exact match: "users"
    SEG_PARAM,   // Parameter capture: ":id"
    SEG_WILDCARD // Wildcard capture: "*path"
} segment_type_t;

typedef struct {
    segment_type_t type;
    char *value; // Literal text, or param name (without : or *)
    size_t value_len;
} segment_t;

typedef struct {
    char *method; // "GET", "POST", etc.
    size_t method_len;
    char *pattern; // Original pattern string
    size_t pattern_len;
    segment_t *segments;
    int segment_count;
    int capture_count;
} route_t;

typedef struct {
    route_t **routes;
    int route_count;
    int route_capacity;
    void *rw_lock; ///< Platform rwlock (SRWLOCK / pthread_rwlock_t)
} rt_http_router_impl;

typedef struct {
    int route_index;
    char *pattern;
    size_t pattern_len;
    void *params; // Map of param name -> value
} rt_route_match_impl;

/// @brief Finalize a managed RouteMatch payload.
/// @param obj RouteMatch payload being finalized.
static void rt_route_match_finalize(void *obj);

void rt_trap_set_recovery(jmp_buf *buf);
void rt_trap_clear_recovery(void);
const char *rt_trap_get_error(void);

/// @brief Validate and expose the complete byte span of a runtime string.
/// @details Rejects null/wrong-kind handles, negative or host-unrepresentable
///          lengths, and embedded NUL bytes. The returned bytes remain borrowed
///          from @p value and are valid only while the caller retains it.
/// @param value Runtime string to inspect.
/// @param allow_empty Whether a zero-byte value is accepted.
/// @param data_out Receives the borrowed NUL-terminated byte view on success.
/// @param len_out Receives the exact byte length on success.
/// @return 1 for a valid complete C-string view; 0 otherwise.
static int router_string_view(rt_string value,
                              int allow_empty,
                              const char **data_out,
                              size_t *len_out) {
    if (data_out)
        *data_out = NULL;
    if (len_out)
        *len_out = 0;
    if (!value || !rt_string_is_handle((const void *)value))
        return 0;

    int64_t len64 = rt_str_len(value);
    if (len64 < 0 || (uint64_t)len64 > (uint64_t)SIZE_MAX || (!allow_empty && len64 == 0))
        return 0;
    const char *data = rt_string_cstr(value);
    if (!data || (len64 > 0 && memchr(data, '\0', (size_t)len64) != NULL))
        return 0;
    if (data_out)
        *data_out = data;
    if (len_out)
        *len_out = (size_t)len64;
    return 1;
}

/// @brief Copy an exact byte span into a newly allocated C string.
/// @details The size computation is overflow-checked and the result is always
///          NUL-terminated. Embedded NUL validation is the caller's concern.
/// @param data Source bytes; may be NULL only when @p len is zero.
/// @param len Number of source bytes.
/// @return Malloc-owned string, or NULL on invalid input, overflow, or OOM.
static char *router_strdup_n(const char *data, size_t len) {
    if ((!data && len != 0) || len == SIZE_MAX)
        return NULL;
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    if (len > 0)
        memcpy(copy, data, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Release an owned managed object and run its finalizer at refcount zero.
/// @param obj Owned managed reference; NULL is a no-op.
static void router_release_object(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Preserve a recovered trap message across cleanup and re-raising.
/// @param buffer Caller-owned output buffer.
/// @param buffer_size Capacity including the terminator.
/// @param fallback Text used when the trap dispatcher has no message.
static void router_save_trap_error(char *buffer, size_t buffer_size, const char *fallback) {
    const char *error = rt_trap_get_error();
    snprintf(buffer, buffer_size, "%s", error && error[0] ? error : fallback);
}

/// @brief Return whether one byte is permitted in an HTTP method token.
/// @details Implements the RFC token alphabet directly in ASCII so validation
///          is locale-independent and custom extension methods remain valid.
/// @param ch Candidate unsigned byte.
/// @return 1 for an ASCII token byte; 0 otherwise.
static int router_is_method_token_byte(unsigned char ch) {
    if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
        return 1;
    return strchr("!#$%&'*+-.^_`|~", (int)ch) != NULL;
}

/// @brief Validate a complete, non-empty HTTP method token.
/// @param method Exact method bytes.
/// @param method_len Number of bytes in @p method.
/// @return 1 when every byte belongs to the HTTP token alphabet.
static int router_method_is_valid(const char *method, size_t method_len) {
    if (!method || method_len == 0)
        return 0;
    for (size_t i = 0; i < method_len; i++) {
        if (!router_is_method_token_byte((unsigned char)method[i]))
            return 0;
    }
    return 1;
}

/// @brief Validate and cast a managed router handle.
/// @param obj Candidate opaque handle.
/// @param context Diagnostic raised when validation fails.
/// @return Router payload, or NULL after trapping.
static rt_http_router_impl *router_require(void *obj, const char *context) {
    if (!rt_obj_is_instance(obj, RT_HTTP_ROUTER_CLASS_ID, sizeof(rt_http_router_impl))) {
        rt_trap(context ? context : "HttpRouter: invalid router");
        return NULL;
    }
    return (rt_http_router_impl *)obj;
}

/// @brief Validate and cast a managed RouteMatch handle.
/// @param obj Candidate opaque handle.
/// @param context Diagnostic raised when validation fails.
/// @return RouteMatch payload, or NULL after trapping.
static rt_route_match_impl *route_match_require(void *obj, const char *context) {
    if (!rt_obj_is_instance(obj, RT_ROUTE_MATCH_CLASS_ID, sizeof(rt_route_match_impl))) {
        rt_trap(context ? context : "RouteMatch: invalid match");
        return NULL;
    }
    return (rt_route_match_impl *)obj;
}

//=============================================================================
// Synchronization
//=============================================================================
// Adds take the write lock; Match/Count take the read lock, so concurrent
// matching stays parallel while registration is exclusive (VDOC-143). Traps
// are always raised OUTSIDE the locked region so a longjmp-based recovery
// hook cannot leak a held lock. Same platform idiom as rt_type_registry.c.

/// @brief Allocate and initialize the router's reader-writer lock.
/// @details A router is never allowed to fall back to unsynchronized operation.
///          Native allocation or pthread initialization failure leaves
///          @c rw_lock NULL and is reported to the constructor.
/// @param router Fresh zeroed router payload.
/// @return 1 when a usable lock was installed; 0 on failure.
static int router_lock_init(rt_http_router_impl *router) {
    if (!router)
        return 0;
#if RT_PLATFORM_WINDOWS
    SRWLOCK *lock = (SRWLOCK *)malloc(sizeof(SRWLOCK));
    if (!lock)
        return 0;
    InitializeSRWLock(lock);
    router->rw_lock = lock;
#else
    pthread_rwlock_t *lock = (pthread_rwlock_t *)malloc(sizeof(pthread_rwlock_t));
    if (!lock)
        return 0;
    if (pthread_rwlock_init(lock, NULL) != 0) {
        free(lock);
        return 0;
    }
    router->rw_lock = lock;
#endif
    return 1;
}

/// @brief Destroy and free the router's lock (no contention at finalize time).
/// @param router Router whose initialized lock is released.
static void router_lock_destroy(rt_http_router_impl *router) {
    if (!router->rw_lock)
        return;
#if !RT_PLATFORM_WINDOWS
    pthread_rwlock_destroy((pthread_rwlock_t *)router->rw_lock);
#endif
    free(router->rw_lock);
    router->rw_lock = NULL;
}

/// @brief Acquire the shared (read) lock on a successfully constructed router.
/// @param router Router whose route table will be read.
static void router_rdlock(rt_http_router_impl *router) {
#if RT_PLATFORM_WINDOWS
    AcquireSRWLockShared((SRWLOCK *)router->rw_lock);
#else
    pthread_rwlock_rdlock((pthread_rwlock_t *)router->rw_lock);
#endif
}

/// @brief Release the shared (read) lock on a successfully constructed router.
/// @param router Router whose shared lock is held by the caller.
static void router_rdunlock(rt_http_router_impl *router) {
#if RT_PLATFORM_WINDOWS
    ReleaseSRWLockShared((SRWLOCK *)router->rw_lock);
#else
    pthread_rwlock_unlock((pthread_rwlock_t *)router->rw_lock);
#endif
}

/// @brief Acquire the exclusive (write) lock on a successfully constructed router.
/// @param router Router whose route table will be modified.
static void router_wrlock(rt_http_router_impl *router) {
#if RT_PLATFORM_WINDOWS
    AcquireSRWLockExclusive((SRWLOCK *)router->rw_lock);
#else
    pthread_rwlock_wrlock((pthread_rwlock_t *)router->rw_lock);
#endif
}

/// @brief Release the exclusive (write) lock on a successfully constructed router.
/// @param router Router whose exclusive lock is held by the caller.
static void router_wrunlock(rt_http_router_impl *router) {
#if RT_PLATFORM_WINDOWS
    ReleaseSRWLockExclusive((SRWLOCK *)router->rw_lock);
#else
    pthread_rwlock_unlock((pthread_rwlock_t *)router->rw_lock);
#endif
}

//=============================================================================
// Parsing
//=============================================================================

/// @brief Ensure a route segment array can hold at least @p needed entries.
/// @details Route patterns are user-facing input, so this helper grows parsed
///          segment storage geometrically with overflow checks instead of
///          imposing an arbitrary fixed segment ceiling. Existing segment
///          contents remain valid if allocation fails.
/// @param segments In/out segment array pointer.
/// @param capacity In/out segment capacity in entries.
/// @param needed Minimum required entry count.
/// @return 1 when capacity is available; 0 on overflow or OOM.
static int reserve_route_segments(segment_t **segments, int *capacity, int needed) {
    if (!segments || !capacity || needed < 0)
        return 0;
    if (needed <= *capacity)
        return 1;

    int new_capacity = *capacity > 0 ? *capacity : INITIAL_ROUTE_SEGMENT_CAPACITY;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((uint64_t)new_capacity > (uint64_t)SIZE_MAX / sizeof(**segments))
        return 0;

    segment_t *grown = (segment_t *)realloc(*segments, (size_t)new_capacity * sizeof(**segments));
    if (!grown)
        return 0;
    memset(grown + *capacity, 0, (size_t)(new_capacity - *capacity) * sizeof(*grown));
    *segments = grown;
    *capacity = new_capacity;
    return 1;
}

/// @brief Ensure the router can hold at least @p needed registered routes.
/// @details Keeps route registration append-only and first-match order stable
///          while removing the previous fixed 256-route table. Reallocation
///          failure leaves the router unchanged so callers can trap cleanly.
/// @param router Router implementation object.
/// @param needed Minimum required route count.
/// @return 1 when capacity is available; 0 on overflow or OOM.
static int reserve_routes(rt_http_router_impl *router, int needed) {
    if (!router || needed < 0)
        return 0;
    if (needed <= router->route_capacity)
        return 1;

    int new_capacity = router->route_capacity > 0 ? router->route_capacity : INITIAL_ROUTE_CAPACITY;
    while (new_capacity < needed) {
        if (new_capacity > INT32_MAX / 2)
            return 0;
        new_capacity *= 2;
    }
    if ((uint64_t)new_capacity > (uint64_t)SIZE_MAX / sizeof(*router->routes))
        return 0;

    route_t **grown = (route_t **)realloc(router->routes, (size_t)new_capacity * sizeof(*grown));
    if (!grown)
        return 0;
    memset(grown + router->route_capacity,
           0,
           (size_t)(new_capacity - router->route_capacity) * sizeof(*grown));
    router->routes = grown;
    router->route_capacity = new_capacity;
    return 1;
}

/// @brief Parse a URL pattern into typed segments for route matching.
/// @details Splits `pattern` at each `/` and classifies each non-empty
///          segment into one of three types:
///          - **`SEG_LITERAL`**: the segment matches its exact text. E.g.
///            `users` in `/users/:id`.
///          - **`SEG_PARAM`**: prefix `:` marks a single-segment capture.
///            E.g. `:id` in `/users/:id` matches one segment and binds it
///            to a route parameter named `id`. The captured segment cannot
///            be empty and cannot span `/` boundaries.
///          - **`SEG_WILDCARD`**: prefix `*` marks a multi-segment capture
///            that consumes the remainder of the path. E.g. `*path` in
///            `/static/*path` matches `assets/img/foo.png` and binds the
///            full remainder to `path`. A wildcard must be the final
///            segment; `add_route` rejects patterns with segments after a
///            wildcard, and duplicate capture names, at registration time.
///
///          A leading `/` is skipped before parsing. Empty segments
///          (consecutive slashes like `/users//profile`) are deliberately
///          normalized away in PATTERNS only — request paths are matched
///          segment-exactly aside from leading/trailing slashes.
///          Segment storage is allocated and grown on demand; on failure all
///          partially parsed segment values are freed and no truncated route is
///          registered.
/// @param pattern Source URL pattern bytes (with or without leading `/`).
/// @param pattern_len Exact byte count in @p pattern.
/// @param segments_out Output segment array; caller owns and frees via `free_route`.
/// @return Number of segments parsed (0 if pattern is empty), or -1 on invalid input/OOM.
static int parse_pattern(const char *pattern, size_t pattern_len, segment_t **segments_out) {
    if ((!pattern && pattern_len != 0) || !segments_out)
        return -1;
    *segments_out = NULL;
    if (pattern_len == 0)
        return 0;

    const char *p = pattern;
    size_t remaining = pattern_len;
    if (remaining > 0 && *p == '/') {
        p++;
        remaining--;
    }

    int count = 0;
    int capacity = 0;
    segment_t *segments = NULL;
    while (remaining > 0) {
        const char *end = (const char *)memchr(p, '/', remaining);
        size_t len = end ? (size_t)(end - p) : remaining;

        if (len == 0) {
            p++;
            remaining--;
            continue;
        }

        if (!reserve_route_segments(&segments, &capacity, count + 1))
            goto fail;

        segment_t *segment = &segments[count];
        memset(segment, 0, sizeof(*segment));
        if (*p == ':' && len > 1) {
            segment->type = SEG_PARAM;
            segment->value = (char *)malloc(len);
            if (!segment->value)
                goto fail;
            memcpy(segment->value, p + 1, len - 1);
            segment->value[len - 1] = '\0';
            segment->value_len = len - 1;
        } else if (*p == '*' && len > 1) {
            segment->type = SEG_WILDCARD;
            segment->value = (char *)malloc(len);
            if (!segment->value)
                goto fail;
            memcpy(segment->value, p + 1, len - 1);
            segment->value[len - 1] = '\0';
            segment->value_len = len - 1;
        } else {
            segment->type = SEG_LITERAL;
            if (len == SIZE_MAX)
                goto fail;
            segment->value = (char *)malloc(len + 1);
            if (!segment->value)
                goto fail;
            memcpy(segment->value, p, len);
            segment->value[len] = '\0';
            segment->value_len = len;
        }

        count++;
        if (!end) {
            remaining = 0;
        } else {
            size_t consumed = len + 1;
            p += consumed;
            remaining -= consumed;
        }
    }

    *segments_out = segments;
    return count;

fail:
    for (int i = 0; i <= count && i < capacity; i++) {
        free(segments[i].value);
        segments[i].value = NULL;
    }
    free(segments);
    return -1;
}

//=============================================================================
// Matching
//=============================================================================

/// @brief Test a path against one parsed route without allocating or trapping.
/// @details This is the only operation performed while the router's shared
///          lock is held. It uses bounded byte spans, cached segment lengths,
///          and exact literal comparison. A terminal wildcard accepts the
///          remaining bytes; parameter segments require one non-empty segment.
/// @param route Immutable, fully published route.
/// @param path Exact path bytes; may be empty to represent the root.
/// @param path_len Number of bytes in @p path.
/// @return 1 for a complete match; 0 otherwise.
static int route_path_matches(const route_t *route, const char *path, size_t path_len) {
    static const char root_path[] = "/";
    if (!route || (!path && path_len != 0))
        return 0;
    if (path_len == 0) {
        path = root_path;
        path_len = 1;
    }

    const char *p = path;
    size_t remaining = path_len;
    if (remaining > 0 && *p == '/') {
        p++;
        remaining--;
    }

    for (int seg_idx = 0; seg_idx < route->segment_count; seg_idx++) {
        const segment_t *seg = &route->segments[seg_idx];
        if (!seg->value)
            return 0;
        if (seg->type == SEG_WILDCARD)
            return 1;

        const char *seg_end = (const char *)memchr(p, '/', remaining);
        size_t seg_len = seg_end ? (size_t)(seg_end - p) : remaining;
        if (seg_len == 0)
            return 0;
        if (seg->type == SEG_LITERAL &&
            (seg->value_len != seg_len || memcmp(seg->value, p, seg_len) != 0))
            return 0;

        if (!seg_end) {
            p += remaining;
            remaining = 0;
        } else {
            size_t consumed = seg_len + 1;
            p += consumed;
            remaining -= consumed;
        }
    }

    for (size_t i = 0; i < remaining; i++) {
        if (p[i] != '/')
            return 0;
    }
    return 1;
}

/// @brief Construct one owned RouteMatch after an allocation-free route search.
/// @details Parameter strings, their Map, and the result object are protected by
///          one local trap recovery boundary. Any managed allocation or Map
///          insertion trap releases every partially owned value before the
///          original diagnostic is re-raised. Literal-only routes avoid
///          allocating an otherwise-empty parameter Map.
/// @param route Stable immutable route selected under the router lock.
/// @param route_index Zero-based registration index of @p route.
/// @param path Exact validated path bytes.
/// @param path_len Number of bytes in @p path.
/// @return Owned RouteMatch, or NULL after cleanup and trap propagation.
static void *router_build_match(const route_t *route,
                                int route_index,
                                const char *path,
                                size_t path_len) {
    static const char root_path[] = "/";
    void *volatile owned_params = NULL;
    rt_string volatile owned_name = NULL;
    rt_string volatile owned_value = NULL;
    rt_route_match_impl *volatile owned_match = NULL;

    jmp_buf recovery;
    rt_trap_set_recovery(&recovery);
    if (setjmp(recovery) != 0) {
        char saved_error[256];
        router_save_trap_error(
            saved_error, sizeof(saved_error), "HttpRouter.Match: allocation failed");
        rt_trap_clear_recovery();
        rt_string_unref((rt_string)owned_name);
        rt_string_unref((rt_string)owned_value);
        router_release_object((void *)owned_match);
        router_release_object((void *)owned_params);
        rt_trap(saved_error);
        return NULL;
    }

    if (route->capture_count > 0) {
        owned_params = rt_map_new();
        if (!owned_params) {
            rt_trap_clear_recovery();
            return NULL;
        }
    }

    if (path_len == 0) {
        path = root_path;
        path_len = 1;
    }
    const char *p = path;
    size_t remaining = path_len;
    if (remaining > 0 && *p == '/') {
        p++;
        remaining--;
    }

    for (int seg_idx = 0; seg_idx < route->segment_count; seg_idx++) {
        const segment_t *seg = &route->segments[seg_idx];
        if (seg->type == SEG_WILDCARD) {
            while (remaining > 0 && p[remaining - 1] == '/')
                remaining--;
            owned_name = rt_string_from_bytes(seg->value, seg->value_len);
            owned_value = rt_string_from_bytes(p, remaining);
            if (!owned_name || !owned_value) {
                rt_trap("HttpRouter.Match: parameter allocation failed");
                break;
            }
            rt_map_set((void *)owned_params, (rt_string)owned_name, (void *)owned_value);
            rt_string_unref((rt_string)owned_name);
            rt_string_unref((rt_string)owned_value);
            owned_name = NULL;
            owned_value = NULL;
            break;
        }

        const char *seg_end = (const char *)memchr(p, '/', remaining);
        size_t seg_len = seg_end ? (size_t)(seg_end - p) : remaining;
        if (seg->type == SEG_PARAM) {
            owned_name = rt_string_from_bytes(seg->value, seg->value_len);
            owned_value = rt_string_from_bytes(p, seg_len);
            if (!owned_name || !owned_value) {
                rt_trap("HttpRouter.Match: parameter allocation failed");
                break;
            }
            rt_map_set((void *)owned_params, (rt_string)owned_name, (void *)owned_value);
            rt_string_unref((rt_string)owned_name);
            rt_string_unref((rt_string)owned_value);
            owned_name = NULL;
            owned_value = NULL;
        }

        if (!seg_end) {
            p += remaining;
            remaining = 0;
        } else {
            size_t consumed = seg_len + 1;
            p += consumed;
            remaining -= consumed;
        }
    }

    owned_match = (rt_route_match_impl *)rt_obj_new_i64(RT_ROUTE_MATCH_CLASS_ID,
                                                        (int64_t)sizeof(rt_route_match_impl));
    if (!owned_match) {
        rt_trap_clear_recovery();
        router_release_object((void *)owned_params);
        return NULL;
    }
    memset((void *)owned_match, 0, sizeof(rt_route_match_impl));
    rt_obj_set_finalizer((void *)owned_match, rt_route_match_finalize);
    owned_match->route_index = route_index;
    owned_match->pattern = router_strdup_n(route->pattern, route->pattern_len);
    if (!owned_match->pattern) {
        rt_trap("HttpRouter.Match: pattern allocation failed");
        rt_trap_clear_recovery();
        router_release_object((void *)owned_match);
        router_release_object((void *)owned_params);
        return NULL;
    }
    owned_match->pattern_len = route->pattern_len;
    owned_match->params = (void *)owned_params;
    owned_params = NULL;

    void *result = (void *)owned_match;
    owned_match = NULL;
    rt_trap_clear_recovery();
    return result;
}

//=============================================================================
// Finalizers
//=============================================================================

/// @brief Free a route's heap-owned strings and parsed segment array.
/// @details The route node itself remains allocated so this helper is usable
///          before both successful and failed publication.
/// @param route Route whose owned fields are released; NULL is a no-op.
static void free_route(route_t *route) {
    if (!route)
        return;
    free(route->method);
    free(route->pattern);
    for (int i = 0; i < route->segment_count; i++)
        free(route->segments[i].value);
    free(route->segments);
    memset(route, 0, sizeof(*route));
}

/// @brief Release a detached or published route node completely.
/// @param route Malloc-owned route node; NULL is a no-op.
static void destroy_route(route_t *route) {
    if (!route)
        return;
    free_route(route);
    free(route);
}

/// @brief GC finalizer for the router: free every parsed route's heap allocations.
/// @param obj Router payload being finalized; may be null.
static void rt_http_router_finalize(void *obj) {
    if (!obj)
        return;
    rt_http_router_impl *router = (rt_http_router_impl *)obj;
    for (int i = 0; i < router->route_count; i++) {
        destroy_route(router->routes[i]);
        router->routes[i] = NULL;
    }
    free(router->routes);
    router->routes = NULL;
    router->route_count = 0;
    router_lock_destroy(router);
    router->route_capacity = 0;
}

/// @brief GC finalizer for a route-match: free the pattern string and release the params Map.
/// @param obj RouteMatch payload being finalized; may be null.
static void rt_route_match_finalize(void *obj) {
    if (!obj)
        return;
    rt_route_match_impl *match = (rt_route_match_impl *)obj;
    free(match->pattern);
    match->pattern = NULL;
    match->pattern_len = 0;
    void *params = match->params;
    match->params = NULL;
    router_release_object(params);
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Construct an empty HTTP router with growable route storage.
/// @details Routes are matched in registration order, so register more-specific
///          patterns first. The route and per-route segment arrays grow as
///          routes are added and are released by the GC finalizer.
/// @return Owned managed router, or null after allocation or synchronization initialization
///         failure.
void *rt_http_router_new(void) {
    rt_http_router_impl *router = (rt_http_router_impl *)rt_obj_new_i64(
        RT_HTTP_ROUTER_CLASS_ID, (int64_t)sizeof(rt_http_router_impl));
    if (!router)
        return NULL;
    memset(router, 0, sizeof(*router));
    if (!router_lock_init(router)) {
        router_release_object(router);
        rt_trap("HttpRouter.New: synchronization allocation failed");
        return NULL;
    }
    rt_obj_set_finalizer(router, rt_http_router_finalize);
    return router;
}

/// @brief Build, validate, and atomically publish one exact route definition.
/// @details All fallible native allocation and grammar validation happens on a
///          detached node. The write lock protects only capacity reservation
///          and pointer publication, and no trap is raised until after unlock.
///          Separately allocated nodes keep pointers selected by concurrent
///          readers stable even when the router's pointer table grows.
/// @param router Valid router payload.
/// @param method Complete validated method bytes.
/// @param method_len Exact byte count in @p method.
/// @param pattern Complete validated pattern bytes.
/// @param pattern_len Exact byte count in @p pattern.
/// @return @p router for fluent chaining, including after a returning trap hook.
static void *add_route(rt_http_router_impl *router,
                       const char *method,
                       size_t method_len,
                       const char *pattern,
                       size_t pattern_len) {
    if (!router || !router_method_is_valid(method, method_len) || (!pattern && pattern_len != 0)) {
        rt_trap("HttpRouter.Add: invalid HTTP method or pattern");
        return router;
    }

    route_t *route = (route_t *)calloc(1, sizeof(*route));
    if (!route) {
        rt_trap("HttpRouter: memory allocation failed");
        return router;
    }
    route->method = router_strdup_n(method, method_len);
    route->method_len = method_len;
    route->pattern = router_strdup_n(pattern, pattern_len);
    route->pattern_len = pattern_len;
    if (!route->method || !route->pattern) {
        destroy_route(route);
        rt_trap("HttpRouter: memory allocation failed");
        return router;
    }
    route->segment_count = parse_pattern(pattern, pattern_len, &route->segments);
    if (route->segment_count < 0) {
        destroy_route(route);
        rt_trap("HttpRouter: invalid route pattern");
        return router;
    }

    // Registration-time grammar validation: a wildcard consumes the rest of
    // the path, so suffix segments after it could never constrain a match —
    // reject them instead of silently ignoring them. Duplicate capture names
    // would overwrite each other in the params Map, so reject those too.
    for (int i = 0; i < route->segment_count; i++) {
        const segment_t *seg = &route->segments[i];
        if (seg->type == SEG_LITERAL && seg->value_len == 1 &&
            (seg->value[0] == ':' || seg->value[0] == '*')) {
            destroy_route(route);
            rt_trap("HttpRouter: capture marker requires a name");
            return router;
        }
        if (seg->type == SEG_WILDCARD && i != route->segment_count - 1) {
            destroy_route(route);
            rt_trap("HttpRouter: wildcard segment must be terminal");
            return router;
        }
        if (seg->type == SEG_PARAM || seg->type == SEG_WILDCARD) {
            route->capture_count++;
            for (int j = 0; j < i; j++) {
                const segment_t *prev = &route->segments[j];
                if ((prev->type == SEG_PARAM || prev->type == SEG_WILDCARD) &&
                    prev->value_len == seg->value_len &&
                    memcmp(prev->value, seg->value, seg->value_len) == 0) {
                    destroy_route(route);
                    rt_trap("HttpRouter: duplicate capture name in pattern");
                    return router;
                }
            }
        }
    }

    // Publish under the write lock so concurrent Match/Count never observe a
    // reallocated routes array or a partially initialized entry.
    int publish_failed = 0;
    router_wrlock(router);
    if (router->route_count == INT32_MAX || !reserve_routes(router, router->route_count + 1)) {
        publish_failed = 1;
    } else {
        router->routes[router->route_count] = route;
        router->route_count++;
    }
    router_wrunlock(router);
    if (publish_failed) {
        destroy_route(route);
        rt_trap("HttpRouter: route storage allocation failed");
        return router;
    }
    return router;
}

/// @brief Register a route for an arbitrary HTTP method (e.g. PATCH, OPTIONS, custom verbs).
/// @details Returns the router for fluent chaining:
///          `router.add("GET", "/x").add("POST", "/y")`. Complete runtime
///          strings and the method token are validated before detached parsing
///          and atomic publication.
/// @param router Managed router receiver.
/// @param method Nonempty, case-sensitive HTTP method token.
/// @param pattern Route pattern to parse.
/// @return The original router on success or a returning registration trap; null for an invalid
///         receiver.
void *rt_http_router_add(void *router, rt_string method, rt_string pattern) {
    rt_http_router_impl *impl = router_require(router, "HttpRouter.Add: invalid router");
    if (!impl)
        return NULL;
    const char *method_bytes = NULL;
    const char *pattern_bytes = NULL;
    size_t method_len = 0;
    size_t pattern_len = 0;
    if (!router_string_view(method, 1, &method_bytes, &method_len) ||
        !router_string_view(pattern, 1, &pattern_bytes, &pattern_len)) {
        rt_trap("HttpRouter.Add: invalid method or pattern string");
        return router;
    }
    if (!router_method_is_valid(method_bytes, method_len)) {
        rt_trap("HttpRouter.Add: invalid HTTP method token");
        return router;
    }
    return add_route(impl, method_bytes, method_len, pattern_bytes, pattern_len);
}

/// @brief Register a route using one trusted convenience-method literal.
/// @details Validates the receiver and complete runtime pattern before using
///          the same transactional publication path as arbitrary methods.
/// @param router Candidate managed router.
/// @param method Static non-empty method token.
/// @param method_len Byte length of @p method.
/// @param pattern Runtime route pattern.
/// @param context Entry-point-specific invalid-router diagnostic.
/// @return Router for fluent chaining, or NULL for an invalid receiver.
static void *add_convenience_route(
    void *router, const char *method, size_t method_len, rt_string pattern, const char *context) {
    rt_http_router_impl *impl = router_require(router, context);
    if (!impl)
        return NULL;
    const char *pattern_bytes = NULL;
    size_t pattern_len = 0;
    if (!router_string_view(pattern, 1, &pattern_bytes, &pattern_len)) {
        rt_trap("HttpRouter.Add: invalid pattern string");
        return router;
    }
    return add_route(impl, method, method_len, pattern_bytes, pattern_len);
}

/// @brief Convenience: register a GET route. Equivalent to `_add(router, "GET", pattern)`.
/// @param router Managed router receiver.
/// @param pattern Route pattern to parse.
/// @return The original router for fluent chaining, or null for an invalid receiver.
void *rt_http_router_get(void *router, rt_string pattern) {
    return add_convenience_route(
        router, "GET", sizeof("GET") - 1, pattern, "HttpRouter.Get: invalid router");
}

/// @brief Convenience: register a POST route.
/// @param router Managed router receiver.
/// @param pattern Route pattern to parse.
/// @return The original router for fluent chaining, or null for an invalid receiver.
void *rt_http_router_post(void *router, rt_string pattern) {
    return add_convenience_route(
        router, "POST", sizeof("POST") - 1, pattern, "HttpRouter.Post: invalid router");
}

/// @brief Convenience: register a PUT route.
/// @param router Managed router receiver.
/// @param pattern Route pattern to parse.
/// @return The original router for fluent chaining, or null for an invalid receiver.
void *rt_http_router_put(void *router, rt_string pattern) {
    return add_convenience_route(
        router, "PUT", sizeof("PUT") - 1, pattern, "HttpRouter.Put: invalid router");
}

/// @brief Convenience: register a DELETE route.
/// @param router Managed router receiver.
/// @param pattern Route pattern to parse.
/// @return The original router for fluent chaining, or null for an invalid receiver.
void *rt_http_router_delete(void *router, rt_string pattern) {
    return add_convenience_route(
        router, "DELETE", sizeof("DELETE") - 1, pattern, "HttpRouter.Delete: invalid router");
}

/// @brief Find the first registered route that matches `(method, path)` and return a Match
/// object with extracted params. Method comparison is **case-sensitive**, matching HTTP's
/// method-token semantics (RFC 9110 §9.1) — register methods in the exact case requests use
/// (the convenience helpers register the canonical uppercase forms).
/// Walks routes in registration order — earlier wins on ties. Returns NULL if no route matches
/// (the server can then return 404). The returned Match object is GC-managed; caller releases.
/// @param obj Managed router receiver; null produces no match.
/// @param method Nonempty HTTP method token.
/// @param path Request path to match.
/// @return Owned RouteMatch for the first matching route, or null for invalid input, no match, or
///         construction failure.
void *rt_http_router_match(void *obj, rt_string method, rt_string path) {
    if (!obj)
        return NULL;

    rt_http_router_impl *router = router_require(obj, "HttpRouter.Match: invalid router");
    if (!router)
        return NULL;
    const char *method_bytes = NULL;
    const char *path_bytes = NULL;
    size_t method_len = 0;
    size_t path_len = 0;
    if (!router_string_view(method, 0, &method_bytes, &method_len) ||
        !router_string_view(path, 1, &path_bytes, &path_len) ||
        !router_method_is_valid(method_bytes, method_len))
        return NULL;

    route_t *matched_route = NULL;
    int matched_index = -1;
    router_rdlock(router);
    for (int i = 0; i < router->route_count; i++) {
        route_t *route = router->routes[i];
        if (!route)
            continue;

        // Check method match (case-sensitive per HTTP method-token semantics)
        if (route->method_len != method_len || memcmp(route->method, method_bytes, method_len) != 0)
            continue;

        if (route_path_matches(route, path_bytes, path_len)) {
            matched_route = route;
            matched_index = i;
            break;
        }
    }
    router_rdunlock(router);

    if (!matched_route)
        return NULL;
    return router_build_match(matched_route, matched_index, path_bytes, path_len);
}

/// @brief Number of routes currently registered (for diagnostics / capacity checks).
/// @param obj Managed router receiver; null reports zero.
/// @return Route count from a shared-lock snapshot.
int64_t rt_http_router_count(void *obj) {
    if (!obj)
        return 0;
    rt_http_router_impl *router = router_require(obj, "HttpRouter.Count: invalid router");
    if (!router)
        return 0;
    router_rdlock(router);
    int64_t count = router->route_count;
    router_rdunlock(router);
    return count;
}

/// @brief Look up a captured parameter from a Match object (e.g. for `/users/:id` with input
/// `/users/42`, `_param("id")` returns "42"). Returns empty string if the param wasn't captured.
/// @param obj Managed RouteMatch receiver; null behaves as an empty match.
/// @param name Case-sensitive capture name.
/// @return Owned reference to the captured String, or a newly allocated empty String when absent.
rt_string rt_route_match_param(void *obj, rt_string name) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_route_match_impl *match = route_match_require(obj, "RouteMatch.Param: invalid match");
    if (!match)
        return NULL;
    if (!match->params)
        return rt_string_from_bytes("", 0);

    void *val = rt_map_get(match->params, name);
    if (!val)
        return rt_string_from_bytes("", 0);
    return rt_string_ref((rt_string)val);
}

/// @brief Index of the route that matched (registration order). -1 for null Match. Useful for
/// dispatching to a parallel handler array indexed by the same numbers.
/// @param obj Managed RouteMatch receiver; null reports -1.
/// @return Zero-based registration index, or -1 for null or invalid input.
int64_t rt_route_match_index(void *obj) {
    if (!obj)
        return -1;
    rt_route_match_impl *match = route_match_require(obj, "RouteMatch.Index: invalid match");
    return match ? match->route_index : -1;
}

/// @brief Read the original pattern string of the matched route (for logging / debug output).
/// @param obj Managed RouteMatch receiver; null behaves as an empty match.
/// @return Owned copy of the registered pattern, an empty String for no pattern, or null after an
///         invalid-handle trap.
rt_string rt_route_match_pattern(void *obj) {
    if (!obj)
        return rt_string_from_bytes("", 0);
    rt_route_match_impl *match = route_match_require(obj, "RouteMatch.Pattern: invalid match");
    if (!match)
        return NULL;
    if (!match->pattern)
        return rt_string_from_bytes("", 0);
    return rt_string_from_bytes(match->pattern, match->pattern_len);
}
