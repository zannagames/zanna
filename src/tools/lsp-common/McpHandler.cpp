//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements MCP lifecycle handling, tool schemas, argument validation,
///        and compiler-bridge tool dispatch.
///
/// Tool methods are available only after initialization, use the configured
/// language prefix, and return MCP text content plus structured JSON when the
/// result has a stable machine-readable shape. All results own their storage.
///
/// @see McpHandler.hpp
/// @see ICompilerBridge.hpp
//
//===----------------------------------------------------------------------===//

#include "tools/lsp-common/McpHandler.hpp"

#include <limits>
#include <optional>

namespace zanna::server {
namespace {

// Each validator returns std::nullopt when the input is acceptable, or an error
// message string describing the first problem found.

/// @brief Require @p value to be a JSON object.
/// @param value Candidate JSON value.
/// @param fieldName Human-readable field label used in the error message.
/// @return Empty optional on success, otherwise a validation error.
std::optional<std::string> requireObject(const JsonValue &value, const char *fieldName) {
    if (value.type() != JsonType::Object)
        return std::string(fieldName) + " must be an object";
    return std::nullopt;
}

/// @brief Require @p obj to contain a String member named @p fieldName.
/// @param obj Arguments object to inspect.
/// @param fieldName Required string member name.
/// @return Empty optional on success, otherwise a missing-or-invalid error.
std::optional<std::string> requireString(const JsonValue &obj, const char *fieldName) {
    const auto *value = obj.get(fieldName);
    if (!value || value->type() != JsonType::String)
        return std::string("Missing or invalid '") + fieldName + "'";
    return std::nullopt;
}

/// @brief Require @p obj to contain an Int member named @p fieldName within bounds.
/// @details MCP callers can provide arbitrary JSON integers. This helper keeps
///          line/column arguments inside the int range consumed by compiler
///          bridges and rejects non-positive cursor positions up front.
/// @param obj Arguments object to inspect.
/// @param fieldName Required integer member name.
/// @param minValue Inclusive lower bound.
/// @param maxValue Inclusive upper bound.
/// @return Empty optional on success, otherwise a validation error string.
std::optional<std::string> requireIntInRange(const JsonValue &obj,
                                             const char *fieldName,
                                             int64_t minValue,
                                             int64_t maxValue) {
    const auto *value = obj.get(fieldName);
    if (!value || value->type() != JsonType::Int)
        return std::string("Missing or invalid '") + fieldName + "'";
    const int64_t raw = value->asInt();
    if (raw < minValue || raw > maxValue)
        return std::string("'") + fieldName + "' is out of range";
    return std::nullopt;
}

/// @brief Require @p obj to contain a non-empty String member named @p fieldName.
/// @details Used for search-like tools where an empty string is technically a
///          string but would expand to the whole catalog and produce noisy output.
/// @param obj Arguments object to inspect.
/// @param fieldName Required member name.
/// @return Empty optional on success, otherwise a validation error string.
std::optional<std::string> requireNonEmptyString(const JsonValue &obj, const char *fieldName) {
    if (auto err = requireString(obj, fieldName))
        return err;
    if (obj[fieldName].asString().empty())
        return std::string("'") + fieldName + "' must not be empty";
    return std::nullopt;
}

/// @brief Allow @p fieldName to be absent, but if present it must be a String.
/// @param obj Arguments object to inspect.
/// @param fieldName Optional string member name.
/// @return Empty optional when absent or valid, otherwise a validation error.
std::optional<std::string> requireOptionalString(const JsonValue &obj, const char *fieldName) {
    const auto *value = obj.get(fieldName);
    if (value && value->type() != JsonType::String)
        return std::string("Invalid optional '") + fieldName + "'";
    return std::nullopt;
}

/// @brief Allow @p fieldName to be absent, but if present it must be a Bool.
/// @param obj Arguments object to inspect.
/// @param fieldName Optional Boolean member name.
/// @return Empty optional when absent or valid, otherwise a validation error.
std::optional<std::string> requireOptionalBool(const JsonValue &obj, const char *fieldName) {
    const auto *value = obj.get(fieldName);
    if (value && value->type() != JsonType::Bool)
        return std::string("Invalid optional '") + fieldName + "'";
    return std::nullopt;
}

/// @brief Validate the args common to source-taking tools: object with a "source"
///        string and an optional "path" string.
/// @param args Candidate tool arguments.
/// @return Empty optional when valid, otherwise the first validation error.
std::optional<std::string> validateCommonSourceArgs(const JsonValue &args) {
    if (auto err = requireObject(args, "arguments"))
        return err;
    if (auto err = requireString(args, "source"))
        return err;
    return requireOptionalString(args, "path");
}

/// @brief Validate position-taking tool args: the common source args plus integer
///        "line" and "col" members.
/// @param args Candidate tool arguments.
/// @return Empty optional when valid, otherwise the first validation error.
std::optional<std::string> validatePositionArgs(const JsonValue &args) {
    if (auto err = validateCommonSourceArgs(args))
        return err;
    if (auto err = requireIntInRange(args, "line", 1, std::numeric_limits<int>::max()))
        return err;
    return requireIntInRange(args, "col", 1, std::numeric_limits<int>::max());
}

/// @brief Return true for MCP methods that are defined as request/response calls.
/// @details JSON-RPC notifications do not receive responses even when they use a
///          request method name. Dispatch uses this helper to avoid returning
///          `id: null` responses for notification-shaped initialize, ping, and
///          tool calls.
/// @param method Incoming MCP method name.
/// @return @c true when the method normally expects a response.
bool isRequestMethod(const std::string &method) {
    return method == "initialize" || method == "tools/list" || method == "tools/call" ||
           method == "ping";
}

} // namespace

/// @brief Construct an MCP handler for one language server.
/// @param bridge Compiler adapter borrowed for the handler lifetime.
/// @param config Language and naming settings copied into the handler.
McpHandler::McpHandler(ICompilerBridge &bridge, const ServerConfig &config)
    : bridge_(bridge), config_(config) {}

// --- Request dispatch ---

/// @brief Dispatch one parsed JSON-RPC message through the MCP lifecycle.
/// @param req Request or notification to process.
/// @return Serialized response for requests, or an empty string for notifications.
std::string McpHandler::handleRequest(const JsonRpcRequest &req) {
    if (req.isNotification() && isRequestMethod(req.method))
        return {};

    if (req.method == "initialize")
        return handleInitialize(req);

    if (req.method == "initialized") {
        if (initializeResponded_)
            initialized_ = true;
        return {}; // Notification — no response
    }

    if (!initializeResponded_ && (req.method == "tools/list" || req.method == "tools/call")) {
        return buildError(req.id, kInvalidRequest, "Server has not been initialized");
    }

    if (req.method == "tools/list")
        return handleToolsList(req);

    if (req.method == "tools/call")
        return handleToolsCall(req);

    if (req.method == "ping")
        return buildResponse(req.id, JsonValue::object({}));

    if (req.isNotification())
        return {}; // Unknown notification — silently ignore

    return buildError(req.id, kMethodNotFound, "Method not found: " + req.method);
}

// --- Lifecycle ---

/// @brief Negotiate the supported MCP protocol and tool capability.
/// @param req Initialize request whose identifier is echoed.
/// @return Response with protocol version, capabilities, and server information.
std::string McpHandler::handleInitialize(const JsonRpcRequest &req) {
    initializeResponded_ = true;
    auto result = JsonValue::object({
        {"protocolVersion", JsonValue("2024-11-05")},
        {"capabilities", JsonValue::object({{"tools", JsonValue::object({})}})},
        {"serverInfo",
         JsonValue::object(
             {{"name", JsonValue(config_.serverName)}, {"version", JsonValue(config_.version)}})},
    });
    return buildResponse(req.id, result);
}

// --- tools/list ---

/// @brief Return all configured MCP compiler-tool definitions.
/// @param req Tools-list request whose identifier is echoed.
/// @return Response containing JSON Schema definitions for each tool.
std::string McpHandler::handleToolsList(const JsonRpcRequest &req) {
    auto result = JsonValue::object({{"tools", buildToolDefinitions()}});
    return buildResponse(req.id, result);
}

// --- Tool schema helpers ---

/// @brief Build a JSON Schema property definition.
/// @param type JSON Schema type name.
/// @param desc Human-readable property description.
/// @return Object containing @c type and @c description.
static JsonValue schemaProp(const char *type, const char *desc) {
    return JsonValue::object({{"type", JsonValue(type)}, {"description", JsonValue(desc)}});
}

/// @brief Build a complete MCP tool definition object.
/// @param name Fully prefixed tool name.
/// @param desc Human-readable tool description.
/// @param properties Input-schema properties moved into the result.
/// @param required Required property names moved into schema order.
/// @return Tool object with a closed object input schema.
static JsonValue toolDef(const std::string &name,
                         const std::string &desc,
                         JsonValue::ObjectType properties,
                         std::vector<std::string> required = {}) {
    JsonValue::ArrayType reqArr;
    reqArr.reserve(required.size());
    for (auto &r : required)
        reqArr.push_back(JsonValue(std::move(r)));

    auto schema = JsonValue::object({
        {"type", JsonValue("object")},
        {"properties", JsonValue(std::move(properties))},
        {"additionalProperties", JsonValue(false)},
    });

    // Only add "required" if non-empty
    if (!reqArr.empty()) {
        auto schemaObj = schema.asObject();
        schemaObj.push_back({"required", JsonValue(std::move(reqArr))});
        schema = JsonValue(std::move(schemaObj));
    }

    return JsonValue::object({
        {"name", JsonValue(name)},
        {"description", JsonValue(desc)},
        {"inputSchema", std::move(schema)},
    });
}

/// @brief Build the configured language server's MCP tool catalog.
/// @return Ordered array of compiler, inspection, and runtime-query schemas.
JsonValue McpHandler::buildToolDefinitions() const {
    JsonValue::ArrayType tools;
    const std::string &prefix = config_.toolPrefix;
    const std::string &lang = config_.langLabel;
    std::string sourceDesc = lang + " source code";

    // <prefix>/check
    tools.push_back(toolDef(prefix + "/check",
                            "Type-check " + lang + " source code and return diagnostics",
                            {
                                {"source", schemaProp("string", sourceDesc.c_str())},
                                {"path", schemaProp("string", "Virtual file path (optional)")},
                            },
                            {"source"}));

    // <prefix>/compile
    tools.push_back(
        toolDef(prefix + "/compile",
                "Compile " + lang + " source code and return success status + diagnostics",
                {
                    {"source", schemaProp("string", sourceDesc.c_str())},
                    {"path", schemaProp("string", "Virtual file path (optional)")},
                },
                {"source"}));

    // <prefix>/completions
    tools.push_back(toolDef(prefix + "/completions",
                            "Get code completions at a cursor position in " + lang + " source",
                            {
                                {"source", schemaProp("string", sourceDesc.c_str())},
                                {"line", schemaProp("integer", "Cursor line (1-based)")},
                                {"col", schemaProp("integer", "Cursor column (1-based)")},
                                {"path", schemaProp("string", "Virtual file path (optional)")},
                            },
                            {"source", "line", "col"}));

    // <prefix>/hover
    tools.push_back(toolDef(prefix + "/hover",
                            "Get type information for a symbol at a cursor position",
                            {
                                {"source", schemaProp("string", sourceDesc.c_str())},
                                {"line", schemaProp("integer", "Cursor line (1-based)")},
                                {"col", schemaProp("integer", "Cursor column (1-based)")},
                                {"path", schemaProp("string", "Virtual file path (optional)")},
                            },
                            {"source", "line", "col"}));

    // <prefix>/symbols
    tools.push_back(toolDef(prefix + "/symbols",
                            "List all top-level declarations in " + lang + " source",
                            {
                                {"source", schemaProp("string", sourceDesc.c_str())},
                                {"path", schemaProp("string", "Virtual file path (optional)")},
                            },
                            {"source"}));

    // <prefix>/dump-il
    tools.push_back(
        toolDef(prefix + "/dump-il",
                "Dump the compiled IL (intermediate language) for " + lang + " source",
                {
                    {"source", schemaProp("string", sourceDesc.c_str())},
                    {"path", schemaProp("string", "Virtual file path (optional)")},
                    {"optimized", schemaProp("boolean", "Apply O1 optimization (default: false)")},
                },
                {"source"}));

    // <prefix>/dump-ast
    tools.push_back(toolDef(prefix + "/dump-ast",
                            "Dump the abstract syntax tree for " + lang + " source",
                            {
                                {"source", schemaProp("string", sourceDesc.c_str())},
                                {"path", schemaProp("string", "Virtual file path (optional)")},
                            },
                            {"source"}));

    // <prefix>/dump-tokens
    tools.push_back(toolDef(prefix + "/dump-tokens",
                            "Dump the token stream for " + lang + " source",
                            {
                                {"source", schemaProp("string", sourceDesc.c_str())},
                                {"path", schemaProp("string", "Virtual file path (optional)")},
                            },
                            {"source"}));

    // <prefix>/runtime-classes
    tools.push_back(toolDef(prefix + "/runtime-classes",
                            "List all Zanna runtime classes with method and property counts",
                            {},
                            {}));

    // <prefix>/runtime-methods
    tools.push_back(toolDef(prefix + "/runtime-methods",
                            "List methods and properties for a specific Zanna runtime class",
                            {
                                {"className",
                                 schemaProp("string",
                                            "Fully qualified class name (e.g., "
                                            "\"Zanna.Terminal\")")},
                            },
                            {"className"}));

    // <prefix>/runtime-search
    tools.push_back(
        toolDef(prefix + "/runtime-search",
                "Search Zanna runtime APIs by keyword (case-insensitive substring match)",
                {
                    {"keyword", schemaProp("string", "Search keyword")},
                },
                {"keyword"}));

    return JsonValue(std::move(tools));
}

// --- tools/call dispatch ---

/// @brief Build an MCP text content response.
/// @param text Text payload to copy.
/// @return One-element MCP content array containing a text block.
static JsonValue textContent(const std::string &text) {
    return JsonValue::array({JsonValue::object({
        {"type", JsonValue("text")},
        {"text", JsonValue(text)},
    })});
}

/// @brief Validate and dispatch an MCP @c tools/call request.
/// @param req Request containing a configured tool name and arguments object.
/// @return Tool response with text and optional structured content, or a
///         JSON-RPC error for invalid arguments or an unknown tool.
std::string McpHandler::handleToolsCall(const JsonRpcRequest &req) {
    if (auto err = requireObject(req.params, "params"))
        return buildError(req.id, kInvalidParams, *err);

    const auto *nameProp = req.params.get("name");
    if (!nameProp || nameProp->type() != JsonType::String)
        return buildError(req.id, kInvalidParams, "Missing or invalid 'name' in tools/call");

    std::string name = nameProp->asString();
    const auto *argsProp = req.params.get("arguments");
    JsonValue emptyArgs = JsonValue::object({});
    const JsonValue &args = argsProp ? *argsProp : emptyArgs;
    if (args.type() != JsonType::Object)
        return buildError(req.id, kInvalidParams, "'arguments' must be an object");

    // Build expected tool names from config prefix
    const std::string &p = config_.toolPrefix;

    ToolCallResult toolResult;

    std::optional<std::string> argError;
    if (name == p + "/check") {
        argError = validateCommonSourceArgs(args);
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callCheck(args);
    } else if (name == p + "/compile") {
        argError = validateCommonSourceArgs(args);
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callCompile(args);
    } else if (name == p + "/completions") {
        argError = validatePositionArgs(args);
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callCompletions(args);
    } else if (name == p + "/hover") {
        argError = validatePositionArgs(args);
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callHover(args);
    } else if (name == p + "/symbols") {
        argError = validateCommonSourceArgs(args);
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callSymbols(args);
    } else if (name == p + "/dump-il") {
        argError = validateCommonSourceArgs(args);
        if (!argError)
            argError = requireOptionalBool(args, "optimized");
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callDumpIL(args);
    } else if (name == p + "/dump-ast") {
        argError = validateCommonSourceArgs(args);
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callDumpAst(args);
    } else if (name == p + "/dump-tokens") {
        argError = validateCommonSourceArgs(args);
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callDumpTokens(args);
    } else if (name == p + "/runtime-classes") {
        if (args.size() != 0)
            return buildError(req.id, kInvalidParams, "runtime-classes does not accept arguments");
        toolResult = callRuntimeClasses(args);
    } else if (name == p + "/runtime-methods") {
        argError = requireString(args, "className");
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callRuntimeMembers(args);
    } else if (name == p + "/runtime-search") {
        argError = requireNonEmptyString(args, "keyword");
        if (argError)
            return buildError(req.id, kInvalidParams, *argError);
        toolResult = callRuntimeSearch(args);
    } else {
        return buildError(req.id, kMethodNotFound, "Unknown tool: " + name);
    }

    JsonValue::ObjectType resultMembers;
    resultMembers.push_back({"content", std::move(toolResult.content)});
    if (toolResult.structuredContent)
        resultMembers.push_back({"structuredContent", std::move(*toolResult.structuredContent)});
    auto result = JsonValue::object(std::move(resultMembers));
    return buildResponse(req.id, result);
}

// --- Tool implementations ---

/// @brief Serialize one DiagnosticInfo into the MCP diagnostic JSON shape.
/// @details The shape is stable for machine consumers: endLine/endColumn are 0
///          when no concrete range is known, and notes/fixits are always
///          present (possibly empty arrays).
/// @param d Compiler diagnostic to serialize.
/// @return Structured diagnostic with location, metadata, notes, and fix-its.
static JsonValue diagnosticToJson(const DiagnosticInfo &d) {
    JsonValue::ArrayType noteArr;
    noteArr.reserve(d.notes.size());
    for (const auto &n : d.notes) {
        noteArr.push_back(JsonValue::object({
            {"message", JsonValue(n.message)},
            {"file", JsonValue(n.file)},
            {"line", JsonValue(static_cast<int64_t>(n.line))},
            {"column", JsonValue(static_cast<int64_t>(n.column))},
        }));
    }

    JsonValue::ArrayType fixArr;
    fixArr.reserve(d.fixits.size());
    for (const auto &f : d.fixits) {
        fixArr.push_back(JsonValue::object({
            {"message", JsonValue(f.message)},
            {"replacement", JsonValue(f.replacement)},
            {"line", JsonValue(static_cast<int64_t>(f.line))},
            {"column", JsonValue(static_cast<int64_t>(f.column))},
            {"endLine", JsonValue(static_cast<int64_t>(f.endLine))},
            {"endColumn", JsonValue(static_cast<int64_t>(f.endColumn))},
        }));
    }

    return JsonValue::object({
        {"severity", JsonValue(d.severity)},
        {"message", JsonValue(d.message)},
        {"line", JsonValue(static_cast<int64_t>(d.line))},
        {"column", JsonValue(static_cast<int64_t>(d.column))},
        {"code", JsonValue(d.code)},
        {"endLine", JsonValue(static_cast<int64_t>(d.endLine))},
        {"endColumn", JsonValue(static_cast<int64_t>(d.endColumn))},
        {"stage", JsonValue(d.stage)},
        {"help", JsonValue(d.help)},
        {"notes", JsonValue(std::move(noteArr))},
        {"fixits", JsonValue(std::move(fixArr))},
    });
}

/// @brief Run the bridge's source-check operation.
/// @param args Validated source and optional path arguments.
/// @return Text and structured diagnostic payloads.
McpHandler::ToolCallResult McpHandler::callCheck(const JsonValue &args) {
    std::string source = args["source"].asString();
    std::string path = args.has("path") ? args["path"].asString() : "untitled" + config_.defaultExt;

    auto diags = bridge_.check(source, path);

    JsonValue::ArrayType diagArr;
    diagArr.reserve(diags.size());
    for (const auto &d : diags)
        diagArr.push_back(diagnosticToJson(d));

    // MCP requires structuredContent to be a JSON object, not a bare array.
    JsonValue structured = JsonValue::object({{"diagnostics", JsonValue(std::move(diagArr))}});
    return {textContent(structured.toCompactString()), std::move(structured)};
}

/// @brief Compile source and report success plus diagnostics.
/// @param args Validated source and optional path arguments.
/// @return Text and structured compilation-result payloads.
McpHandler::ToolCallResult McpHandler::callCompile(const JsonValue &args) {
    std::string source = args["source"].asString();
    std::string path = args.has("path") ? args["path"].asString() : "untitled" + config_.defaultExt;

    auto result = bridge_.compile(source, path);

    JsonValue::ArrayType diagArr;
    diagArr.reserve(result.diagnostics.size());
    for (const auto &d : result.diagnostics)
        diagArr.push_back(diagnosticToJson(d));

    auto obj = JsonValue::object({
        {"succeeded", JsonValue(result.succeeded)},
        {"diagnostics", JsonValue(std::move(diagArr))},
    });
    return {textContent(obj.toCompactString()), std::move(obj)};
}

/// @brief Query bridge completions at a validated source position.
/// @param args Source, optional path, and one-based line/column arguments.
/// @return Text and structured completion-list payloads.
McpHandler::ToolCallResult McpHandler::callCompletions(const JsonValue &args) {
    std::string source = args["source"].asString();
    int line = static_cast<int>(args["line"].asInt());
    int col = static_cast<int>(args["col"].asInt());
    std::string path = args.has("path") ? args["path"].asString() : "untitled" + config_.defaultExt;

    auto items = bridge_.completions(source, line, col, path);

    JsonValue::ArrayType arr;
    arr.reserve(items.size());
    for (const auto &item : items) {
        arr.push_back(JsonValue::object({
            {"label", JsonValue(item.label)},
            {"insertText", JsonValue(item.insertText)},
            {"kind", JsonValue(item.kind)},
            {"detail", JsonValue(item.detail)},
            {"documentation", JsonValue(item.documentation)},
        }));
    }

    JsonValue structured = JsonValue::object({{"completions", JsonValue(std::move(arr))}});
    return {textContent(structured.toCompactString()), std::move(structured)};
}

/// @brief Query bridge hover text at a validated source position.
/// @param args Source, optional path, and one-based line/column arguments.
/// @return Text content containing hover information or a no-information marker.
McpHandler::ToolCallResult McpHandler::callHover(const JsonValue &args) {
    std::string source = args["source"].asString();
    int line = static_cast<int>(args["line"].asInt());
    int col = static_cast<int>(args["col"].asInt());
    std::string path = args.has("path") ? args["path"].asString() : "untitled" + config_.defaultExt;

    auto result = bridge_.hover(source, line, col, path);
    return {textContent(result.empty() ? "(no type information)" : result), std::nullopt};
}

/// @brief Enumerate top-level symbols in source text.
/// @param args Validated source and optional path arguments.
/// @return Text and structured symbol-list payloads.
McpHandler::ToolCallResult McpHandler::callSymbols(const JsonValue &args) {
    std::string source = args["source"].asString();
    std::string path = args.has("path") ? args["path"].asString() : "untitled" + config_.defaultExt;

    auto syms = bridge_.symbols(source, path);

    JsonValue::ArrayType arr;
    arr.reserve(syms.size());
    for (const auto &s : syms) {
        arr.push_back(JsonValue::object({
            {"name", JsonValue(s.name)},
            {"kind", JsonValue(s.kind)},
            {"type", JsonValue(s.type)},
            {"line", JsonValue(static_cast<int64_t>(s.line))},
            {"column", JsonValue(static_cast<int64_t>(s.column))},
        }));
    }

    JsonValue structured = JsonValue::object({{"symbols", JsonValue(std::move(arr))}});
    return {textContent(structured.toCompactString()), std::move(structured)};
}

/// @brief Produce the bridge's intermediate-language dump.
/// @param args Source, optional path, and optional optimization flag.
/// @return Text-only MCP content containing the IL dump.
McpHandler::ToolCallResult McpHandler::callDumpIL(const JsonValue &args) {
    std::string source = args["source"].asString();
    std::string path = args.has("path") ? args["path"].asString() : "untitled" + config_.defaultExt;
    bool optimized = args.has("optimized") ? args["optimized"].asBool() : false;

    return {textContent(bridge_.dumpIL(source, path, optimized)), std::nullopt};
}

/// @brief Produce the bridge's abstract-syntax-tree dump.
/// @param args Validated source and optional path arguments.
/// @return Text-only MCP content containing the AST dump.
McpHandler::ToolCallResult McpHandler::callDumpAst(const JsonValue &args) {
    std::string source = args["source"].asString();
    std::string path = args.has("path") ? args["path"].asString() : "untitled" + config_.defaultExt;

    return {textContent(bridge_.dumpAst(source, path)), std::nullopt};
}

/// @brief Produce the bridge's lexical-token dump.
/// @param args Validated source and optional path arguments.
/// @return Text-only MCP content containing the token dump.
McpHandler::ToolCallResult McpHandler::callDumpTokens(const JsonValue &args) {
    std::string source = args["source"].asString();
    std::string path = args.has("path") ? args["path"].asString() : "untitled" + config_.defaultExt;

    return {textContent(bridge_.dumpTokens(source, path)), std::nullopt};
}

/// @brief List registered runtime classes and their member counts.
/// @param args Validated empty arguments object; its contents are unused.
/// @return Text and structured runtime-class payloads.
McpHandler::ToolCallResult McpHandler::callRuntimeClasses(const JsonValue & /*args*/) {
    auto classes = bridge_.runtimeClasses();

    JsonValue::ArrayType arr;
    arr.reserve(classes.size());
    for (const auto &cls : classes) {
        arr.push_back(JsonValue::object({
            {"qname", JsonValue(cls.qname)},
            {"propertyCount", JsonValue(cls.propertyCount)},
            {"methodCount", JsonValue(cls.methodCount)},
            {"documentation",
             JsonValue::object({{"summary", JsonValue(cls.summary)},
                                {"details", JsonValue(cls.details)},
                                {"format", JsonValue("markdown")}})},
        }));
    }

    JsonValue structured = JsonValue::object({{"classes", JsonValue(std::move(arr))}});
    return {textContent(structured.toCompactString()), std::move(structured)};
}

/// @brief List members of one registered runtime class.
/// @param args Validated object containing @c className.
/// @return Text and structured runtime-member payloads.
McpHandler::ToolCallResult McpHandler::callRuntimeMembers(const JsonValue &args) {
    std::string className = args["className"].asString();
    auto members = bridge_.runtimeMembers(className);

    JsonValue::ArrayType arr;
    arr.reserve(members.size());
    for (const auto &m : members) {
        arr.push_back(JsonValue::object({
            {"name", JsonValue(m.name)},
            {"memberKind", JsonValue(m.memberKind)},
            {"signature", JsonValue(m.signature)},
        }));
    }

    JsonValue structured = JsonValue::object({{"members", JsonValue(std::move(arr))}});
    return {textContent(structured.toCompactString()), std::move(structured)};
}

/// @brief Search registered runtime APIs by keyword.
/// @param args Validated object containing a nonempty @c keyword.
/// @return Text and structured runtime-search payloads.
McpHandler::ToolCallResult McpHandler::callRuntimeSearch(const JsonValue &args) {
    std::string keyword = args["keyword"].asString();
    auto results = bridge_.runtimeSearch(keyword);

    JsonValue::ArrayType arr;
    arr.reserve(results.size());
    for (const auto &r : results) {
        arr.push_back(JsonValue::object({
            {"name", JsonValue(r.name)},
            {"memberKind", JsonValue(r.memberKind)},
            {"signature", JsonValue(r.signature)},
        }));
    }

    JsonValue structured = JsonValue::object({{"results", JsonValue(std::move(arr))}});
    return {textContent(structured.toCompactString()), std::move(structured)};
}

} // namespace zanna::server
