//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/common/packaging/XarWriter.cpp
// Purpose: Native XAR writer used by macOS flat installer packages.
//
// Key invariants:
//   - Header is 28 bytes, big-endian; TOC and (optionally) file data are
//     zlib-compressed; the heap begins with 20 SHA-1 checksum bytes at offset 0.
//   - The TOC is XML describing a directory tree rebuilt from flat entry paths.
//   - Per-file archived/extracted SHA-1 checksums are recorded in the TOC.
//
// Ownership/Lifetime:
//   - Single-use writer; entries are copied in and owned by the writer.
//
// Links: XarWriter.hpp, PkgZlib.hpp (compression), PkgHash.hpp (SHA-1)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements deterministic in-memory XAR archive generation.
/// @details Flat sanitized entries are rebuilt into a directory-tree TOC, file
///          payloads may be zlib-compressed, and SHA-1 metadata is emitted in
///          the big-endian XAR container format used by macOS flat packages.

#include "XarWriter.hpp"

#include "PkgHash.hpp"
#include "PkgUtils.hpp"
#include "PkgZlib.hpp"

#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace zanna::pkg {
namespace {

/// @brief Sanitize an entry path and require it to be non-empty.
/// @param path Caller-supplied archive-relative path.
/// @return Sanitized non-empty path.
/// @throws std::runtime_error when the path is empty, ".", or unsafe.
std::string normalizeXarPath(const std::string &path) {
    const std::string clean = sanitizePackageRelativePath(path, "xar file path");
    if (clean.empty())
        throw std::runtime_error("xar file path must not be empty or special");
    return clean;
}

/// @brief Escape the five XML metacharacters (&, <, >, ", ') for TOC text.
/// @param text Plain text to embed in an XML node.
/// @return Text with all five predefined XML entities escaped.
std::string xmlEscape(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&apos;";
                break;
            default:
                out.push_back(ch);
                break;
        }
    }
    return out;
}

/// @brief Append a 16-bit value to @p out in big-endian byte order (XAR header).
/// @param out Destination byte vector.
/// @param value Host-order value to encode.
void appendBE16(std::vector<uint8_t> &out, uint16_t value) {
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

/// @brief Append a 32-bit value to @p out in big-endian byte order (XAR header).
/// @param out Destination byte vector.
/// @param value Host-order value to encode.
void appendBE32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    out.push_back(static_cast<uint8_t>(value & 0xffu));
}

/// @brief Append a 64-bit value to @p out in big-endian byte order (XAR header).
/// @param out Destination byte vector.
/// @param value Host-order value to encode.
void appendBE64(std::vector<uint8_t> &out, uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8)
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xffu));
}

/// @brief A file entry resolved for serialization: heap bytes and TOC metadata.
struct PreparedEntry {
    std::string path;              ///< Archive-relative path.
    std::vector<uint8_t> extracted; ///< Uncompressed bytes (for size/checksum).
    std::vector<uint8_t> archived;  ///< Bytes as stored in the heap (maybe gzip).
    bool compressed{false};         ///< Whether @c archived is zlib-compressed.
    uint32_t mode{0644};            ///< Permission bits (octal).
    uint64_t offset{0};             ///< Byte offset within the heap.
};

/// @brief A node in the directory tree rebuilt from flat entry paths.
/// @details Directories own a map of child nodes; file nodes point at the
///          PreparedEntry holding their data. The tree drives nested TOC XML.
struct XarTreeNode {
    std::string name;             ///< Leaf component name.
    std::string path;             ///< Full archive-relative path.
    bool directory{true};         ///< True for directories, false for files.
    uint32_t mode{0755};          ///< Permission bits (octal).
    const PreparedEntry *file{nullptr}; ///< Backing file entry (file nodes only).
    std::map<std::string, std::unique_ptr<XarTreeNode>> children; ///< Child nodes.
};

/// @brief Split a '/'-separated path into its components (including a trailing
///        empty component if the path ends in '/').
/// @param path Slash-separated archive path.
/// @return Components in order, preserving a terminal empty component.
std::vector<std::string> splitPathComponents(const std::string &path) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= path.size()) {
        const size_t next = path.find('/', pos);
        parts.push_back(next == std::string::npos ? path.substr(pos)
                                                  : path.substr(pos, next - pos));
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return parts;
}

