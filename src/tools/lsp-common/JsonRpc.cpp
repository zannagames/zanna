//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements JSON-RPC 2.0 request validation and compact message
///        construction.
///
/// Every constructed message carries the required @c "jsonrpc":"2.0" member.
/// Requests distinguish an omitted identifier from an explicit null
/// identifier, notifications omit identifiers, and returned strings own their
/// serialized data.
///
/// @see JsonRpc.hpp
//
//===----------------------------------------------------------------------===//

#include "tools/lsp-common/JsonRpc.hpp"

namespace zanna::server {

/// @brief Validate and decode a JSON-RPC request or notification.
/// @param msg Parsed JSON value expected to contain a protocol object.
/// @param out Destination reset before validation and populated on success.
/// @return @c true when the version, method, parameters, and optional identifier
///         satisfy the accepted JSON-RPC request shape; @c false otherwise.
/// @details The method must be a nonempty string. Parameters default to null.
///          Identifiers may be null, strings, or integers; an absent identifier
///          marks a notification, while explicit null remains a request ID.
bool parseRequest(const JsonValue &msg, JsonRpcRequest &out) {
    out = JsonRpcRequest{};
    if (msg.type() != JsonType::Object)
        return false;

    const auto *jsonrpc = msg.get("jsonrpc");
    if (!jsonrpc || jsonrpc->type() != JsonType::String || jsonrpc->asString() != "2.0")
        return false;

    // Method is required
    const auto *method = msg.get("method");
    if (!method || method->type() != JsonType::String)
        return false;
    out.method = method->asString();
    if (out.method.empty())
        return false;

    // Params is optional (default to null)
    const auto *params = msg.get("params");
    out.params = params ? *params : JsonValue();

    // ID is optional. JSON-RPC notifications omit the field entirely; an
    // explicit null id is still a request id and must be echoed in a response.
    const auto *id = msg.get("id");
    if (id && !(id->isNull() || id->type() == JsonType::String || id->type() == JsonType::Int))
        return false;
    out.id = id ? *id : JsonValue();
    out.hasId = id != nullptr;

    return true;
}

/// @brief Serialize a successful JSON-RPC response.
/// @param id Request identifier to echo verbatim.
/// @param result Successful result payload.
/// @return Owned compact JSON object containing @c jsonrpc, @c id, and @c result.
std::string buildResponse(const JsonValue &id, const JsonValue &result) {
    auto response = JsonValue::object({
        {"jsonrpc", JsonValue("2.0")},
        {"id", id},
        {"result", result},
    });
    return response.toCompactString();
}

/// @brief Serialize a JSON-RPC error response.
/// @param id Request identifier to echo, commonly null when it cannot be recovered.
/// @param code Numeric JSON-RPC or application error code.
/// @param message Human-readable error description.
/// @param data Optional structured details; omitted from the error object when null.
/// @return Owned compact JSON object containing the protocol error response.
std::string buildError(const JsonValue &id,
                       int code,
                       const std::string &message,
                       const JsonValue &data) {
    JsonValue::ObjectType errObj;
    errObj.emplace_back("code", JsonValue(static_cast<int64_t>(code)));
    errObj.emplace_back("message", JsonValue(message));
    if (!data.isNull())
        errObj.emplace_back("data", data);

    auto response = JsonValue::object({
        {"jsonrpc", JsonValue("2.0")},
        {"id", id},
        {"error", JsonValue(std::move(errObj))},
    });
    return response.toCompactString();
}

/// @brief Serialize a JSON-RPC notification.
/// @param method Nonempty notification method name supplied by the caller.
/// @param params Notification parameter payload.
/// @return Owned compact JSON object with no request identifier.
std::string buildNotification(const std::string &method, const JsonValue &params) {
    auto notification = JsonValue::object({
        {"jsonrpc", JsonValue("2.0")},
        {"method", JsonValue(method)},
        {"params", params},
    });
    return notification.toCompactString();
}

} // namespace zanna::server
