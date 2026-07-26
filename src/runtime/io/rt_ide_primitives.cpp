//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/io/rt_ide_primitives.cpp
// Purpose: Workspace, asset, manifest, and transactional edit helpers used by
//          Zanna Studio and editor-style tooling.
// Key invariants:
//   - Workspace edit targets are validated before any disk mutation is attempted.
//   - Workspace/file-index helpers never depend on compiler-layer services.
// Ownership/Lifetime:
//   - Runtime strings borrowed from lower-level APIs are released after copying.
//   - Map and sequence results are runtime-owned objects returned to callers.
// Links: src/runtime/io/rt_ide_primitives.h, src/runtime/io/rt_watcher.h
//
//===----------------------------------------------------------------------===//
/**
 * @file
 * @brief Implements editor-facing workspace indexing and transactional file edits.
 * @details Provides nested gitignore evaluation with bounded caches, resumable
 * file-index pages and status fingerprints, normalized watcher batches,
 * multi-source asset resolution, manifest parsing with diagnostics, and
 * validate-stage-recheck-commit-rollback workspace edit transactions.
 */

#include "rt_ide_primitives.h"

#include "rt_asset.h"
#include "rt_box.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"
#include "rt_watcher.h"

#include "rt_hash_util.h" // rt_keyed_hash_bytes for unpredictable sidecar names (VDOC-196)

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <vector>

#if RT_PLATFORM_WINDOWS
#include <sys/stat.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

/** Maximum number of parsed nested `.gitignore` entries retained in the cache. */
constexpr size_t kGitignoreCacheMaxEntries = 64;
/** Maximum number of resumable workspace-index cursors retained process-wide. */
constexpr size_t kFileIndexPageCursorMaxEntries = 8;
/** Hard count limit for a complete workspace file-index traversal. */
constexpr int64_t kWorkspaceFileIndexMaxEntries = 100000;
/** FNV-1a offset basis used for deterministic workspace fingerprints. */
constexpr uint64_t kWorkspaceFingerprintOffset = 14695981039346656037ull;
/** FNV-1a prime used for deterministic workspace fingerprints. */
constexpr uint64_t kWorkspaceFingerprintPrime = 1099511628211ull;

// Keep the cache root trivially initialized: native-linked tools may call this
// runtime object before C++ global constructors from archive members have run.
/** One process-lifetime cached `.gitignore` parse keyed by path and modification state. */
struct GitignoreCacheEntry {
    std::string key;
    int64_t modified{-2};
    std::vector<std::string> patterns;
    GitignoreCacheEntry *next{nullptr};
};

/** Head of the trivially initialized gitignore-cache linked list. */
GitignoreCacheEntry *g_gitignoreCacheHead = nullptr;
/** Spin lock protecting gitignore-cache lookup, insertion, and eviction. */
std::atomic_flag g_gitignoreCacheLock = ATOMIC_FLAG_INIT;
/** Unique input counter for keyed transactional-edit sidecar nonces. */
std::atomic<uint64_t> g_workspaceEditTempCounter{0};
/** Spin lock protecting the workspace file-index cursor cache. */
std::atomic_flag g_fileIndexPageCursorLock = ATOMIC_FLAG_INIT;
/** Monotonic recency counter used for file-index cursor LRU eviction. */
std::atomic<uint64_t> g_fileIndexPageCursorClock{0};

/// @brief Scope guard for the process-wide gitignore cache spin lock.
/// @details The file-index runtime archive is linked into native programs, so
///          this lock deliberately avoids heap allocation and C++ static
///          destructor registration. Gitignore cache critical sections are
///          short and only protect an in-memory linked list, making an atomic
///          spin lock preferable here to a lazily allocated `std::mutex`.
struct GitignoreCacheLockGuard {
    /// @brief Acquire exclusive access to the gitignore cache list.
    /// @details Uses acquire ordering so subsequent cache reads observe writes
    ///          from the previous holder before traversing `g_gitignoreCacheHead`.
    GitignoreCacheLockGuard() {
        while (g_gitignoreCacheLock.test_and_set(std::memory_order_acquire)) {
        }
    }

    /// @brief Release exclusive access to the gitignore cache list.
    /// @details Uses release ordering so newly inserted or evicted cache nodes
    ///          are visible to the next thread that acquires the guard.
    ~GitignoreCacheLockGuard() {
        g_gitignoreCacheLock.clear(std::memory_order_release);
    }

    /// @brief Prevent accidental copies of the active lock guard.
    /// @details Copying a guard would make ownership ambiguous and could clear
    ///          the process-wide spin lock while the original guard is still
    ///          in scope.
    GitignoreCacheLockGuard(const GitignoreCacheLockGuard &) = delete;

    /// @brief Prevent assigning one active lock guard to another.
    /// @details Assignment would have the same ownership ambiguity as copying
    ///          and is not meaningful for a scope-bound cache lock.
    GitignoreCacheLockGuard &operator=(const GitignoreCacheLockGuard &) = delete;
};

/// @brief Scope guard for the private FileIndex.Page cursor cache.
/// @details The public paging API stays stateless, but sequential callers should
///          not pay for a fresh recursive traversal on every page. A small
///          process-local cursor cache lets independent IDE subsystems page
///          concurrently without resetting each other's traversal state.
struct FileIndexPageCursorLockGuard {
    /// @brief Acquire exclusive access to the file-index page cursor cache.
    /// @details Spins with acquire ordering because protected operations only traverse or update
    ///          the small process-local cursor list.
    FileIndexPageCursorLockGuard() {
        while (g_fileIndexPageCursorLock.test_and_set(std::memory_order_acquire)) {
        }
    }

    /// @brief Release exclusive access to the file-index page cursor cache.
    /// @details Release ordering publishes cursor insertion, repositioning, and eviction changes
    ///          before another thread acquires the guard.
    ~FileIndexPageCursorLockGuard() {
        g_fileIndexPageCursorLock.clear(std::memory_order_release);
    }

    /// @brief Prevent copying ownership of the active page-cursor cache lock.
    FileIndexPageCursorLockGuard(const FileIndexPageCursorLockGuard &) = delete;

    /// @brief Prevent assigning ownership between active page-cursor cache guards.
    FileIndexPageCursorLockGuard &operator=(const FileIndexPageCursorLockGuard &) = delete;
};

/// @brief Copy a runtime string into an owning native string.
/// @details Null, invalid, and empty runtime strings produce an empty result. Embedded NUL bytes
///          are preserved because construction uses the runtime-reported byte length.
/// @param s Borrowed runtime string handle.
/// @return Native byte string copied from @p s.
std::string toStd(rt_string s) {
    if (!s)
        return {};
    const char *data = rt_string_cstr(s);
    const int64_t len = rt_str_len(s);
    if (!data || len <= 0)
        return {};
    return std::string(data, static_cast<size_t>(len));
}

/// @brief Copy a runtime string or boxed-string object into a native string.
/// @param value Runtime collection element to inspect.
/// @param out Receives the copied bytes on success.
/// @return True only when @p value represents a string.
bool objectToStdString(void *value, std::string &out) {
    rt_string text = nullptr;
    if (rt_string_is_handle(value))
        text = rt_string_ref(static_cast<rt_string>(value));
    else if (rt_box_type(value) == RT_BOX_STR)
        text = rt_unbox_str(value);
    else
        return false;
    out = toStd(text);
    rt_string_unref(text);
    return true;
}

/// @brief Copy a native byte string into a runtime-managed string.
/// @param value Native bytes to copy, including any embedded NUL bytes.
/// @return Runtime string created from the complete byte span.
rt_string makeString(const std::string &value) {
    return rt_string_from_bytes(value.data(), value.size());
}

/// @brief Release an owned runtime object reference and destroy it at zero references.
/// @param obj Runtime object reference to release; NULL is ignored.
void releaseObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Store a copied native string under a constant key in a runtime map.
/// @details Releases the temporary string reference after the map retains the value.
/// @param map Runtime map to update.
/// @param key Null-terminated constant key name.
/// @param value Native byte string to copy into the map.
void mapSetStr(void *map, const char *key, const std::string &value) {
    rt_string s = makeString(value);
    rt_map_set_str(map, rt_const_cstr(key), s);
    rt_string_unref(s);
}

/// @brief Store a sequence object under a constant key in a runtime map.
/// @param map Runtime map to update.
/// @param key Null-terminated constant key name.
/// @param seq Borrowed sequence reference retained by the map.
void mapSetSeq(void *map, const char *key, void *seq) {
    rt_map_set(map, rt_const_cstr(key), seq);
}

/// @brief Append an object to a runtime sequence and release the caller's reference.
/// @param seq Runtime sequence that will retain @p obj.
/// @param obj Owned runtime object reference to transfer into @p seq.
void seqPushOwned(void *seq, void *obj) {
    rt_seq_push(seq, obj);
    releaseObject(obj);
}

/// @brief Remove leading and trailing locale-classified whitespace bytes.
/// @param input Native text view to trim.
/// @return Owning string containing the untrimmed middle span.
std::string trim(std::string_view input) {
    size_t first = 0;
    while (first < input.size() && std::isspace(static_cast<unsigned char>(input[first])))
        first++;
    size_t last = input.size();
    while (last > first && std::isspace(static_cast<unsigned char>(input[last - 1])))
        last--;
    return std::string(input.substr(first, last - first));
}

/// @brief Normalize one `.gitignore` pattern while preserving gitignore escapes.
/// @details Git treats unescaped leading `#` as a comment, leading `!` as a
///          negation marker, and trailing spaces as insignificant unless they
///          are escaped. This helper trims the syntactic whitespace while
///          preserving escaped leading `#` / `!` so later comment/negation
///          logic can distinguish them from control markers.
/// @param input Raw line from a `.gitignore` file or caller-supplied ignore list.
/// @return Normalized pattern; empty means the line should be ignored.
std::string normalizeGitignorePattern(std::string_view input) {
    size_t first = 0;
    while (first < input.size() &&
           (input[first] == ' ' || input[first] == '\t' || input[first] == '\r'))
        first++;
    std::string out(input.substr(first));
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t' || out.back() == '\r')) {
        size_t slash_count = 0;
        for (size_t i = out.size() - 1; i > 0 && out[i - 1] == '\\'; --i)
            slash_count++;
        if ((slash_count & 1u) != 0)
            break;
        out.pop_back();
    }
    return out;
}

/// @brief Rebase a pattern from a nested `.gitignore` file to root-relative form.
/// @details Runtime enumeration compares every candidate as a root-relative path.
///          Patterns loaded from `dir/.gitignore` therefore need to be scoped to
///          `dir`. Plain basename patterns become `dir/**/name` so they match
///          descendants of that directory but not siblings outside it. Negated
///          patterns keep their leading `!`.
/// @param base_rel Directory containing the `.gitignore`, relative to workspace root.
/// @param pattern Normalized gitignore pattern.
/// @return Root-relative pattern equivalent for the runtime matcher.
std::string rebaseGitignorePattern(const std::string &base_rel, const std::string &pattern) {
    if (base_rel.empty() || pattern.empty())
        return pattern;
    bool negated = pattern[0] == '!';
    std::string body = negated ? pattern.substr(1) : pattern;
    while (!body.empty() && body.front() == '/')
        body.erase(body.begin());
    std::string rebased = negated ? "!" : "";
    rebased += base_rel;
    if (!rebased.empty() && rebased.back() != '/')
        rebased.push_back('/');
    if (body.find('/') == std::string::npos)
        rebased += "**/";
    rebased += body;
    return rebased;
}

/// @brief Lowercase a native byte string using the active C locale.
/// @param value String to transform in place.
/// @return Lowercased string.
std::string lower(std::string value) {
    for (char &ch : value)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return value;
}

/// @brief Normalize path separators and strip repeated leading `./` components.
/// @param value Path text to normalize.
/// @return Path using forward slashes with no leading current-directory markers.
std::string normalizeSlashes(std::string value) {
    for (char &ch : value) {
        if (ch == '\\')
            ch = '/';
    }
    while (value.rfind("./", 0) == 0)
        value.erase(0, 2);
    return value;
}

/// @brief Split a comma-, semicolon-, or newline-delimited configuration list.
/// @details Delimiters inside matching single or double quotes are preserved and quote bytes are
///          removed. Each item is trimmed and empty items are discarded.
/// @param value Serialized list text.
/// @return Parsed nonempty items in source order.
std::vector<std::string> splitList(const std::string &value) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;
    char quote = 0;
    for (char ch : value) {
        if ((ch == '"' || ch == '\'') && (!quoted || quote == ch)) {
            quoted = !quoted;
            quote = quoted ? ch : 0;
            continue;
        }
        if (!quoted && (ch == ',' || ch == ';' || ch == '\n')) {
            std::string item = trim(cur);
            if (!item.empty())
                out.push_back(item);
            cur.clear();
            continue;
        }
        cur.push_back(ch);
    }
    std::string item = trim(cur);
    if (!item.empty())
        out.push_back(item);
    return out;
}

/// @brief Match a byte string against `*` and `?` wildcards without separator rules.
/// @details Uses iterative last-star backtracking; `*` consumes any byte sequence and `?`
///          consumes exactly one byte.
/// @param text Candidate byte string.
/// @param pattern Wildcard pattern.
/// @return True when the complete text matches the complete pattern.
bool wildcardMatchRec(std::string_view text, std::string_view pattern) {
    size_t ti = 0;
    size_t pi = 0;
    size_t star = std::string_view::npos;
    size_t match = 0;
    while (ti < text.size()) {
        if (pi < pattern.size() && pattern[pi] == '*') {
            star = pi++;
            match = ti;
        } else if (pi < pattern.size() && (pattern[pi] == '?' || pattern[pi] == text[ti])) {
            pi++;
            ti++;
        } else if (star != std::string_view::npos) {
            pi = star + 1;
            ti = ++match;
        } else {
            return false;
        }
    }
    while (pi < pattern.size() && pattern[pi] == '*')
        pi++;
    return pi == pattern.size();
}

