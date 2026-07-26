//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/network/rt_network.h
// Purpose: Public C ABI for managed TCP/UDP sockets, TCP listeners, DNS
//   utilities, simple HTTP requests, request/response objects, and URL parsing
//   and construction used by Zanna.Network.
//
// Key invariants:
//   - TCP connections use blocking I/O with optional timeouts.
//   - TCP_NODELAY is enabled by default to minimize latency.
//   - UDP packets are discrete messages; partial sends are not guaranteed.
//   - DNS resolution is synchronous; it may block for the DNS timeout duration.
//
// Ownership/Lifetime:
//   - Connection objects are GC-managed opaque pointers.
//   - Callers should not free connection objects directly.
//
// Links: src/runtime/network/rt_network.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the managed Zanna.Network C ABI.
 * @details Defines stable TCP, listener, and UDP identities plus synchronous
 * connection, I/O, timeout, datagram, DNS, HTTP request/response, and URL
 * services with explicit managed result ownership.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable runtime class identifier for managed `Zanna.Network.Tcp` handles.
/// @details Native entry points use this tag together with the minimum payload
///          size to reject forged, wrong-class, and undersized objects before
///          interpreting their storage as a TCP connection.
#define RT_TCP_CLASS_ID INT64_C(-0x720206)

/// @brief Stable runtime class identifier for managed `Zanna.Network.TcpServer` handles.
/// @details The listener API validates this tag at every non-null receiver
///          boundary so unrelated managed values cannot be treated as native
///          socket state.
#define RT_TCP_SERVER_CLASS_ID INT64_C(-0x720207)

/// @brief Stable runtime class identifier for managed `Zanna.Network.Udp` handles.
/// @details UDP entry points validate this tag, complete private payload size,
///          and an initialization marker before interpreting any native socket
///          state supplied through an opaque receiver.
#define RT_UDP_CLASS_ID INT64_C(-0x720208)

//=========================================================================
// Tcp Client - Connection Creation
//=========================================================================

/// @brief Connect to a remote TCP endpoint with a 30-second timeout.
/// @details Resolves IPv4 and IPv6 candidates, tries them in resolver order,
///          and returns a blocking, TCP_NODELAY-enabled managed connection.
/// @param host Nonempty hostname or numeric address without embedded NUL bytes.
/// @param port Remote port in [1, 65535].
/// @return Newly owned Tcp connection, or NULL after a returning trap hook.
/// @note Traps on invalid input, resolution failure, refusal, timeout,
///       allocation failure, or another network error.
void *rt_tcp_connect(rt_string host, int64_t port);

/// @brief Connect to a remote TCP endpoint with an explicit timeout.
/// @details Applies the timeout independently to each resolved address
///          candidate. Zero selects the platform blocking-connect behavior.
/// @param host Nonempty hostname or numeric address without embedded NUL bytes.
/// @param port Remote port in [1, 65535].
/// @param timeout_ms Per-candidate timeout in [0, @c INT_MAX] milliseconds.
/// @return Newly owned Tcp connection, or NULL after a returning trap hook.
/// @note Traps on invalid input, resolution failure, refusal, timeout,
///       allocation failure, or another network error.
void *rt_tcp_connect_for(rt_string host, int64_t port, int64_t timeout_ms);

//=========================================================================
// Tcp Client - Properties
//=========================================================================

/// @brief Copy the remote host text recorded when a connection was opened.
/// @param obj Required Tcp receiver.
/// @return Newly owned host String; endpoint metadata remains available after
///         the socket closes.
rt_string rt_tcp_host(void *obj);

/// @brief Get the remote port recorded for a connection.
/// @param obj Required Tcp receiver.
/// @return Remote port number, or zero after a returning invalid-handle trap.
int64_t rt_tcp_port(void *obj);

/// @brief Get the local port assigned to a connection.
/// @param obj Required Tcp receiver.
/// @return Captured local port, or zero when unavailable or after a returning
///         invalid-handle trap.
int64_t rt_tcp_local_port(void *obj);

/// @brief Check whether a Tcp object's native socket remains open.
/// @param obj Required Tcp receiver.
/// @return One while open, otherwise zero.
int8_t rt_tcp_is_open(void *obj);

/// @brief Take a best-effort snapshot of bytes queued for receive.
/// @details Zero is ambiguous between no queued data, a closed connection, and
///          an unsupported or failed native availability query.
/// @param obj Required Tcp receiver.
/// @return Nonnegative queued-byte count.
int64_t rt_tcp_available(void *obj);

//=========================================================================
// Tcp Client - Send Methods
//=========================================================================

/// @brief Perform one native send from a managed Bytes payload.
/// @details A normal short write is returned rather than retried, and a payload
///          larger than @c INT_MAX is limited to one @c INT_MAX-byte call.
/// @param obj Required open Tcp receiver.
/// @param data Required managed Bytes payload.
/// @return Bytes written, zero for an empty payload, or -1 after a returning
///         validation or socket-error trap.
int64_t rt_tcp_send(void *obj, void *data);

/// @brief Perform one native send from the exact bytes of a managed String.
/// @details Embedded NUL bytes are sent as data. A normal short write is
///          returned, and one native call is limited to @c INT_MAX bytes.
/// @param obj Required open Tcp receiver.
/// @param text Required managed String payload.
/// @return Bytes written, zero for an empty String, or -1 after a returning
///         validation or socket-error trap.
int64_t rt_tcp_send_str(void *obj, rt_string text);

/// @brief Send an entire managed Bytes payload.
/// @details Retries partial writes and chunks large buffers until all bytes
///          have been delivered. A failed or zero write marks the connection
///          closed and traps.
/// @param obj Required open Tcp receiver.
/// @param data Required managed Bytes payload.
void rt_tcp_send_all(void *obj, void *data);