/// @brief Create (or reuse) the directory node chain for @p path under @p root.
/// @details Walks each path component, creating intermediate directory nodes as
///          needed, and sets the final node's mode. Throws if a component already
///          exists as a file.
/// @param root Tree root to insert under.
/// @param path Archive-relative directory path.
/// @param mode Permission bits for the leaf directory.
/// @return Pointer to the leaf directory node.
XarTreeNode *ensureDirectoryNode(XarTreeNode &root, const std::string &path, uint32_t mode) {
    XarTreeNode *node = &root;
    std::string current;
    for (const std::string &part : splitPathComponents(path)) {
        if (!current.empty())
            current += "/";
        current += part;
        auto &child = node->children[part];
        if (!child) {
            child = std::make_unique<XarTreeNode>();
            child->name = part;
            child->path = current;
            child->directory = true;
            child->mode = 0755;
        }
        if (!child->directory)
            throw std::runtime_error("xar path component is already a file: " + current);
        node = child.get();
    }
    node->mode = mode & 07777u;
    return node;
}

/// @brief Insert a file node for @p entry into the tree, creating parent dirs.
/// @details Creates intermediate directory nodes for all but the last path
///          component, then attaches a leaf file node pointing at @p entry.
///          Throws on a duplicate path or a parent that is already a file.
/// @param root Tree root to modify.
/// @param entry Prepared file whose path and metadata back the new leaf.
/// @throws std::runtime_error On empty, duplicate, or file-as-parent paths.
void addFileNode(XarTreeNode &root, const PreparedEntry &entry) {
    const auto parts = splitPathComponents(entry.path);
    if (parts.empty())
        throw std::runtime_error("xar file path must not be empty");
    XarTreeNode *node = &root;
    std::string current;
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!current.empty())
            current += "/";
        current += parts[i];
        auto &child = node->children[parts[i]];
        if (!child) {
            child = std::make_unique<XarTreeNode>();
            child->name = parts[i];
            child->path = current;
            child->directory = true;
            child->mode = 0755;
        }
        if (!child->directory)
            throw std::runtime_error("xar path component is already a file: " + current);
        node = child.get();
    }
    auto &leaf = node->children[parts.back()];
    if (leaf)
        throw std::runtime_error("duplicate xar file path: " + entry.path);
    leaf = std::make_unique<XarTreeNode>();
    leaf->name = parts.back();
    leaf->path = entry.path;
    leaf->directory = false;
    leaf->mode = entry.mode;
    leaf->file = &entry;
}

/// @brief Emit the `<data>` element describing a file's heap payload in the TOC.
/// @details Writes archived/extracted SHA-1 checksums, the encoding (gzip or
///          octet-stream), the uncompressed size, and the heap offset/length.
/// @param toc TOC string stream being built.
/// @param entry Prepared file entry to describe.
/// @param depth Indentation depth (spaces) for pretty-printing.
void writeFileDataXml(std::ostringstream &toc, const PreparedEntry &entry, int depth) {
    const std::string indent(static_cast<size_t>(depth) * 1u, ' ');
    toc << indent << "<data>\n";
    toc << indent << " <archived-checksum style=\"sha1\">"
        << sha1Hex(entry.archived.data(), entry.archived.size()) << "</archived-checksum>\n";
    toc << indent << " <extracted-checksum style=\"sha1\">"
        << sha1Hex(entry.extracted.data(), entry.extracted.size()) << "</extracted-checksum>\n";
    toc << indent << " <encoding style=\""
        << (entry.compressed ? "application/x-gzip" : "application/octet-stream") << "\"/>\n";
    toc << indent << " <size>" << entry.extracted.size() << "</size>\n";
    toc << indent << " <offset>" << entry.offset << "</offset>\n";
    toc << indent << " <length>" << entry.archived.size() << "</length>\n";
    toc << indent << "</data>\n";
}

