//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tools/lsp-common/DocumentStore.hpp
// Purpose: In-memory document storage for LSP open files.
// Key invariants:
//   - URI → (version, content) mapping
//   - Thread-safe is NOT required (single-threaded server)
//   - Documents are only tracked while open (didOpen → didClose)
// Ownership/Lifetime:
//   - Owns document content strings
// Links: tools/lsp-common/LspHandler.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

/// @file
/// @brief Declares single-threaded open-document storage and URI conversion.

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace zanna::server {

/// @brief In-memory store for open documents, keyed by URI.
///
/// Tracks the current content and version of each open file so that
/// LSP requests (completion, hover, etc.) can retrieve the latest text
/// without re-reading from disk.
class DocumentStore {
  public:
    /// @brief Open or update a document.
    /// @param uri Document URI (e.g., "file:///path/to/file.zia").
    /// @param documentVersion Version number from the client.
    /// @param content Full text content of the document.
    void open(const std::string &uri, int documentVersion, std::string content);

    /// @brief Update a document's content and version.
    /// @param uri Document URI used as the verbatim key.
    /// @param documentVersion New client version.
    /// @param content New full text, moved into storage.
    void update(const std::string &uri, int documentVersion, std::string content);

    /// @brief Close a document (remove from store).
    /// @param uri Document URI used as the verbatim key.
    void close(const std::string &uri);

    /// @brief Get the content of a document.
    /// @param uri Document URI used as the verbatim key.
    /// @return Pointer to content string, or nullptr if not found.
    const std::string *getContent(const std::string &uri) const;

    /// @brief Check if a document is open.
    /// @param uri Document URI used as the verbatim key.
    /// @return true when the URI is tracked.
    bool isOpen(const std::string &uri) const;

    /// @brief Return the last client-provided document version for @p uri.
    /// @param uri Document URI exactly as supplied by the LSP client.
    /// @return The stored version, or std::nullopt when the document is not open.
    std::optional<int> version(const std::string &uri) const;

    /// @brief Extract a file path from a URI.
    /// @details Strips "file://" prefix and URL-decodes valid %XX sequences.
    /// @param uri File URI or plain path accepted by the broad converter.
    /// @return Decoded filesystem path.
    /// @throws std::runtime_error when the URI is malformed or contains an encoded path separator.
    static std::string uriToPath(const std::string &uri);

    /// @brief Try to extract a filesystem path from a document URI without throwing.
    /// @details Accepts plain paths and file URIs, decodes valid percent escapes,
    ///          rejects malformed escapes, and rejects percent-encoded '/' or '\'
    ///          so decoding cannot create new path separators after validation.
    /// @param uri Client-provided URI or plain path.
    /// @param outPath Receives the decoded filesystem path on success.
    /// @param err Receives a human-readable failure reason when non-null.
    /// @return True when @p uri was accepted and @p outPath was populated.
    static bool tryUriToPath(const std::string &uri,
                             std::string &outPath,
                             std::string *err = nullptr);

    /// @brief Try to extract a filesystem path from an LSP file URI without throwing.
    /// @details This stricter wrapper is intended for LSP document state, where the protocol
    ///          requires document identifiers to be URI-shaped. It rejects plain paths before
    ///          delegating to tryUriToPath(), preserving the broader helper for MCP and tests that
    ///          intentionally operate on raw filesystem paths.
    /// @param uri Client-provided LSP document URI.
    /// @param outPath Receives the decoded filesystem path on success.
    /// @param err Receives a human-readable failure reason when non-null.
    /// @return True when @p uri used file:// form and decoded successfully.
    static bool tryFileUriToPath(const std::string &uri,
                                 std::string &outPath,
                                 std::string *err = nullptr);

  private:
    /// @brief Stored state for one open document.
    struct Document {
        int version{0};      ///< Last client-reported version number.
        std::string content; ///< Full current text of the document.
    };

    std::unordered_map<std::string, Document> docs_;
};

} // namespace zanna::server
