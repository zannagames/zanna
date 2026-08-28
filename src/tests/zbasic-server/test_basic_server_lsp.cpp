//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/zbasic-server/test_basic_server_lsp.cpp
// Purpose: Integration tests for LSP protocol handler with BASIC bridge.
// Key invariants:
//   - Tests exercise the LSP lifecycle: initialize → didOpen → features → shutdown
//   - A mock transport captures outgoing notifications (publishDiagnostics)
//   - Feature requests validate response structure against LSP 3.17 spec
// Ownership/Lifetime:
//   - Test-only file
// Links: tools/zbasic-server/BasicCompilerBridge.hpp,
//        tools/lsp-common/LspHandler.hpp
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"
#include "tools/lsp-common/DocumentStore.hpp"
#include "tools/lsp-common/Json.hpp"
#include "tools/lsp-common/JsonRpc.hpp"
#include "tools/lsp-common/LspHandler.hpp"
#include "tools/lsp-common/Transport.hpp"
#include "tools/zbasic-server/BasicCompilerBridge.hpp"

#include <string>
#include <vector>

using namespace zanna::server;

static const ServerConfig kBasicConfig{
    "zbasic-server", "0.1.0", "zbasic", "basic", ".bas", "Zanna BASIC"};

// --- Mock transport that captures written messages ---

class MockTransport : public Transport {
  public:
    std::vector<std::string> written;

    bool readMessage(RawMessage & /*msg*/) override {
        return false;
    }

    void writeMessage(const std::string &message) override {
        written.push_back(message);
    }
};

/// Helper: build a JsonRpcRequest.
static JsonRpcRequest makeReq(const std::string &method,
                              JsonValue params = JsonValue::object({}),
                              JsonValue id = JsonValue(1)) {
    JsonRpcRequest req{method, std::move(params), std::move(id)};
    req.hasId = true;
    return req;
}

/// Helper: build a notification (null id).
static JsonRpcRequest makeNotif(const std::string &method,
                                JsonValue params = JsonValue::object({})) {
    return {method, std::move(params), JsonValue(), false};
}

/// Helper: parse response.
static JsonValue parseResponse(const std::string &resp) {
    EXPECT_TRUE(!resp.empty());
    return JsonValue::parse(resp);
}

/// Helper: drive an LSP handler through initialize and initialized.
/// Feature tests call this before document or request operations so they
/// exercise normal LSP-ready behavior instead of pre-initialization rejection.
static void startLspSession(LspHandler &handler) {
    (void)handler.handleRequest(makeReq("initialize"));
    (void)handler.handleRequest(makeNotif("initialized"));
}

/// Standard valid BASIC source for testing.
static const char *kValidSource = "DIM x AS INTEGER\nPRINT x\nEND\n";

// ===== Lifecycle =====

TEST(BasicLsp, Initialize) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);

    auto resp = parseResponse(handler.handleRequest(makeReq("initialize")));
    EXPECT_EQ(resp["jsonrpc"].asString(), "2.0");

    auto caps = resp["result"]["capabilities"];
    EXPECT_EQ(caps["textDocumentSync"].asInt(), 1);
    EXPECT_TRUE(caps["hoverProvider"].asBool());
    EXPECT_TRUE(caps["documentSymbolProvider"].asBool());
    EXPECT_TRUE(caps.has("completionProvider"));
    EXPECT_FALSE(caps.has("definitionProvider"));
    EXPECT_FALSE(caps.has("referencesProvider"));
    EXPECT_FALSE(caps.has("renameProvider"));
    EXPECT_FALSE(caps.has("signatureHelpProvider"));
    EXPECT_FALSE(caps.has("workspaceSymbolProvider"));
    EXPECT_FALSE(caps.has("semanticTokensProvider"));

    auto info = resp["result"]["serverInfo"];
    EXPECT_EQ(info["name"].asString(), "zbasic-server");
}

TEST(BasicLsp, Shutdown) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);

    auto resp = parseResponse(handler.handleRequest(makeReq("shutdown")));
    EXPECT_EQ(resp["id"].asInt(), 1);
    EXPECT_TRUE(resp["result"].isNull());
}

// ===== Document Sync =====