/// @brief Recursively emit a `<file>` element (and children) for a tree node.
/// @details Writes the name/type/mode/owner fields, then recurses into child
///          nodes for directories or emits the `<data>` block for files.
/// @param toc TOC string stream being built.
/// @param node Tree node to emit.
/// @param id Monotonic file-id counter, incremented per element.
/// @param depth Indentation depth (spaces) for pretty-printing.
void writeTreeNodeXml(std::ostringstream &toc, const XarTreeNode &node, int &id, int depth) {
    const std::string indent(static_cast<size_t>(depth) * 1u, ' ');
    toc << indent << "<file id=\"" << id++ << "\">\n";
    toc << indent << " <name>" << xmlEscape(node.name) << "</name>\n";
    toc << indent << " <type>" << (node.directory ? "directory" : "file") << "</type>\n";
    toc << indent << " <mode>0" << std::oct << node.mode << std::dec << "</mode>\n";
    toc << indent << " <uid>0</uid>\n";
    toc << indent << " <user>root</user>\n";
    toc << indent << " <gid>0</gid>\n";
    toc << indent << " <group>wheel</group>\n";
    if (node.directory) {
        for (const auto &child : node.children)
            writeTreeNodeXml(toc, *child.second, id, depth + 1);
    } else {
        if (node.file == nullptr)
            throw std::runtime_error("xar file node is missing data: " + node.path);
        writeFileDataXml(toc, *node.file, depth + 1);
    }
    toc << indent << "</file>\n";
}

} // namespace

/// @brief Add or merge a directory in the pending XAR tree.
/// @param path Archive-relative directory path.
/// @param mode Permission bits, masked to the supported low 12 bits.
/// @throws std::runtime_error If @p path is empty or unsafe.
void XarWriter::addDirectory(const std::string &path, uint32_t mode) {
    const std::string clean = normalizeXarPath(path);
    if (!seenNames_.insert(clean).second)
        return;
    Entry entry;
    entry.kind = EntryKind::Directory;
    entry.path = clean;
    entry.mode = mode & 07777u;
    entries_.push_back(std::move(entry));
}

/// @brief Add a copied raw-buffer file to the pending archive.
/// @param name Archive-relative file path.
/// @param data File bytes; may be null only when @p size is zero.
/// @param size Number of bytes available at @p data.
/// @param compress Whether to zlib-compress the heap payload.
/// @param mode Permission bits, masked to the supported low 12 bits.
/// @throws std::runtime_error On unsafe/duplicate paths or null non-empty data.
void XarWriter::addFile(
    const std::string &name, const uint8_t *data, size_t size, bool compress, uint32_t mode) {
    const std::string clean = normalizeXarPath(name);
    if (!seenNames_.insert(clean).second)
        throw std::runtime_error("duplicate xar file path: " + clean);
    Entry entry;
    entry.kind = EntryKind::File;
    entry.path = clean;
    if (size != 0) {
        if (data == nullptr)
            throw std::runtime_error("xar file data pointer is null: " + clean);
        entry.data.assign(data, data + size);
    }
    entry.compress = compress;
    entry.mode = mode & 07777u;
    entries_.push_back(std::move(entry));
}

/// @brief Add a copied byte-vector file to the pending archive.
/// @param name Archive-relative file path.
/// @param data File bytes to copy.
/// @param compress Whether to zlib-compress the heap payload.
/// @param mode Permission bits, masked to the supported low 12 bits.
/// @throws std::runtime_error On unsafe or duplicate paths.
void XarWriter::addFileVec(const std::string &name,
                           const std::vector<uint8_t> &data,
                           bool compress,
                           uint32_t mode) {
    addFile(name, data.data(), data.size(), compress, mode);
}

/// @brief Add copied string bytes as a pending archive file.
/// @param name Archive-relative file path.
/// @param content File bytes to copy without transcoding.
/// @param compress Whether to zlib-compress the heap payload.
/// @param mode Permission bits, masked to the supported low 12 bits.
/// @throws std::runtime_error On unsafe or duplicate paths.
void XarWriter::addFileString(const std::string &name,
                              const std::string &content,
                              bool compress,
                              uint32_t mode) {
    addFile(
        name, reinterpret_cast<const uint8_t *>(content.data()), content.size(), compress, mode);
}

