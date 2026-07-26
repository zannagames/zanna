//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_sse.h
// Purpose: Strict HTTP(S) EventSource client with lossless timed receives.
// Key invariants:
//   - HTTP heads, body framing, event lines, and payload growth are bounded.
//   - Receives serialize; timeout preserves parser state; Close cancels active I/O.
//   - Public receivers and URL Strings are validated before native state access.
// Ownership/Lifetime:
//   - Clients are GC-managed and own URL, parser, native transport, and metadata state.
// Links: rt_sse.c (implementation), rt_network.h (TCP), rt_tls.h (TLS),
//        docs/zannalib/network.md (runtime contract)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_sse.h
 * @brief Declares the managed HTTP EventSource client API.
 * @details SSE clients own their transport, incremental parser, reconnect
 *          metadata, and last-event fields. The API provides blocking and
 *          deadline-aware event receive operations, explicit Result reporting,
 *          synchronized state queries, and cancellation-safe close.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable runtime class identifier for managed `Zanna.Network.SseClient` handles.
/// @details Every non-NULL SSE receiver is checked against this tag and the
///          complete native payload size before transport or parser state is
///          accessed. The negative value is reserved for runtime-owned network
///          classes and cannot collide with compiler-assigned user classes.
#define RT_SSE_CLASS_ID INT64_C(-0x720217)

/// @brief Open a strict HTTP(S) EventSource connection.
/// @details The URL must be a live, nonempty, NUL-free runtime String. Connect
///          follows at most five redirects, consumes bounded informational
///          responses, and accepts only an HTTP 200 `text/event-stream` body
///          with unambiguous close, Content-Length, or chunked framing.
/// @param url Absolute HTTP or HTTPS URL.
/// @return Caller-owned GC-managed client, or NULL after a returning trap hook.
void *rt_sse_connect(rt_string url);
/// @brief Block until one complete event dispatch or terminal close.
/// @details Receive callers serialize. Consecutive data fields, including
///          empty fields, are joined by LF. Protocol, limit, and allocation
///          failures trap after operation cleanup.
/// @param client Valid SSE client; NULL returns the legacy empty sentinel.
/// @return Caller-owned event-data String, or empty after close.
rt_string rt_sse_recv(void *client);
/// @brief Like `_recv`, with one monotonic deadline for the complete event receive.
/// @details Partial lines and event fields survive a timeout and resume on the
///          next receive. A zero timeout retains the blocking `_recv` behavior.
/// @param client Valid SSE client; NULL returns the legacy empty sentinel.
/// @param timeout_ms Zero for blocking, otherwise 1..INT_MAX milliseconds.
/// @return Caller-owned event data, or empty after timeout/close.
rt_string rt_sse_recv_for(void *client, int64_t timeout_ms);

/// @brief Timed receive that preserves empty-event, timeout, and close distinctions.
/// @details Returns OkStr(data) on dispatch, including empty data. Timeout,
///          terminal close, and caught receive traps become ErrStr. Partial
///          managed publication is released if Result allocation traps.
/// @param client Valid non-NULL SSE client.
/// @param timeout_ms Zero for blocking, otherwise 1..INT_MAX milliseconds.
/// @return Caller-owned Result containing OkStr or ErrStr.
void *rt_sse_recv_for_result(void *client, int64_t timeout_ms);
/// @brief Query synchronized local transport/session state.
/// @details This does not probe remote liveness.
/// @param client Valid SSE client; NULL returns false.
/// @return One while locally open, otherwise zero.
int8_t rt_sse_is_open(void *client);
/// @brief Permanently cancel and close the SSE connection.
/// @details Concurrent Close marks cancellation and shuts down the descriptor
///          to wake an active receive. The receive owner performs final
///          transport release; an idle client closes immediately.
/// @param client Valid SSE client; NULL is an idempotent no-op.
void rt_sse_close(void *client);
/// @brief Get the type of the most recently dispatched event.
/// @details An event without an `event:` field reports the default empty type;
///          the previous event's type is never inherited. The returned String
///          is an independent caller-owned snapshot.
/// @param client Valid SSE client; NULL returns the legacy empty sentinel.
/// @return Caller-owned event-type String.
rt_string rt_sse_last_event_type(void *client);
/// @brief Snapshot the most recent accepted EventSource `id` field.
/// @details Safe nonempty IDs become Last-Event-ID on reconnect. The returned
///          String is independent and caller-owned.
/// @param client Valid SSE client; NULL returns the legacy empty sentinel.
/// @return Caller-owned event-ID String.
rt_string rt_sse_last_event_id(void *client);

#ifdef __cplusplus
}
#endif
