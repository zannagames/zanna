//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements structured and legacy REPL history persistence.
/// @details Version-two history stores each nonempty submission as a decimal
///          byte length followed by its exact bytes, preserving embedded
///          newlines. Loading remains backward compatible with line-delimited
///          files. Malformed structured input stops at the first incomplete or
///          invalid record while retaining previously decoded entries.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplHistoryCodec.cpp
// Purpose: Implementation of structured REPL history persistence.
// Key invariants:
//   - Corrupt structured records stop decoding without reading out of bounds.
//   - Legacy history remains accepted for compatibility.
// Ownership/Lifetime:
//   - All decoded entries are owned by the returned vector.
// Links: src/repl/ReplHistoryCodec.hpp
//
//===----------------------------------------------------------------------===//

#include "ReplHistoryCodec.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <limits>

namespace zanna::repl {
namespace {

constexpr const char *kHistoryMagic = "# zanna-repl-history-v2";

/// @brief Trim @p entries to the newest @p maxEntries elements.
/// @details Relative order of retained entries is preserved.
/// @param entries History entries to mutate in place.
/// @param maxEntries Maximum number of entries to keep.
void trimToMaxEntries(std::vector<std::string> &entries, size_t maxEntries) {
    if (maxEntries == 0) {
        entries.clear();
        return;
    }
    if (entries.size() > maxEntries) {
        auto removeCount = static_cast<std::ptrdiff_t>(entries.size() - maxEntries);
        entries.erase(entries.begin(), entries.begin() + removeCount);
    }
}

/// @brief Decode one unsigned byte count from @p text at @p pos.
/// @details Stops at the next newline and rejects empty or non-decimal lengths.
/// @param text Complete history file text.
/// @param[in,out] pos Current offset, advanced past the newline on success.
/// @param[out] len Decoded length.
/// @return True when a syntactically valid length was decoded.
bool readLengthLine(const std::string &text, size_t &pos, size_t &len) {
    if (pos >= text.size())
        return false;
    size_t lineEnd = text.find('\n', pos);
    if (lineEnd == std::string::npos)
        return false;
    if (lineEnd == pos)
        return false;

    size_t value = 0;
    for (size_t i = pos; i < lineEnd; ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (!std::isdigit(c))
            return false;
        size_t digit = static_cast<size_t>(c - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }

    pos = lineEnd + 1;
    len = value;
    return true;
}

/// @brief Load legacy line-delimited history from @p text.
/// @details Empty lines are omitted, `decodedEntryCount` records the number
///          accepted before retention trimming, and returned strings own their
///          content.
/// @param text Entire history file contents.
/// @param maxEntries Maximum number of entries to retain.
/// @return Decoded entries and pre-trim count.
ReplHistoryLoadResult loadLegacy(const std::string &text, size_t maxEntries) {
    ReplHistoryLoadResult result;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        std::string entry = text.substr(start, end - start);
        if (!entry.empty()) {
            result.entries.push_back(std::move(entry));
            ++result.decodedEntryCount;
        }
        if (end == text.size())
            break;
        start = end + 1;
    }
    trimToMaxEntries(result.entries, maxEntries);
    return result;
}

/// @brief Load structured length-prefixed history from @p text.
/// @details Decoding begins after the magic line and stops safely at the first
///          malformed length or truncated payload. Empty records are omitted.
/// @param text Entire history file contents.
/// @param maxEntries Maximum number of entries to retain.
/// @return Decoded entries and pre-trim count.
ReplHistoryLoadResult loadStructured(const std::string &text, size_t maxEntries) {
    ReplHistoryLoadResult result;
    size_t pos = text.find('\n');
    if (pos == std::string::npos)
        return result;
    ++pos;

    while (pos < text.size()) {
        size_t len = 0;
        if (!readLengthLine(text, pos, len))
            break;
        if (len > text.size() - pos)
            break;

        std::string entry = text.substr(pos, len);
        pos += len;
        if (pos < text.size() && text[pos] == '\n')
            ++pos;

        if (!entry.empty()) {
            result.entries.push_back(std::move(entry));
            ++result.decodedEntryCount;
        }
    }

    trimToMaxEntries(result.entries, maxEntries);
    return result;
}

} // namespace

/// @brief Load REPL history from a filesystem path.
/// @details The file is read in binary mode so structured payload byte counts
///          remain exact. A leading version-two magic string selects structured
///          decoding; all other content uses legacy line decoding.
/// @param path History file to read.
/// @param maxEntries Maximum newest entries to retain; zero returns none while
///        preserving the pre-trim decoded count.
/// @return Owned decoded entries and their pre-trim count, or an empty result
///         when the file cannot be opened.
ReplHistoryLoadResult ReplHistoryCodec::load(const std::filesystem::path &path, size_t maxEntries) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open())
        return {};

    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.rfind(kHistoryMagic, 0) == 0)
        return loadStructured(text, maxEntries);
    return loadLegacy(text, maxEntries);
}

/// @brief Save REPL history in the version-two length-prefixed format.
/// @details Missing parent directories are created, empty entries are skipped,
///          and payloads are written in binary mode with exact byte lengths.
/// @param path Destination history file.
/// @param entries Entries to serialize in order; source strings are borrowed.
/// @return `true` when directory creation, opening, and every write succeed.
bool ReplHistoryCodec::save(const std::filesystem::path &path,
                            const std::vector<std::string> &entries) {
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec)
            return false;
    }

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open())
        return false;

    file << kHistoryMagic << "\n";
    for (const auto &entry : entries) {
        if (entry.empty())
            continue;
        file << entry.size() << "\n";
        file.write(entry.data(), static_cast<std::streamsize>(entry.size()));
        file << "\n";
    }
    return file.good();
}

} // namespace zanna::repl