/// @brief Serialize the archive: prepare heap entries, build the TOC tree/XML,
///        compress the TOC, then emit header + TOC + checksum + heap.
/// @details File payloads are laid out in the heap (after the 20 reserved
///          checksum bytes) and their offsets recorded; the directory tree is
///          rebuilt and rendered to XML; the TOC is zlib-compressed and its SHA-1
///          stored at heap offset 0.
/// @return Complete caller-owned XAR byte stream; pending entries are unchanged.
/// @throws std::runtime_error On tree conflicts, compression failure, or size overflow.
std::vector<uint8_t> XarWriter::finish() const {
    std::vector<PreparedEntry> prepared;
    prepared.reserve(entries_.size());
    uint64_t heapOffset = 20; // XAR TOC checksum bytes live at heap offset 0.
    for (const Entry &entry : entries_) {
        if (entry.kind == EntryKind::Directory)
            continue;
        PreparedEntry out;
        out.path = entry.path;
        out.extracted = entry.data;
        out.archived =
            entry.compress ? zlibCompress(entry.data.data(), entry.data.size()) : entry.data;
        out.compressed = entry.compress;
        out.mode = entry.mode;
        out.offset = heapOffset;
        if (out.archived.size() > std::numeric_limits<uint64_t>::max() - heapOffset)
            throw std::runtime_error("XarWriter: heap offset overflow");
        heapOffset += out.archived.size();
        prepared.push_back(std::move(out));
    }

    XarTreeNode root;
    for (const Entry &entry : entries_) {
        if (entry.kind == EntryKind::Directory)
            ensureDirectoryNode(root, entry.path, entry.mode);
    }
    for (const PreparedEntry &entry : prepared)
        addFileNode(root, entry);

    std::ostringstream toc;
    toc << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    toc << "<xar>\n";
    toc << " <toc>\n";
    toc << "  <checksum style=\"sha1\">\n";
    toc << "   <size>20</size>\n";
    toc << "   <offset>0</offset>\n";
    toc << "  </checksum>\n";
    toc << "  <creation-time>1970-01-01T00:00:00Z</creation-time>\n";
    int id = 1;
    for (const auto &child : root.children)
        writeTreeNodeXml(toc, *child.second, id, 2);
    toc << " </toc>\n";
    toc << "</xar>\n";

    const std::string tocText = toc.str();
    const auto tocBytes = reinterpret_cast<const uint8_t *>(tocText.data());
    const auto tocCompressed = zlibCompress(tocBytes, tocText.size());
    const auto tocChecksum = sha1Bytes(tocCompressed.data(), tocCompressed.size());

    std::vector<uint8_t> out;
    if (heapOffset > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        throw std::runtime_error("XarWriter: archive is too large");
    size_t reserveBytes = 28;
    checkedAddSize(reserveBytes, tocCompressed.size(), "XarWriter");
    checkedAddSize(reserveBytes, static_cast<size_t>(heapOffset), "XarWriter");
    out.reserve(reserveBytes);
    appendBE32(out, 0x78617221u); // "xar!"
    appendBE16(out, 28);
    appendBE16(out, 1);
    appendBE64(out, tocCompressed.size());
    appendBE64(out, tocText.size());
    appendBE32(out, 1); // SHA-1 TOC checksum
    out.insert(out.end(), tocCompressed.begin(), tocCompressed.end());
    out.insert(out.end(), tocChecksum.begin(), tocChecksum.end());
    for (const PreparedEntry &entry : prepared)
        out.insert(out.end(), entry.archived.begin(), entry.archived.end());
    return out;
}

/// @brief Serialize the archive and atomically replace the destination file.
/// @param path Native output path.
/// @throws std::runtime_error On serialization, temporary-file, or replacement failure.
void XarWriter::finishToFile(const std::string &path) const {
    auto data = finish();
    writeFileAtomic(path, data);
}

} // namespace zanna::pkg