/// @brief Match a normalized slash path against component-aware wildcards.
/// @details A single `*` stays within one component, `?` consumes one byte, `**/` consumes zero
///          or more complete components, and bare `**` may consume separators.
/// @param text Normalized candidate path.
/// @param pattern Normalized wildcard pattern.
/// @return True when the complete path matches the complete pattern.
bool pathGlobMatch(std::string_view text, std::string_view pattern) {
    std::vector<std::string_view> stackText{text};
    if (pattern.find("**") == std::string_view::npos)
        return wildcardMatchRec(text, pattern);

    // Small recursive matcher for ** over normalized slash paths.
    /// @brief Match suffixes beginning at one text and pattern offset.
    /// @param ti Current byte offset in @p text.
    /// @param pi Current byte offset in @p pattern.
    /// @return `true` when both remaining suffixes match completely.
    std::function<bool(size_t, size_t)> rec = [&](size_t ti, size_t pi) -> bool {
        if (pi == pattern.size())
            return ti == text.size();
        if (pi + 1 < pattern.size() && pattern[pi] == '*' && pattern[pi + 1] == '*') {
            size_t next = pi + 2;
            bool hadSlash = false;
            if (next < pattern.size() && pattern[next] == '/') {
                next++;
                hadSlash = true;
            }
            if (hadSlash) {
                // `**/` consumes zero or more COMPLETE path components, so the
                // remainder may only begin at a component boundary — the current
                // position (zero components) or immediately after a '/'. Trying
                // it at every byte offset let a pattern like `**/bar` match
                // `foobar` mid-component (VDOC-192).
                for (size_t i = ti; i <= text.size(); i++) {
                    if (i == ti || text[i - 1] == '/') {
                        if (rec(i, next))
                            return true;
                    }
                }
                return false;
            }
            // A bare `**` (no trailing slash, e.g. `a/**`) matches any run of
            // characters including separators.
            for (size_t i = ti; i <= text.size(); i++) {
                if (rec(i, next))
                    return true;
            }
            return false;
        }
        if (ti >= text.size())
            return false;
        if (pattern[pi] == '*') {
            for (size_t i = ti; i <= text.size() && (i == ti || text[i - 1] != '/'); i++) {
                if (rec(i, pi + 1))
                    return true;
            }
            return false;
        }
        if (pattern[pi] == '?' || pattern[pi] == text[ti])
            return rec(ti + 1, pi + 1);
        return false;
    };
    return rec(0, 0);
}

/// @brief Evaluate one normalized gitignore-style pattern against a relative path.
/// @details Handles escaped control markers, root anchoring, basename-only patterns, and
///          directory-only trailing slashes. Negation is handled by the caller.
/// @param pattern Pattern to normalize and evaluate.
/// @param relativePath Workspace-relative candidate path.
/// @param isDir True when the candidate itself is a directory.
/// @return True when the pattern selects the candidate or its containing directory as applicable.
bool patternMatchesPath(std::string pattern, const std::string &relativePath, bool isDir) {
    pattern = normalizeGitignorePattern(pattern);
    if (pattern.empty())
        return false;
    for (size_t i = 0; i + 1 < pattern.size();) {
        if (pattern[i] == '\\' && (pattern[i + 1] == '#' || pattern[i + 1] == '!') &&
            (i == 0 || pattern[i - 1] == '/')) {
            pattern.erase(i, 1);
            continue;
        }
        i++;
    }
    pattern = normalizeSlashes(pattern);
    bool dirOnly = !pattern.empty() && pattern.back() == '/';
    if (dirOnly)
        pattern.pop_back();
    if (pattern.rfind("/", 0) == 0)
        pattern.erase(0, 1);

    std::string rel = normalizeSlashes(relativePath);
    std::string relDir = isDir ? rel : fs::path(rel).parent_path().generic_string();
    std::string basename = fs::path(rel).filename().generic_string();

    if (dirOnly) {
        if (isDir && (pathGlobMatch(rel, pattern) || pathGlobMatch(basename, pattern)))
            return true;
        std::string prefix = pattern;
        if (!prefix.empty() && prefix.back() != '/')
            prefix.push_back('/');
        return rel.rfind(prefix, 0) == 0 || rel.find("/" + prefix) != std::string::npos ||
               (!relDir.empty() && (relDir == pattern || relDir.rfind(prefix, 0) == 0 ||
                                    relDir.find("/" + prefix) != std::string::npos));
    }

    if (pattern.find('/') == std::string::npos) {
        if (pathGlobMatch(basename, pattern))
            return true;
        std::stringstream ss(rel);
        std::string segment;
        while (std::getline(ss, segment, '/')) {
            if (pathGlobMatch(segment, pattern))
                return true;
        }
        return false;
    }
    return pathGlobMatch(rel, pattern);
}

/// @brief Load active patterns from the `.gitignore` directly beneath a directory.
/// @details Normalizes whitespace and discards blank and unescaped comment lines. An absent or
///          unreadable file produces an empty vector.
/// @param root Directory whose `.gitignore` should be read.
/// @return Normalized non-comment patterns in file order.
std::vector<std::string> readGitignorePatterns(const fs::path &root) {
    std::vector<std::string> patterns;
    std::ifstream in(root / ".gitignore");
    std::string line;
    while (std::getline(in, line)) {
        line = normalizeGitignorePattern(line);
        if (line.empty() || line[0] == '#')
            continue;
        patterns.push_back(line);
    }
    return patterns;
}

