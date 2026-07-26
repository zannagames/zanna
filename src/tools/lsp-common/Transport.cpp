//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements newline-delimited MCP and Content-Length-framed LSP
///        transports over borrowed C streams.
///
/// Reads enforce bounded messages, lines, and header counts. Writes are checked
/// and flushed synchronously. Windows standard streams are switched to binary
/// mode so the protocol bytes are not altered by newline translation.
///
/// @see Transport.hpp
//
//===----------------------------------------------------------------------===//

#include "tools/lsp-common/Transport.hpp"

#include "common/PlatformCapabilities.hpp"

#include <cctype>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>

#if ZANNA_HOST_WINDOWS
#include <fcntl.h>
#include <io.h>
#endif

namespace zanna::server {
namespace {

constexpr size_t kMaxProtocolMessageBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxProtocolLineBytes = 64u * 1024u;
constexpr size_t kMaxProtocolHeaders = 64u;

/// @brief Write @p size bytes to @p out, throwing if the write is short.
/// @details No-op for zero-length input; otherwise a partial fwrite raises a
///          std::runtime_error so callers never silently emit a truncated message.
/// @param out Borrowed destination stream.
/// @param data Buffer containing at least @p size bytes.
/// @param size Exact number of bytes to write.
/// @throws std::runtime_error When the stream accepts fewer than @p size bytes.
void checkedWrite(FILE *out, const char *data, size_t size) {
    if (size == 0)
        return;
    if (std::fwrite(data, 1, size, out) != size)
        throw std::runtime_error("protocol write failed");
}

} // namespace

// --- Platform init ---

/// @brief Configure standard streams for byte-exact protocol I/O.
/// @details Sets stdin and stdout to binary mode on Windows and is a no-op on
///          platforms whose C streams do not translate newlines.
void platformInitStdio() {
#if ZANNA_HOST_WINDOWS
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
}

// --- McpTransport ---

/// @brief Construct a newline-delimited MCP transport.
/// @param in Borrowed input stream.
/// @param out Borrowed output stream.
McpTransport::McpTransport(FILE *in, FILE *out) : in_(in), out_(out) {}

/// @brief Read the next nonempty newline-delimited MCP message.
/// @param out Receives the line without its LF or optional preceding CR.
/// @return @c true when a message was read; @c false on clean EOF, stream error,
///         or a message exceeding the configured size limit.
bool McpTransport::readMessage(RawMessage &out) {
    lastReadHadError_ = false;
    std::string line;
    int c;
    while ((c = std::fgetc(in_)) != EOF) {
        if (c == '\n') {
            // Strip trailing \r if present
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            // Skip empty lines
            if (line.empty())
                continue;
            out.content = std::move(line);
            return true;
        }
        line += static_cast<char>(c);
        if (line.size() > kMaxProtocolMessageBytes) {
            lastReadHadError_ = true;
            return false;
        }
    }
    if (std::ferror(in_)) {
        lastReadHadError_ = true;
        return false;
    }
    // Handle last line without trailing newline
    if (!line.empty()) {
        if (line.back() == '\r')
            line.pop_back();
        out.content = std::move(line);
        return true;
    }
    return false;
}

/// @brief Report whether the most recent MCP read failed rather than reached EOF.
/// @return @c true after a stream error or oversized message.
bool McpTransport::lastReadFailedDueToError() const {
    return lastReadHadError_;
}

/// @brief Write and flush one newline-delimited MCP message.
/// @param json JSON document bytes, without a terminating newline.
/// @throws std::runtime_error On a short write, newline failure, or flush failure.
void McpTransport::writeMessage(const std::string &json) {
    checkedWrite(out_, json.data(), json.size());
    if (std::fputc('\n', out_) == EOF)
        throw std::runtime_error("protocol write failed");
    if (std::fflush(out_) != 0)
        throw std::runtime_error("protocol flush failed");
}

// --- LspTransport ---

/// @brief Construct a Content-Length-framed LSP transport.
/// @param in Borrowed input stream.
/// @param out Borrowed output stream.
LspTransport::LspTransport(FILE *in, FILE *out) : in_(in), out_(out) {}

/// @brief Report whether the most recent LSP read encountered invalid input.
/// @return @c true after a stream/framing error, oversized line/message, duplicate
///         Content-Length, excessive headers, or truncated body.
bool LspTransport::lastReadFailedDueToError() const {
    return lastReadHadError_;
}

/// @brief Read one bounded protocol header line.
/// @param line Destination cleared before reading and populated without CR/LF.
/// @return @c true for a terminated line or final unterminated nonempty line;
///         @c false for clean EOF, stream error, or an oversized line.
bool LspTransport::readLine(std::string &line) {
    line.clear();
    int c;
    while ((c = std::fgetc(in_)) != EOF) {
        if (c == '\n') {
            // Strip trailing \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return true;
        }
        line += static_cast<char>(c);
        if (line.size() > kMaxProtocolLineBytes) {
            lastReadHadError_ = true;
            return false;
        }
    }
    if (std::ferror(in_)) {
        lastReadHadError_ = true;
        return false;
    }
    return !line.empty();
}

/// @brief Read one Content-Length-framed LSP message.
/// @param out Receives exactly the declared body bytes on success.
/// @return @c true for a complete valid frame; @c false on EOF, invalid headers,
///         bounds violations, stream errors, or a truncated body.
/// @details Header names are compared case-insensitively. Exactly one positive
///          Content-Length within the message limit is required.
bool LspTransport::readMessage(RawMessage &out) {
    lastReadHadError_ = false;

    // Read headers until empty line
    size_t contentLength = 0;
    bool haveContentLength = false;
    size_t headerCount = 0;
    std::string line;

    while (readLine(line)) {
        if (line.empty())
            break; // End of headers
        if (++headerCount > kMaxProtocolHeaders) {
            lastReadHadError_ = true;
            return false;
        }

        // Parse Content-Length header (case-insensitive prefix match)
        const char *prefix = "Content-Length:";
        size_t prefixLen = std::strlen(prefix);
        if (line.size() >= prefixLen) {
            // Case-insensitive comparison of the prefix
            bool match = true;
            for (size_t i = 0; i < prefixLen; ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) !=
                    std::tolower(static_cast<unsigned char>(prefix[i]))) {
                    match = false;
                    break;
                }
            }
            if (match) {
                if (haveContentLength) {
                    lastReadHadError_ = true;
                    return false;
                }
                // Skip whitespace after colon
                size_t valStart = prefixLen;
                while (valStart < line.size() && (line[valStart] == ' ' || line[valStart] == '\t'))
                    ++valStart;
                size_t valEnd = line.size();
                while (valEnd > valStart && (line[valEnd - 1] == ' ' || line[valEnd - 1] == '\t')) {
                    --valEnd;
                }
                size_t parsed = 0;
                const char *begin = line.data() + valStart;
                const char *end = line.data() + valEnd;
                auto [ptr, ec] = std::from_chars(begin, end, parsed);
                if (ec != std::errc{} || ptr != end || parsed == 0 ||
                    parsed > kMaxProtocolMessageBytes) {
                    lastReadHadError_ = true;
                    return false;
                }
                contentLength = parsed;
                haveContentLength = true;
            }
        }
    }

    if (!haveContentLength) {
        if (headerCount != 0 || !line.empty())
            lastReadHadError_ = true;
        return false;
    }

    // Read exactly contentLength bytes
    out.content.resize(contentLength);
    size_t bytesRead = std::fread(out.content.data(), 1, contentLength, in_);
    if (bytesRead != contentLength) {
        lastReadHadError_ = true;
        return false;
    }

    return true;
}

/// @brief Write and flush one Content-Length-framed LSP message.
/// @param json JSON document bytes used as the exact body.
/// @throws std::runtime_error When header formatting, writing, or flushing fails.
void LspTransport::writeMessage(const std::string &json) {
    char header[64];
    int headerLen =
        std::snprintf(header, sizeof(header), "Content-Length: %zu\r\n\r\n", json.size());
    if (headerLen <= 0 || static_cast<size_t>(headerLen) >= sizeof(header))
        throw std::runtime_error("protocol header formatting failed");
    checkedWrite(out_, header, static_cast<size_t>(headerLen));
    checkedWrite(out_, json.data(), json.size());
    if (std::fflush(out_) != 0)
        throw std::runtime_error("protocol flush failed");
}

} // namespace zanna::server