/// @brief Send an entire borrowed raw memory range.
/// @details Internal HTTP/WebSocket helper with the same retry and connection
///          failure semantics as @ref rt_tcp_send_all.
/// @param obj Required open Tcp receiver.
/// @param data Buffer of at least @p len bytes, or NULL when @p len is zero.
/// @param len Nonnegative byte count.
void rt_tcp_send_all_raw(void *obj, const void *data, int64_t len);

//=========================================================================
// Tcp Client - Receive Methods
//=========================================================================

/// @brief Receive up to a requested number of bytes in one native read.
/// @details Returns exact-sized Bytes. Empty Bytes represents either timeout
///          or orderly peer close; only peer close clears the open state.
/// @param obj Required open Tcp receiver.
/// @param max_bytes Maximum receive size in [0, @c INT_MAX].
/// @return Newly owned Bytes, which may be empty, or NULL after a returning
///         validation, allocation, or non-timeout socket trap.
void *rt_tcp_recv(void *obj, int64_t max_bytes);

/// @brief Receive once and convert the resulting Bytes to a String.
/// @param obj Required open Tcp receiver.
/// @param max_bytes Maximum receive size in [0, @c INT_MAX].
/// @return Newly owned String; an empty String represents timeout, orderly
///         close, or a zero-byte request.
rt_string rt_tcp_recv_str(void *obj, int64_t max_bytes);

/// @brief Block until an exact number of bytes has been received.
/// @details Timeout and premature peer close are reported as errors rather than
///          successful empty or partial results.
/// @param obj Required open Tcp receiver.
/// @param count Exact receive size in [0, @c INT_MAX].
/// @return Newly owned Bytes containing exactly @p count bytes.
/// @note Traps on invalid input, timeout, premature close, allocation failure,
///       or another receive error.
void *rt_tcp_recv_exact(void *obj, int64_t count);

/// @brief Receive an LF-terminated line of at most 64 KiB.
/// @details Excludes LF and an immediately preceding CR from the returned
///          String. No partial line is returned on timeout or peer close.
/// @param obj Required open Tcp receiver.
/// @return Newly owned line String.
/// @note Traps on timeout, premature close, overlong input, allocation failure,
///       or another receive error.
rt_string rt_tcp_recv_line(void *obj);

//=========================================================================
// Tcp Client - Timeout and Close
//=========================================================================

/// @brief Set the persistent native receive timeout.
/// @param obj Required open Tcp receiver.
/// @param timeout_ms Timeout in [0, @c INT_MAX] milliseconds; zero disables it.
/// @note Traps when the receiver, range, or native socket option is invalid.
void rt_tcp_set_recv_timeout(void *obj, int64_t timeout_ms);

/// @brief Set the persistent native send timeout.
/// @param obj Required open Tcp receiver.
/// @param timeout_ms Timeout in [0, @c INT_MAX] milliseconds; zero disables it.
/// @note Traps when the receiver, range, or native socket option is invalid.
void rt_tcp_set_send_timeout(void *obj, int64_t timeout_ms);

/// @brief Close a Tcp connection idempotently.
/// @details Endpoint metadata remains readable after the native socket closes.
///          NULL is accepted as a no-op.
/// @param obj Tcp receiver, or NULL.
void rt_tcp_close(void *obj);

//=========================================================================
// TcpServer - Creation
//=========================================================================

/// @brief Start a non-blocking TCP listener on all local interfaces.
/// @param port Port in [0, 65535]; zero requests an OS-selected ephemeral port.
/// @return Newly owned TcpServer object.
/// @note Traps on invalid input, bind/listen/configuration failure, permission
///       denial, or allocation failure.
void *rt_tcp_server_listen(int64_t port);

/// @brief Start a non-blocking TCP listener on a specific local address.
/// @param address Nonempty hostname or numeric address without embedded NUL.
/// @param port Port in [0, 65535]; zero requests an OS-selected ephemeral port.
/// @return Newly owned TcpServer object.
/// @note Traps on invalid input, resolution, bind/listen/configuration failure,
///       permission denial, or allocation failure.
void *rt_tcp_server_listen_at(rt_string address, int64_t port);

//=========================================================================
// TcpServer - Properties
//=========================================================================

/// @brief Get a TcpServer's recorded local port.
/// @param obj Required TcpServer receiver.
/// @return Bound port, including the actual OS-selected ephemeral port.
int64_t rt_tcp_server_port(void *obj);

/// @brief Copy a TcpServer's recorded bound address.
/// @details The metadata remains available after the listener closes.
/// @param obj Required TcpServer receiver.
/// @return Newly owned bound-address String.
rt_string rt_tcp_server_address(void *obj);

/// @brief Check whether a TcpServer still accepts connections.
/// @param obj Required TcpServer receiver.
/// @return One while listening, otherwise zero.
int8_t rt_tcp_server_is_listening(void *obj);

//=========================================================================
// TcpServer - Accept and Close
//=========================================================================

/// @brief Accept the next client without an application timeout.
/// @details The listener is polled in bounded slices so a concurrent server
///          close can safely terminate the wait.
/// @param obj Required listening TcpServer receiver.
/// @return Newly owned connected Tcp object, or NULL when concurrently closed
///         or after a returning trap.
void *rt_tcp_server_accept(void *obj);