/// @brief Bound the gitignore cache by dropping least-recently inserted entries.
/// @details The cache is a simple singly linked list with newest entries at the
///          head. When more than `kGitignoreCacheMaxEntries` roots have been
///          seen, this helper deletes the tail nodes under the cache mutex.
static void pruneGitignoreCacheLocked() {
    size_t count = 0;
    GitignoreCacheEntry *prev = nullptr;
    GitignoreCacheEntry *entry = g_gitignoreCacheHead;
    while (entry) {
        count++;
        if (count > kGitignoreCacheMaxEntries) {
            if (prev)
                prev->next = nullptr;
            while (entry) {
                GitignoreCacheEntry *next = entry->next;
                delete entry;
                entry = next;
            }
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

/// @brief Return a file modification time truncated to whole seconds.
/// @details Used for the human-facing `modified` field in enumeration maps.
/// @param path Filesystem path to stat.
/// @return Modification time in Unix epoch seconds, or -1 when metadata cannot be read.
int64_t fileTimeSeconds(const fs::path &path) {
#if RT_PLATFORM_WINDOWS
    struct _stat64 st{};
    const std::wstring wide = path.wstring();
    if (_wstat64(wide.c_str(), &st) != 0)
        return -1;
#else
    struct stat st{};
    if (stat(path.c_str(), &st) != 0)
        return -1;
#endif
    return static_cast<int64_t>(st.st_mtime);
}

/// @brief Return a content-derived cache identity for a `.gitignore` file.
/// @details Hashes the raw file bytes (with the length folded in), so ANY edit
///          changes the identity and invalidates the cache regardless of the
///          modification timestamp's resolution — a whole-second mtime missed a
///          same-second rewrite entirely (VDOC-193). `.gitignore` files are
///          small, so reading them to hash is cheap. Returns -1 when the file
///          does not exist (so callers keep treating a negative result as "no
///          `.gitignore`").
/// @param path Path of the `.gitignore` file to fingerprint.
/// @return Nonnegative content identity for a readable file, or -1 when absent or unreadable.
int64_t gitignoreCacheIdentity(const fs::path &path) {
    std::error_code ec;
    if (!fs::exists(path, ec) || ec)
        return -1;

    const uintmax_t fileSize = fs::file_size(path, ec);
    if (ec)
        return -1;

    std::ifstream in(path, std::ios::binary);
    if (!in)
        return -1;

    uint64_t h = 1469598103934665603ULL; // FNV-1a 64-bit offset basis
    const uint64_t prime = 1099511628211ULL;
    char buf[4096];
    uint64_t total = 0;
    uintmax_t remaining = fileSize;
    while (remaining != 0) {
        const size_t chunk = static_cast<size_t>(std::min<uintmax_t>(remaining, sizeof(buf)));
        in.read(buf, static_cast<std::streamsize>(chunk));
        if (!in)
            return -1;
        for (size_t i = 0; i < chunk; ++i) {
            h ^= static_cast<unsigned char>(buf[i]);
            h *= prime;
        }
        total += static_cast<uint64_t>(chunk);
        remaining -= chunk;
    }
    // Fold the length so a truncation that leaves a hash-colliding prefix still
    // changes the identity.
    for (int i = 0; i < 8; ++i) {
        h ^= (total >> (i * 8)) & 0xFFu;
        h *= prime;
    }
    // Mask to non-negative so a present file never collides with the -1
    // "absent" sentinel.
    return static_cast<int64_t>(h & 0x7FFFFFFFFFFFFFFFULL);
}

/// @brief Mix one byte into a workspace file-index fingerprint.
/// @details Uses FNV-1a because the status helper needs a deterministic,
///          allocation-free summary rather than a cryptographic hash. The
///          fingerprint is only used to notice that the project tree changed
///          between fallback watcher scans.
/// @param hash Current fingerprint accumulator.
/// @param byte Byte to fold into @p hash.
/// @return Updated fingerprint accumulator.
uint64_t workspaceFingerprintByte(uint64_t hash, unsigned char byte) {
    hash ^= static_cast<uint64_t>(byte);
    return hash * kWorkspaceFingerprintPrime;
}

/// @brief Mix a string field into a workspace file-index fingerprint.
/// @details A trailing NUL separator prevents adjacent fields from producing
///          the same byte stream after concatenation.
/// @param hash Current fingerprint accumulator.
/// @param text Field text to fold into @p hash.
/// @return Updated fingerprint accumulator.
uint64_t workspaceFingerprintString(uint64_t hash, std::string_view text) {
    for (unsigned char ch : text)
        hash = workspaceFingerprintByte(hash, ch);
    return workspaceFingerprintByte(hash, 0);
}

/// @brief Mix an integer field into a workspace file-index fingerprint.
/// @details Integers are folded as eight little-endian bytes so size and mtime
///          changes influence the same stable fingerprint as path changes.
/// @param hash Current fingerprint accumulator.
/// @param value Integer field value to fold into @p hash.
/// @return Updated fingerprint accumulator.
uint64_t workspaceFingerprintInt(uint64_t hash, int64_t value) {
    uint64_t raw = static_cast<uint64_t>(value);
    for (int i = 0; i < 8; i++) {
        hash = workspaceFingerprintByte(hash, static_cast<unsigned char>((raw >> (i * 8)) & 0xffu));
    }
    return hash;
}

/// @brief Mix one emitted file-index entry into a workspace fingerprint.
/// @details Includes the normalized relative path, directory flag, file size,
///          and modified timestamp. This is intentionally metadata-only so the
///          fallback watcher scan stays cheap on large workspaces.
/// @param hash Current fingerprint accumulator.
/// @param relativePath Normalized project-relative path.
/// @param isDir True when the entry is a directory.
/// @param size File size in bytes, or 0 for directories.
/// @param modified Last modification time in seconds when available.
/// @return Updated fingerprint accumulator.
uint64_t workspaceFingerprintEntry(
    uint64_t hash, std::string_view relativePath, bool isDir, int64_t size, int64_t modified) {
    hash = workspaceFingerprintString(hash, relativePath);
    hash = workspaceFingerprintInt(hash, isDir ? 1 : 0);
    hash = workspaceFingerprintInt(hash, size);
    hash = workspaceFingerprintInt(hash, modified);
    return hash;
}

/// @brief Return cached normalized patterns for one directory's `.gitignore`.
/// @details Keys entries by normalized absolute directory and content-derived identity. Cache
///          misses are loaded outside the spin lock, then reconciled under the lock; allocation
///          failure simply returns the uncached patterns. The bounded cache retains at most 64
///          directory entries.
/// @param root Directory whose `.gitignore` patterns are requested.
/// @return Pattern vector copied from the cache or freshly read from disk.
std::vector<std::string> cachedGitignorePatterns(const fs::path &root) {
    std::error_code ec;
    std::string key = normalizeSlashes(fs::absolute(root, ec).lexically_normal().string());
    if (ec)
        key = normalizeSlashes(root.lexically_normal().string());

    // Use a high-resolution identity (mtime sec+subsec, size, inode) rather
    // than a whole-second mtime, so a same-second `.gitignore` rewrite is not
    // served from a stale cache (VDOC-193).
    const int64_t modified = gitignoreCacheIdentity(root / ".gitignore");
    {
        GitignoreCacheLockGuard lock;
        for (GitignoreCacheEntry *entry = g_gitignoreCacheHead; entry; entry = entry->next) {
            if (entry->key == key && entry->modified == modified)
                return entry->patterns;
        }
    }

    std::vector<std::string> patterns;
    if (modified >= 0)
        patterns = readGitignorePatterns(root);

    {
        GitignoreCacheLockGuard lock;
        for (GitignoreCacheEntry *entry = g_gitignoreCacheHead; entry; entry = entry->next) {
            if (entry->key == key && entry->modified == modified)
                return entry->patterns;
            if (entry->key == key) {
                entry->modified = modified;
                entry->patterns = patterns;
                return patterns;
            }
        }

        auto *entry = new (std::nothrow) GitignoreCacheEntry();
        if (!entry)
            return patterns;
        entry->key = key;
        entry->modified = modified;
        entry->patterns = patterns;
        entry->next = g_gitignoreCacheHead;
        g_gitignoreCacheHead = entry;
        pruneGitignoreCacheLocked();
    }
    return patterns;
}

/// @brief Collect root and nested `.gitignore` patterns for a candidate path.
/// @details Walks from the workspace root to the candidate's parent directory,
///          loading each `.gitignore` through the shared cache. Patterns from
///          nested files are rebased to root-relative paths so the existing
///          matcher can evaluate the combined list without knowing which file
///          contributed each pattern.
/// @param root Workspace root.
/// @param relative_path Candidate path relative to @p root.
/// @return Combined normalized patterns in evaluation order.
std::vector<std::string> gitignorePatternsForPath(const fs::path &root,
                                                  const std::string &relative_path) {
    std::vector<std::string> combined;
    std::vector<fs::path> dirs;
    dirs.push_back(root);
    fs::path rel_path(relative_path);
    fs::path current;
    for (const auto &part : rel_path.parent_path()) {
        current /= part;
        dirs.push_back(root / current);
    }
    for (const auto &dir : dirs) {
        std::error_code ec;
        fs::path rel = fs::relative(dir, root, ec);
        std::string base_rel = (!ec && rel != ".") ? normalizeSlashes(rel.generic_string()) : "";
        for (const auto &pattern : cachedGitignorePatterns(dir))
            combined.push_back(rebaseGitignorePattern(base_rel, pattern));
    }
    return combined;
}

/// @brief Apply hard exclusions and ordered caller/gitignore patterns to one path.
/// @details Hard exclusions cannot be negated. Remaining patterns are evaluated in order with
///          last-match-wins behavior; leading `!` clears the ignored state for a matching path.
/// @param relativePath Workspace-relative candidate path.
/// @param isDir True when the candidate is a directory.
/// @param extraPatterns Caller-supplied patterns evaluated before `.gitignore` patterns.
/// @param gitignorePatterns Root-rebased `.gitignore` patterns.
/// @return True when the candidate should be omitted.
bool shouldIgnorePathWithPatterns(const std::string &relativePath,
                                  bool isDir,
                                  const std::vector<std::string> &extraPatterns,
                                  const std::vector<std::string> &gitignorePatterns) {
    static const char *hardExcludes[] = {".*/",
                                         ".*",
                                         ".git/",
                                         ".hg/",
                                         ".svn/",
                                         ".zanna/",
                                         ".zanna-cache/",
                                         "build/",
                                         "cmake-build-*/",
                                         "node_modules/",
                                         ".DS_Store"};

    std::string rel = normalizeSlashes(relativePath);
    for (const char *pattern : hardExcludes) {
        if (patternMatchesPath(pattern, rel, isDir))
            return true;
    }

    std::vector<std::string> patterns = extraPatterns;
    patterns.insert(patterns.end(), gitignorePatterns.begin(), gitignorePatterns.end());

    bool ignored = false;
    for (std::string pattern : patterns) {
        pattern = normalizeGitignorePattern(pattern);
        if (pattern.empty() || pattern[0] == '#')
            continue;
        bool negated = pattern[0] == '!';
        if (negated)
            pattern.erase(0, 1);
        if (patternMatchesPath(pattern, rel, isDir))
            ignored = !negated;
    }
    return ignored;
}

/// @brief Determine whether a workspace path is excluded by configured ignore rules.
/// @param root Workspace root used to locate nested `.gitignore` files.
/// @param relativePath Candidate path relative to @p root.
/// @param isDir True when the candidate is a directory.
/// @param extraPatterns Caller-supplied exclusion and negation patterns.
/// @param includeGitignore True to load root and nested `.gitignore` patterns.
/// @return True when hard exclusions or the ordered patterns ignore the path.
bool shouldIgnorePath(const fs::path &root,
                      const std::string &relativePath,
                      bool isDir,
                      const std::vector<std::string> &extraPatterns,
                      bool includeGitignore) {
    std::vector<std::string> gitignorePatterns;
    if (includeGitignore)
        gitignorePatterns = gitignorePatternsForPath(root, relativePath);
    return shouldIgnorePathWithPatterns(relativePath, isDir, extraPatterns, gitignorePatterns);
}

/// @brief Derive a deterministic positive identifier from normalized path text.
/// @details Uses FNV-1a and clears the sign bit; this is a stable UI/index key rather than a
///          collision-proof or cryptographic identity.
/// @param path Path bytes to hash.
/// @return Nonnegative 63-bit path identifier.
int64_t stablePathId(const std::string &path) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char ch : path) {
        hash ^= ch;
        hash *= 1099511628211ULL;
    }
    return static_cast<int64_t>(hash & 0x7fffffffffffffffULL);
}

/// @brief Append one structured diagnostic map to a runtime sequence.
/// @param seq Runtime sequence that will own the new diagnostic.
/// @param message Human-readable diagnostic text.
/// @param file Associated path, or an empty string when not file-specific.
/// @param line One-based associated line number, or zero when unavailable.
/// @param code Stable diagnostic code.
void pushDiagnostic(void *seq,
                    const std::string &message,
                    const std::string &file,
                    int64_t line,
                    const std::string &code);

/**
 * @brief Cached recursive iterator and filter state for one workspace index page.
 * @details A cursor is detached under the cache lock while scanning, tracks
 * the logical matched offset and traversal cap, then is destroyed or reinserted
 * with a refreshed recency timestamp.
 */
struct WorkspaceFileIndexPageCursor {
    std::string key;
    fs::path root;
    std::set<std::string> extensions;
    std::vector<std::string> extraPatterns;
    bool includeDirs{false};
    fs::recursive_directory_iterator it;
    fs::recursive_directory_iterator end;
    std::error_code ec;
    int64_t matched{0};
    bool done{false};
    bool truncated{false};
    uint64_t lastUsed{0};
    WorkspaceFileIndexPageCursor *next{nullptr};
};

/** Head of the process-local resumable workspace file-index cursor cache. */
WorkspaceFileIndexPageCursor *g_fileIndexPageCursorHead = nullptr;

/// @brief Parse and normalize a file-extension allow-list.
/// @details Splits the serialized list, prepends a dot when absent, lowercases each extension,
///          and deduplicates through an ordered set.
/// @param extensionsCsv Comma-, semicolon-, or newline-delimited extension text.
/// @return Normalized lowercase extension set; empty means no extension filtering.
std::set<std::string> parseExtensionSet(const std::string &extensionsCsv) {
    std::set<std::string> extensions;
    for (std::string ext : splitList(extensionsCsv)) {
        if (!ext.empty() && ext[0] != '.')
            ext.insert(ext.begin(), '.');
        extensions.insert(lower(ext));
    }
    return extensions;
}

/// @brief Build the private cache key for a paged file-index traversal.
/// @param root Normalized workspace root.
/// @param extensionsCsv Serialized extension filter.
/// @param excludesCsv Serialized caller exclusion patterns.
/// @param includeDirs True when directory entries are part of the traversal.
/// @return Composite key containing normalized root and exact filter inputs.
std::string fileIndexPageKey(const fs::path &root,
                             const std::string &extensionsCsv,
                             const std::string &excludesCsv,
                             bool includeDirs) {
    return normalizeSlashes(root.generic_string()) + "\n" + extensionsCsv + "\n" + excludesCsv +
           "\n" + (includeDirs ? "1" : "0");
}

/// @brief Destroy one FileIndex.Page cursor node.
/// @details The recursive_directory_iterator and owned filter vectors release
///          through the cursor destructor. The caller must unlink the node from
///          the cache list before calling this helper.
/// @param cursor Cursor node to destroy; may be NULL.
void destroyFileIndexPageCursor(WorkspaceFileIndexPageCursor *cursor) {
    delete cursor;
}

/// @brief Insert @p cursor at the front of the process-local page cursor cache.
/// @details Callers hold FileIndexPageCursorLockGuard while mutating the list.
/// @param cursor Cursor node to link; ignored when NULL.
void linkFileIndexPageCursor(WorkspaceFileIndexPageCursor *cursor) {
    if (!cursor)
        return;
    cursor->next = g_fileIndexPageCursorHead;
    g_fileIndexPageCursorHead = cursor;
}

/// @brief Remove @p cursor from the process-local page cursor cache.
/// @details No memory is freed here; this split lets callers copy final cursor
///          state before destroying a completed traversal.
/// @param cursor Cursor node to unlink; ignored when NULL or absent.
void unlinkFileIndexPageCursor(WorkspaceFileIndexPageCursor *cursor) {
    if (!cursor)
        return;
    WorkspaceFileIndexPageCursor **slot = &g_fileIndexPageCursorHead;
    while (*slot) {
        if (*slot == cursor) {
            *slot = cursor->next;
            cursor->next = nullptr;
            return;
        }
        slot = &(*slot)->next;
    }
}

/// @brief Count live FileIndex.Page cursors in the process-local cache.
/// @return Number of linked cursor nodes.
size_t fileIndexPageCursorCount() {
    size_t count = 0;
    for (WorkspaceFileIndexPageCursor *cursor = g_fileIndexPageCursorHead; cursor;
         cursor = cursor->next) {
        count++;
    }
    return count;
}

/// @brief Evict least-recently-used page cursors until the cache is under limit.
/// @details Each cursor can hold a recursive_directory_iterator. Keeping the
///          cache deliberately small prevents a burst of unrelated page requests
///          from retaining too many native directory handles.
/// @return Nothing.
void evictFileIndexPageCursorsIfNeeded() {
    while (fileIndexPageCursorCount() > kFileIndexPageCursorMaxEntries) {
        WorkspaceFileIndexPageCursor *oldest = nullptr;
        for (WorkspaceFileIndexPageCursor *cursor = g_fileIndexPageCursorHead; cursor;
             cursor = cursor->next) {
            if (!oldest || cursor->lastUsed < oldest->lastUsed)
                oldest = cursor;
        }
        if (!oldest)
            return;
        unlinkFileIndexPageCursor(oldest);
        destroyFileIndexPageCursor(oldest);
    }
}

/// @brief Find a cursor positioned exactly at @p offset for @p key.
/// @details Exact-offset matching preserves the public stateless contract: random
///          offsets start a fresh traversal, while sequential callers resume the
///          cursor returned by their previous nextOffset.
/// @param key Normalized traversal key for root, filters, and include-dir mode.
/// @param offset Logical match offset the caller wants to resume from.
/// @return Matching live cursor, or NULL when a fresh traversal is required.
WorkspaceFileIndexPageCursor *findFileIndexPageCursor(const std::string &key, int64_t offset) {
    for (WorkspaceFileIndexPageCursor *cursor = g_fileIndexPageCursorHead; cursor;
         cursor = cursor->next) {
        if (cursor->key == key && cursor->matched == offset && !cursor->done)
            return cursor;
    }
    return nullptr;
}

/// @brief Start a new FileIndex.Page traversal cursor.
/// @details The returned cursor is not linked into the cache; callers link it
///          only after construction succeeds so failed traversals cannot leave
///          partially initialized cache entries behind.
/// @param key Normalized traversal key for cache lookup.
/// @param root Absolute workspace root path.
/// @param extensionsCsv Comma-separated extension allow-list.
/// @param excludesCsv Comma-separated extra ignore patterns.
/// @param includeDirs True to emit matching directory entries.
/// @param diagnostics Runtime sequence receiving traversal diagnostics.
/// @return Newly allocated cursor, or NULL on allocation/traversal failure.
WorkspaceFileIndexPageCursor *startFileIndexPageCursor(const std::string &key,
                                                       const fs::path &root,
                                                       const std::string &extensionsCsv,
                                                       const std::string &excludesCsv,
                                                       bool includeDirs,
                                                       void *diagnostics) {
    auto *cursor = new (std::nothrow) WorkspaceFileIndexPageCursor();
    if (!cursor) {
        pushDiagnostic(diagnostics,
                       "workspace file-index cursor allocation failed",
                       root.string(),
                       0,
                       "fileindex.cursor");
        return nullptr;
    }
    cursor->key = key;
    cursor->root = root;
    cursor->extensions = parseExtensionSet(extensionsCsv);
    cursor->extraPatterns = splitList(excludesCsv);
    cursor->includeDirs = includeDirs;
    cursor->lastUsed = g_fileIndexPageCursorClock.fetch_add(1, std::memory_order_relaxed) + 1;
    cursor->it = fs::recursive_directory_iterator(
        root, fs::directory_options::skip_permission_denied, cursor->ec);
    if (cursor->ec) {
        pushDiagnostic(diagnostics,
                       "workspace traversal failed: " + cursor->ec.message(),
                       root.generic_string(),
                       0,
                       "fileindex.traverse");
        delete cursor;
        return nullptr;
    }
    return cursor;
}

/// @brief Construct and append one workspace file-index entry map.
/// @details Emits absolute and relative paths, name, extension, kind, directory flag, stable path
///          ID, bounded file size, and modification time. The result sequence takes ownership of
///          the newly created map.
/// @param entries Runtime sequence receiving the entry.
/// @param root Workspace root associated with the entry; retained for interface consistency.
/// @param dirEntry Native directory entry supplying path and metadata.
/// @param relativePath Normalized path relative to @p root.
/// @param isDir True when @p dirEntry represents a directory.
void emitFileIndexEntry(void *entries,
                        const fs::path &root,
                        const fs::directory_entry &dirEntry,
                        const std::string &relativePath,
                        bool isDir) {
    void *entry = rt_map_new();
    std::error_code pathEc;
    fs::path absPath = fs::absolute(dirEntry.path(), pathEc).lexically_normal();
    const std::string path = pathEc ? dirEntry.path().generic_string() : absPath.generic_string();
    mapSetStr(entry, "path", path);
    mapSetStr(entry, "relativePath", relativePath);
    mapSetStr(entry, "name", dirEntry.path().filename().generic_string());
    mapSetStr(entry, "extension", dirEntry.path().extension().generic_string());
    mapSetStr(entry, "kind", isDir ? "directory" : "file");
    rt_map_set_bool(entry, rt_const_cstr("isDirectory"), isDir ? 1 : 0);
    rt_map_set_int(entry, rt_const_cstr("id"), stablePathId(normalizeSlashes(path)));
    int64_t fileSize = 0;
    if (!isDir) {
        std::error_code sizeEc;
        uintmax_t rawSize = dirEntry.file_size(sizeEc);
        if (!sizeEc && rawSize <= static_cast<uintmax_t>(INT64_MAX))
            fileSize = static_cast<int64_t>(rawSize);
    }
    rt_map_set_int(entry, rt_const_cstr("size"), fileSize);
    rt_map_set_int(entry, rt_const_cstr("modified"), fileTimeSeconds(dirEntry.path()));
    seqPushOwned(entries, entry);
    (void)root;
}

/// @brief Emit up to @p limit entries from @p cursor starting at @p offset.
/// @details Cursor state advances across calls. The caller owns cache locking and
///          is responsible for unlinking/destroying the cursor when done.
/// @param cursor Live traversal cursor to scan.
/// @param entries Runtime sequence receiving emitted file-index maps.
/// @param offset Logical match offset requested by the caller.
/// @param limit Maximum number of entries to emit.
/// @return Number of entries emitted into @p entries.
int64_t scanFileIndexPageCursor(WorkspaceFileIndexPageCursor *cursor,
                                void *entries,
                                int64_t offset,
                                int64_t limit) {
    if (!cursor || cursor->done)
        return 0;

    int64_t emitted = 0;
    for (; !cursor->ec && cursor->it != cursor->end; cursor->it.increment(cursor->ec)) {
        std::error_code relEc;
        const fs::directory_entry &dirEntry = *cursor->it;
        std::string rel =
            normalizeSlashes(fs::relative(dirEntry.path(), cursor->root, relEc).generic_string());
        if (relEc || rel.empty() || rel == ".")
            continue;

        bool isDir = dirEntry.is_directory(cursor->ec);
        const auto gitignorePatterns = gitignorePatternsForPath(cursor->root, rel);
        if (shouldIgnorePathWithPatterns(rel, isDir, cursor->extraPatterns, gitignorePatterns)) {
            if (isDir)
                cursor->it.disable_recursion_pending();
            continue;
        }
        if (isDir && !cursor->includeDirs)
            continue;
        if (!isDir && !cursor->extensions.empty()) {
            std::string ext = lower(dirEntry.path().extension().generic_string());
            if (!cursor->extensions.count(ext))
                continue;
        }
        if (cursor->matched >= kWorkspaceFileIndexMaxEntries) {
            cursor->truncated = true;
            cursor->done = true;
            break;
        }

        if (cursor->matched >= offset && emitted < limit) {
            emitFileIndexEntry(entries, cursor->root, dirEntry, rel, isDir);
            emitted++;
        }
        cursor->matched++;
        if (emitted >= limit) {
            cursor->it.increment(cursor->ec);
            cursor->done = cursor->ec || cursor->it == cursor->end;
            return emitted;
        }
    }

    cursor->done = true;
    return emitted;
}

/// @brief Create a structured runtime diagnostic map.
/// @param message Human-readable diagnostic text.
/// @param file Associated path, or an empty string.
/// @param line One-based line number, or zero when unavailable.
/// @param code Stable diagnostic code, or an empty string.
/// @return Fresh runtime map containing `message`, `file`, `code`, and `line`.
void *makeDiagnostic(const std::string &message,
                     const std::string &file = {},
                     int64_t line = 0,
                     const std::string &code = {}) {
    void *diag = rt_map_new();
    mapSetStr(diag, "message", message);
    mapSetStr(diag, "file", file);
    mapSetStr(diag, "code", code);
    rt_map_set_int(diag, rt_const_cstr("line"), line);
    return diag;
}

/// @brief Append a newly constructed diagnostic map to an owning sequence.
/// @param seq Runtime sequence that receives ownership of the map.
/// @param message Human-readable diagnostic text.
/// @param file Associated path, or an empty string.
/// @param line One-based line number, or zero when unavailable.
/// @param code Stable diagnostic code, or an empty string.
void pushDiagnostic(void *seq,
                    const std::string &message,
                    const std::string &file = {},
                    int64_t line = 0,
                    const std::string &code = {}) {
    void *diag = makeDiagnostic(message, file, line, code);
    seqPushOwned(seq, diag);
}

/// @brief Copy native strings into a fresh owning runtime sequence.
/// @param items Native byte strings to convert in order.
/// @return Fresh owning Seq of runtime strings.
void *makeStringSeq(const std::vector<std::string> &items) {
    void *seq = rt_seq_new_owned();
    for (const auto &item : items) {
        rt_string s = makeString(item);
        rt_seq_push(seq, s);
        rt_string_unref(s);
    }
    return seq;
}

/// @brief Copy a runtime map's string field into a native string.
/// @param map Borrowed runtime map.
/// @param key Null-terminated field name.
/// @return Copied field bytes, or an empty string when the field is absent/invalid/empty.
std::string mapGetString(void *map, const char *key) {
    rt_string value = rt_map_get_str(map, rt_const_cstr(key));
    std::string out = toStd(value);
    rt_string_unref(value);
    return out;
}

/// @brief Convert a watcher event code to its manifest-friendly name.
/// @param type `RT_WATCH_EVENT_*` code.
/// @return `created`, `modified`, `deleted`, `renamed`, `overflow`, or `none`.
std::string eventTypeName(int64_t type) {
    switch (type) {
        case RT_WATCH_EVENT_CREATED:
            return "created";
        case RT_WATCH_EVENT_MODIFIED:
            return "modified";
        case RT_WATCH_EVENT_DELETED:
            return "deleted";
        case RT_WATCH_EVENT_RENAMED:
            return "renamed";
        case RT_WATCH_EVENT_OVERFLOW:
            return "overflow";
        default:
            return "none";
    }
}

/// @brief Create a project-manifest map populated with runtime defaults.
/// @return Fresh runtime map with scalar defaults and empty owning collections.
void *newManifestMap() {
    void *map = rt_map_new();
    mapSetStr(map, "name", "");
    mapSetStr(map, "version", "0.0.0");
    mapSetStr(map, "language", "zia");
    mapSetStr(map, "entry", "");
    mapSetStr(map, "defaultScene", "");
    mapSetSeq(map, "sourceGlobs", makeStringSeq({"."}));
    mapSetSeq(map, "excludes", makeStringSeq({}));
    mapSetSeq(map, "assetRoots", makeStringSeq({}));
    mapSetSeq(map, "sceneRoots", makeStringSeq({}));
    mapSetSeq(map, "runConfigs", rt_seq_new_owned());
    mapSetSeq(map, "buildConfigs", rt_seq_new_owned());
    mapSetSeq(map, "diagnostics", rt_seq_new_owned());
    rt_map_set_bool(map, rt_const_cstr("valid"), 1);
    return map;
}

/// @brief Replace a manifest field with a fresh owning sequence of strings.
/// @param map Runtime manifest map to update.
/// @param key Null-terminated field name.
/// @param items Native strings to copy into the replacement sequence.
void replaceStringSeq(void *map, const char *key, const std::vector<std::string> &items) {
    void *seq = makeStringSeq(items);
    rt_map_set(map, rt_const_cstr(key), seq);
    releaseObject(seq);
}

/// @brief Append a copied string to a sequence-valued map field.
/// @details Creates and installs an owning sequence when the field is absent.
/// @param map Runtime map containing the collection field.
/// @param key Null-terminated field name.
/// @param value Native string to copy and append.
void appendToStringSeqField(void *map, const char *key, const std::string &value) {
    void *seq = rt_map_get(map, rt_const_cstr(key));
    if (!seq) {
        seq = rt_seq_new_owned();
        rt_map_set(map, rt_const_cstr(key), seq);
        releaseObject(seq);
        seq = rt_map_get(map, rt_const_cstr(key));
    }
    rt_string s = makeString(value);
    rt_seq_push(seq, s);
    rt_string_unref(s);
}

/// @brief Append a configuration map to a sequence-valued manifest field.
/// @details Creates the owning sequence when absent; the sequence retains @p config.
/// @param map Runtime manifest map containing the collection field.
/// @param key Null-terminated field name.
/// @param config Borrowed runtime configuration map to append.
void appendConfigMap(void *map, const char *key, void *config) {
    void *seq = rt_map_get(map, rt_const_cstr(key));
    if (!seq) {
        seq = rt_seq_new_owned();
        rt_map_set(map, rt_const_cstr(key), seq);
        releaseObject(seq);
        seq = rt_map_get(map, rt_const_cstr(key));
    }
    rt_seq_push(seq, config);
}

/// @brief Split a manifest directive into trimmed key and value text.
/// @details Prefers the earliest `=` or `:` separator, then falls back to the first horizontal
///          whitespace. A line without any separator produces an empty value.
/// @param line Directive line to parse.
/// @return Pair containing trimmed key and value.
std::pair<std::string, std::string> splitDirectiveLine(const std::string &line) {
    size_t eq = line.find('=');
    size_t colon = line.find(':');
    size_t sep = std::min(eq == std::string::npos ? line.size() : eq,
                          colon == std::string::npos ? line.size() : colon);
    if (sep != line.size())
        return {trim(std::string_view(line).substr(0, sep)),
                trim(std::string_view(line).substr(sep + 1))};
    size_t ws = line.find_first_of(" \t");
    if (ws == std::string::npos)
        return {trim(line), ""};
    return {trim(std::string_view(line).substr(0, ws)),
            trim(std::string_view(line).substr(ws + 1))};
}

/// @brief Canonicalize a manifest key for spelling-insensitive dispatch.
/// @param key Key text to normalize.
/// @return Lowercase key with hyphens, underscores, and periods removed.
std::string manifestKey(std::string key) {
    key = lower(key);
    /// @brief Identify manifest-key punctuation removed during canonicalization.
    /// @param c Candidate key byte.
    /// @return `true` for hyphen, underscore, or period.
    key.erase(std::remove_if(
                  key.begin(), key.end(), [](char c) { return c == '-' || c == '_' || c == '.'; }),
              key.end());
    return key;
}

/// @brief Split text into lines while normalizing CRLF endings.
/// @details Removes a terminal carriage return from each `getline` result and preserves an empty
///          final logical line when @p text ends with LF.
/// @param text Complete manifest or source text.
/// @return Lines in source order without LF/CRLF terminators.
std::vector<std::string> readLines(const std::string &text) {
    std::vector<std::string> lines;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    if (!text.empty() && text.back() == '\n')
        lines.push_back("");
    return lines;
}

/// @brief Convert a one-based byte line/column position to a string offset.
/// @details LF advances the line and resets the column; all other bytes advance one column.
///          Positions at end-of-input are accepted when they coincide with the requested cursor.
/// @param text Source bytes to index.
/// @param line One-based line number.
/// @param column One-based byte column.
/// @return Zero-based byte offset, or `std::nullopt` for invalid/out-of-range positions.
std::optional<size_t> offsetForLineColumn(const std::string &text, int64_t line, int64_t column) {
    if (line < 1 || column < 1)
        return std::nullopt;
    int64_t curLine = 1;
    int64_t curCol = 1;
    for (size_t i = 0; i <= text.size(); i++) {
        if (curLine == line && curCol == column)
            return i;
        if (i == text.size())
            break;
        if (text[i] == '\n') {
            curLine++;
            curCol = 1;
        } else {
            curCol++;
        }
    }
    return std::nullopt;
}

/**
 * @brief Validated native representation of one requested text replacement.
 * @details Stores canonical file identity, one-based source coordinates,
 * optional version expectations, replacement bytes, and resolved byte offsets
 * used for overlap detection and transactional application.
 */
struct EditRecord {
    std::string file;
    int64_t startLine{0};
    int64_t startColumn{0};
    int64_t endLine{0};
    int64_t endColumn{0};
    std::string newText;
    int64_t expectedMtime{-1};
    int64_t expectedSize{-1};
    size_t startOffset{0};
    size_t endOffset{0};
};

/// @brief Return whether a canonical path is equal to or below a canonical root.
/// @param candidate Canonical absolute edit target.
/// @param root Canonical absolute workspace root.
/// @return True when the relative path contains no parent traversal.
bool editTargetIsInRoot(const fs::path &candidate, const fs::path &root) {
    std::error_code ec;
    fs::path relative = fs::relative(candidate, root, ec);
    if (ec || relative.is_absolute())
        return false;
    for (const auto &part : relative) {
        if (part == "..")
            return false;
    }
    return true;
}

/// @brief Resolve an edit target and optionally constrain it to workspace roots.
/// @details The existing edit API accepts paths directly. Rooted callers pass
///          canonical roots so this helper can reject traversal outside every
///          opened workspace before any file is read or written. Accepted paths
///          are returned as canonical absolute strings so equivalent and
///          symlinked spellings group as one file.
/// @param file User-supplied edit target path.
/// @param roots Optional canonical roots; NULL keeps the legacy unrooted behavior.
/// @param out Receives the normalized absolute path on success.
/// @return `true` when the target is usable for validation/apply.
bool resolveEditTarget(const std::string &file,
                       const std::vector<fs::path> *roots,
                       std::string &out) {
    std::error_code ec;
    fs::path candidate(file);
    if (roots && candidate.is_relative()) {
        if (roots->size() != 1)
            return false;
        candidate = roots->front() / candidate;
    }
    candidate = fs::absolute(candidate, ec).lexically_normal();
    if (ec)
        return false;
    if (roots) {
        candidate = fs::weakly_canonical(candidate, ec);
        if (ec)
            return false;
        bool contained = false;
        for (const auto &root : *roots) {
            if (editTargetIsInRoot(candidate, root)) {
                contained = true;
                break;
            }
        }
        if (!contained)
            return false;
    }
    out = candidate.string();
    return true;
}

/// @brief Decode and perform structural validation on one workspace edit map.
/// @param obj Runtime map expected to contain file, range, replacement, and optional version
///            fields.
/// @param[out] out Native edit record populated from valid fields.
/// @param diagnostics Owning runtime sequence receiving validation diagnostics.
/// @param index Edit index recorded as the diagnostic line surrogate.
/// @return True when the object is a map with a nonempty file and positive one-based range.
bool loadEditRecord(void *obj, EditRecord &out, void *diagnostics, int64_t index) {
    if (!obj || rt_obj_class_id(obj) != RT_MAP_CLASS_ID) {
        pushDiagnostic(diagnostics, "workspace edit entry is not a map", "", index, "edit.invalid");
        return false;
    }
    out.file = mapGetString(obj, "file");
    out.startLine = rt_map_get_int(obj, rt_const_cstr("startLine"));
    out.startColumn = rt_map_get_int(obj, rt_const_cstr("startColumn"));
    out.endLine = rt_map_get_int(obj, rt_const_cstr("endLine"));
    out.endColumn = rt_map_get_int(obj, rt_const_cstr("endColumn"));
    out.newText = mapGetString(obj, "newText");
    out.expectedMtime = rt_map_get_int_or(obj, rt_const_cstr("expectedMtime"), -1);
    out.expectedSize = rt_map_get_int_or(obj, rt_const_cstr("expectedSize"), -1);
    if (out.file.empty()) {
        pushDiagnostic(diagnostics, "workspace edit missing file", "", index, "edit.file");
        return false;
    }
    if (out.startLine < 1 || out.startColumn < 1 || out.endLine < 1 || out.endColumn < 1) {
        pushDiagnostic(
            diagnostics, "workspace edit has invalid 1-based range", out.file, index, "edit.range");
        return false;
    }
    return true;
}

/// @brief Resolve, load, version-check, range-check, and overlap-check an edit batch.
/// @details Reads each target at most once into @p contents, converts ranges to byte offsets, then
///          groups and sorts edits per file to reject overlap. All discovered failures append
///          diagnostics so callers receive a complete validation report.
/// @param[in,out] records Edit records whose paths and byte offsets are normalized on success.
/// @param[out] contents Cache populated with the original bytes of each readable target.
/// @param diagnostics Runtime sequence receiving structured failures.
/// @param roots Optional canonical workspace roots constraining every target.
/// @return True only when every record passes all batch validation checks.
bool validateEditRecords(std::vector<EditRecord> &records,
                         std::unordered_map<std::string, std::string> &contents,
                         void *diagnostics,
                         const std::vector<fs::path> *roots) {
    bool ok = true;
    for (auto &record : records) {
        std::string resolvedFile;
        if (!resolveEditTarget(record.file, roots, resolvedFile)) {
            pushDiagnostic(diagnostics,
                           "workspace edit target is outside the workspace",
                           record.file,
                           0,
                           "edit.root");
            ok = false;
            continue;
        }
        record.file = resolvedFile;
        if (!contents.count(record.file)) {
            std::ifstream in(record.file, std::ios::binary);
            if (!in) {
                pushDiagnostic(diagnostics, "cannot read edit target", record.file, 0, "edit.read");
                ok = false;
                continue;
            }
            std::ostringstream buffer;
            buffer << in.rdbuf();
            contents[record.file] = buffer.str();
        }
        if (record.expectedMtime >= 0 && fileTimeSeconds(record.file) != record.expectedMtime) {
            pushDiagnostic(diagnostics,
                           "edit target changed since expectedMtime",
                           record.file,
                           0,
                           "edit.version");
            ok = false;
            continue;
        }
        if (record.expectedSize >= 0) {
            std::error_code sizeEc;
            uintmax_t raw_size = fs::file_size(record.file, sizeEc);
            if (sizeEc || raw_size != static_cast<uintmax_t>(record.expectedSize)) {
                pushDiagnostic(diagnostics,
                               "edit target changed since expectedSize",
                               record.file,
                               0,
                               "edit.version");
                ok = false;
                continue;
            }
        }
        auto &text = contents[record.file];
        auto start = offsetForLineColumn(text, record.startLine, record.startColumn);
        auto end = offsetForLineColumn(text, record.endLine, record.endColumn);
        if (!start || !end || *start > *end) {
            pushDiagnostic(diagnostics,
                           "workspace edit range is outside the file",
                           record.file,
                           0,
                           "edit.range");
            ok = false;
            continue;
        }
        record.startOffset = *start;
        record.endOffset = *end;
    }

    std::map<std::string, std::vector<EditRecord *>> byFile;
    for (auto &record : records)
        byFile[record.file].push_back(&record);
    for (auto &[file, vec] : byFile) {
        /// @brief Order edit pointers by ascending start offset for overlap validation.
        /// @param a First edit pointer.
        /// @param b Second edit pointer.
        /// @return `true` when @p a begins before @p b.
        std::sort(vec.begin(), vec.end(), [](const EditRecord *a, const EditRecord *b) {
            return a->startOffset < b->startOffset;
        });
        for (size_t i = 1; i < vec.size(); i++) {
            if (vec[i - 1]->endOffset > vec[i]->startOffset) {
                pushDiagnostic(
                    diagnostics, "workspace edit ranges overlap", file, 0, "edit.overlap");
                ok = false;
            }
        }
    }
    return ok;
}

/// @brief Convert root text into a canonical filesystem path.
/// @details Rooted edit APIs use this to define the trust boundary for every
///          target file. The root must name an existing directory so symlinks
///          and relative segments can be resolved before target comparison.
/// @param rootText Runtime root string copied into native storage.
/// @param diagnostics Diagnostic sequence that receives root validation errors.
/// @param index Root index reported with diagnostics.
/// @param out Receives the canonical root on success.
/// @return `true` when @p rootText names a usable directory.
bool workspaceEditRootFromText(const std::string &rootText,
                               void *diagnostics,
                               int64_t index,
                               fs::path &out) {
    if (rootText.empty()) {
        pushDiagnostic(diagnostics, "workspace edit root is empty", "", index, "edit.root");
        return false;
    }
    std::error_code ec;
    fs::path root = fs::absolute(fs::path(rootText), ec);
    if (!ec)
        root = fs::weakly_canonical(root, ec);
    if (ec || !fs::is_directory(root, ec)) {
        pushDiagnostic(
            diagnostics, "workspace edit root is not a directory", rootText, index, "edit.root");
        return false;
    }
    out = root;
    return true;
}

/// @brief Convert a runtime root string into a canonical filesystem path.
/// @param root_s Runtime string provided by the caller.
/// @param diagnostics Diagnostic sequence that receives root validation errors.
/// @param out Receives the canonical root on success.
/// @return `true` when @p root_s names a usable directory.
bool workspaceEditRootFromString(rt_string root_s, void *diagnostics, fs::path &out) {
    return workspaceEditRootFromText(toStd(root_s), diagnostics, 0, out);
}

/// @brief Convert a runtime Seq of root strings into canonical directories.
/// @details Duplicate canonical roots are collapsed. A malformed or empty
///          sequence fails before any edit target is inspected.
/// @param rootValues Runtime Seq containing string or boxed-string elements.
/// @param diagnostics Diagnostic sequence that receives root errors.
/// @param out Receives at least one canonical root on success.
/// @return True when every supplied root is a usable directory.
bool workspaceEditRootsFromSequence(void *rootValues,
                                    void *diagnostics,
                                    std::vector<fs::path> &out) {
    if (!rootValues || rt_obj_class_id(rootValues) != RT_SEQ_CLASS_ID) {
        pushDiagnostic(diagnostics, "workspace edit roots must be a sequence", "", 0, "edit.root");
        return false;
    }
    const int64_t count = rt_seq_len(rootValues);
    if (count <= 0) {
        pushDiagnostic(diagnostics, "workspace edit roots are empty", "", 0, "edit.root");
        return false;
    }
    bool ok = true;
    for (int64_t index = 0; index < count; ++index) {
        std::string rootText;
        if (!objectToStdString(rt_seq_get(rootValues, index), rootText)) {
            pushDiagnostic(
                diagnostics, "workspace edit root is not a string", "", index, "edit.root");
            ok = false;
            continue;
        }
        fs::path root;
        if (!workspaceEditRootFromText(rootText, diagnostics, index, root)) {
            ok = false;
            continue;
        }
        if (std::find(out.begin(), out.end(), root) == out.end())
            out.push_back(std::move(root));
    }
    return ok && !out.empty();
}

} // namespace