TEST(BasicLsp, DidOpenPublishesDiagnostics) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);
    startLspSession(handler);

    auto params = JsonValue::object({
        {"textDocument",
         JsonValue::object({
             {"uri", JsonValue("file:///test.bas")},
             {"languageId", JsonValue("basic")},
             {"version", JsonValue(1)},
             {"text", JsonValue(kValidSource)},
         })},
    });
    handler.handleRequest(makeNotif("textDocument/didOpen", std::move(params)));

    // Should have published diagnostics
    EXPECT_TRUE(transport.written.size() > 0u);
    auto diag = JsonValue::parse(transport.written[0]);
    EXPECT_EQ(diag["method"].asString(), "textDocument/publishDiagnostics");
    EXPECT_EQ(diag["params"]["uri"].asString(), "file:///test.bas");
}

TEST(BasicLsp, DidChangeUpdatesDiagnostics) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);
    startLspSession(handler);

    // Open document
    auto openParams = JsonValue::object({
        {"textDocument",
         JsonValue::object({
             {"uri", JsonValue("file:///test.bas")},
             {"languageId", JsonValue("basic")},
             {"version", JsonValue(1)},
             {"text", JsonValue(kValidSource)},
         })},
    });
    handler.handleRequest(makeNotif("textDocument/didOpen", std::move(openParams)));
    transport.written.clear();

    // Change to have an error
    auto changeParams = JsonValue::object({
        {"textDocument",
         JsonValue::object({
             {"uri", JsonValue("file:///test.bas")},
             {"version", JsonValue(2)},
         })},
        {"contentChanges",
         JsonValue::array({JsonValue::object({{"text", JsonValue("PRINT x +\nEND\n")}})})},
    });
    handler.handleRequest(makeNotif("textDocument/didChange", std::move(changeParams)));

    EXPECT_TRUE(transport.written.size() > 0u);
    auto diag = JsonValue::parse(transport.written[0]);
    // Should have diagnostics (errors from the bad source)
    EXPECT_TRUE(diag["params"]["diagnostics"].size() > 0u);
}

TEST(BasicLsp, DidCloseClearsDiagnostics) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);
    startLspSession(handler);

    auto openParams = JsonValue::object({
        {"textDocument",
         JsonValue::object({
             {"uri", JsonValue("file:///test.bas")},
             {"languageId", JsonValue("basic")},
             {"version", JsonValue(1)},
             {"text", JsonValue(kValidSource)},
         })},
    });
    handler.handleRequest(makeNotif("textDocument/didOpen", std::move(openParams)));
    transport.written.clear();

    auto closeParams = JsonValue::object({
        {"textDocument", JsonValue::object({{"uri", JsonValue("file:///test.bas")}})},
    });
    handler.handleRequest(makeNotif("textDocument/didClose", std::move(closeParams)));

    EXPECT_TRUE(transport.written.size() > 0u);
    auto diag = JsonValue::parse(transport.written[0]);
    EXPECT_EQ(diag["params"]["diagnostics"].size(), 0u);
}

// ===== Completion =====

TEST(BasicLsp, CompletionReturnsItems) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);
    startLspSession(handler);

    std::string source = "DIM x AS INTEGER\nPRI\n";
    auto openParams = JsonValue::object({
        {"textDocument",
         JsonValue::object({
             {"uri", JsonValue("file:///test.bas")},
             {"languageId", JsonValue("basic")},
             {"version", JsonValue(1)},
             {"text", JsonValue(source)},
         })},
    });
    handler.handleRequest(makeNotif("textDocument/didOpen", std::move(openParams)));

    // LSP 0-based: line 1, character 3 (after "PRI")
    auto compParams = JsonValue::object({
        {"textDocument", JsonValue::object({{"uri", JsonValue("file:///test.bas")}})},
        {"position", JsonValue::object({{"line", JsonValue(1)}, {"character", JsonValue(3)}})},
    });
    auto resp = parseResponse(
        handler.handleRequest(makeReq("textDocument/completion", std::move(compParams))));

    auto result = resp["result"];
    // Should have PRINT and possibly other PRI-prefixed completions
    EXPECT_TRUE(result.size() > 0u);
}