/// @brief Accept the next client with an optional timeout.
/// @details Safely coordinates concurrent accept and close operations and
///          returns accepted sockets in blocking, TCP_NODELAY-enabled mode.
/// @param obj Required listening TcpServer receiver.
/// @param timeout_ms Timeout in [0, @c INT_MAX] milliseconds; zero waits
///        indefinitely.
/// @return Newly owned connected Tcp object, or NULL on timeout or concurrent
///         close.
/// @note Traps on invalid input, closed-at-entry server, accept/configuration
///       failure, or allocation failure.
void *rt_tcp_server_accept_for(void *obj, int64_t timeout_ms);

/// @brief Stop listening and close a TcpServer idempotently.
/// @details Waits for registered accept operations to leave before closing the
///          descriptor, preventing descriptor-reuse races. NULL is a no-op.
/// @param obj TcpServer receiver, or NULL.
void rt_tcp_server_close(void *obj);

//=========================================================================
// Udp - Creation
//=========================================================================

/// @brief Create an unbound UDP socket for sending datagrams.
/// @details Prefers a dual-stack IPv6 socket so one handle can reach IPv4 and
///          IPv6 destinations, and falls back to IPv4 when unavailable.
/// @return Newly owned Udp object, or NULL after a returning creation or
///         allocation trap.
void *rt_udp_new(void);

/// @brief Create a UDP socket bound on all local interfaces.
/// @param port Local port in [0, 65535]; zero requests an ephemeral port.
/// @return Newly owned bound Udp object.
/// @note Traps on invalid input, address/bind failure, permission denial, or
///       allocation failure.
void *rt_udp_bind(int64_t port);

/// @brief Create a UDP socket bound to a specific local endpoint.
/// @details Resolves IPv4, IPv6, or hostname input and records the actual port
///          selected by the OS for an ephemeral bind.
/// @param address Nonempty local address without embedded NUL bytes.
/// @param port Local port in [0, 65535]; zero requests an ephemeral port.
/// @return Newly owned bound Udp object.
/// @note Traps on invalid input, resolution/bind failure, permission denial, or
///       allocation failure.
void *rt_udp_bind_at(rt_string address, int64_t port);

//=========================================================================
// Udp - Properties
//=========================================================================

/// @brief Get a UDP socket's recorded bound port.
/// @param obj Required Udp receiver.
/// @return Bound port, or zero for an unbound or explicitly closed socket.
int64_t rt_udp_port(void *obj);

/// @brief Copy a UDP socket's recorded bound address.
/// @param obj Required Udp receiver.
/// @return Newly owned address String, or an empty String for an unbound
///         socket. The last address remains available after explicit close.
rt_string rt_udp_address(void *obj);

/// @brief Check whether a UDP socket has a fixed local binding.
/// @param obj Required Udp receiver.
/// @return One for a socket created through a bind API, otherwise zero.
int8_t rt_udp_is_bound(void *obj);

//=========================================================================
// Udp - Send Methods
//=========================================================================

/// @brief Send one managed Bytes payload as a UDP datagram.
/// @details Resolves IPv4, IPv6, and DNS destinations compatibly with the
///          socket family. Payloads are capped at 65,507 bytes and a partial
///          native datagram send is treated as an error.
/// @param obj Required open Udp receiver.
/// @param host Nonempty destination hostname or address without embedded NUL.
/// @param port Destination port in [1, 65535].
/// @param data Required managed Bytes payload.
/// @return Exact datagram byte count, or -1 after a returning trap.
/// @note Traps on invalid input, resolution failure, oversized payload, partial
///       send, or another socket error.
int64_t rt_udp_send_to(void *obj, rt_string host, int64_t port, void *data);

/// @brief Send one managed String payload as a UDP datagram.
/// @details Sends the exact stored bytes without a NUL terminator and otherwise
///          has the same family resolution, size limit, and atomic-send
///          semantics as @ref rt_udp_send_to.
/// @param obj Required open Udp receiver.
/// @param host Nonempty destination hostname or address without embedded NUL.
/// @param port Destination port in [1, 65535].
/// @param text Required managed String payload.
/// @return Exact datagram byte count, or -1 after a returning trap.
int64_t rt_udp_send_to_str(void *obj, rt_string host, int64_t port, rt_string text);

//=========================================================================
// Udp - Receive Methods
//=========================================================================

/// @brief Receive one UDP datagram and record its sender.
/// @details Convenience alias for @ref rt_udp_recv_from. A persistent socket
///          timeout returns empty Bytes; an oversized datagram traps instead of
///          silently exposing a truncated packet.
/// @param obj Required open Udp receiver.
/// @param max_bytes Receive-buffer size in [0, @c INT_MAX].
/// @return Newly owned exact-sized Bytes, possibly empty.
void *rt_udp_recv(void *obj, int64_t max_bytes);

/// @brief Receive one complete UDP datagram and record its sender.
/// @details Right-sizes the returned Bytes and updates sender metadata only
///          after a successful receive. Persistent timeout returns empty Bytes;
///          a datagram larger than @p max_bytes raises @c Err_ProtocolError.
/// @param obj Required open Udp receiver.
/// @param max_bytes Receive-buffer size in [0, @c INT_MAX].
/// @return Newly owned exact-sized Bytes, possibly empty.
/// @see rt_udp_sender_host
/// @see rt_udp_sender_port
void *rt_udp_recv_from(void *obj, int64_t max_bytes);

/// @brief Receive one UDP datagram with a one-shot readiness timeout.
/// @details A positive timeout waits before delegating to
///          @ref rt_udp_recv_from. This differs from the persistent socket
///          timeout: readiness expiry returns NULL, while socket-timeout expiry
///          during receive returns empty Bytes. Zero skips the readiness wait.
/// @param obj Required open Udp receiver.
/// @param max_bytes Receive-buffer size in [0, @c INT_MAX].
/// @param timeout_ms One-shot timeout in [0, @c INT_MAX] milliseconds.
/// @return Newly owned exact-sized Bytes, or NULL on readiness timeout or after
///         a returning trap.
void *rt_udp_recv_for(void *obj, int64_t max_bytes, int64_t timeout_ms);

