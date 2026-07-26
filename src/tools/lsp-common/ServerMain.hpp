//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Defines the shared stdio entry-point runner for Zanna language servers.
///
/// The runner parses common protocol flags, owns a language-specific bridge and
/// transport for the process lifetime, and invokes the MCP or LSP event loop.
/// MCP uses newline-delimited JSON-RPC; LSP uses Content-Length framing.
/// Auto-detection accepts only a leading JSON-object byte or Content-Length
/// header and never consumes the byte used to select the protocol.
///
/// @see LspHandler.hpp
/// @see McpHandler.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "tools/lsp-common/Json.hpp"
#include "tools/lsp-common/JsonRpc.hpp"
#include "tools/lsp-common/LspHandler.hpp"
#include "tools/lsp-common/McpHandler.hpp"
#include "tools/lsp-common/Transport.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>

namespace zanna::server {

/// @brief Protocol mode selected explicitly or inferred from standard input.
enum class LanguageServerMode {
    Mcp,        ///< Serve newline-delimited MCP JSON-RPC.
    Lsp,        ///< Serve Content-Length framed LSP JSON-RPC.
    AutoDetect, ///< Inspect the first byte of stdin and choose MCP or LSP.
};

/// @brief Print common language-server usage text.
/// @param executableName Name shown in the usage synopsis.
/// @param out C stream receiving the help text.
inline void printLanguageServerUsage(const char *executableName, FILE *out) {
    std::fprintf(out,
                 "Usage: %s [--mcp | --lsp | --help | --version]\n"
                 "\n"
                 "  --mcp      Serve MCP protocol (newline-delimited JSON-RPC)\n"
                 "  --lsp      Serve LSP protocol (Content-Length framed JSON-RPC)\n"
                 "  --help     Show this help message\n"
                 "  --version  Show version\n"
                 "\n"
                 "Default: auto-detect protocol from the first input byte.\n",
                 executableName);
}

/// @brief Print a language-server version banner.
/// @param executableName Server executable name.
/// @param config Server configuration carrying the version string.
/// @param out C stream receiving the banner.
inline void printLanguageServerVersion(const char *executableName,
                                       const ServerConfig &config,
                                       FILE *out) {
    std::fprintf(out, "%s %s\n", executableName, config.version.c_str());
}

/// @brief Peek at the first protocol byte without consuming it.
/// @details Auto-detection is deliberately strict. LSP starts with
///          `Content-Length`, while MCP stdio messages are JSON objects and must
///          start with `{`. Leading bytes are not discarded, so malformed
///          clients get a clear error instead of being routed to the wrong
///          protocol reader.
/// @return First input byte or EOF when stdin has no data.
inline int peekLanguageServerProtocolByte() {
    const int c = std::fgetc(stdin);
    if (c != EOF)
        std::ungetc(c, stdin);
    return c;
}

/// @brief Run the MCP event loop for a concrete compiler bridge.
/// @tparam Bridge Language-specific compiler bridge type.
/// @param transport Transport used for framed message I/O.
/// @param bridge Bridge backing MCP tool implementations.
/// @param config Server metadata and language labels.
/// @return Process exit status.
template <typename Bridge>
int runLanguageMcpServer(Transport &transport, Bridge &bridge, const ServerConfig &config) {
    McpHandler handler(bridge, config);
    RawMessage msg;

    while (transport.readMessage(msg)) {
        JsonValue json;
        try {
            json = JsonValue::parse(msg.content);
        } catch (const std::exception &) {
            transport.writeMessage(buildError(JsonValue(), kParseError, "Parse error"));
            continue;
        }

        JsonRpcRequest req;
        if (!parseRequest(json, req)) {
            transport.writeMessage(buildError(JsonValue(), kInvalidRequest, "Invalid Request"));
            continue;
        }

        std::string response;
        try {
            response = handler.handleRequest(req);
        } catch (const std::exception &e) {
            if (!req.isNotification())
                response = buildError(req.id, kInternalError, e.what());
        }
        if (!response.empty())
            transport.writeMessage(response);
    }
    return transport.lastReadFailedDueToError() ? 1 : 0;
}

/// @brief Run the LSP event loop for a concrete compiler bridge.
/// @tparam Bridge Language-specific compiler bridge type.
/// @param transport Transport used for framed message I/O.
/// @param bridge Bridge backing LSP feature requests.
/// @param config Server metadata and language labels.
/// @return Process exit status.
template <typename Bridge>
int runLanguageLspServer(Transport &transport, Bridge &bridge, const ServerConfig &config) {
    LspHandler handler(bridge, transport, config);
    RawMessage msg;
    const bool verbose = std::getenv("ZANNA_LSP_LOG") != nullptr;

    if (verbose) {
        std::fprintf(stderr, "[%s] LSP server started\n", config.serverName.c_str());
        std::fflush(stderr);
    }

    while (transport.readMessage(msg)) {
        if (verbose) {
            std::fprintf(
                stderr, "[%s] recv %zu bytes\n", config.serverName.c_str(), msg.content.size());
            std::fflush(stderr);
        }

        JsonValue json;
        try {
            json = JsonValue::parse(msg.content);
        } catch (const std::exception &e) {
            if (verbose) {
                std::fprintf(stderr, "[%s] parse error: %s\n", config.serverName.c_str(), e.what());
                std::fflush(stderr);
            }
            transport.writeMessage(buildError(JsonValue(), kParseError, "Parse error"));
            continue;
        }

        JsonRpcRequest req;
        if (!parseRequest(json, req)) {
            if (verbose) {
                std::fprintf(stderr, "[%s] invalid request\n", config.serverName.c_str());
                std::fflush(stderr);
            }
            transport.writeMessage(buildError(JsonValue(), kInvalidRequest, "Invalid Request"));
            continue;
        }

        if (verbose) {
            std::fprintf(stderr,
                         "[%s] method=%s id=%s\n",
                         config.serverName.c_str(),
                         req.method.c_str(),
                         req.id.isNull() ? "null" : req.id.toCompactString().c_str());
            std::fflush(stderr);
        }

        if (req.method == "exit")
            return handler.shutdownRequested() ? 0 : 1;

        std::string response;
        try {
            response = handler.handleRequest(req);
        } catch (const std::exception &e) {
            if (verbose) {
                std::fprintf(
                    stderr, "[%s] handler error: %s\n", config.serverName.c_str(), e.what());
                std::fflush(stderr);
            }
            if (!req.isNotification())
                response = buildError(req.id, kInternalError, e.what());
        }
        if (!response.empty()) {
            if (verbose) {
                std::fprintf(
                    stderr, "[%s] resp %zu bytes\n", config.serverName.c_str(), response.size());
                std::fflush(stderr);
            }
            transport.writeMessage(response);
        } else if (verbose) {
            std::fprintf(
                stderr, "[%s] (no response - notification handled)\n", config.serverName.c_str());
            std::fflush(stderr);
        }
    }

    if (verbose) {
        std::fprintf(stderr, "[%s] LSP server exiting\n", config.serverName.c_str());
        std::fflush(stderr);
    }
    return transport.lastReadFailedDueToError() ? 1 : 0;
}

/// @brief Run a Zanna language server from a normal `main` function.
/// @details Parses common protocol flags, constructs the language-specific
///          compiler bridge, selects MCP or LSP transport, and runs the matching
///          event loop. Unknown options and malformed auto-detect input produce
///          concise stderr diagnostics.
/// @tparam Bridge Language-specific compiler bridge type.
/// @param argc Argument count from the C runtime.
/// @param argv Argument vector from the C runtime.
/// @param executableName Executable name for usage/version output.
/// @param config Server metadata and language labels.
/// @return Process exit status.
template <typename Bridge>
int runLanguageServerMain(int argc,
                          char **argv,
                          const char *executableName,
                          const ServerConfig &config) {
    platformInitStdio();

    LanguageServerMode mode = LanguageServerMode::AutoDetect;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mcp") == 0)
            mode = LanguageServerMode::Mcp;
        else if (std::strcmp(argv[i], "--lsp") == 0)
            mode = LanguageServerMode::Lsp;
        else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printLanguageServerUsage(executableName, stdout);
            return 0;
        } else if (std::strcmp(argv[i], "--version") == 0) {
            printLanguageServerVersion(executableName, config, stdout);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            printLanguageServerUsage(executableName, stderr);
            return 1;
        }
    }

    if (mode == LanguageServerMode::AutoDetect) {
        const int c = peekLanguageServerProtocolByte();
        if (c == EOF)
            return 0;
        if (c == 'C' || c == 'c') {
            mode = LanguageServerMode::Lsp;
        } else if (c == '{') {
            mode = LanguageServerMode::Mcp;
        } else {
            std::fprintf(stderr,
                         "error: cannot auto-detect protocol; expected '{' for MCP or "
                         "'Content-Length' for LSP\n");
            return 1;
        }
    }

    Bridge bridge;
    std::unique_ptr<Transport> transport;
    if (mode == LanguageServerMode::Lsp) {
        transport = std::make_unique<LspTransport>(stdin, stdout);
        return runLanguageLspServer(*transport, bridge, config);
    }

    transport = std::make_unique<McpTransport>(stdin, stdout);
    return runLanguageMcpServer(*transport, bridge, config);
}

} // namespace zanna::server