extern "C" {

/// @brief Return a bounded page of workspace file-index entries.
/// @details This is the allocation-bounded companion to
///          `rt_workspace_file_index_enumerate`. It walks the same ordered
///          recursive traversal, applies the same ignore and extension filters,
///          and emits at most @p limit entry maps. Sequential calls that pass the
///          returned `nextOffset` continue through a private runtime cursor so
///          later pages do not rescan the whole prefix. Random offsets restart
///          traversal and skip to the requested logical match.
/// @param root_s Runtime string naming the root directory.
/// @param extensions_csv Comma-separated extension allow-list.
/// @param excludes_csv Comma-separated additional ignore patterns.
/// @param include_dirs Non-zero to include directories that pass filters.
/// @param offset Zero-based logical match offset to start returning.
/// @param limit Maximum entries to return, clamped to 1..4096.
/// @return Runtime map containing page metadata, diagnostics, and `entries`.
void *rt_workspace_file_index_page(rt_string root_s,
                                   rt_string extensions_csv,
                                   rt_string excludes_csv,
                                   int8_t include_dirs,
                                   int64_t offset,
                                   int64_t limit) {
    void *result = rt_map_new();
    void *entries = rt_seq_new_owned();
    void *diagnostics = rt_seq_new_owned();
    if (offset < 0)
        offset = 0;
    if (limit <= 0)
        limit = 512;
    if (limit > 4096)
        limit = 4096;

    rt_map_set_bool(result, rt_const_cstr("valid"), 1);
    mapSetStr(result, "root", "");
    rt_map_set_int(result, rt_const_cstr("offset"), offset);
    rt_map_set_int(result, rt_const_cstr("limit"), limit);
    rt_map_set_int(result, rt_const_cstr("emitted"), 0);
    rt_map_set_int(result, rt_const_cstr("nextOffset"), offset);
    rt_map_set_int(result, rt_const_cstr("scanned"), 0);
    rt_map_set_int(result, rt_const_cstr("maxEntries"), kWorkspaceFileIndexMaxEntries);
    rt_map_set_bool(result, rt_const_cstr("done"), 1);
    rt_map_set_bool(result, rt_const_cstr("truncated"), 0);
    rt_map_set(result, rt_const_cstr("entries"), entries);
    rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);

    try {
        fs::path root = toStd(root_s);
        if (root.empty()) {
            rt_map_set_bool(result, rt_const_cstr("valid"), 0);
            pushDiagnostic(diagnostics, "workspace root is empty", "", 0, "fileindex.root");
            releaseObject(entries);
            releaseObject(diagnostics);
            return result;
        }

        std::error_code ec;
        root = fs::absolute(root, ec).lexically_normal();
        if (ec || !fs::is_directory(root, ec)) {
            rt_map_set_bool(result, rt_const_cstr("valid"), 0);
            pushDiagnostic(diagnostics,
                           "workspace root is not a directory",
                           root.generic_string(),
                           0,
                           "fileindex.root");
            releaseObject(entries);
            releaseObject(diagnostics);
            return result;
        }
        mapSetStr(result, "root", root.generic_string());

        const std::string extensionsCsv = toStd(extensions_csv);
        const std::string excludesCsv = toStd(excludes_csv);
        const std::string key =
            fileIndexPageKey(root, extensionsCsv, excludesCsv, include_dirs != 0);
        int64_t emitted = 0;
        bool done = true;
        bool truncated = false;
        int64_t matched = offset;
        WorkspaceFileIndexPageCursor *cursor = nullptr;

        {
            FileIndexPageCursorLockGuard cursorLock;
            cursor = findFileIndexPageCursor(key, offset);
            if (cursor) {
                unlinkFileIndexPageCursor(cursor);
            }
        }

        if (!cursor) {
            cursor = startFileIndexPageCursor(
                key, root, extensionsCsv, excludesCsv, include_dirs != 0, diagnostics);
            if (!cursor) {
                rt_map_set_bool(result, rt_const_cstr("valid"), 0);
                releaseObject(entries);
                releaseObject(diagnostics);
                return result;
            }
        }

        cursor->lastUsed = g_fileIndexPageCursorClock.fetch_add(1, std::memory_order_relaxed) + 1;
        emitted = scanFileIndexPageCursor(cursor, entries, offset, limit);
        matched = cursor->matched;
        done = cursor->done;
        truncated = cursor->truncated;
        ec = cursor->ec;
        cursor->lastUsed = g_fileIndexPageCursorClock.fetch_add(1, std::memory_order_relaxed) + 1;

        {
            FileIndexPageCursorLockGuard cursorLock;
            if (done) {
                destroyFileIndexPageCursor(cursor);
            } else {
                linkFileIndexPageCursor(cursor);
                evictFileIndexPageCursorsIfNeeded();
            }
        }

        if (ec) {
            pushDiagnostic(diagnostics,
                           "workspace traversal stopped early",
                           root.generic_string(),
                           0,
                           "fileindex.walk");
        }
        rt_map_set_int(result, rt_const_cstr("emitted"), emitted);
        rt_map_set_int(result, rt_const_cstr("nextOffset"), matched);
        rt_map_set_int(result, rt_const_cstr("scanned"), matched);
        rt_map_set_bool(result, rt_const_cstr("done"), done ? 1 : 0);
        rt_map_set_bool(result, rt_const_cstr("truncated"), truncated ? 1 : 0);
        releaseObject(entries);
        releaseObject(diagnostics);
        return result;
    } catch (...) {
        rt_map_set_bool(result, rt_const_cstr("valid"), 0);
        pushDiagnostic(
            diagnostics, "workspace file-index page failed", "", 0, "fileindex.exception");
        releaseObject(entries);
        releaseObject(diagnostics);
        return result;
    }
}