/// @brief Copy the numeric address of the most recent datagram sender.
/// @param obj Required Udp receiver.
/// @return Newly owned IPv4 or IPv6 String, empty before the first successful
///         receive.
rt_string rt_udp_sender_host(void *obj);

/// @brief Get the source port of the most recent datagram.
/// @param obj Required Udp receiver.
/// @return Sender port, or zero before the first successful receive.
int64_t rt_udp_sender_port(void *obj);

//=========================================================================
// Udp - Options and Close
//=========================================================================

/// @brief Enable or disable native UDP broadcast permission.
/// @details Sets @c SO_BROADCAST, which is required for limited or directed
///          IPv4 broadcast destinations.
/// @param obj Required open Udp receiver.
/// @param enable Nonzero to enable broadcast, zero to disable it.
/// @note Traps when the socket option cannot be changed.
void rt_udp_set_broadcast(void *obj, int8_t enable);

/// @brief Join an IPv4 or IPv6 multicast group on the default interface.
/// @param obj Required open Udp receiver.
/// @param group_addr Numeric IPv4 multicast address in 224.0.0.0/4 or IPv6
///        multicast address in ff00::/8, without embedded NUL.
/// @note Traps on invalid input, unsupported IPv6 membership, or native option
///       failure.
void rt_udp_join_group(void *obj, rt_string group_addr);

/// @brief Best-effort leave an IPv4 or IPv6 multicast group.
/// @details Closed sockets, empty or malformed addresses, unsupported IPv6
///          membership, and native drop-membership errors are silent no-ops.
/// @param obj Required valid Udp receiver.
/// @param group_addr Numeric multicast group address.
void rt_udp_leave_group(void *obj, rt_string group_addr);

/// @brief Set the persistent native UDP receive timeout.
/// @details Subsequent receive timeout returns empty Bytes; this differs from
///          the NULL readiness-timeout result of @ref rt_udp_recv_for.
/// @param obj Required open Udp receiver.
/// @param timeout_ms Timeout in [0, @c INT_MAX] milliseconds; zero disables it.
void rt_udp_set_recv_timeout(void *obj, int64_t timeout_ms);

/// @brief Close a UDP socket idempotently.
/// @details Clears bound state and the port while preserving the last bound
///          address for diagnostics. NULL is accepted as a no-op.
/// @param obj Udp receiver, or NULL.
void rt_udp_close(void *obj);

//=========================================================================
// Dns - Static Utility Class
//=========================================================================

/// @brief Resolve a hostname to the first usable IPv4 or IPv6 address.
/// @details Preserves operating-system resolver order and skips malformed or
///          unsupported native records. The returned String is caller-owned.
/// @param hostname Non-empty hostname without embedded NUL bytes.
/// @return Numeric address String, including an IPv6 scope zone when supplied
///         by the resolver.
/// @note Traps with `Err_DnsError` when no usable address is available.
rt_string rt_dns_resolve(rt_string hostname);

/// @brief Resolve a hostname to every unique IPv4 and IPv6 address.
/// @details Preserves resolver order, removes exact duplicates, and returns an
///          element-owning Seq whose Strings are released with the sequence.
/// @param hostname Non-empty hostname without embedded NUL bytes.
/// @return Caller-owned Seq of numeric address Strings.
/// @note Traps with `Err_DnsError` when no usable address is available and
///       cleans native resolver state if managed allocation fails.
void *rt_dns_resolve_all(rt_string hostname);

/// @brief Resolve a hostname to its first usable IPv4 address.
/// @param hostname Non-empty hostname without embedded NUL bytes.
/// @return Caller-owned canonical dotted-decimal IPv4 String.
/// @note Traps with `Err_DnsError` when no IPv4 address is available.
rt_string rt_dns_resolve4(rt_string hostname);

/// @brief Resolve a hostname to its first usable IPv6 address.
/// @param hostname Non-empty hostname without embedded NUL bytes.
/// @return Caller-owned numeric IPv6 String, preserving any scope zone.
/// @note Traps with `Err_DnsError` when no IPv6 address is available.
rt_string rt_dns_resolve6(rt_string hostname);

/// @brief Reverse-resolve a numeric IPv4 or IPv6 address to a hostname.
/// @param ip_address Canonical IPv4 or valid IPv6 address String; IPv6 may
///        include a `%zone` suffix.
/// @return Caller-owned hostname String.
/// @note Traps with `Err_InvalidUrl` for invalid syntax and `Err_DnsError` when
///       no required PTR name is available.
rt_string rt_dns_reverse(rt_string ip_address);

/// @brief Check for canonical dotted-decimal IPv4 syntax.
/// @details This allocation-free parser rejects multi-digit leading zeroes,
///          out-of-range octets, embedded NULs, and trailing bytes.
/// @param address Address String to check.
/// @return One for valid IPv4 text, otherwise zero.
int8_t rt_dns_is_ipv4(rt_string address);

/// @brief Check IPv6 syntax with an optional non-empty scope zone.
/// @details The allocation-free parser accepts portable `%zone` identifiers
///          without resolving them against the current interface table.
/// @param address Address String to check.
/// @return One for valid IPv6 text, otherwise zero.
int8_t rt_dns_is_ipv6(rt_string address);

/// @brief Check whether a String is canonical IPv4 or valid IPv6 text.
/// @param address Address String to check.
/// @return One for either supported family, otherwise zero.
int8_t rt_dns_is_ip(rt_string address);

