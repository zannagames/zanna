//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/network/rt_smtp.h
// Purpose: Simple SMTP client for sending emails with optional STARTTLS.
// Key invariants:
//   - Strict CRLF reply parsing precedes EHLO, optional TLS/AUTH, and DATA.
//   - STARTTLS and implicit TLS publish one cancellation-visible owner before handshake.
//   - MIME messages stream through bounded native storage with dot transparency.
//   - Sends and configuration changes are serialized; Close may cancel active I/O.
// Ownership/Lifetime:
//   - Client objects are GC-managed and own copied host/credential/error bytes.
// Links: rt_smtp.c (implementation), rt_network.h (TCP), rt_tls.h (TLS)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_smtp.h
 * @brief Declares the managed SMTP client and message-sending API.
 * @details Clients retain copied endpoint and optional authentication state,
 *          support implicit TLS or STARTTLS, and send plain-text or HTML
 *          messages through serialized, cancellation-aware sessions. Boolean
 *          and Result-returning interfaces expose protocol diagnostics without
 *          transferring client ownership.
 */

#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Stable runtime class identifier for managed `Zanna.Network.SmtpClient` handles.
/// @details Public SMTP entry points validate this tag, the complete private
///          payload size, and native-lock initialization before reading any
///          connection, credential, or diagnostic state.
#define RT_SMTP_CLASS_ID INT64_C(-0x720218)

/// @brief Create an SMTP client targeting @p host on the given port.
/// @details Host must be a live, NUL-free runtime String. Empty hosts, URL
///          delimiters, malformed bracketed IPv6, and ports outside 1..65535
///          trap. Port 465 enables implicit TLS by default; other ports begin
///          as plaintext until @ref rt_smtp_set_tls enables STARTTLS.
/// @param host DNS name, numeric address, or bracketed IPv6 literal.
/// @param port Destination port in 1..65535.
/// @return Caller-owned GC-managed SmtpClient, or NULL after a returning trap hook.
void *rt_smtp_new(rt_string host, int64_t port);
/// @brief Set credentials for AUTH LOGIN.
/// @details Username and password are replaced transactionally, wiped when
///          superseded, and are never transmitted on an unencrypted session.
///          Pass two NULL strings to disable authentication. Supplying only
///          one value, a forged/NUL-bearing String, or a field beyond 16 KiB
///          traps without changing the previous credentials. This call waits
///          for an active send to finish.
/// @param client Valid SmtpClient receiver; NULL preserves the legacy no-op.
/// @param username NUL-free username, or NULL with @p password to disable AUTH.
/// @param password NUL-free password, or NULL with @p username to disable AUTH.
void rt_smtp_set_auth(void *client, rt_string username, rt_string password);
/// @brief Toggle encrypted SMTP for subsequent serialized sends.
/// @details On ports other than 465, nonzero enables a required STARTTLS
///          capability and upgrade after EHLO. On port 465 it controls the
///          implicit TLS handshake. Disabling TLS while credentials are set
///          does not permit plaintext AUTH; the send fails safely instead.
/// @param client Valid SmtpClient receiver; NULL preserves the legacy no-op.
/// @param enable Any nonzero value enables TLS.
void rt_smtp_set_tls(void *client, int8_t enable);
/// @brief Send a plain-text UTF-8 email on a fresh SMTP connection.
/// @details Concurrent calls serialize. Headers are injection-sanitized and
///          the body is CRLF-normalized and dot-stuffed using constant native
///          memory. Successful DATA acceptance returns one; protocol/network
///          failures return zero and populate LastError. Lower runtime traps
///          are re-raised only after transport and mutex cleanup.
/// @param client Valid SmtpClient; NULL returns zero for legacy compatibility.
/// @param from Single sender mailbox path.
/// @param to Single recipient mailbox path.
/// @param subject Optional subject; NULL means empty.
/// @param body Optional body; NULL means empty.
/// @return One after server acceptance, otherwise zero.
int8_t rt_smtp_send(void *client, rt_string from, rt_string to, rt_string subject, rt_string body);
/// @brief Send a plain-text email and return a Result instead of using LastError.
/// @details This performs the same SMTP session as rt_smtp_send(). Success is
///          returned as OkI64(1); failures converted to the legacy Boolean/LastError
///          path and lower-level transport traps are returned as ErrStr(message).
///          Use this in new code instead of calling rt_smtp_send() and then
///          rt_smtp_last_error().
/// @param client Opaque Zanna.Network.SmtpClient object.
/// @param from Sender mailbox path.
/// @param to Recipient mailbox path.
/// @param subject Optional email subject; controls are replaced by spaces.
/// @param body Optional plain-text message body.
/// @return Opaque Zanna.Result object containing OkI64(1) or ErrStr(message).
void *rt_smtp_send_result(
    void *client, rt_string from, rt_string to, rt_string subject, rt_string body);
/// @brief Send an HTML-format UTF-8 email on a fresh serialized connection.
/// @details Identical to @ref rt_smtp_send except for `Content-Type: text/html`.
///          The runtime frames but does not escape or sanitize HTML body bytes.
/// @param client Valid SmtpClient; NULL returns zero for legacy compatibility.
/// @param from Single sender mailbox path.
/// @param to Single recipient mailbox path.
/// @param subject Optional subject; controls are replaced by spaces.
/// @param html_body Optional HTML body; NULL means empty.
/// @return One after server acceptance, otherwise zero.
int8_t rt_smtp_send_html(
    void *client, rt_string from, rt_string to, rt_string subject, rt_string html_body);
/// @brief Send an HTML email and return a Result instead of using LastError.
/// @details Mirrors rt_smtp_send_html() with a side-channel-free result shape.
///          Success is OkI64(1); failures are ErrStr(message).
/// @param client Opaque Zanna.Network.SmtpClient object.
/// @param from Sender mailbox path.
/// @param to Recipient mailbox path.
/// @param subject Optional email subject; controls are replaced by spaces.
/// @param html_body Optional HTML body; callers remain responsible for escaping.
/// @return Opaque Zanna.Result object containing OkI64(1) or ErrStr(message).
void *rt_smtp_send_html_result(
    void *client, rt_string from, rt_string to, rt_string subject, rt_string html_body);
/// @brief Get a synchronized snapshot of the latest SMTP diagnostic.
/// @details The returned String is caller-owned and remains stable while other
///          threads send or clear LastError. A successful send clears the
///          diagnostic. NULL returns an owned empty String for compatibility;
///          forged non-NULL receivers trap before native state access.
/// @param client Candidate SmtpClient receiver.
/// @return Caller-owned diagnostic String, possibly empty.
rt_string rt_smtp_last_error(void *client);
/// @brief Cancel active I/O or immediately close an idle SMTP transport.
/// @details Active TLS/TCP storage remains owned by the serialized send while
///          Close performs native shutdown to wake it. The send owner releases
///          that storage; a later send clears cancellation and reconnects.
///          Repeated Close calls and NULL are safe.
/// @param client Candidate SmtpClient receiver.
void rt_smtp_close(void *client);

#ifdef __cplusplus
}
#endif
