//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/zbasic-server/test_basic_server_mcp.cpp
// Purpose: Integration tests for MCP protocol handler with BASIC bridge.
// Key invariants:
//   - Tests exercise the full MCP lifecycle: initialize → tools/list → tools/call
//   - Tool dispatch uses "basic/" prefix for all tool names
//   - Response structure matches MCP 2024-11-05 specification
// Ownership/Lifetime:
//   - Test-only file
// Links: tools/zbasic-server/BasicCompilerBridge.hpp,
//        tools/lsp-common/McpHandler.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"
#include "tools/lsp-common/Json.hpp"
#include "tools/lsp-common/JsonRpc.hpp"
#include "tools/lsp-common/McpHandler.hpp"
#include "tools/zbasic-server/BasicCompilerBridge.hpp"

#include <string>

using namespace zanna::server;

static const ServerConfig kBasicConfig{
    "zbasic-server", "0.1.0", "zbasic", "basic", ".bas", "Zanna BASIC"};

/// Helper: build a JsonRpcRequest from method, params, and id.
static JsonRpcRequest makeReq(const std::string &method,
                              JsonValue params = JsonValue::object({}),
                              JsonValue id = JsonValue(1)) {
    JsonRpcRequest req{method, std::move(params), std::move(id)};
    req.hasId = true;
    return req;
}

/// Helper: parse a JSON-RPC response string and return the parsed JSON.
static JsonValue parseResponse(const std::string &resp) {
    EXPECT_TRUE(!resp.empty());
    return JsonValue::parse(resp);
}

/// Helper: drive the handler through the MCP lifecycle until tools may be used.
/// The shared MCP handler gates `tools/list` and `tools/call` until it has
/// received the post-initialize `initialized` notification, matching the
/// protocol sequence used by real clients.
static void startMcpSession(McpHandler &handler) {
    (void)handler.handleRequest(makeReq("initialize"));
    (void)handler.handleRequest({"initialized", JsonValue::object({}), JsonValue()});
}

// ===== Lifecycle =====

TEST(BasicMcp, Initialize) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    handler.handleRequest(makeReq("initialize"));

    auto resp = parseResponse(handler.handleRequest(makeReq("initialize")));
    EXPECT_EQ(resp["jsonrpc"].asString(), "2.0");
    EXPECT_EQ(resp["id"].asInt(), 1);

    auto result = resp["result"];
    EXPECT_EQ(result["protocolVersion"].asString(), "2024-11-05");
    EXPECT_EQ(result["serverInfo"]["name"].asString(), "zbasic-server");
    EXPECT_TRUE(result["capabilities"].has("tools"));
}

TEST(BasicMcp, InitializedNotification) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    handler.handleRequest(makeReq("initialize"));

    auto resp = handler.handleRequest({"initialized", JsonValue::object({}), JsonValue()});
    EXPECT_TRUE(resp.empty());
}

TEST(BasicMcp, Ping) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto resp = parseResponse(handler.handleRequest(makeReq("ping")));
    EXPECT_EQ(resp["id"].asInt(), 1);
    EXPECT_TRUE(resp.has("result"));
}

TEST(BasicMcp, UnknownMethod) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto resp = parseResponse(handler.handleRequest(makeReq("nonexistent/method")));
    EXPECT_TRUE(resp.has("error"));
    EXPECT_EQ(resp["error"]["code"].asInt(), kMethodNotFound);
}

// ===== tools/list =====

TEST(BasicMcp, ToolsListReturns11Tools) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto resp = parseResponse(handler.handleRequest(makeReq("tools/list")));
    auto tools = resp["result"]["tools"];
    EXPECT_EQ(tools.size(), 11u);
}