/// @brief Get local machine hostname.
/// @return Caller-owned local hostname String.
/// @note Traps with `Err_DnsError` if native host-name discovery fails.
rt_string rt_dns_local_host(void);

/// @brief Enumerate unique numeric addresses on active local interfaces.
/// @details Includes loopback when exposed by the platform and preserves IPv6
///          scope zones. The returned Seq owns its String elements.
/// @return Caller-owned Seq; native enumeration failure yields an empty Seq.
/// @note Managed allocation failure releases the native interface snapshot
///       before propagating its trap.
void *rt_dns_local_addrs(void);

//=========================================================================
// Http - Static Utility Class (Simple HTTP/1.1 Client)
//=========================================================================

// Note: "get" in rt_http_get / rt_http_get_bytes is the HTTP GET method,
// not a property accessor. These perform network requests.

/// @brief Perform a one-shot HTTP GET and return its body as a String.
/// @details Uses the default timeout and redirect limit. Response bytes are
///          passed through as UTF-8 without charset detection.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @return Newly owned response-body String.
/// @note Traps on invalid URL, transport/protocol failure, or allocation error.
rt_string rt_http_get(rt_string url);

/// @brief Perform a one-shot HTTP GET and return its raw body.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @return Newly owned response-body Bytes.
/// @note Uses the default timeout and redirect limit and traps on invalid URL,
///       transport/protocol failure, or allocation error.
void *rt_http_get_bytes(rt_string url);

/// @brief Perform a one-shot HTTP POST with a String body.
/// @details Copies the exact body bytes and supplies `Content-Type:
///          text/plain; charset=utf-8` for nonempty content.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @param body Required managed request-body String.
/// @return Newly owned response-body String.
rt_string rt_http_post(rt_string url, rt_string body);

/// @brief Perform a one-shot HTTP POST with a Bytes body.
/// @details Borrows the Bytes for the synchronous request and supplies
///          `Content-Type: application/octet-stream` for nonempty content.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @param body Required managed request-body Bytes.
/// @return Newly owned response-body Bytes.
void *rt_http_post_bytes(rt_string url, void *body);

/// @brief Download a URL transactionally to a local file.
/// @details The response is streamed to an exclusively created sibling temp
///          file, flushed and synchronized, then atomically installed at the
///          destination. Replacing an existing file preserves its ordinary
///          read/write/execute permission bits but never copies special mode
///          bits. Invalid handles, embedded NULs, transport failures, managed
///          allocation traps, and filesystem failures are contained by this
///          Boolean API; staged content is removed and an existing destination
///          is left intact whenever publication does not complete.
/// @param url Full HTTP or HTTPS URL without embedded NUL bytes.
/// @param dest_path Destination path without embedded NUL bytes.
/// @return One only after atomic publication; zero on any failure.
int8_t rt_http_download(rt_string url, rt_string dest_path);

/// @brief Perform a one-shot HTTP HEAD and return its headers.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @return Newly owned Map snapshot of response header names to values.
/// @note Uses the default timeout and redirect limit and traps on invalid URL,
///       transport/protocol failure, or allocation error.
void *rt_http_head(rt_string url);

/// @brief Perform a one-shot HTTP PATCH with a String body.
/// @details Copies the body and supplies `Content-Type: text/plain;
///          charset=utf-8` for nonempty content.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @param body Required managed request-body String.
/// @return Newly owned response-body String.
rt_string rt_http_patch(rt_string url, rt_string body);

/// @brief Perform a one-shot HTTP OPTIONS request.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @return Newly owned response-body String, often empty for metadata-only
///         OPTIONS responses.
rt_string rt_http_options(rt_string url);

/// @brief Perform a one-shot HTTP PUT with a String body.
/// @details Copies the body and supplies `Content-Type: text/plain;
///          charset=utf-8` for nonempty content.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @param body Required managed request-body String.
/// @return Newly owned response-body String.
rt_string rt_http_put(rt_string url, rt_string body);

/// @brief Perform a one-shot HTTP PUT with a Bytes body.
/// @details Supplies `Content-Type: application/octet-stream` for nonempty
///          content and returns the raw response body.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @param body Required managed request-body Bytes.
/// @return Newly owned response-body Bytes.
void *rt_http_put_bytes(rt_string url, void *body);

/// @brief Perform a one-shot HTTP DELETE and return its body as a String.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @return Newly owned response-body String.
/// @note Uses the default timeout and redirect limit and traps on invalid URL
///       or request failure.
rt_string rt_http_delete(rt_string url);

/// @brief Perform a one-shot HTTP DELETE and return its raw body.
/// @param url Nonempty absolute HTTP or HTTPS URL.
/// @return Newly owned response-body Bytes.
/// @note Uses the default timeout and redirect limit and traps on invalid URL
///       or request failure.
void *rt_http_delete_bytes(rt_string url);

//=========================================================================
// HttpReq - Request Builder (Instance Class)
//=========================================================================

/// @brief Create a configurable managed HTTP request.
/// @details Validates the method as an HTTP token and parses the URL eagerly.
///          Defaults include a 30-second timeout, TLS verification, gzip
///          handling, and automatic redirects up to the runtime default cap.
/// @param method Nonempty HTTP method token without embedded NUL.
/// @param url Nonempty absolute HTTP or HTTPS URL without embedded NUL.
/// @return Newly owned HttpReq object.
void *rt_http_req_new(rt_string method, rt_string url);