/// @brief Enumerate a filtered workspace tree into file-index entry maps.
/// @details Traverses recursively with permission-denied entries skipped, applies hard exclusions,
///          nested `.gitignore` rules, caller patterns, and an optional case-normalized extension
///          allow-list. Directory entries are optional and output is capped at 100,000 maps.
///          Invalid roots or caught C++ exceptions return an empty owning sequence.
/// @param root_s Runtime string naming the workspace root.
/// @param extensions_csv Delimited extension allow-list; empty includes every file extension.
/// @param excludes_csv Delimited additional gitignore-style patterns.
/// @param include_dirs Nonzero to include directory maps as well as file maps.
/// @return Fresh owning Seq of workspace entry maps.
void *rt_workspace_file_index_enumerate(rt_string root_s,
                                        rt_string extensions_csv,
                                        rt_string excludes_csv,
                                        int8_t include_dirs) {
    try {
        void *out = rt_seq_new_owned();
        fs::path root = toStd(root_s);
        if (root.empty())
            return out;
        std::error_code ec;
        root = fs::absolute(root, ec).lexically_normal();
        if (ec || !fs::is_directory(root, ec))
            return out;

        std::set<std::string> extensions;
        for (std::string ext : splitList(toStd(extensions_csv))) {
            if (!ext.empty() && ext[0] != '.')
                ext.insert(ext.begin(), '.');
            extensions.insert(lower(ext));
        }
        const auto extraPatterns = splitList(toStd(excludes_csv));

        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        int64_t emitted = 0;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code relEc;
            std::string rel =
                normalizeSlashes(fs::relative(it->path(), root, relEc).generic_string());
            if (relEc || rel.empty() || rel == ".")
                continue;
            bool isDir = it->is_directory(ec);
            const auto gitignorePatterns = gitignorePatternsForPath(root, rel);
            if (shouldIgnorePathWithPatterns(rel, isDir, extraPatterns, gitignorePatterns)) {
                if (isDir)
                    it.disable_recursion_pending();
                continue;
            }
            if (isDir && !include_dirs)
                continue;
            if (!isDir && !extensions.empty()) {
                std::string ext = lower(it->path().extension().generic_string());
                if (!extensions.count(ext))
                    continue;
            }
            if (emitted >= kWorkspaceFileIndexMaxEntries)
                break;

            void *entry = rt_map_new();
            const std::string path = fs::absolute(it->path(), ec).lexically_normal().string();
            mapSetStr(entry, "path", path);
            mapSetStr(entry, "relativePath", rel);
            mapSetStr(entry, "name", it->path().filename().generic_string());
            mapSetStr(entry, "extension", it->path().extension().generic_string());
            mapSetStr(entry, "kind", isDir ? "directory" : "file");
            rt_map_set_bool(entry, rt_const_cstr("isDirectory"), isDir ? 1 : 0);
            rt_map_set_int(entry, rt_const_cstr("id"), stablePathId(normalizeSlashes(path)));
            int64_t file_size = 0;
            if (!isDir) {
                std::error_code sizeEc;
                uintmax_t raw_size = it->file_size(sizeEc);
                if (!sizeEc && raw_size <= static_cast<uintmax_t>(INT64_MAX))
                    file_size = static_cast<int64_t>(raw_size);
            }
            rt_map_set_int(entry, rt_const_cstr("size"), file_size);
            rt_map_set_int(entry, rt_const_cstr("modified"), fileTimeSeconds(it->path()));
            seqPushOwned(out, entry);
            emitted++;
        }
        return out;
    } catch (...) {
        return rt_seq_new_owned();
    }
}

