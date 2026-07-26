//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Declares bounded stdio message transports for MCP and LSP.
///
/// McpTransport exchanges one JSON document per line; LspTransport exchanges
/// Content-Length-framed documents. Both borrow their FILE handles, distinguish
/// clean EOF from malformed input, and use binary-safe byte I/O.
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdio>
#include <string>

namespace zanna::server {

/// @brief A raw message read from or written to the transport.
struct RawMessage {
    std::string content; ///< Owned JSON document bytes without transport framing.
};

/// @brief Abstract base for reading/writing protocol messages over stdio.
class Transport {
  public:
    /// @brief Destroy a transport through its abstract interface.
    virtual ~Transport() = default;

    /// @brief Read the next message from the transport.
    /// @param out Populated with owned message content on success.
    /// @return @c true when a message was read; @c false on EOF or error.
    virtual bool readMessage(RawMessage &out) = 0;

    /// @brief Write a message to the transport.
    /// @param json Complete JSON document to frame and send.
    /// @throws std::runtime_error When writing or flushing fails.
    virtual void writeMessage(const std::string &json) = 0;

    /// @brief True when the last failed read ended because of malformed protocol data.
    /// @details A false return from readMessage can mean either clean EOF or a
    ///          framing/protocol error. Transports that can distinguish those
    ///          states override this so server loops can avoid reporting success
    ///          for malformed input.
    /// @return @c true when the last false read result represents an error.
    [[nodiscard]] virtual bool lastReadFailedDueToError() const {
        return false;
    }
};

/// @brief MCP transport: newline-delimited JSON over stdin/stdout.
///
/// Each message is a single JSON object on one line, terminated by newline.
/// Handles both \n and \r\n line endings on read.
class McpTransport : public Transport {
  public:
    /// @brief Construct a newline-delimited transport over borrowed streams.
    /// @param in Input stream that must outlive the transport.
    /// @param out Output stream that must outlive the transport.
    McpTransport(FILE *in, FILE *out);

    /// @copydoc Transport::readMessage
    bool readMessage(RawMessage &out) override;

    /// @copydoc Transport::writeMessage
    void writeMessage(const std::string &json) override;

    /// @copydoc Transport::lastReadFailedDueToError
    [[nodiscard]] bool lastReadFailedDueToError() const override;

  private:
    FILE *in_;
    FILE *out_;
    bool lastReadHadError_{false};
};

/// @brief LSP transport: Content-Length framed messages over stdin/stdout.
///
/// Messages are framed with HTTP-style headers:
///   Content-Length: <length>\r\n
///   \r\n
///   <json-body>
class LspTransport : public Transport {
  public:
    /// @brief Construct a Content-Length transport over borrowed streams.
    /// @param in Input stream that must outlive the transport.
    /// @param out Output stream that must outlive the transport.
    LspTransport(FILE *in, FILE *out);

    /// @copydoc Transport::readMessage
    bool readMessage(RawMessage &out) override;

    /// @copydoc Transport::writeMessage
    void writeMessage(const std::string &json) override;

    /// @copydoc Transport::lastReadFailedDueToError
    [[nodiscard]] bool lastReadFailedDueToError() const override;

  private:
    FILE *in_;
    FILE *out_;
    bool lastReadHadError_{false};

    /// @brief Read a single line terminated by \r\n from the input.
    /// @param line Destination populated without its line ending.
    /// @return @c true when a line was read, including an unterminated final line.
    bool readLine(std::string &line);
};

/// @brief Set stdin/stdout to binary mode on Windows (no-op on Unix).
/// @details Prevents the C runtime from translating protocol newline bytes.
void platformInitStdio();

} // namespace zanna::server
