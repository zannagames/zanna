//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_internal.h
// Purpose: Core helpers shared by the Zanna.Services front ends: the platform
//          core (rt_services.c) and the feature classes (progress, social,
//          cloud). Providers use rt_services_provider.h instead.
// Key invariants:
//   - Every helper runs on the main thread; callers perform the main-thread
//     check first so a trap is reported under the public member's name.
//   - Argument traps use the message form "Services.<Class>.<Member>: <detail>".
// Ownership/Lifetime:
//   - Text helpers return caller-owned runtime strings or borrowed C strings
//     whose lifetime is the argument's.
//   - Request and Result helpers return caller-owned runtime objects.
// Links: src/runtime/services/rt_services.c,
//        src/runtime/services/rt_services_provider.h,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_internal.h
 * @brief Declares helpers shared by the Zanna.Services core and feature classes.
 */

#pragma once

#include "rt_platform.h"
#include "rt_services_provider.h"
#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Borrow the bytes of a possibly-NULL runtime string.
/// @param text Runtime string or NULL.
/// @return NUL-terminated bytes; the empty string for NULL.
const char *rt_services_internal_cstr(rt_string text);

/// @brief Create a caller-owned runtime string from C text.
/// @param text NUL-terminated text or NULL.
/// @return Owned copy, or the immortal empty string for NULL or empty input.
rt_string rt_services_internal_owned_text(const char *text);

/// @brief Normalize a provider string result.
/// @param value Owned provider result or NULL.
/// @return @p value, or the immortal empty string when NULL.
rt_string rt_services_internal_owned_or_empty(rt_string value);

/// @brief Return the active provider without a thread check.
/// @return Active provider table, or NULL when none is started.
const rt_services_provider *rt_services_internal_active_provider(void);

/// @brief Trap with an argument error for a public member.
/// @details Formats "Services.<member>: <detail>" and reports it through rt_trap.
/// @param member Class-qualified member name, for example "Stats.SetInt".
/// @param format printf-style detail format.
void rt_services_internal_trap_argument(const char *member, const char *format, ...)
    RT_PRINTF_FORMAT(2, 3);

/// @brief Require a non-empty identifier argument.
/// @details Traps with "Services.<member>: <what> must not be empty" for NULL
///          or empty input.
/// @param text Candidate identifier.
/// @param member Class-qualified member name.
/// @param what Argument description, for example "achievement id".
/// @return Borrowed identifier bytes, or NULL after reporting the trap.
const char *rt_services_internal_require_name(rt_string text, const char *member, const char *what);

/// @brief Start a request against the active provider.
/// @details Performs the main-thread check. When no provider is started, the
///          provider lacks begin_request, or the pending limit is reached, the
///          request is returned already failed.
/// @param args Validated request kind and arguments.
/// @param member Class-qualified member name for diagnostics.
/// @return Caller-owned Zanna.Services.Request, or NULL after a trap.
void *rt_services_internal_begin_request(const rt_services_request_args *args, const char *member);

/// @brief Build a caller-owned Err(string) Result.
/// @param text Error message; NULL is empty.
/// @return Caller-owned Zanna.Result.
void *rt_services_internal_err_result(const char *text);

#ifdef __cplusplus
}
#endif