/// @brief Return traversal metadata for a workspace file-index request.
/// @details Mirrors `rt_workspace_file_index_enumerate` filtering and ignore
///          behavior, but records only count/cap/diagnostic data so IDEs can
///          present large-workspace status without allocating every entry map.
/// @param root_s Runtime string naming the root directory.
/// @param extensions_csv Comma-separated extension allow-list.
/// @param excludes_csv Comma-separated additional ignore patterns.
/// @param include_dirs Non-zero to count directories that pass filters.
/// @return Runtime map containing
/// valid/root/entryCount/maxEntries/truncated/fingerprint/diagnostics.
void *rt_workspace_file_index_status(rt_string root_s,
                                     rt_string extensions_csv,
                                     rt_string excludes_csv,
                                     int8_t include_dirs) {
    void *status = rt_map_new();
    void *diagnostics = rt_seq_new_owned();
    rt_map_set_bool(status, rt_const_cstr("valid"), 1);
    mapSetStr(status, "root", "");
    rt_map_set_int(status, rt_const_cstr("entryCount"), 0);
    rt_map_set_int(status, rt_const_cstr("maxEntries"), kWorkspaceFileIndexMaxEntries);
    rt_map_set_int(status, rt_const_cstr("fingerprint"), 0);
    rt_map_set_bool(status, rt_const_cstr("truncated"), 0);
    rt_map_set(status, rt_const_cstr("diagnostics"), diagnostics);

    try {
        fs::path root = toStd(root_s);
        if (root.empty()) {
            rt_map_set_bool(status, rt_const_cstr("valid"), 0);
            pushDiagnostic(diagnostics, "workspace root is empty", "", 0, "fileindex.root");
            releaseObject(diagnostics);
            return status;
        }
        std::error_code ec;
        root = fs::absolute(root, ec).lexically_normal();
        if (ec || !fs::is_directory(root, ec)) {
            rt_map_set_bool(status, rt_const_cstr("valid"), 0);
            pushDiagnostic(diagnostics,
                           "workspace root is not a directory",
                           root.string(),
                           0,
                           "fileindex.root");
            releaseObject(diagnostics);
            return status;
        }
        mapSetStr(status, "root", root.string());

        std::set<std::string> extensions;
        for (std::string ext : splitList(toStd(extensions_csv))) {
            if (!ext.empty() && ext[0] != '.')
                ext.insert(ext.begin(), '.');
            extensions.insert(lower(ext));
        }
        const auto extraPatterns = splitList(toStd(excludes_csv));

        fs::recursive_directory_iterator it(
            root, fs::directory_options::skip_permission_denied, ec);
        fs::recursive_directory_iterator end;
        int64_t counted = 0;
        uint64_t fingerprint = kWorkspaceFingerprintOffset;
        for (; !ec && it != end; it.increment(ec)) {
            std::error_code relEc;
            std::string rel =
                normalizeSlashes(fs::relative(it->path(), root, relEc).generic_string());
            if (relEc || rel.empty() || rel == ".")
                continue;
            bool isDir = it->is_directory(ec);
            const auto gitignorePatterns = gitignorePatternsForPath(root, rel);
            if (shouldIgnorePathWithPatterns(rel, isDir, extraPatterns, gitignorePatterns)) {
                if (isDir)
                    it.disable_recursion_pending();
                continue;
            }
            if (isDir && !include_dirs)
                continue;
            if (!isDir && !extensions.empty()) {
                std::string ext = lower(it->path().extension().generic_string());
                if (!extensions.count(ext))
                    continue;
            }
            if (counted >= kWorkspaceFileIndexMaxEntries) {
                rt_map_set_bool(status, rt_const_cstr("truncated"), 1);
                pushDiagnostic(diagnostics,
                               "workspace file index entry cap reached",
                               root.string(),
                               0,
                               "fileindex.truncated");
                break;
            }
            int64_t fileSize = 0;
            if (!isDir) {
                std::error_code sizeEc;
                auto rawSize = it->file_size(sizeEc);
                fileSize = sizeEc ? -1 : static_cast<int64_t>(rawSize);
            }
            fingerprint = workspaceFingerprintEntry(
                fingerprint, rel, isDir, fileSize, fileTimeSeconds(it->path()));
            counted++;
        }
        if (ec) {
            rt_map_set_bool(status, rt_const_cstr("valid"), 0);
            pushDiagnostic(diagnostics,
                           "workspace traversal failed: " + ec.message(),
                           root.string(),
                           0,
                           "fileindex.traverse");
        }
        rt_map_set_int(status, rt_const_cstr("entryCount"), counted);
        rt_map_set_int(status, rt_const_cstr("fingerprint"), static_cast<int64_t>(fingerprint));
        releaseObject(diagnostics);
        return status;
    } catch (...) {
        rt_map_set_bool(status, rt_const_cstr("valid"), 0);
        pushDiagnostic(
            diagnostics, "workspace file index status failed", "", 0, "fileindex.exception");
        releaseObject(diagnostics);
        return status;
    }
}