TEST(BasicMcp, ToolsListHasAllBasicTools) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto resp = parseResponse(handler.handleRequest(makeReq("tools/list")));
    auto tools = resp["result"]["tools"].asArray();

    std::vector<std::string> expected = {
        "basic/check",
        "basic/compile",
        "basic/completions",
        "basic/hover",
        "basic/symbols",
        "basic/dump-il",
        "basic/dump-ast",
        "basic/dump-tokens",
        "basic/runtime-classes",
        "basic/runtime-methods",
        "basic/runtime-search",
    };

    for (const auto &name : expected) {
        bool found = false;
        for (const auto &tool : tools) {
            if (tool["name"].asString() == name)
                found = true;
        }
        EXPECT_TRUE(found);
    }
}

TEST(BasicMcp, ToolsListDescriptionsUseBasicLabel) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto resp = parseResponse(handler.handleRequest(makeReq("tools/list")));
    auto tools = resp["result"]["tools"].asArray();

    // The check tool description should mention "Zanna BASIC"
    for (const auto &tool : tools) {
        if (tool["name"].asString() == "basic/check") {
            auto desc = tool["description"].asString();
            EXPECT_TRUE(desc.find("Zanna BASIC") != std::string::npos);
        }
    }
}

// ===== tools/call =====

TEST(BasicMcp, ToolsCallCheck) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto params = JsonValue::object({
        {"name", JsonValue("basic/check")},
        {"arguments",
         JsonValue::object({
             {"source", JsonValue("PRINT 42\nEND\n")},
         })},
    });
    auto resp = parseResponse(handler.handleRequest(makeReq("tools/call", std::move(params))));
    EXPECT_TRUE(resp.has("result"));
    auto content = resp["result"]["content"];
    EXPECT_TRUE(content.size() > 0u);
    EXPECT_EQ(content.at(0)["type"].asString(), "text");
}

TEST(BasicMcp, ToolsCallCompile) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto params = JsonValue::object({
        {"name", JsonValue("basic/compile")},
        {"arguments",
         JsonValue::object({
             {"source", JsonValue("PRINT 42\nEND\n")},
         })},
    });
    auto resp = parseResponse(handler.handleRequest(makeReq("tools/call", std::move(params))));
    auto text = resp["result"]["content"].at(0)["text"].asString();
    auto parsed = JsonValue::parse(text);
    EXPECT_TRUE(parsed["succeeded"].asBool());
}

TEST(BasicMcp, ToolsCallDumpTokens) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto params = JsonValue::object({
        {"name", JsonValue("basic/dump-tokens")},
        {"arguments",
         JsonValue::object({
             {"source", JsonValue("PRINT 42\n")},
         })},
    });
    auto resp = parseResponse(handler.handleRequest(makeReq("tools/call", std::move(params))));
    auto text = resp["result"]["content"].at(0)["text"].asString();
    EXPECT_TRUE(!text.empty());
}

TEST(BasicMcp, ToolsCallRuntimeClasses) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto params = JsonValue::object({
        {"name", JsonValue("basic/runtime-classes")},
        {"arguments", JsonValue::object({})},
    });
    auto resp = parseResponse(handler.handleRequest(makeReq("tools/call", std::move(params))));
    auto text = resp["result"]["content"].at(0)["text"].asString();
    auto parsed = JsonValue::parse(text);
    EXPECT_TRUE(parsed.size() > 0u);
}

TEST(BasicMcp, ToolsCallUnknownTool) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto params = JsonValue::object({
        {"name", JsonValue("nonexistent/tool")},
        {"arguments", JsonValue::object({})},
    });
    auto resp = parseResponse(handler.handleRequest(makeReq("tools/call", std::move(params))));
    EXPECT_TRUE(resp.has("error"));
}

TEST(BasicMcp, ToolsCallMissingName) {
    BasicCompilerBridge bridge;
    McpHandler handler(bridge, kBasicConfig);
    startMcpSession(handler);

    auto resp = parseResponse(handler.handleRequest(makeReq("tools/call", JsonValue::object({}))));
    EXPECT_TRUE(resp.has("error"));
    EXPECT_EQ(resp["error"]["code"].asInt(), kInvalidParams);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
