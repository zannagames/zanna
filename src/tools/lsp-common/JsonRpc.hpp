//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares JSON-RPC 2.0 request parsing and response/notification
///        construction helpers.
///
/// Protocol messages are represented with owned JsonValue data. Request IDs
/// may be strings, integers, or explicit null values; only omission of the
/// @c id member denotes a notification.
///
/// @see Json.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tools/lsp-common/Json.hpp"

#include <string>

namespace zanna::server {

/// @brief JSON-RPC parse-error code for text that is not valid JSON.
constexpr int kParseError = -32700;

/// @brief JSON-RPC invalid-request code for a malformed protocol object.
constexpr int kInvalidRequest = -32600;

/// @brief JSON-RPC method-not-found code for an unknown requested operation.
constexpr int kMethodNotFound = -32601;

/// @brief JSON-RPC invalid-parameters code for unsuitable method arguments.
constexpr int kInvalidParams = -32602;

/// @brief JSON-RPC internal-error code for failures within the server.
constexpr int kInternalError = -32603;

/// @brief A parsed JSON-RPC 2.0 request or notification.
/// @details All fields own their data. @ref hasId records field presence
///          independently of the value stored in @ref id, preserving the
///          distinction between a notification and an explicit null ID.
struct JsonRpcRequest {
    std::string method; ///< Nonempty method name such as @c "initialize".
    JsonValue params;   ///< Parameters, defaulting to null when omitted.
    JsonValue id;       ///< Request ID (string, int, or null when hasId is true).
    bool hasId{false};  ///< True when the incoming message contained an id field.

    /// @brief Test whether the message omits a response identifier.
    /// @return @c true when the incoming object had no @c id member and no
    ///         response is expected.
    [[nodiscard]] bool isNotification() const {
        return !hasId;
    }
};

/// @brief Parse a JSON-RPC 2.0 message from a JSON value.
/// @param msg Parsed JSON value expected to contain a protocol object.
/// @param out Destination reset before validation and populated on success.
/// @return @c true for an accepted JSON-RPC 2.0 request or notification;
///         @c false for a wrong version, missing/empty method, invalid ID, or
///         non-object input.
bool parseRequest(const JsonValue &msg, JsonRpcRequest &out);

/// @brief Build a JSON-RPC 2.0 success response.
/// @param id Request identifier to echo verbatim.
/// @param result Successful result payload.
/// @return Owned compact JSON object containing @c jsonrpc, @c id, and @c result.
std::string buildResponse(const JsonValue &id, const JsonValue &result);

/// @brief Build a JSON-RPC 2.0 error response.
/// @param id Request identifier to echo; may be null when unavailable.
/// @param code Error code such as kParseError or kMethodNotFound.
/// @param message Human-readable error description.
/// @param data Optional structured details, omitted when null.
/// @return Owned compact JSON error response.
std::string buildError(const JsonValue &id,
                       int code,
                       const std::string &message,
                       const JsonValue &data = JsonValue());

/// @brief Build a JSON-RPC 2.0 notification (no id, no response expected).
/// @param method Notification method name.
/// @param params Notification parameter payload.
/// @return Owned compact JSON object without an @c id member.
std::string buildNotification(const std::string &method, const JsonValue &params);

} // namespace zanna::server