TEST(BasicLsp, CompletionOnClosedDoc) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);
    startLspSession(handler);

    auto compParams = JsonValue::object({
        {"textDocument", JsonValue::object({{"uri", JsonValue("file:///nonexistent.bas")}})},
        {"position", JsonValue::object({{"line", JsonValue(0)}, {"character", JsonValue(0)}})},
    });
    auto resp = parseResponse(
        handler.handleRequest(makeReq("textDocument/completion", std::move(compParams))));
    EXPECT_EQ(resp["result"].size(), 0u);
}

// ===== Hover =====

TEST(BasicLsp, HoverOnVariable) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);
    startLspSession(handler);

    std::string source = "DIM x AS INTEGER\nPRINT x\nEND\n";
    auto openParams = JsonValue::object({
        {"textDocument",
         JsonValue::object({
             {"uri", JsonValue("file:///test.bas")},
             {"languageId", JsonValue("basic")},
             {"version", JsonValue(1)},
             {"text", JsonValue(source)},
         })},
    });
    handler.handleRequest(makeNotif("textDocument/didOpen", std::move(openParams)));

    // LSP 0-based: line 1, character 6 = 'x' in "PRINT x"
    auto hoverParams = JsonValue::object({
        {"textDocument", JsonValue::object({{"uri", JsonValue("file:///test.bas")}})},
        {"position", JsonValue::object({{"line", JsonValue(1)}, {"character", JsonValue(6)}})},
    });
    auto resp =
        parseResponse(handler.handleRequest(makeReq("textDocument/hover", std::move(hoverParams))));

    EXPECT_FALSE(resp["result"].isNull());
    auto contents = resp["result"]["contents"];
    EXPECT_EQ(contents["kind"].asString(), "markdown");
    auto value = contents["value"].asString();
    // BASIC lexer uppercases identifiers: "x" → "X"
    EXPECT_TRUE(value.find("X") != std::string::npos);
    EXPECT_TRUE(value.find("INTEGER") != std::string::npos);
}

TEST(BasicLsp, HoverOnWhitespaceReturnsNull) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);
    startLspSession(handler);

    std::string source = "    DIM x AS INTEGER\nEND\n";
    auto openParams = JsonValue::object({
        {"textDocument",
         JsonValue::object({
             {"uri", JsonValue("file:///test.bas")},
             {"languageId", JsonValue("basic")},
             {"version", JsonValue(1)},
             {"text", JsonValue(source)},
         })},
    });
    handler.handleRequest(makeNotif("textDocument/didOpen", std::move(openParams)));

    // LSP 0-based: line 0, character 0 = leading whitespace
    auto hoverParams = JsonValue::object({
        {"textDocument", JsonValue::object({{"uri", JsonValue("file:///test.bas")}})},
        {"position", JsonValue::object({{"line", JsonValue(0)}, {"character", JsonValue(0)}})},
    });
    auto resp =
        parseResponse(handler.handleRequest(makeReq("textDocument/hover", std::move(hoverParams))));

    EXPECT_TRUE(resp["result"].isNull());
}

// ===== Document Symbol =====

TEST(BasicLsp, DocumentSymbolsListsVariables) {
    BasicCompilerBridge bridge;
    MockTransport transport;
    LspHandler handler(bridge, transport, kBasicConfig);
    startLspSession(handler);

    auto openParams = JsonValue::object({
        {"textDocument",
         JsonValue::object({
             {"uri", JsonValue("file:///test.bas")},
             {"languageId", JsonValue("basic")},
             {"version", JsonValue(1)},
             {"text", JsonValue(kValidSource)},
         })},
    });
    handler.handleRequest(makeNotif("textDocument/didOpen", std::move(openParams)));

    auto symParams = JsonValue::object({
        {"textDocument", JsonValue::object({{"uri", JsonValue("file:///test.bas")}})},
    });
    auto resp = parseResponse(
        handler.handleRequest(makeReq("textDocument/documentSymbol", std::move(symParams))));

    auto result = resp["result"];
    // BASIC lexer uppercases identifiers: "x" → "X"
    bool foundX = false;
    for (size_t i = 0; i < result.size(); ++i) {
        if (result.at(i)["name"].asString() == "X")
            foundX = true;
    }
    EXPECT_TRUE(foundX);
}

int main(int argc, char **argv) {
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