/// @brief Set a request header, replacing any existing case-insensitive match.
/// @details Header names must be valid HTTP tokens and values may not contain
///          CR or LF. NULL name or value is an intentional no-op.
/// @param obj Required HttpReq receiver.
/// @param name Header field name.
/// @param value Header field value.
/// @return @p obj for fluent chaining, or NULL after a returning
///         invalid-receiver trap.
void *rt_http_req_set_header(void *obj, rt_string name, rt_string value);

/// @brief Append a request header without replacing same-name fields.
/// @details Intended for repeatable fields. It applies the same token,
///          embedded-NUL, and CR/LF validation as the replacing setter; NULL
///          name or value is an intentional no-op.
/// @param obj Required HttpReq receiver.
/// @param name Header field name.
/// @param value Header field value.
/// @return @p obj for fluent chaining, or NULL after a returning
///         invalid-receiver trap.
void *rt_http_req_add_header(void *obj, rt_string name, rt_string value);

/// @brief Replace a request body with a private copy of managed Bytes.
/// @details Frees the previous body. NULL clears the body without error, and
///          later mutation or release of @p data cannot affect the request.
/// @param obj Required HttpReq receiver.
/// @param data Managed Bytes body, or NULL to clear it.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_body(void *obj, void *data);

/// @brief Replace a request body with a private copy of a String's bytes.
/// @details Copies the exact logical length, including embedded NUL bytes.
///          NULL clears the previous body without error.
/// @param obj Required HttpReq receiver.
/// @param text Managed String body, or NULL to clear it.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_body_str(void *obj, rt_string text);

/// @brief Set the per-request network timeout.
/// @param obj Required HttpReq receiver.
/// @param timeout_ms Timeout in [0, @c INT_MAX] milliseconds.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_timeout(void *obj, int64_t timeout_ms);

/// @brief Enable or disable TLS certificate verification for this request.
/// @details Verification is enabled by default and should remain enabled for
///          production HTTPS traffic.
/// @param obj Required HttpReq receiver.
/// @param verify Nonzero to verify the certificate chain and hostname, zero to
///        skip both checks.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_tls_verify(void *obj, int8_t verify);

/// @brief Disable HTTPS certificate and hostname verification for a local test request.
/// @details This is intentionally named as a testing-only escape hatch. It sets the
///          same request flag as `rt_http_req_set_tls_verify(obj, 0)` while making
///          the insecure behavior visible in code review and generated API docs.
///          Production HTTPS requests should leave verification enabled.
/// @param obj Required HttpReq receiver.
/// @return @p obj for fluent chaining.
void *rt_http_req_allow_insecure_certificates_for_testing(void *obj);

/// @brief Enable or disable keep-alive on this request.
/// @details Standalone requests attach the process-wide default pool when
///          sending with keep-alive enabled, allowing actual socket reuse.
/// @param obj Required HttpReq receiver.
/// @param keep_alive Nonzero to permit pooled reuse, zero to close afterward.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_keep_alive(void *obj, int8_t keep_alive);

/// @brief Restrict this request to HTTP/1.1 transport even over TLS.
/// @param obj Required HttpReq receiver.
/// @param force Nonzero to advertise only `http/1.1` via ALPN (forcing HTTP/1.1
///        framing, including the `Connection: keep-alive` header), 0 for
///        the default `h2,http/1.1` negotiation.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_force_http1(void *obj, int8_t force);

/// @brief Enable or disable automatic redirect following for this request.
/// @param obj Required HttpReq receiver.
/// @param follow Nonzero to follow supported redirects, zero to return the
///        redirect response itself.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_follow_redirects(void *obj, int8_t follow);

/// @brief Set the maximum number of redirects to follow for this request.
/// @details Negative values clamp to zero; values above @c INT_MAX trap and
///          leave the previous cap unchanged.
/// @param obj Required HttpReq receiver.
/// @param max_redirects Maximum redirect hops; zero disables following.
/// @return @p obj for fluent chaining.
void *rt_http_req_set_max_redirects(void *obj, int64_t max_redirects);

/// @brief Execute a configured HTTP request.
/// @details Adds `Content-Type: application/octet-stream` for a nonempty body
///          when no content type was supplied and attaches the default pool for
///          standalone keep-alive requests.
/// @param obj Required HttpReq receiver.
/// @return Newly owned HttpRes object, or NULL after a returning trap.
/// @note Traps on invalid state, connection/TLS/protocol failure, timeout, or
///       allocation error.
void *rt_http_req_send(void *obj);

/// @brief Execute HTTP request and return a Result-wrapped response.
/// @details Transport setup failures, connection failures, timeouts, and other
///          traps are captured as `Result.ErrStr`; successful HTTP exchanges,
///          including non-2xx status codes, return `Result.Ok(HttpRes)` so the
///          caller can inspect status, headers, and body explicitly.
/// @param obj HttpReq receiver to execute.
/// @return Newly owned opaque `Zanna.Result` containing `Ok(HttpRes)` or
///         `Err(String)`.
void *rt_http_req_send_result(void *obj);

//=========================================================================
// HttpRes - Response (Instance Class)
//=========================================================================

/// @brief Get an HTTP response status code.
/// @param obj HttpRes receiver, or NULL.
/// @return HTTP status code (e.g., 200, 404).
/// @note NULL returns zero; any other invalid managed handle traps.
int64_t rt_http_res_status(void *obj);

/// @brief Copy an HTTP response reason phrase.
/// @param obj HttpRes receiver, or NULL.
/// @return Newly owned status text (e.g. `OK`), or an empty String when absent.
/// @note NULL returns an empty String; any other invalid managed handle traps.
rt_string rt_http_res_status_text(void *obj);