/// @brief Evaluate workspace ignore rules for one relative path.
/// @details Applies built-in exclusions, caller patterns, and root/nested `.gitignore` files. A
///          trailing slash or backslash marks the candidate as a directory. An empty root uses
///          the current directory; caught exceptions conservatively return false.
/// @param root_s Runtime string naming the workspace root.
/// @param relative_path Runtime string containing the candidate path relative to the root.
/// @param patterns Delimited caller-supplied ignore and negation patterns.
/// @return 1 when the path should be ignored; otherwise 0.
int8_t rt_workspace_file_index_should_ignore(rt_string root_s,
                                             rt_string relative_path,
                                             rt_string patterns) {
    try {
        fs::path root = toStd(root_s);
        if (root.empty())
            root = ".";
        std::string rel = toStd(relative_path);
        bool isDir = !rel.empty() && (rel.back() == '/' || rel.back() == '\\');
        return shouldIgnorePath(root, rel, isDir, splitList(toStd(patterns)), true) ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

/// @brief Poll up to a bounded number of events from a workspace watcher.
/// @details Converts each event into a map containing path, numeric/type-name fields, overflow
///          count, and a rescan flag. Nonpositive limits default to 64; polling stops at the first
///          `NONE` event. Null watchers and caught exceptions return an empty sequence.
/// @param watcher Borrowed opaque watcher handle.
/// @param max_events Maximum events to poll, or a nonpositive value for the default batch size.
/// @return Fresh owning Seq of watcher event maps.
void *rt_workspace_watcher_poll_batch(void *watcher, int64_t max_events) {
    try {
        void *events = rt_seq_new_owned();
        if (!watcher)
            return events;
        if (max_events <= 0)
            max_events = 64;
        for (int64_t i = 0; i < max_events; i++) {
            int64_t type = rt_watcher_poll(watcher);
            if (type == RT_WATCH_EVENT_NONE)
                break;
            void *event = rt_map_new();
            rt_string path = rt_watcher_event_path(watcher);
            mapSetStr(event, "path", toStd(path));
            rt_string_unref(path);
            mapSetStr(event, "typeName", eventTypeName(type));
            rt_map_set_int(event, rt_const_cstr("type"), type);
            rt_map_set_int(
                event,
                rt_const_cstr("overflowCount"),
                type == RT_WATCH_EVENT_OVERFLOW ? rt_watcher_event_overflow_count(watcher) : 0);
            rt_map_set_bool(
                event, rt_const_cstr("requiresRescan"), type == RT_WATCH_EVENT_OVERFLOW ? 1 : 0);
            seqPushOwned(events, event);
        }
        return events;
    } catch (...) {
        return rt_seq_new_owned();
    }
}

/// @brief Resolve an asset reference against editor and runtime search locations.
/// @details Tries an absolute asset path, the scene directory, project root, configured asset
///          roots, then the mounted runtime asset registry. Relative scene and asset-root paths
///          are anchored to the project root. The result map always reports path/display/source,
///          found/exists flags, and a diagnostic.
/// @param scene_path_s Runtime path of the referencing scene; may be empty.
/// @param project_root_s Runtime project-root path; empty uses the current directory.
/// @param asset_roots_csv Delimited configured asset-root paths.
/// @param asset_path_s Runtime asset reference to resolve.
/// @return Fresh result map describing the first match or a stable missing/error result.
void *rt_asset_resolver_resolve(rt_string scene_path_s,
                                rt_string project_root_s,
                                rt_string asset_roots_csv,
                                rt_string asset_path_s) {
    try {
        void *result = rt_map_new();
        const std::string assetPath = toStd(asset_path_s);
        fs::path scenePath = toStd(scene_path_s);
        fs::path projectRoot = toStd(project_root_s);
        if (projectRoot.empty())
            projectRoot = ".";
        projectRoot = fs::absolute(projectRoot).lexically_normal();

        // A relative scene path is project-relative, not process-CWD-relative:
        // base it under projectRoot so the same project resolves identically no
        // matter what the editor's current directory is (VDOC-197).
        if (!scenePath.empty() && scenePath.is_relative())
            scenePath = (projectRoot / scenePath).lexically_normal();

        mapSetStr(result, "path", "");
        mapSetStr(result, "displayPath", assetPath);
        mapSetStr(result, "source", "missing");
        mapSetStr(result, "diagnostic", "");
        rt_map_set_bool(result, rt_const_cstr("exists"), 0);
        rt_map_set_bool(result, rt_const_cstr("found"), 0);

        // An empty asset name must not resolve to the project directory itself
        // (`projectRoot / "" == projectRoot`, which exists) — reject it up front
        // (VDOC-197).
        if (assetPath.empty()) {
            mapSetStr(result, "diagnostic", "empty asset name");
            return result;
        }

        std::vector<std::pair<std::string, fs::path>> candidates;
        fs::path asset(assetPath);
        if (asset.is_absolute())
            candidates.push_back({"absolute", asset});
        if (!scenePath.empty())
            candidates.push_back({"scene", scenePath.parent_path() / asset});
        candidates.push_back({"project", projectRoot / asset});
        for (const auto &root : splitList(toStd(asset_roots_csv))) {
            fs::path assetRoot(root);
            if (assetRoot.is_relative())
                assetRoot = projectRoot / assetRoot;
            candidates.push_back({"assetRoot", assetRoot / asset});
        }

        std::error_code ec;
        for (auto &[source, candidate] : candidates) {
            candidate = candidate.lexically_normal();
            if (fs::exists(candidate, ec)) {
                const std::string resolved =
                    fs::absolute(candidate, ec).lexically_normal().string();
                mapSetStr(result, "path", resolved);
                mapSetStr(result,
                          "displayPath",
                          fs::relative(candidate, projectRoot, ec).generic_string());
                mapSetStr(result, "source", source);
                rt_map_set_bool(result, rt_const_cstr("exists"), 1);
                rt_map_set_bool(result, rt_const_cstr("found"), 1);
                return result;
            }
        }

        rt_string assetName = makeString(assetPath);
        if (rt_asset_exists(assetName)) {
            mapSetStr(result, "path", assetPath);
            mapSetStr(result, "displayPath", assetPath);
            mapSetStr(result, "source", "mounted");
            rt_map_set_bool(result, rt_const_cstr("exists"), 1);
            rt_map_set_bool(result, rt_const_cstr("found"), 1);
            rt_string_unref(assetName);
            return result;
        }
        rt_string_unref(assetName);

        mapSetStr(result, "diagnostic", "asset not found: " + assetPath);
        return result;
    } catch (...) {
        void *result = rt_map_new();
        mapSetStr(result, "path", "");
        mapSetStr(result, "displayPath", "");
        mapSetStr(result, "source", "missing");
        mapSetStr(result, "diagnostic", "asset resolver failed");
        rt_map_set_bool(result, rt_const_cstr("exists"), 0);
        rt_map_set_bool(result, rt_const_cstr("found"), 0);
        return result;
    }
}

/// @brief Parse project-manifest directive text into a structured runtime map.
/// @details Supports top-level project metadata, source/exclude/asset/scene lists, shorthand run
///          declarations, and `[run.NAME]`/`[build.NAME]` sections. Blank/comment lines are
///          ignored, an initial UTF-8 BOM is stripped, and unknown/malformed directives append
///          diagnostics and mark the manifest invalid.
/// @param text_s Borrowed runtime string containing the complete manifest text.
/// @return Fresh manifest map with defaults, parsed fields, owning configuration/diagnostic
///         sequences, and a `valid` flag.
void *rt_project_manifest_parse_text(rt_string text_s) {
    try {
        void *manifest = newManifestMap();
        void *diagnostics = rt_map_get(manifest, rt_const_cstr("diagnostics"));
        std::string section;
        void *sectionMap = nullptr;
        std::string sectionKind;
        // True while inside an UNKNOWN [section]: its body directives must be
        // ignored (with a diagnostic), not applied to the top level. Without
        // this, `sectionMap == nullptr` was ambiguous between "top level" and
        // "inside an unknown section", so unknown-section directives hijacked
        // top-level defaults like `entry` (VDOC-194).
        bool inUnknownSection = false;

        int64_t lineNo = 0;
        for (std::string line : readLines(toStd(text_s))) {
            lineNo++;
            if (lineNo == 1 && line.rfind("\xEF\xBB\xBF", 0) == 0)
                line.erase(0, 3);
            std::string stripped = trim(line);
            if (stripped.empty() || stripped[0] == '#')
                continue;
            if (stripped.rfind("//", 0) == 0)
                continue;
            if (stripped.front() == '[' && stripped.back() == ']') {
                section = stripped.substr(1, stripped.size() - 2);
                sectionMap = rt_map_new();
                mapSetStr(sectionMap, "name", section);
                sectionKind.clear();
                inUnknownSection = false;
                if (section.rfind("run.", 0) == 0) {
                    sectionKind = "runConfigs";
                    mapSetStr(sectionMap, "name", section.substr(4));
                } else if (section.rfind("build.", 0) == 0) {
                    sectionKind = "buildConfigs";
                    mapSetStr(sectionMap, "name", section.substr(6));
                } else {
                    pushDiagnostic(diagnostics,
                                   "unknown manifest section '" + section + "'",
                                   "",
                                   lineNo,
                                   "manifest.section");
                    releaseObject(sectionMap);
                    sectionMap = nullptr;
                    inUnknownSection = true;
                }
                if (sectionMap)
                    appendConfigMap(manifest, sectionKind.c_str(), sectionMap);
                continue;
            }

            auto [key, value] = splitDirectiveLine(stripped);
            if (key.empty() || value.empty()) {
                pushDiagnostic(
                    diagnostics, "manifest directive missing value", "", lineNo, "manifest.value");
                continue;
            }
            const std::string canonical = manifestKey(key);
            if (sectionMap) {
                if (canonical == "args" || canonical == "env")
                    replaceStringSeq(sectionMap, key.c_str(), splitList(value));
                else
                    mapSetStr(sectionMap, key.c_str(), value);
                continue;
            }
            if (inUnknownSection) {
                // Directives inside an unknown section are diagnosed and ignored;
                // they must not fall through to mutate top-level defaults
                // (VDOC-194).
                pushDiagnostic(diagnostics,
                               "ignoring directive '" + key + "' in unknown manifest section",
                               "",
                               lineNo,
                               "manifest.directive");
                continue;
            }
            if (canonical == "project" || canonical == "name")
                mapSetStr(manifest, "name", value);
            else if (canonical == "version")
                mapSetStr(manifest, "version", value);
            else if (canonical == "lang" || canonical == "language")
                mapSetStr(manifest, "language", lower(value));
            else if (canonical == "entry" || canonical == "main")
                mapSetStr(manifest, "entry", value);
            else if (canonical == "sources" || canonical == "sourceglobs")
                appendToStringSeqField(manifest, "sourceGlobs", value);
            else if (canonical == "exclude" || canonical == "excludes")
                appendToStringSeqField(manifest, "excludes", value);
            else if (canonical == "assetroot" || canonical == "assetroots")
                appendToStringSeqField(manifest, "assetRoots", value);
            else if (canonical == "sceneroot" || canonical == "sceneroots")
                appendToStringSeqField(manifest, "sceneRoots", value);
            else if (canonical == "defaultscene")
                mapSetStr(manifest, "defaultScene", value);
            else if (canonical == "run") {
                void *run = rt_map_new();
                mapSetStr(run, "name", value);
                appendConfigMap(manifest, "runConfigs", run);
            } else {
                pushDiagnostic(diagnostics,
                               "unknown manifest directive '" + key + "'",
                               "",
                               lineNo,
                               "manifest.directive");
            }
        }

        rt_map_set_bool(manifest, rt_const_cstr("valid"), rt_seq_len(diagnostics) == 0 ? 1 : 0);
        if (mapGetString(manifest, "name").empty())
            mapSetStr(manifest, "name", "ZannaProject");
        return manifest;
    } catch (...) {
        void *manifest = newManifestMap();
        void *diagnostics = rt_map_get(manifest, rt_const_cstr("diagnostics"));
        pushDiagnostic(diagnostics, "manifest parse failed", "", 0, "manifest.exception");
        rt_map_set_bool(manifest, rt_const_cstr("valid"), 0);
        return manifest;
    }
}

/// @brief Read and parse a project manifest from disk.
/// @details Reads bytes in binary mode, delegates to rt_project_manifest_parse_text(), records the
///          source path, and derives a default project name from the parent directory when the
///          parser retained its generic name. Open/read exceptions produce an invalid manifest
///          with diagnostics.
/// @param path_s Borrowed runtime string naming the manifest file.
/// @return Fresh parsed or error manifest map.
void *rt_project_manifest_parse_file(rt_string path_s) {
    try {
        const std::string path = toStd(path_s);
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            void *manifest = newManifestMap();
            void *diagnostics = rt_map_get(manifest, rt_const_cstr("diagnostics"));
            pushDiagnostic(diagnostics, "cannot open manifest", path, 0, "manifest.open");
            rt_map_set_bool(manifest, rt_const_cstr("valid"), 0);
            return manifest;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        rt_string text = makeString(buffer.str());
        void *manifest = rt_project_manifest_parse_text(text);
        rt_string_unref(text);
        if (mapGetString(manifest, "name") == "ZannaProject") {
            fs::path p(path);
            if (!p.parent_path().empty())
                mapSetStr(manifest, "name", p.parent_path().filename().generic_string());
        }
        mapSetStr(manifest, "path", path);
        return manifest;
    } catch (...) {
        void *manifest = newManifestMap();
        void *diagnostics = rt_map_get(manifest, rt_const_cstr("diagnostics"));
        pushDiagnostic(diagnostics, "manifest read failed", "", 0, "manifest.exception");
        rt_map_set_bool(manifest, rt_const_cstr("valid"), 0);
        return manifest;
    }
}

} // extern "C"

namespace {

/// @brief Validate normalized workspace edit records and package diagnostics.
/// @details Shared implementation for the public rooted and unrooted validators.
///          It loads runtime edit maps, checks target versions and edit ranges,
///          and rejects overlapping edits before returning the stable result-map
///          shape consumed by editor tooling.
/// @param edits Runtime Seq of edit maps.
/// @param roots Optional canonical workspace roots that bound every edit target.
/// @return Result map containing `success`, `editCount`, and `diagnostics`.
static void *workspace_edit_validate_impl(void *edits, const std::vector<fs::path> *roots) {
    void *result = rt_map_new();
    void *diagnostics = rt_seq_new_owned();
    std::vector<EditRecord> records;
    std::unordered_map<std::string, std::string> contents;
    bool ok = edits != nullptr;
    if (!edits) {
        pushDiagnostic(diagnostics, "workspace edits sequence is null", "", 0, "edit.null");
    } else {
        const int64_t len = rt_seq_len(edits);
        for (int64_t i = 0; i < len; i++) {
            EditRecord record;
            if (loadEditRecord(rt_seq_get(edits, i), record, diagnostics, i))
                records.push_back(std::move(record));
            else
                ok = false;
        }
        if (!validateEditRecords(records, contents, diagnostics, roots))
            ok = false;
    }
    rt_map_set_bool(result, rt_const_cstr("success"), ok ? 1 : 0);
    rt_map_set_int(result, rt_const_cstr("editCount"), static_cast<int64_t>(records.size()));
    rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
    releaseObject(diagnostics);
    return result;
}

/// @brief Staging paths for one transactional workspace file replacement.
/// @details `file` is the destination, `temp` contains the new content before
///          commit, and `backup` holds the original content after the first
///          rename succeeds. `backupCreated` lets rollback distinguish pending
///          writes from already-mutated files.
struct PendingWorkspaceWrite {
    std::string file;
    std::string temp;
    std::string backup;
    bool backupReserved{false}; ///< Backup name exclusively reserved (empty placeholder on disk).
    bool backupCreated{false};  ///< Original target has been renamed into the backup.
};

/// @brief Return an unpredictable per-process nonce for edit sidecar names.
/// @details Folds the process-local atomic counter through the runtime's
///          per-process keyed hash (SipHash seeded from the OS CSPRNG), so
///          sidecar names cannot be predicted or pre-created by another process
///          (VDOC-196). The counter guarantees uniqueness within the process;
///          the keyed hash guarantees unpredictability across processes.
/// @return Nonce mixed from a unique process-local counter and keyed runtime hash.
static uint64_t workspaceEditNonce() {
    uint64_t counter = ++g_workspaceEditTempCounter;
    return rt_keyed_hash_bytes(&counter, sizeof(counter)) ^ counter;
}

/// @brief Create a same-directory temporary path for a workspace edit target.
/// @details The path is derived from the target filename plus an unpredictable
///          nonce. Content temps are opened with exclusive-create semantics, and
///          backups are exclusively reserved before rename (see
///          `reserveWorkspaceEditBackup`). Same-directory renames keep successful
///          replacements on the destination filesystem.
/// @param file Target file path.
/// @param suffix Suffix distinguishing content temps from rollback backups.
/// @return Candidate temporary path.
static fs::path workspaceEditTempPath(const fs::path &file, const char *suffix) {
    fs::path dir = file.parent_path();
    if (dir.empty())
        dir = ".";
    char nonce[17];
    std::snprintf(
        nonce, sizeof(nonce), "%016llx", static_cast<unsigned long long>(workspaceEditNonce()));
    std::string leaf = "." + file.filename().generic_string() + ".zanna-edit-" + nonce + suffix;
    return dir / leaf;
}

/// @brief Exclusively reserve a fresh backup path in the target's directory.
/// @details Generates unpredictable candidate names and creates each with
///          exclusive-create (`O_EXCL` / `CREATE_NEW`) semantics, so a stale
///          artifact or another process cannot make the later
///          `rename(target, backup)` clobber unrelated data (VDOC-196). The
///          reserved empty file is atomically replaced by the target during the
///          commit rename. Returns false after exhausting its retry budget.
/// @param target Destination file whose parent will contain the reservation.
/// @param[out] reserved Receives the selected backup path after exclusive creation.
/// @return True when an empty backup placeholder was exclusively created; otherwise false.
static bool reserveWorkspaceEditBackup(const fs::path &target, std::string &reserved) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        fs::path candidate = workspaceEditTempPath(target, ".bak");
#if RT_PLATFORM_WINDOWS
        HANDLE handle = CreateFileW(candidate.wstring().c_str(),
                                    GENERIC_WRITE,
                                    0,
                                    NULL,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                    NULL);
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
            reserved = candidate.string();
            return true;
        }
#else
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        int fd = open(candidate.c_str(), flags, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            close(fd);
            reserved = candidate.string();
            return true;
        }
#endif
    }
    return false;
}