/// @brief Copy all response headers into a fresh Map.
/// @details Keys are the parser's lower-case canonical names. Mutating the
///          returned Map cannot alter the response.
/// @param obj HttpRes receiver, or NULL.
/// @return Newly owned Map of header name to String value.
/// @note The returned Map is an independent snapshot. NULL returns an empty
///       Map; any other invalid managed handle traps.
void *rt_http_res_headers(void *obj);

/// @brief Copy an HTTP response body as raw Bytes.
/// @param obj HttpRes receiver, or NULL.
/// @return Newly owned response-body Bytes.
/// @note The returned Bytes owns a copy. NULL returns empty Bytes; any other
///       invalid managed handle traps.
void *rt_http_res_body(void *obj);

/// @brief Copy an HTTP response body as a String.
/// @details Performs no content-type, charset, or UTF-8 validation.
/// @param obj HttpRes receiver, or NULL.
/// @return Newly owned response-body String.
/// @note NULL returns an empty String; any other invalid managed handle traps.
rt_string rt_http_res_body_str(void *obj);

/// @brief Copy one response header using a case-insensitive name.
/// @param obj HttpRes receiver, or NULL.
/// @param name Header field name without embedded NUL.
/// @return Newly owned header value, or an empty String when absent.
/// @note The returned String is an independent copy. A non-NULL invalid
///       receiver or header-name handle traps before native payload access.
rt_string rt_http_res_header(void *obj, rt_string name);

/// @brief Check whether an HTTP response has a successful 2xx status.
/// @param obj HttpRes receiver, or NULL.
/// @return One for status 200 through 299, otherwise zero.
/// @note NULL returns zero; any other invalid managed handle traps.
int8_t rt_http_res_is_ok(void *obj);

//=========================================================================
// Url - URL Parsing and Construction
//=========================================================================

/// @brief Parse the runtime URL-reference grammar into a Url object.
/// @details Accepts network URLs, authority-less schemes such as `mailto:`,
///          and relative references, while rejecting embedded NUL, whitespace,
///          controls, backslashes, and malformed components. Use
///          @ref rt_url_is_valid_absolute when a network endpoint is required.
/// @param url_str Managed String containing a URL reference.
/// @return Newly owned Url object, or NULL after a returning trap.
void *rt_url_parse(rt_string url_str);

/// @brief Create an empty Url object for component-by-component construction.
/// @return Newly owned Url with every component unset.
void *rt_url_new(void);

/// @brief Copy a URL's lower-case scheme component.
/// @param obj Required Url receiver.
/// @return Newly owned scheme String, or an empty String when unset.
rt_string rt_url_scheme(void *obj);

/// @brief Replace and ASCII-lowercase a URL's scheme.
/// @param obj Required Url receiver.
/// @param scheme Valid RFC 3986 scheme String, or NULL/empty to clear it.
/// @note Traps with @c Err_InvalidUrl for invalid scheme syntax.
void rt_url_set_scheme(void *obj, rt_string scheme);

/// @brief Copy the stored host without adding IPv6 brackets.
/// @param obj Required Url receiver.
/// @return Newly owned host String, or an empty String when unset.
rt_string rt_url_host(void *obj);

/// @brief Replace a URL's host after validating component safety.
/// @param obj Required Url receiver.
/// @param host Host text without `/`, `?`, `#`, `@`, unsafe bytes, or embedded
///        NUL; NULL clears it.
void rt_url_set_host(void *obj, rt_string host);

/// @brief Get a URL's explicit numeric port.
/// @param obj Required Url receiver.
/// @return Stored port in [0, 65535]; zero means unspecified.
int64_t rt_url_port(void *obj);

/// @brief Set or clear a URL's explicit port.
/// @param obj Required Url receiver.
/// @param port Port in [0, 65535]; zero clears explicit port formatting.
void rt_url_set_port(void *obj, int64_t port);

/// @brief Copy a URL's stored path component.
/// @param obj Required Url receiver.
/// @return Newly owned path String, or an empty String when unset.
rt_string rt_url_path(void *obj);

/// @brief Replace a URL's encoded path component.
/// @param obj Required Url receiver.
/// @param path Path text without raw query/fragment delimiters or unsafe
///        bytes; NULL clears it.
void rt_url_set_path(void *obj, rt_string path);

/// @brief Copy a URL's query without the leading question mark.
/// @param obj Required Url receiver.
/// @return Newly owned query String, or an empty String when unset.
rt_string rt_url_query(void *obj);

/// @brief Replace a URL's encoded query component.
/// @param obj Required Url receiver.
/// @param query Query text without a leading `?`, raw `#`, unsafe bytes, or
///        embedded NUL; NULL clears it.
void rt_url_set_query(void *obj, rt_string query);

/// @brief Copy a URL's fragment without the leading hash.
/// @param obj Required Url receiver.
/// @return Newly owned fragment String, or an empty String when unset.
rt_string rt_url_fragment(void *obj);

/// @brief Replace a URL's encoded fragment component.
/// @param obj Required Url receiver.
/// @param fragment Fragment text without unsafe bytes or embedded NUL; NULL
///        clears it.
void rt_url_set_fragment(void *obj, rt_string fragment);

/// @brief Copy the user portion of URL userinfo.
/// @param obj Required Url receiver.
/// @return Newly owned user String, or an empty String when unset.
rt_string rt_url_user(void *obj);

/// @brief Replace the user portion of URL userinfo.
/// @param obj Required Url receiver.
/// @param user Encoded user text without authority delimiters or unsafe bytes;
///        NULL clears it.
void rt_url_set_user(void *obj, rt_string user);

/// @brief Copy the password portion of URL userinfo.
/// @param obj Required Url receiver.
/// @return Newly owned password String, or an empty String when unset.
rt_string rt_url_pass(void *obj);

/// @brief Replace the password portion of URL userinfo.
/// @param obj Required Url receiver.
/// @param pass Encoded password text without authority delimiters or unsafe
///        bytes; NULL clears it.
void rt_url_set_pass(void *obj, rt_string pass);

/// @brief Compose a URL's userinfo, host, and explicit port.
/// @details Includes only set components and brackets a colon-containing IPv6
///          host.
/// @param obj Required Url receiver.
/// @return Newly owned authority String, or empty when all components are unset.
rt_string rt_url_authority(void *obj);

/// @brief Compose a URL's host and nondefault port.
/// @details Omits userinfo and scheme, brackets IPv6 hosts, and suppresses a
///          port that is the default for the stored scheme.
/// @param obj Required Url receiver.
/// @return Newly owned host/port String, or empty when the host is unset.
rt_string rt_url_host_port(void *obj);

/// @brief Serialize every set URL component.
/// @details Produces a reference that round-trips through @ref rt_url_parse and
///          includes scheme, authority, path, query, and fragment only when set.
/// @param obj Required Url receiver.
/// @return Newly owned serialized URL String.
rt_string rt_url_full(void *obj);

/// @brief Set or replace one URL query parameter.
/// @details Decodes the current query into a temporary Map, replaces the key,
///          and percent-encodes the complete query again. NULL value stores an
///          empty String.
/// @param obj Required Url receiver.
/// @param name Query parameter name.
/// @param value Query parameter value, or NULL for an empty value.
/// @return @p obj for fluent chaining.
void *rt_url_set_query_param(void *obj, rt_string name, rt_string value);

/// @brief Read one decoded URL query parameter.
/// @param obj Required Url receiver.
/// @param name Query parameter name.
/// @return Newly owned decoded value, or an empty String when absent.
rt_string rt_url_get_query_param(void *obj, rt_string name);

/// @brief Check whether a decoded URL query contains a named parameter.
/// @param obj Required Url receiver.
/// @param name Query parameter name.
/// @return One when present, otherwise zero.
int8_t rt_url_has_query_param(void *obj, rt_string name);

/// @brief Remove one URL query parameter.
/// @details Missing parameters are a no-op; the remaining query is encoded
///          again from a temporary Map.
/// @param obj Required Url receiver.
/// @param name Query parameter name to remove.
/// @return @p obj for fluent chaining.
void *rt_url_del_query_param(void *obj, rt_string name);

/// @brief Decode all URL query parameters into a fresh Map.
/// @details Keys and values are decoded Strings; later duplicate keys replace
///          earlier occurrences.
/// @param obj Required Url receiver.
/// @return Newly owned Map, empty when the query is unset.
void *rt_url_query_map(void *obj);

/// @brief Resolve a URL reference against a base URL.
/// @details Implements RFC 3986 reference resolution for schemed, authority,
///          absolute-path, relative-path, and same-document references. NULL or
///          empty input clones the base.
/// @param obj Required base Url receiver.
/// @param relative URL-reference String.
/// @return Newly owned independent resolved Url.
void *rt_url_resolve(void *obj, rt_string relative);

/// @brief Deep-copy a URL object.
/// @param obj Required Url receiver.
/// @return Newly owned Url whose component storage is independent of @p obj.
void *rt_url_clone(void *obj);

/// @brief Percent-encode exact String bytes for URL use.
/// @details RFC 3986 unreserved bytes pass through; all other bytes, including
///          non-ASCII bytes, become uppercase `%XX` triples.
/// @param text Managed String to encode; NULL is treated as empty.
/// @return Newly owned encoded String.
rt_string rt_url_encode(rt_string text);

/// @brief Decode percent-encoded String bytes.
/// @details Valid `%XX` triples are decoded and malformed escapes pass through
///          verbatim, preserving the exact resulting byte length.
/// @param text Managed encoded String; NULL is treated as empty.
/// @return Newly owned decoded String.
rt_string rt_url_decode(rt_string text);

/// @brief Encode a managed Map as a URL query string.
/// @details Percent-encodes keys and supported values and joins them as
///          `name=value`. String and boxed String values are used verbatim;
///          boxed integer, double, and Boolean values use canonical runtime
///          formatting; NULL becomes empty. Unsupported object values trap.
/// @param map Managed Map with String keys and supported scalar values, or NULL.
/// @return Newly owned encoded query String, empty for NULL or an empty Map.
rt_string rt_url_encode_query(void *map);

/// @brief Decode a query string into a Map of Strings.
/// @details Splits on ampersands and the first equals sign, decodes percent
///          escapes and form-style plus characters, and lets later duplicate
///          keys replace earlier values.
/// @param query Encoded query without a leading question mark; NULL is empty.
/// @return Newly owned Map of decoded String keys and values.
void *rt_url_decode_query(rt_string query);

/// @brief Check whether a String matches the runtime URL-reference grammar.
/// @details Rejects empty input, embedded NUL, unencoded whitespace, malformed
///          scheme markers, and invalid component syntax. Relative references
///          and authority-less schemes are allowed.
/// @param url_str Candidate URL-reference String.
/// @return One when parseable, otherwise zero.
int8_t rt_url_is_valid(rt_string url_str);

/// @brief Check for a strict absolute network URL.
/// @details Requires @ref rt_url_is_valid plus a nonempty scheme and host.
///          Relative references, scheme-less text, and authority-less forms
///          such as `mailto:` return zero.
/// @param url_str Candidate absolute network URL String.
/// @return One when parsing yields both a scheme and host, otherwise zero.
int8_t rt_url_is_valid_absolute(rt_string url_str);

#ifdef __cplusplus
}
#endif