/// @brief Write a string to a newly created temporary file.
/// @details Opens @p path with exclusive-create semantics so an existing file,
///          symlink, or racing creator cannot be overwritten. The write loop
///          chunks large strings to platform API limits before flushing and
///          closing the handle.
/// @param path Temporary file path to write.
/// @param text Complete replacement file contents.
/// @return `true` when the file was written and flushed successfully.
static bool writeWorkspaceEditTemp(const fs::path &path, const std::string &text) {
#if RT_PLATFORM_WINDOWS
    HANDLE handle = CreateFileW(path.wstring().c_str(),
                                GENERIC_WRITE,
                                0,
                                NULL,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    size_t pos = 0;
    const size_t chunk_max = static_cast<size_t>(std::numeric_limits<DWORD>::max());
    while (pos < text.size()) {
        size_t chunk = std::min(chunk_max, text.size() - pos);
        DWORD written = 0;
        if (!WriteFile(handle, text.data() + pos, (DWORD)chunk, &written, NULL) ||
            written != (DWORD)chunk) {
            CloseHandle(handle);
            return false;
        }
        pos += chunk;
    }
    bool ok = FlushFileBuffers(handle) != 0;
    ok = CloseHandle(handle) != 0 && ok;
    return ok;
#else
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return false;
    size_t pos = 0;
    while (pos < text.size()) {
        ssize_t written = write(fd, text.data() + pos, text.size() - pos);
        if (written <= 0) {
            close(fd);
            return false;
        }
        pos += (size_t)written;
    }
    bool ok = fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    return ok;
#endif
}

/// @brief Flush the parent directory for a workspace edit target after renames.
/// @details Atomic rename only becomes crash-durable once the directory entry is
///          also flushed on platforms that expose directory flushing. This helper
///          is intentionally best-effort: filesystems that reject directory flushes
///          still keep the existing temp-file fsync and rename behavior.
/// @param file Target file whose parent directory should be flushed.
/// @return true when the directory was flushed, false when the platform or
///         filesystem rejected the request.
static bool flushWorkspaceEditDirectory(const fs::path &file) {
    fs::path dir = file.parent_path();
    if (dir.empty())
        dir = ".";
#if RT_PLATFORM_WINDOWS
    HANDLE handle = CreateFileW(dir.wstring().c_str(),
                                FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    bool ok = FlushFileBuffers(handle) != 0;
    CloseHandle(handle);
    return ok;
#else
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int fd = open(dir.c_str(), flags);
    if (fd < 0)
        return false;
    bool ok = fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    return ok;
#endif
}

/// @brief Copy basic filesystem permissions from an existing edit target to a temp file.
/// @details Workspace edits replace files through same-directory temp files. Keeping
///          original permission bits prevents refactors from stripping executable
///          or read-only metadata on POSIX-like filesystems. Errors are best-effort
///          because some platforms virtualize or deny permission updates.
/// @param source Existing target file whose permissions are authoritative.
/// @param temp Newly written temporary file that will replace @p source.
static void preserveWorkspaceEditPermissions(const fs::path &source, const fs::path &temp) {
    std::error_code ec;
    auto status = fs::status(source, ec);
    if (ec)
        return;
    ec.clear();
    fs::permissions(temp, status.permissions(), fs::perm_options::replace, ec);
}

/// @brief Remove staged temp files and restore backups after an apply failure.
/// @details Best-effort rollback: any replacement already moved into place is
///          removed before its backup is renamed back. Remaining staged temps
///          are deleted. Diagnostics for rollback failures are intentionally not
///          appended so the original write failure remains the primary signal.
/// @param writes Pending write records accumulated for this apply attempt.
static void rollbackWorkspaceWrites(const std::vector<PendingWorkspaceWrite> &writes) {
    for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
        std::error_code ec;
        if (it->backupCreated) {
            fs::remove(it->file, ec);
            ec.clear();
            fs::rename(it->backup, it->file, ec);
            ec.clear();
            fs::remove(it->backup, ec);
        } else if (it->backupReserved) {
            // Reserved but never used: remove the empty placeholder we created
            // (VDOC-196) so a failed apply leaves no stale sidecar.
            fs::remove(it->backup, ec);
            ec.clear();
        }
        ec.clear();
        fs::remove(it->temp, ec);
    }
}

/// @brief Apply a validated workspace edit batch with best-effort rollback.
/// @details This routine revalidates edits before staging, applies edits in
///          descending range order per file, stages every new file image into a
///          same-directory temporary file, then commits via rename. Immediately
///          before replacing each live target it re-reads the file and confirms
///          it still matches the content validated earlier (optimistic
///          concurrency), aborting the whole batch with a rollback if an
///          external write changed it during staging, so newer content is never
///          silently overwritten (VDOC-195).
///          If any backup or replacement fails, earlier replacements are
///          restored from their backups and staged temps are removed.
/// @param edits Runtime Seq of edit maps.
/// @param roots Optional canonical workspace roots that bound every edit target.
/// @return Result map containing validation fields plus `appliedFiles`.
static void *workspace_edit_apply_impl(void *edits, const std::vector<fs::path> *roots) {
    void *result = workspace_edit_validate_impl(edits, roots);
    if (!rt_map_get_bool(result, rt_const_cstr("success"))) {
        rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
        return result;
    }

    void *diagnostics = rt_map_get(result, rt_const_cstr("diagnostics"));
    std::vector<EditRecord> records;
    std::unordered_map<std::string, std::string> contents;
    const int64_t len = rt_seq_len(edits);
    for (int64_t i = 0; i < len; i++) {
        EditRecord record;
        if (loadEditRecord(rt_seq_get(edits, i), record, diagnostics, i))
            records.push_back(std::move(record));
    }
    if (!validateEditRecords(records, contents, diagnostics, roots)) {
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
        return result;
    }

    // Snapshot the validated original content of every target BEFORE applying
    // edits, so the commit loop can confirm the file has not changed underneath
    // us since validation (VDOC-195). The edit loop below mutates `contents` in
    // place into the new image, so the originals must be copied first.
    std::unordered_map<std::string, std::string> validatedOriginals = contents;

    std::map<std::string, std::vector<EditRecord>> byFile;
    for (const auto &record : records)
        byFile[record.file].push_back(record);

    for (auto &[file, vec] : byFile) {
        /// @brief Order edits from right to left so earlier offsets remain stable.
        /// @param a First edit.
        /// @param b Second edit.
        /// @return `true` when @p a begins after @p b.
        std::sort(vec.begin(), vec.end(), [](const EditRecord &a, const EditRecord &b) {
            return a.startOffset > b.startOffset;
        });
        std::string &text = contents[file];
        for (const auto &edit : vec)
            text.replace(edit.startOffset, edit.endOffset - edit.startOffset, edit.newText);
    }

    std::vector<PendingWorkspaceWrite> writes;
    for (const auto &[file, text] : contents) {
        fs::path target(file);
        PendingWorkspaceWrite write;
        write.file = file;
        write.temp = workspaceEditTempPath(target, ".tmp").string();
        // Exclusively reserve the backup name up front so no stale artifact or
        // racing process can make the commit-time rename clobber unrelated data
        // (VDOC-196). The reserved empty file is replaced by the target during
        // the commit rename.
        if (!reserveWorkspaceEditBackup(target, write.backup)) {
            pushDiagnostic(
                diagnostics, "cannot reserve backup for edit target", file, 0, "edit.write");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        write.backupReserved = true;
        if (!writeWorkspaceEditTemp(write.temp, text)) {
            pushDiagnostic(
                diagnostics, "cannot write temporary edit target", file, 0, "edit.write");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        preserveWorkspaceEditPermissions(target, write.temp);
        writes.push_back(std::move(write));
    }

    int64_t applied = 0;
    for (auto &write : writes) {
        std::error_code ec;
        // Optimistic-concurrency recheck: confirm the live target still matches
        // the content we validated, immediately before replacing it. An editor,
        // formatter, or external process that wrote the file during staging
        // would otherwise be silently overwritten (VDOC-195).
        {
            std::ifstream in(write.file, std::ios::binary);
            std::string current;
            if (in) {
                std::ostringstream buffer;
                buffer << in.rdbuf();
                current = buffer.str();
            }
            auto expected = validatedOriginals.find(write.file);
            if (!in || expected == validatedOriginals.end() || current != expected->second) {
                pushDiagnostic(diagnostics,
                               "edit target changed since validation",
                               write.file,
                               0,
                               "edit.version");
                rt_map_set_bool(result, rt_const_cstr("success"), 0);
                rollbackWorkspaceWrites(writes);
                rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
                return result;
            }
        }
        fs::rename(write.file, write.backup, ec);
        if (ec) {
            pushDiagnostic(diagnostics, "cannot back up edit target", write.file, 0, "edit.write");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        write.backupCreated = true;
        ec.clear();
        fs::rename(write.temp, write.file, ec);
        if (ec) {
            pushDiagnostic(diagnostics, "cannot replace edit target", write.file, 0, "edit.write");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        flushWorkspaceEditDirectory(write.file);
        applied++;
    }
    for (const auto &write : writes) {
        std::error_code ec;
        fs::remove(write.backup, ec);
        flushWorkspaceEditDirectory(write.file);
    }
    rt_map_set_int(result, rt_const_cstr("appliedFiles"), applied);
    return result;
}

} // namespace

extern "C" {

/// @brief Validate an unrooted workspace edit batch without modifying files.
/// @details Decodes every edit, resolves targets to absolute lexical paths, verifies optional
///          mtime/size versions, converts one-based ranges to byte offsets, and rejects overlap.
///          C++ exceptions are converted into a stable failed result map.
/// @param edits Borrowed runtime Seq of edit maps.
/// @return Fresh map containing `success`, accepted `editCount`, and owning `diagnostics`.
void *rt_workspace_edit_validate(void *edits) {
    try {
        return workspace_edit_validate_impl(edits, nullptr);
    } catch (...) {
        void *result = rt_map_new();
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit validation failed", "", 0, "edit.exception");
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("editCount"), 0);
        rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
        releaseObject(diagnostics);
        return result;
    }
}

/// @brief Validate an edit batch constrained to one canonical workspace root.
/// @details The root must be an existing directory. Relative targets resolve beneath it, and
///          canonicalized targets that escape it are rejected before file contents are read.
/// @param edits Borrowed runtime Seq of edit maps.
/// @param root Borrowed runtime string naming the workspace root.
/// @return Fresh validation result map with success, edit count, and diagnostics.
void *rt_workspace_edit_validate_in_root(void *edits, rt_string root) {
    try {
        void *diagnostics = rt_seq_new_owned();
        fs::path resolvedRoot;
        if (!workspaceEditRootFromString(root, diagnostics, resolvedRoot)) {
            void *result = rt_map_new();
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rt_map_set_int(result, rt_const_cstr("editCount"), 0);
            rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
            releaseObject(diagnostics);
            return result;
        }
        releaseObject(diagnostics);
        std::vector<fs::path> resolvedRoots;
        resolvedRoots.push_back(std::move(resolvedRoot));
        return workspace_edit_validate_impl(edits, &resolvedRoots);
    } catch (...) {
        void *result = rt_map_new();
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit validation failed", "", 0, "edit.exception");
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("editCount"), 0);
        rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
        releaseObject(diagnostics);
        return result;
    }
}

/// @brief Transactionally apply an unrooted workspace edit batch.
/// @details Revalidates the batch, stages complete replacement images beside each file, checks
///          optimistic concurrency immediately before commit, and uses backups for best-effort
///          batch rollback. C++ exceptions produce a failed stable-shape result.
/// @param edits Borrowed runtime Seq of edit maps.
/// @return Fresh result map containing validation fields and `appliedFiles`.
void *rt_workspace_edit_apply(void *edits) {
    try {
        return workspace_edit_apply_impl(edits, nullptr);
    } catch (...) {
        void *result = rt_map_new();
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit apply failed", "", 0, "edit.exception");
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("editCount"), 0);
        rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
        rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
        releaseObject(diagnostics);
        return result;
    }
}

/// @brief Transactionally apply an edit batch constrained to one workspace root.
/// @details Canonical root containment is validated before staging; commit, concurrency checks,
///          permission preservation, and rollback follow workspace_edit_apply_impl().
/// @param edits Borrowed runtime Seq of edit maps.
/// @param root Borrowed runtime string naming an existing workspace directory.
/// @return Fresh result map containing success, edit count, applied-file count, and diagnostics.
void *rt_workspace_edit_apply_in_root(void *edits, rt_string root) {
    try {
        void *diagnostics = rt_seq_new_owned();
        fs::path resolvedRoot;
        if (!workspaceEditRootFromString(root, diagnostics, resolvedRoot)) {
            void *result = rt_map_new();
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rt_map_set_int(result, rt_const_cstr("editCount"), 0);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
            releaseObject(diagnostics);
            return result;
        }
        releaseObject(diagnostics);
        std::vector<fs::path> resolvedRoots;
        resolvedRoots.push_back(std::move(resolvedRoot));
        return workspace_edit_apply_impl(edits, &resolvedRoots);
    } catch (...) {
        void *result = rt_map_new();
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit apply failed", "", 0, "edit.exception");
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("editCount"), 0);
        rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
        rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
        releaseObject(diagnostics);
        return result;
    }
}

/// @brief Validate one edit batch against multiple canonical workspace roots.
/// @details Root strings are decoded and deduplicated first. With multiple roots, edit targets
///          must be absolute; every canonical target must lie within at least one root.
/// @param edits Borrowed runtime Seq of edit maps.
/// @param roots Borrowed runtime Seq of string or boxed-string workspace roots.
/// @return Fresh validation result map with success, edit count, and diagnostics.
void *rt_workspace_edit_validate_in_roots(void *edits, void *roots) {
    try {
        void *diagnostics = rt_seq_new_owned();
        std::vector<fs::path> resolvedRoots;
        if (!workspaceEditRootsFromSequence(roots, diagnostics, resolvedRoots)) {
            void *result = rt_map_new();
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rt_map_set_int(result, rt_const_cstr("editCount"), 0);
            rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
            releaseObject(diagnostics);
            return result;
        }
        releaseObject(diagnostics);
        return workspace_edit_validate_impl(edits, &resolvedRoots);
    } catch (...) {
        void *result = rt_map_new();
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit validation failed", "", 0, "edit.exception");
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("editCount"), 0);
        rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
        releaseObject(diagnostics);
        return result;
    }
}

/// @brief Transactionally apply one edit batch spanning explicit workspace roots.
/// @details Validates all canonical roots and target containment before staging. The shared apply
///          transaction covers every file across roots and rolls back earlier replacements when
///          a later commit fails.
/// @param edits Borrowed runtime Seq of edit maps.
/// @param roots Borrowed runtime Seq of string or boxed-string workspace roots.
/// @return Fresh result map containing success, edit count, applied-file count, and diagnostics.
void *rt_workspace_edit_apply_in_roots(void *edits, void *roots) {
    try {
        void *diagnostics = rt_seq_new_owned();
        std::vector<fs::path> resolvedRoots;
        if (!workspaceEditRootsFromSequence(roots, diagnostics, resolvedRoots)) {
            void *result = rt_map_new();
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rt_map_set_int(result, rt_const_cstr("editCount"), 0);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
            releaseObject(diagnostics);
            return result;
        }
        releaseObject(diagnostics);
        return workspace_edit_apply_impl(edits, &resolvedRoots);
    } catch (...) {
        void *result = rt_map_new();
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit apply failed", "", 0, "edit.exception");
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("editCount"), 0);
        rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
        rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
        releaseObject(diagnostics);
        return result;
    }
}

} // extern "C"
