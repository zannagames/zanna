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
// Links: src/runtime/io/rt_ide_primitives.h, src/runtime/io/rt_watcher.h,
//        docs/adr/0151-transactional-multi-root-workspace-edits.md,
//        docs/adr/0280-prepared-workspace-edit-transactions.md,
//        docs/adr/0287-generation-safe-workspace-index-cursors.md,
//        docs/adr/0303-complete-owned-workspace-index-cursors.md
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
#include "rt_hash.h"
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
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
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
#if RT_PLATFORM_LINUX
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif
#include <sys/stat.h>
#if RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
#include <sys/xattr.h>
#endif
#include <unistd.h>
#endif

#if RT_PLATFORM_MACOS && defined(_POSIX_C_SOURCE)
// Darwin hides fchflags behind _DARWIN_C_SOURCE when strict POSIX feature
// selection is active, although the descriptor API remains exported.
extern "C" int fchflags(int, uint32_t);
#endif

namespace fs = std::filesystem;

namespace {

/** Maximum number of parsed nested `.gitignore` entries retained in the cache. */
constexpr size_t kGitignoreCacheMaxEntries = 64;
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
/** Monotonic identity for explicit workspace file-index traversals. */
std::atomic<uint64_t> g_fileIndexCursorGeneration{0};

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

/// @brief Compute the runtime's canonical lowercase SHA-256 text digest.
/// @param value Complete byte string to hash.
/// @return Owning native copy of the 64-character hexadecimal digest.
std::string sha256Text(const std::string &value) {
    rt_string source = makeString(value);
    rt_string digest = rt_hash_sha256(source);
    std::string result = toStd(digest);
    rt_string_unref(digest);
    rt_string_unref(source);
    return result;
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

/// @brief Match a normalized slash path against component-aware wildcards.
/// @details Memoized states keep adversarial wildcard runs polynomial. A single
///          `*` stays within one component, `?` consumes one non-separator byte,
///          `**/` consumes complete components, and bracket classes follow Git.
/// @param text Normalized candidate path.
/// @param pattern Normalized wildcard pattern.
/// @return True when the complete path matches the complete pattern.
bool pathGlobMatch(std::string_view text, std::string_view pattern) {
    const size_t width = pattern.size() + 1;
    std::vector<int8_t> memo((text.size() + 1) * width, -1);
    std::function<bool(size_t, size_t)> rec = [&](size_t ti, size_t pi) -> bool {
        int8_t &cached = memo[ti * width + pi];
        if (cached >= 0)
            return cached != 0;
        bool matched = false;
        if (pi == pattern.size()) {
            matched = ti == text.size();
        } else if (pattern[pi] == '\\' && pi + 1 < pattern.size()) {
            matched = ti < text.size() && text[ti] == pattern[pi + 1] && rec(ti + 1, pi + 2);
        } else if (pattern[pi] == '[') {
            size_t end = pi + 1;
            bool negated = false;
            if (end < pattern.size() && (pattern[end] == '!' || pattern[end] == '^')) {
                negated = true;
                end++;
            }
            bool classMatch = false;
            bool hasMember = false;
            while (end < pattern.size() && pattern[end] != ']') {
                char first = pattern[end];
                if (first == '\\' && end + 1 < pattern.size())
                    first = pattern[++end];
                hasMember = true;
                if (end + 2 < pattern.size() && pattern[end + 1] == '-' &&
                    pattern[end + 2] != ']') {
                    char last = pattern[end + 2];
                    if (ti < text.size() && text[ti] >= first && text[ti] <= last)
                        classMatch = true;
                    end += 3;
                } else {
                    if (ti < text.size() && text[ti] == first)
                        classMatch = true;
                    end++;
                }
            }
            if (end < pattern.size() && hasMember && ti < text.size() && text[ti] != '/') {
                if (negated)
                    classMatch = !classMatch;
                matched = classMatch && rec(ti + 1, end + 1);
            } else {
                matched = ti < text.size() && text[ti] == '[' && rec(ti + 1, pi + 1);
            }
        } else if (pi + 1 < pattern.size() && pattern[pi] == '*' && pattern[pi + 1] == '*') {
            size_t next = pi + 2;
            if (next < pattern.size() && pattern[next] == '/') {
                next++;
                matched = rec(ti, next);
                size_t slash = text.find('/', ti);
                if (!matched && slash != std::string_view::npos)
                    matched = rec(slash + 1, pi);
            } else {
                matched = rec(ti, next) || (ti < text.size() && rec(ti + 1, pi));
            }
        } else if (pattern[pi] == '*') {
            matched = rec(ti, pi + 1) || (ti < text.size() && text[ti] != '/' && rec(ti + 1, pi));
        } else if (pattern[pi] == '?') {
            matched = ti < text.size() && text[ti] != '/' && rec(ti + 1, pi + 1);
        } else {
            matched = ti < text.size() && pattern[pi] == text[ti] && rec(ti + 1, pi + 1);
        }
        cached = matched ? 1 : 0;
        return matched;
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

/// @brief Return a file modification timestamp at the platform's native precision.
/// @details `std::filesystem::last_write_time` preserves the native clock's
///          subsecond resolution without reaching through platform-specific stat
///          fields. The value is an opaque file-clock nanosecond count intended
///          for equality comparisons, not wall-clock presentation.
/// @param path Filesystem path to stat.
/// @return File-clock nanoseconds, or -1 when metadata cannot be read safely.
int64_t fileTimeNanoseconds(const fs::path &path) {
    std::error_code ec;
    const fs::file_time_type modified = fs::last_write_time(path, ec);
    if (ec)
        return -1;
    const auto nanos =
        std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch()).count();
    return nanos == -1 ? -2 : static_cast<int64_t>(nanos);
}

/// @brief Hash fixed-size samples from the beginning, middle, and end of a file.
/// @details The fallback workspace watcher uses this alongside precise metadata
///          to notice same-size rewrites. At most 192 bytes are read per emitted
///          regular-file row, independent of file size.
/// @param path Regular-file path to sample.
/// @param fileSize Valid nonnegative file size.
/// @return Nonnegative deterministic sample hash, or -1 on an I/O failure.
int64_t boundedFileSampleHash(const fs::path &path, int64_t fileSize) {
    if (fileSize < 0)
        return -1;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return -1;

    constexpr int64_t kSampleBytes = 64;
    const int64_t last = fileSize > kSampleBytes ? fileSize - kSampleBytes : 0;
    const int64_t middle = fileSize > kSampleBytes ? (fileSize - kSampleBytes) / 2 : 0;
    const int64_t offsets[3] = {0, middle, last};
    uint64_t hash = kWorkspaceFingerprintOffset;
    int64_t previous = -1;
    for (int sample = 0; sample < 3; ++sample) {
        const int64_t offset = offsets[sample];
        if (offset == previous)
            continue;
        previous = offset;
        const int64_t count = std::min<int64_t>(kSampleBytes, fileSize - offset);
        for (int byte = 0; byte < 8; ++byte) {
            hash ^=
                static_cast<unsigned char>((static_cast<uint64_t>(offset) >> (byte * 8)) & 0xffu);
            hash *= kWorkspaceFingerprintPrime;
        }
        if (count <= 0)
            continue;
        input.clear();
        input.seekg(offset, std::ios::beg);
        if (!input)
            return -1;
        char buffer[kSampleBytes];
        input.read(buffer, static_cast<std::streamsize>(count));
        if (input.gcount() != static_cast<std::streamsize>(count))
            return -1;
        for (int64_t i = 0; i < count; ++i) {
            hash ^= static_cast<unsigned char>(buffer[i]);
            hash *= kWorkspaceFingerprintPrime;
        }
    }
    for (int byte = 0; byte < 8; ++byte) {
        hash ^= static_cast<unsigned char>((static_cast<uint64_t>(fileSize) >> (byte * 8)) & 0xffu);
        hash *= kWorkspaceFingerprintPrime;
    }
    return static_cast<int64_t>(hash & 0x7fffffffffffffffull);
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

    bool ignored = false;
    auto applyPatterns = [&](const std::vector<std::string> &patterns) {
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
    };
    applyPatterns(extraPatterns);
    applyPatterns(gitignorePatterns);
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
 * @brief Explicit recursive iterator and filter state for one workspace traversal.
 * @details The owning caller controls lifetime. A per-cursor seen-path set
 * prevents duplicate emission if a live directory iterator revisits an entry,
 * and every page carries the immutable traversal generation.
 */
struct WorkspaceFileIndexPageCursor {
    fs::path root;
    std::set<std::string> extensions;
    std::vector<std::string> extraPatterns;
    std::unordered_map<std::string, std::vector<std::string>> gitignorePatternsByDirectory;
    bool includeDirs{false};
    bool sampleContent{false};
    fs::recursive_directory_iterator it;
    fs::recursive_directory_iterator end;
    std::error_code ec;
    int64_t matched{0};
    int64_t scanned{0};
    int64_t maxEntries{kWorkspaceFileIndexMaxEntries};
    bool done{false};
    bool truncated{false};
    int64_t generation{0};
};

/** Registry node that keeps public cursor tokens generation-safe after Destroy. */
struct WorkspaceFileIndexCursorRegistration {
    uintptr_t token{0};
    WorkspaceFileIndexPageCursor *cursor{nullptr};
    int64_t leases{0};
    std::atomic_flag operationLock = ATOMIC_FLAG_INIT;
    bool destroyed{false};
    WorkspaceFileIndexCursorRegistration *next{nullptr};
};

/** Trivially initialized registry for opaque cursor tokens. */
WorkspaceFileIndexCursorRegistration *g_fileIndexCursorRegistry = nullptr;
std::atomic_flag g_fileIndexCursorRegistryLock = ATOMIC_FLAG_INIT;

/// @brief Serialize access to the cursor-token registry.
struct FileIndexCursorRegistryLockGuard {
    FileIndexCursorRegistryLockGuard() {
        while (g_fileIndexCursorRegistryLock.test_and_set(std::memory_order_acquire)) {
        }
    }

    ~FileIndexCursorRegistryLockGuard() {
        g_fileIndexCursorRegistryLock.clear(std::memory_order_release);
    }

    FileIndexCursorRegistryLockGuard(const FileIndexCursorRegistryLockGuard &) = delete;
};

/// @brief Serialize iterator mutation for one retained cursor registration.
struct FileIndexCursorOperationGuard {
    WorkspaceFileIndexCursorRegistration *registration;

    explicit FileIndexCursorOperationGuard(WorkspaceFileIndexCursorRegistration *value)
        : registration(value) {
        while (registration->operationLock.test_and_set(std::memory_order_acquire)) {
        }
    }

    ~FileIndexCursorOperationGuard() {
        registration->operationLock.clear(std::memory_order_release);
    }

    FileIndexCursorOperationGuard(const FileIndexCursorOperationGuard &) = delete;
};

/// @brief Publish a cursor behind a non-dereferenceable monotonic token.
void *registerFileIndexCursor(WorkspaceFileIndexPageCursor *cursor) {
    if (!cursor)
        return nullptr;
    auto *registration = new (std::nothrow) WorkspaceFileIndexCursorRegistration();
    if (!registration)
        return nullptr;
    registration->token = static_cast<uintptr_t>(cursor->generation);
    if (registration->token == 0) {
        delete registration;
        return nullptr;
    }
    registration->cursor = cursor;
    {
        FileIndexCursorRegistryLockGuard lock;
        registration->next = g_fileIndexCursorRegistry;
        g_fileIndexCursorRegistry = registration;
    }
    return reinterpret_cast<void *>(registration->token);
}

/// @brief Retain a live registration addressed by an opaque public token.
WorkspaceFileIndexCursorRegistration *retainFileIndexCursor(void *handle) {
    const uintptr_t token = reinterpret_cast<uintptr_t>(handle);
    if (token == 0)
        return nullptr;
    FileIndexCursorRegistryLockGuard lock;
    for (auto *entry = g_fileIndexCursorRegistry; entry; entry = entry->next) {
        if (entry->token == token && !entry->destroyed) {
            entry->leases++;
            return entry;
        }
    }
    return nullptr;
}

/// @brief Release a registry lease and retire a previously destroyed cursor.
void releaseFileIndexCursor(WorkspaceFileIndexCursorRegistration *registration) {
    if (!registration)
        return;
    bool deleteNow = false;
    {
        FileIndexCursorRegistryLockGuard lock;
        registration->leases--;
        deleteNow = registration->leases == 0 && registration->destroyed;
    }
    if (deleteNow) {
        delete registration->cursor;
        delete registration;
    }
}

/// @brief Remove a public token; active operations retain the cursor until completion.
void unregisterFileIndexCursor(void *handle) {
    const uintptr_t token = reinterpret_cast<uintptr_t>(handle);
    if (token == 0)
        return;
    WorkspaceFileIndexCursorRegistration *removed = nullptr;
    bool deleteNow = false;
    {
        FileIndexCursorRegistryLockGuard lock;
        WorkspaceFileIndexCursorRegistration **link = &g_fileIndexCursorRegistry;
        while (*link) {
            if ((*link)->token == token) {
                removed = *link;
                *link = removed->next;
                removed->next = nullptr;
                removed->destroyed = true;
                deleteNow = removed->leases == 0;
                break;
            }
            link = &(*link)->next;
        }
    }
    if (deleteNow) {
        delete removed->cursor;
        delete removed;
    }
}

/// @brief Return nested ignore rules while loading each directory at most once per cursor.
const std::vector<std::string> &cursorGitignorePatternsForDirectory(
    WorkspaceFileIndexPageCursor *cursor, std::string directory) {
    static const std::vector<std::string> empty;
    if (!cursor)
        return empty;
    directory = normalizeSlashes(directory);
    if (directory == ".")
        directory.clear();
    auto existing = cursor->gitignorePatternsByDirectory.find(directory);
    if (existing != cursor->gitignorePatternsByDirectory.end())
        return existing->second;

    std::vector<std::string> patterns;
    if (!directory.empty()) {
        std::string ancestor = normalizeSlashes(fs::path(directory).parent_path().generic_string());
        if (ancestor == ".")
            ancestor.clear();
        const auto &inherited = cursorGitignorePatternsForDirectory(cursor, ancestor);
        patterns = inherited;
    }
    for (const auto &pattern : cachedGitignorePatterns(cursor->root / directory))
        patterns.push_back(rebaseGitignorePattern(directory, pattern));
    return cursor->gitignorePatternsByDirectory.emplace(directory, std::move(patterns))
        .first->second;
}

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

/// @brief Destroy one file-index cursor.
/// @param cursor Cursor node to destroy; may be NULL.
void destroyFileIndexPageCursor(WorkspaceFileIndexPageCursor *cursor) {
    delete cursor;
}

/// @brief Start a new FileIndex.Page traversal cursor.
/// @details Construction assigns an immutable monotonic generation so callers
///          can reject a page produced by an obsolete traversal.
/// @param root Absolute workspace root path.
/// @param extensionsCsv Comma-separated extension allow-list.
/// @param excludesCsv Comma-separated extra ignore patterns.
/// @param includeDirs True to emit matching directory entries.
/// @param diagnostics Runtime sequence receiving traversal diagnostics.
/// @return Newly allocated cursor, or NULL on allocation/traversal failure.
WorkspaceFileIndexPageCursor *startFileIndexPageCursor(const fs::path &root,
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
    cursor->root = root;
    cursor->extensions = parseExtensionSet(extensionsCsv);
    cursor->extraPatterns = splitList(excludesCsv);
    cursor->includeDirs = includeDirs;
    cursor->sampleContent = includeDirs && cursor->extensions.empty();
    const uint64_t generation =
        g_fileIndexCursorGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    cursor->generation = static_cast<int64_t>(generation & 0x7fffffffffffffffull);
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
/// @param sampleContent True to add a bounded regular-file content sample.
void emitFileIndexEntry(void *entries,
                        const fs::path &root,
                        const fs::directory_entry &dirEntry,
                        const std::string &relativePath,
                        bool isDir,
                        bool sampleContent) {
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
    int64_t fileSize = isDir ? 0 : -1;
    int64_t sampleHash = isDir ? 0 : -1;
    if (!isDir) {
        std::error_code sizeEc;
        uintmax_t rawSize = dirEntry.file_size(sizeEc);
        if (!sizeEc && rawSize <= static_cast<uintmax_t>(INT64_MAX)) {
            fileSize = static_cast<int64_t>(rawSize);
            std::error_code symlinkEc;
            if (sampleContent && !dirEntry.is_symlink(symlinkEc) && !symlinkEc)
                sampleHash = boundedFileSampleHash(dirEntry.path(), fileSize);
        }
    }
    rt_map_set_int(entry, rt_const_cstr("size"), fileSize);
    rt_map_set_int(entry, rt_const_cstr("modified"), fileTimeSeconds(dirEntry.path()));
    rt_map_set_int(entry, rt_const_cstr("modifiedNs"), fileTimeNanoseconds(dirEntry.path()));
    rt_map_set_int(entry, rt_const_cstr("sampleHash"), sampleHash);
    seqPushOwned(entries, entry);
    (void)root;
}

/// @brief Emit up to @p limit entries from @p cursor starting at @p offset.
/// @details Cursor state advances across calls. Each matching relative path is
///          emitted at most once for the cursor generation.
/// @param cursor Live traversal cursor to scan.
/// @param entries Runtime sequence receiving emitted file-index maps.
/// @param offset Logical match offset requested by the caller.
/// @param limit Maximum number of entries to emit.
/// @return Number of entries emitted into @p entries.
int64_t scanFileIndexPageCursor(WorkspaceFileIndexPageCursor *cursor,
                                void *entries,
                                int64_t offset,
                                int64_t limit,
                                int64_t workLimit) {
    if (!cursor || cursor->done)
        return 0;

    int64_t emitted = 0;
    int64_t work = 0;
    for (; work < workLimit && !cursor->ec && cursor->it != cursor->end;
         cursor->it.increment(cursor->ec)) {
        work++;
        cursor->scanned++;
        std::error_code relEc;
        const fs::directory_entry &dirEntry = *cursor->it;
        std::string rel =
            normalizeSlashes(fs::relative(dirEntry.path(), cursor->root, relEc).generic_string());
        if (relEc || rel.empty() || rel == ".")
            continue;

        bool isDir = dirEntry.is_directory(cursor->ec);
        std::string parent = normalizeSlashes(fs::path(rel).parent_path().generic_string());
        const auto &gitignorePatterns =
            cursorGitignorePatternsForDirectory(cursor, parent == "." ? "" : parent);
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
        if (cursor->maxEntries > 0 && cursor->matched >= cursor->maxEntries) {
            cursor->truncated = true;
            cursor->done = true;
            break;
        }

        if (cursor->matched >= offset && emitted < limit) {
            emitFileIndexEntry(entries, cursor->root, dirEntry, rel, isDir, cursor->sampleContent);
            emitted++;
        }
        cursor->matched++;
        if (emitted >= limit) {
            cursor->it.increment(cursor->ec);
            cursor->done = cursor->ec || cursor->it == cursor->end;
            return emitted;
        }
    }

    cursor->done = cursor->ec || cursor->it == cursor->end;
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

/// Workspace-edit resource ceilings keep opaque runtime input from turning one
/// validation call into an unbounded allocation or filesystem traversal.
constexpr int64_t kWorkspaceEditMaxRecords = 100000;
constexpr size_t kWorkspaceEditMaxFiles = 20000;
constexpr size_t kWorkspaceEditMaxFileBytes = 64u * 1024u * 1024u;
constexpr size_t kWorkspaceEditMaxSourceBytes = 256u * 1024u * 1024u;
constexpr size_t kWorkspaceEditMaxReplacementBytes = 128u * 1024u * 1024u;
constexpr size_t kWorkspaceEditMaxOutputBytes = 256u * 1024u * 1024u;

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
    std::string expectedHash;
    int64_t maxBytes{-1};
    bool wholeFile{false};
    size_t startOffset{0};
    size_t endOffset{0};
    bool valid{false};
};

/// @brief Stable identity captured for one validated regular file.
struct WorkspaceEditFileIdentity {
#if RT_PLATFORM_WINDOWS
    DWORD volumeSerial{0};
    DWORD fileIndexHigh{0};
    DWORD fileIndexLow{0};
#else
    dev_t device{0};
    ino_t inode{0};
#endif
    bool valid{false};
};

/// @brief Convert a stable file identity into a collision-free native key.
static std::string workspaceEditIdentityKey(const WorkspaceEditFileIdentity &identity) {
#if RT_PLATFORM_WINDOWS
    return std::to_string(identity.volumeSerial) + ":" + std::to_string(identity.fileIndexHigh) +
           ":" + std::to_string(identity.fileIndexLow);
#else
    return std::to_string(static_cast<uintmax_t>(identity.device)) + ":" +
           std::to_string(static_cast<uintmax_t>(identity.inode));
#endif
}

/// @brief Compare two captured stable file identities.
static bool workspaceEditIdentityEqual(const WorkspaceEditFileIdentity &left,
                                       const WorkspaceEditFileIdentity &right) {
    if (!left.valid || !right.valid)
        return false;
#if RT_PLATFORM_WINDOWS
    return left.volumeSerial == right.volumeSerial && left.fileIndexHigh == right.fileIndexHigh &&
           left.fileIndexLow == right.fileIndexLow;
#else
    return left.device == right.device && left.inode == right.inode;
#endif
}

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
    candidate = fs::weakly_canonical(candidate, ec);
    if (ec)
        return false;
    if (roots) {
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

/// @brief Handle-relative access to one workspace-edit target.
/// @details Rooted calls open every directory without following links and keep
///          the verified parent anchored for all later file operations. Windows
///          retains ancestor handles without delete sharing, preventing a path
///          component from being renamed while a transaction uses full-path
///          APIs. POSIX performs leaf operations through @c *at APIs on the
///          retained parent descriptor.
struct WorkspaceEditTargetAccess {
    fs::path file;
    fs::path parent;
    std::string leaf;
#if RT_PLATFORM_WINDOWS
    std::wstring leafWide;
    std::vector<HANDLE> directoryHandles;
    HANDLE fileHandle{INVALID_HANDLE_VALUE};
#else
    int parentFd{-1};
    int fileFd{-1};
#endif

    WorkspaceEditTargetAccess() = default;
    WorkspaceEditTargetAccess(const WorkspaceEditTargetAccess &) = delete;
    WorkspaceEditTargetAccess &operator=(const WorkspaceEditTargetAccess &) = delete;

    ~WorkspaceEditTargetAccess() {
#if RT_PLATFORM_WINDOWS
        if (fileHandle != INVALID_HANDLE_VALUE)
            CloseHandle(fileHandle);
        for (auto it = directoryHandles.rbegin(); it != directoryHandles.rend(); ++it)
            CloseHandle(*it);
#else
        if (fileFd >= 0)
            close(fileFd);
        if (parentFd >= 0)
            close(parentFd);
#endif
    }
};

/// @brief Open a directory without following its final path component.
#if RT_PLATFORM_WINDOWS
static HANDLE openWorkspaceEditDirectoryWindows(const fs::path &path, bool retainNameLock) {
    const DWORD sharing =
        FILE_SHARE_READ | FILE_SHARE_WRITE | (retainNameLock ? 0 : FILE_SHARE_DELETE);
    HANDLE handle = CreateFileW(path.wstring().c_str(),
                                FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                sharing,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        CloseHandle(handle);
        return INVALID_HANDLE_VALUE;
    }
    return handle;
}
#else
static int openWorkspaceEditDirectoryPosix(const fs::path &path) {
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path.c_str(), flags);
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    if (fd >= 0) {
        const int oldFlags = fcntl(fd, F_GETFD);
        if (oldFlags >= 0)
            (void)fcntl(fd, F_SETFD, oldFlags | FD_CLOEXEC);
    }
#endif
    if (fd < 0)
        return -1;
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}

static int openWorkspaceEditChildDirectoryPosix(int parentFd, const fs::path &name) {
    const std::string component = name.string();
    if (component.empty() || component == "." || component == "..")
        return -1;
    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = openat(parentFd, component.c_str(), flags);
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    if (fd >= 0) {
        const int oldFlags = fcntl(fd, F_GETFD);
        if (oldFlags >= 0)
            (void)fcntl(fd, F_SETFD, oldFlags | FD_CLOEXEC);
    }
#endif
    if (fd < 0)
        return -1;
    struct stat status{};
    if (fstat(fd, &status) != 0 || !S_ISDIR(status.st_mode)) {
        close(fd);
        return -1;
    }
    return fd;
}
#endif

/// @brief Open one canonical target through its trusted workspace root.
/// @param file Canonical absolute target returned by @ref resolveEditTarget.
/// @param roots Optional canonical roots; null denotes the legacy unrooted API.
/// @param requireDeleteAccess Windows-only request for commit-time rename access.
/// @return Owned access object, or null when any component/leaf is unsafe.
static std::unique_ptr<WorkspaceEditTargetAccess> openWorkspaceEditTarget(
    const std::string &file, const std::vector<fs::path> *roots, bool requireDeleteAccess) {
    auto access = std::make_unique<WorkspaceEditTargetAccess>();
    access->file = fs::path(file);
    access->parent = access->file.parent_path();
    access->leaf = access->file.filename().string();
    if (access->leaf.empty() || access->leaf == "." || access->leaf == "..")
        return nullptr;
#if RT_PLATFORM_WINDOWS
    access->leafWide = access->file.filename().wstring();
#endif

    fs::path traversalRoot = access->parent;
    fs::path relativeParent;
    bool rooted = false;
    if (roots) {
        for (const fs::path &root : *roots) {
            if (!editTargetIsInRoot(access->file, root))
                continue;
            std::error_code relativeError;
            fs::path relative = fs::relative(access->file, root, relativeError);
            if (relativeError || relative.empty() || relative.is_absolute())
                continue;
            bool unsafe = false;
            for (const fs::path &part : relative)
                unsafe = unsafe || part == "..";
            if (unsafe)
                continue;
            traversalRoot = root;
            relativeParent = relative.parent_path();
            rooted = true;
            break;
        }
        if (!rooted)
            return nullptr;
    }

#if RT_PLATFORM_WINDOWS
    fs::path currentPath = traversalRoot;
    HANDLE current = openWorkspaceEditDirectoryWindows(currentPath, rooted);
    if (current == INVALID_HANDLE_VALUE)
        return nullptr;
    access->directoryHandles.push_back(current);
    if (rooted) {
        for (const fs::path &part : relativeParent) {
            if (part.empty() || part == "." || part == "..")
                return nullptr;
            currentPath /= part;
            current = openWorkspaceEditDirectoryWindows(currentPath, true);
            if (current == INVALID_HANDLE_VALUE)
                return nullptr;
            access->directoryHandles.push_back(current);
        }
    }
    const DWORD desired =
        GENERIC_READ | FILE_READ_ATTRIBUTES | READ_CONTROL | (requireDeleteAccess ? DELETE : 0);
    const DWORD fileSharing =
        FILE_SHARE_READ | FILE_SHARE_WRITE | (requireDeleteAccess ? 0 : FILE_SHARE_DELETE);
    access->fileHandle = CreateFileW(access->file.wstring().c_str(),
                                     desired,
                                     fileSharing,
                                     nullptr,
                                     OPEN_EXISTING,
                                     FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                     nullptr);
    if (access->fileHandle == INVALID_HANDLE_VALUE)
        return nullptr;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(
            access->fileHandle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
            0)
        return nullptr;
#else
    int current = openWorkspaceEditDirectoryPosix(traversalRoot);
    if (current < 0)
        return nullptr;
    if (rooted) {
        for (const fs::path &part : relativeParent) {
            int next = openWorkspaceEditChildDirectoryPosix(current, part);
            close(current);
            if (next < 0)
                return nullptr;
            current = next;
        }
    }
    access->parentFd = current;
    int flags = O_RDONLY;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    access->fileFd = openat(access->parentFd, access->leaf.c_str(), flags);
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    if (access->fileFd >= 0) {
        const int oldFlags = fcntl(access->fileFd, F_GETFD);
        if (oldFlags >= 0)
            (void)fcntl(access->fileFd, F_SETFD, oldFlags | FD_CLOEXEC);
    }
#endif
    struct stat status{};
    if (access->fileFd < 0 || fstat(access->fileFd, &status) != 0 || !S_ISREG(status.st_mode))
        return nullptr;
#endif
    return access;
}

/// @brief Capture stable identity from an already-open target handle.
static bool workspaceEditFileIdentity(const WorkspaceEditTargetAccess &access,
                                      WorkspaceEditFileIdentity &out) {
    out = {};
#if RT_PLATFORM_WINDOWS
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(access.fileHandle, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return false;
    out.volumeSerial = info.dwVolumeSerialNumber;
    out.fileIndexHigh = info.nFileIndexHigh;
    out.fileIndexLow = info.nFileIndexLow;
#else
    struct stat info{};
    if (fstat(access.fileFd, &info) != 0 || !S_ISREG(info.st_mode))
        return false;
    out.device = info.st_dev;
    out.inode = info.st_ino;
#endif
    out.valid = true;
    return true;
}

/// @brief Read one regular edit target within per-file and aggregate ceilings.
/// @details The file size is sampled before allocation, the exact byte count is
///          read, and trailing data is rejected if the file grows during the
///          read. This prevents special files and racing growth from bypassing
///          the workspace-edit memory budget.
/// @param file Canonical target path.
/// @param sourceBytes Bytes already retained for other files in the batch.
/// @param diagnostics Owning runtime sequence receiving failures.
/// @param[out] text Receives the exact target bytes on success.
/// @return True when a bounded, stable snapshot was read.
bool readWorkspaceEditTarget(const std::string &file,
                             const std::vector<fs::path> *roots,
                             size_t sourceBytes,
                             void *diagnostics,
                             std::string &text,
                             WorkspaceEditFileIdentity &identity,
                             int64_t &modifiedSeconds,
                             int64_t &fileSize) {
    std::unique_ptr<WorkspaceEditTargetAccess> access = openWorkspaceEditTarget(file, roots, false);
    if (!access || !workspaceEditFileIdentity(*access, identity)) {
        pushDiagnostic(
            diagnostics, "workspace edit target is not a safe regular file", file, 0, "edit.read");
        return false;
    }

    uint64_t rawSize = 0;
#if RT_PLATFORM_WINDOWS
    BY_HANDLE_FILE_INFORMATION before{};
    LARGE_INTEGER nativeSize{};
    if (!GetFileInformationByHandle(access->fileHandle, &before) ||
        !GetFileSizeEx(access->fileHandle, &nativeSize) || nativeSize.QuadPart < 0)
        return false;
    rawSize = static_cast<uint64_t>(nativeSize.QuadPart);
    ULARGE_INTEGER ticks{};
    ticks.LowPart = before.ftLastWriteTime.dwLowDateTime;
    ticks.HighPart = before.ftLastWriteTime.dwHighDateTime;
    constexpr uint64_t kWindowsToUnixEpoch100ns = 116444736000000000ull;
    modifiedSeconds =
        ticks.QuadPart >= kWindowsToUnixEpoch100ns
            ? static_cast<int64_t>((ticks.QuadPart - kWindowsToUnixEpoch100ns) / 10000000ull)
            : -1;
#else
    struct stat before{};
    if (fstat(access->fileFd, &before) != 0 || before.st_size < 0)
        return false;
    rawSize = static_cast<uint64_t>(before.st_size);
    modifiedSeconds = static_cast<int64_t>(before.st_mtime);
#endif
    if (rawSize > kWorkspaceEditMaxFileBytes ||
        rawSize > kWorkspaceEditMaxSourceBytes - sourceBytes) {
        pushDiagnostic(diagnostics,
                       "workspace edit target exceeds the bounded read budget",
                       file,
                       0,
                       "edit.limit");
        return false;
    }

    const size_t size = static_cast<size_t>(rawSize);
    fileSize = static_cast<int64_t>(size);
    text.assign(size, '\0');
#if RT_PLATFORM_WINDOWS
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(access->fileHandle, beginning, nullptr, FILE_BEGIN))
        return false;
    size_t offset = 0;
    while (offset < size) {
        const DWORD chunk =
            static_cast<DWORD>(std::min<size_t>(size - offset, std::numeric_limits<DWORD>::max()));
        DWORD read = 0;
        if (!ReadFile(access->fileHandle, text.data() + offset, chunk, &read, nullptr) ||
            read != chunk) {
            pushDiagnostic(
                diagnostics, "edit target changed while being read", file, 0, "edit.version");
            text.clear();
            return false;
        }
        offset += read;
    }
    char trailing = 0;
    DWORD trailingRead = 0;
    if (!ReadFile(access->fileHandle, &trailing, 1, &trailingRead, nullptr) || trailingRead != 0) {
        pushDiagnostic(
            diagnostics, "edit target changed while being read", file, 0, "edit.version");
        text.clear();
        return false;
    }
#else
    size_t offset = 0;
    while (offset < size) {
        const ssize_t readBytes =
            pread(access->fileFd, text.data() + offset, size - offset, offset);
        if (readBytes <= 0) {
            pushDiagnostic(
                diagnostics, "edit target changed while being read", file, 0, "edit.version");
            text.clear();
            return false;
        }
        offset += static_cast<size_t>(readBytes);
    }
    char trailing = 0;
    if (pread(access->fileFd, &trailing, 1, static_cast<off_t>(size)) != 0) {
        pushDiagnostic(
            diagnostics, "edit target changed while being read", file, 0, "edit.version");
        text.clear();
        return false;
    }
#endif
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
    out.expectedHash = mapGetString(obj, "expectedHash");
    out.maxBytes = rt_map_get_int_or(obj, rt_const_cstr("maxBytes"), -1);
    out.wholeFile = rt_map_get_bool_or(obj, rt_const_cstr("wholeFile"), 0) != 0;
    if (out.file.empty()) {
        pushDiagnostic(diagnostics, "workspace edit missing file", "", index, "edit.file");
        return false;
    }
    if (!out.wholeFile &&
        (out.startLine < 1 || out.startColumn < 1 || out.endLine < 1 || out.endColumn < 1)) {
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
/// @param[out] identities Stable entry identity captured beside each original file image.
/// @param diagnostics Runtime sequence receiving structured failures.
/// @param roots Optional canonical workspace roots constraining every target.
/// @return True only when every record passes all batch validation checks.
bool validateEditRecords(std::vector<EditRecord> &records,
                         std::unordered_map<std::string, std::string> &contents,
                         std::unordered_map<std::string, WorkspaceEditFileIdentity> &identities,
                         void *diagnostics,
                         const std::vector<fs::path> *roots) {
    bool ok = true;
    size_t sourceBytes = 0;
    std::unordered_map<std::string, std::string> identityOwners;
    std::unordered_map<std::string, int64_t> modifiedTimes;
    for (auto &record : records) {
        record.valid = false;
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
            if (contents.size() >= kWorkspaceEditMaxFiles) {
                pushDiagnostic(diagnostics,
                               "workspace edit batch exceeds the file-count limit",
                               record.file,
                               0,
                               "edit.limit");
                ok = false;
                continue;
            }
            WorkspaceEditFileIdentity identity;
            int64_t modifiedSeconds = -1;
            int64_t fileSize = -1;
            std::string text;
            if (!readWorkspaceEditTarget(record.file,
                                         roots,
                                         sourceBytes,
                                         diagnostics,
                                         text,
                                         identity,
                                         modifiedSeconds,
                                         fileSize)) {
                ok = false;
                continue;
            }
            const std::string identityKey = workspaceEditIdentityKey(identity);
            auto owner = identityOwners.find(identityKey);
            if (owner != identityOwners.end() && owner->second != record.file) {
                pushDiagnostic(diagnostics,
                               "workspace edit batch contains multiple paths for one file",
                               record.file,
                               0,
                               "edit.alias");
                ok = false;
                continue;
            }
            sourceBytes += text.size();
            contents.emplace(record.file, std::move(text));
            identities.emplace(record.file, identity);
            identityOwners.emplace(identityKey, record.file);
            modifiedTimes.emplace(record.file, modifiedSeconds);
        }
        auto modified = modifiedTimes.find(record.file);
        if (record.expectedMtime >= 0 &&
            (modified == modifiedTimes.end() || modified->second != record.expectedMtime)) {
            pushDiagnostic(diagnostics,
                           "edit target changed since expectedMtime",
                           record.file,
                           0,
                           "edit.version");
            ok = false;
            continue;
        }
        if (record.expectedSize >= 0) {
            auto content = contents.find(record.file);
            if (content == contents.end() ||
                content->second.size() != static_cast<uint64_t>(record.expectedSize)) {
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
        if (record.maxBytes >= 0 &&
            (text.size() > static_cast<uint64_t>(record.maxBytes) ||
             record.newText.size() > static_cast<uint64_t>(record.maxBytes))) {
            pushDiagnostic(diagnostics,
                           "workspace edit exceeds the caller byte limit",
                           record.file,
                           0,
                           "edit.limit");
            ok = false;
            continue;
        }
        if (!record.expectedHash.empty() && sha256Text(text) != record.expectedHash) {
            pushDiagnostic(diagnostics,
                           "edit target changed since expectedHash",
                           record.file,
                           0,
                           "edit.version");
            ok = false;
            continue;
        }
        if (record.wholeFile) {
            record.startOffset = 0;
            record.endOffset = text.size();
            record.valid = true;
            continue;
        }
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
        record.valid = true;
    }

    std::map<std::string, std::vector<EditRecord *>> byFile;
    for (auto &record : records) {
        if (record.valid)
            byFile[record.file].push_back(&record);
    }
    for (auto &[file, vec] : byFile) {
        bool hasWholeFile = false;
        for (const EditRecord *record : vec)
            hasWholeFile = hasWholeFile || record->wholeFile;
        if (hasWholeFile && vec.size() != 1) {
            pushDiagnostic(diagnostics,
                           "whole-file replacement cannot be combined with other edits",
                           file,
                           0,
                           "edit.overlap");
            ok = false;
            continue;
        }
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

    // Output-size arithmetic assumes every range is valid and disjoint. A
    // failed structural/version/range check already rejects the transaction,
    // so avoid deriving sizes from incomplete offsets in that case.
    if (!ok)
        return false;

    size_t outputBytes = 0;
    for (const auto &[file, text] : contents) {
        size_t finalSize = text.size();
        auto fileEdits = byFile.find(file);
        if (fileEdits != byFile.end()) {
            for (const EditRecord *record : fileEdits->second) {
                const size_t removed = record->endOffset - record->startOffset;
                finalSize -= removed;
                if (record->newText.size() > kWorkspaceEditMaxFileBytes - finalSize) {
                    pushDiagnostic(diagnostics,
                                   "workspace edit output exceeds the per-file limit",
                                   file,
                                   0,
                                   "edit.limit");
                    ok = false;
                    finalSize = kWorkspaceEditMaxFileBytes;
                    break;
                }
                finalSize += record->newText.size();
            }
        }
        if (finalSize > kWorkspaceEditMaxOutputBytes - outputBytes) {
            pushDiagnostic(diagnostics,
                           "workspace edit batch exceeds the output-byte limit",
                           file,
                           0,
                           "edit.limit");
            ok = false;
            break;
        }
        outputBytes += finalSize;
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

/// @brief Create an explicitly owned workspace file-index traversal.
/// @details Unlike legacy offset paging, the handle retains its iterator until
///          Destroy and cannot be evicted by unrelated callers.
void *rt_workspace_file_index_cursor_new(rt_string root_s,
                                         rt_string extensions_csv,
                                         rt_string excludes_csv,
                                         int8_t include_dirs) {
    try {
        fs::path root = toStd(root_s);
        if (root.empty())
            return nullptr;
        std::error_code ec;
        root = fs::absolute(root, ec).lexically_normal();
        if (ec || !fs::is_directory(root, ec))
            return nullptr;
        void *diagnostics = rt_seq_new_owned();
        WorkspaceFileIndexPageCursor *cursor = startFileIndexPageCursor(
            root, toStd(extensions_csv), toStd(excludes_csv), include_dirs != 0, diagnostics);
        releaseObject(diagnostics);
        // An explicitly owned cursor retains only one bounded page at a time,
        // so it can safely traverse workspaces larger than legacy materialized
        // result limits. Stateless page/enumerate callers keep the hard cap.
        if (cursor)
            cursor->maxEntries = 0;
        void *handle = registerFileIndexCursor(cursor);
        if (!handle)
            destroyFileIndexPageCursor(cursor);
        return handle;
    } catch (...) {
        return nullptr;
    }
}

/// @brief Test whether an explicit file-index cursor was created successfully.
int8_t rt_workspace_file_index_cursor_is_valid(void *handle) {
    auto *registration = retainFileIndexCursor(handle);
    const int8_t valid = registration ? 1 : 0;
    releaseFileIndexCursor(registration);
    return valid;
}

/// @brief Return the immutable generation assigned to an explicit traversal.
int64_t rt_workspace_file_index_cursor_generation(void *handle) {
    auto *registration = retainFileIndexCursor(handle);
    const int64_t generation = registration ? registration->cursor->generation : 0;
    releaseFileIndexCursor(registration);
    return generation;
}

/// @brief Advance one explicitly owned traversal by a bounded result page.
void *rt_workspace_file_index_cursor_next(void *handle, int64_t limit) {
    void *result = rt_map_new();
    void *entries = rt_seq_new_owned();
    void *diagnostics = rt_seq_new_owned();
    if (limit <= 0)
        limit = 512;
    if (limit > 4096)
        limit = 4096;
    auto *registration = retainFileIndexCursor(handle);
    auto *cursor = registration ? registration->cursor : nullptr;
    const int64_t offset = cursor ? cursor->matched : 0;
    rt_map_set_bool(result, rt_const_cstr("valid"), cursor ? 1 : 0);
    mapSetStr(result, "root", cursor ? cursor->root.generic_string() : "");
    rt_map_set_int(result, rt_const_cstr("offset"), offset);
    rt_map_set_int(result, rt_const_cstr("limit"), limit);
    rt_map_set_int(result, rt_const_cstr("emitted"), 0);
    rt_map_set_int(result, rt_const_cstr("work"), 0);
    rt_map_set_int(result, rt_const_cstr("nextOffset"), offset);
    rt_map_set_int(result, rt_const_cstr("scanned"), cursor ? cursor->scanned : 0);
    rt_map_set_int(result, rt_const_cstr("generation"), cursor ? cursor->generation : 0);
    rt_map_set_int(result, rt_const_cstr("maxEntries"), cursor ? cursor->maxEntries : 0);
    rt_map_set_bool(result, rt_const_cstr("done"), cursor && !cursor->done ? 0 : 1);
    rt_map_set_bool(result, rt_const_cstr("truncated"), cursor && cursor->truncated ? 1 : 0);
    rt_map_set_bool(result, rt_const_cstr("stale"), 0);
    rt_map_set(result, rt_const_cstr("entries"), entries);
    rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
    if (!cursor) {
        pushDiagnostic(diagnostics,
                       "workspace file-index cursor is invalid",
                       "",
                       0,
                       "fileindex.cursor.invalid");
        releaseObject(entries);
        releaseObject(diagnostics);
        releaseFileIndexCursor(registration);
        return result;
    }
    try {
        FileIndexCursorOperationGuard operation(registration);
        const int64_t scannedBefore = cursor->scanned;
        const int64_t workLimit = std::min<int64_t>(32768, std::max<int64_t>(64, limit * 8));
        const int64_t emitted = scanFileIndexPageCursor(cursor, entries, offset, limit, workLimit);
        if (cursor->ec) {
            pushDiagnostic(diagnostics,
                           "workspace traversal stopped early",
                           cursor->root.generic_string(),
                           0,
                           "fileindex.walk");
        }
        rt_map_set_int(result, rt_const_cstr("emitted"), emitted);
        rt_map_set_int(result, rt_const_cstr("work"), cursor->scanned - scannedBefore);
        rt_map_set_int(result, rt_const_cstr("nextOffset"), cursor->matched);
        rt_map_set_int(result, rt_const_cstr("scanned"), cursor->scanned);
        rt_map_set_bool(result, rt_const_cstr("done"), cursor->done ? 1 : 0);
        rt_map_set_bool(result, rt_const_cstr("truncated"), cursor->truncated ? 1 : 0);
    } catch (...) {
        cursor->done = true;
        rt_map_set_bool(result, rt_const_cstr("valid"), 0);
        rt_map_set_bool(result, rt_const_cstr("done"), 1);
        pushDiagnostic(diagnostics,
                       "workspace file-index cursor failed",
                       cursor->root.generic_string(),
                       0,
                       "fileindex.cursor.exception");
    }
    releaseObject(entries);
    releaseObject(diagnostics);
    releaseFileIndexCursor(registration);
    return result;
}

/// @brief Destroy an explicit workspace file-index traversal handle.
void rt_workspace_file_index_cursor_destroy(void *handle) {
    unregisterFileIndexCursor(handle);
}

/// @brief Return a bounded page of workspace file-index entries.
/// @details This is the allocation-bounded companion to
///          `rt_workspace_file_index_enumerate`. It walks the same ordered
///          recursive traversal, applies the same ignore and extension filters,
///          and emits at most @p limit entry maps. This compatibility entry point
///          is stateless and rescans to @p offset; long-lived callers should own
///          a FileIndexCursor so traversal state cannot be evicted or confused
///          with another generation.
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
    rt_map_set_int(result, rt_const_cstr("generation"), 0);
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
        int64_t emitted = 0;
        bool done = true;
        bool truncated = false;
        int64_t matched = offset;
        WorkspaceFileIndexPageCursor *cursor = startFileIndexPageCursor(
            root, extensionsCsv, excludesCsv, include_dirs != 0, diagnostics);
        if (!cursor) {
            rt_map_set_bool(result, rt_const_cstr("valid"), 0);
            releaseObject(entries);
            releaseObject(diagnostics);
            return result;
        }

        emitted = scanFileIndexPageCursor(
            cursor, entries, offset, limit, std::numeric_limits<int64_t>::max());
        matched = cursor->matched;
        done = cursor->done;
        truncated = cursor->truncated;
        ec = cursor->ec;
        const int64_t generation = cursor->generation;
        const int64_t scanned = cursor->scanned;
        destroyFileIndexPageCursor(cursor);

        if (ec) {
            pushDiagnostic(diagnostics,
                           "workspace traversal stopped early",
                           root.generic_string(),
                           0,
                           "fileindex.walk");
        }
        rt_map_set_int(result, rt_const_cstr("emitted"), emitted);
        rt_map_set_int(result, rt_const_cstr("nextOffset"), matched);
        rt_map_set_int(result, rt_const_cstr("scanned"), scanned);
        rt_map_set_int(result, rt_const_cstr("generation"), generation);
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
            int64_t file_size = isDir ? 0 : -1;
            int64_t sample_hash = isDir ? 0 : -1;
            if (!isDir) {
                std::error_code sizeEc;
                uintmax_t raw_size = it->file_size(sizeEc);
                if (!sizeEc && raw_size <= static_cast<uintmax_t>(INT64_MAX)) {
                    file_size = static_cast<int64_t>(raw_size);
                }
            }
            rt_map_set_int(entry, rt_const_cstr("size"), file_size);
            rt_map_set_int(entry, rt_const_cstr("modified"), fileTimeSeconds(it->path()));
            rt_map_set_int(entry, rt_const_cstr("modifiedNs"), fileTimeNanoseconds(it->path()));
            rt_map_set_int(entry, rt_const_cstr("sampleHash"), sample_hash);
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
/// @details Converts each event into a map containing its primary path, explicit
///          old/new rename endpoints, numeric/type-name fields, overflow count,
///          and a rescan flag. A rename missing either endpoint requires a
///          conservative rescan. Nonpositive limits default to 64; polling
///          stops at the first `NONE` event. Null watchers and caught exceptions
///          return an empty sequence.
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
            std::string oldPath;
            std::string newPath;
            if (type == RT_WATCH_EVENT_RENAMED) {
                rt_string oldPathValue = rt_watcher_event_old_path(watcher);
                rt_string newPathValue = rt_watcher_event_new_path(watcher);
                oldPath = toStd(oldPathValue);
                newPath = toStd(newPathValue);
                rt_string_unref(oldPathValue);
                rt_string_unref(newPathValue);
            }
            mapSetStr(event, "oldPath", oldPath);
            mapSetStr(event, "newPath", newPath);
            mapSetStr(event, "typeName", eventTypeName(type));
            rt_map_set_int(event, rt_const_cstr("type"), type);
            rt_map_set_int(
                event,
                rt_const_cstr("overflowCount"),
                type == RT_WATCH_EVENT_OVERFLOW ? rt_watcher_event_overflow_count(watcher) : 0);
            int requiresRescan =
                type == RT_WATCH_EVENT_OVERFLOW ||
                (type == RT_WATCH_EVENT_RENAMED && (oldPath.empty() || newPath.empty()));
            rt_map_set_bool(event, rt_const_cstr("requiresRescan"), requiresRescan ? 1 : 0);
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
            else if (canonical == "runprofile")
                mapSetStr(manifest, "runProfile", lower(value));
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

/// @brief Validate normalized workspace edits into a reusable native snapshot.
/// @details Loads runtime edit maps, reads each target once, checks versions and
///          ranges, and retains canonical records, original bytes, and stable
///          identities for a later prepared commit.
/// @param edits Runtime Seq of edit maps.
/// @param roots Optional canonical workspace roots that bound every edit target.
/// @param[out] records Canonical validated edit records.
/// @param[out] contents Exact original bytes keyed by canonical path.
/// @param[out] identities Stable identities captured with the original bytes.
/// @return Result map containing `success`, `editCount`, and `diagnostics`.
static void *workspace_edit_validate_into(
    void *edits,
    const std::vector<fs::path> *roots,
    std::vector<EditRecord> &records,
    std::unordered_map<std::string, std::string> &contents,
    std::unordered_map<std::string, WorkspaceEditFileIdentity> &identities) {
    void *result = rt_map_new();
    void *diagnostics = rt_seq_new_owned();
    bool ok = edits != nullptr && rt_obj_class_id(edits) == RT_SEQ_CLASS_ID;
    if (!edits) {
        pushDiagnostic(diagnostics, "workspace edits sequence is null", "", 0, "edit.null");
    } else if (rt_obj_class_id(edits) != RT_SEQ_CLASS_ID) {
        pushDiagnostic(diagnostics, "workspace edits must be a sequence", "", 0, "edit.invalid");
    } else {
        const int64_t len = rt_seq_len(edits);
        if (len > kWorkspaceEditMaxRecords) {
            pushDiagnostic(diagnostics,
                           "workspace edit batch exceeds the edit-count limit",
                           "",
                           0,
                           "edit.limit");
            ok = false;
        } else {
            size_t replacementBytes = 0;
            for (int64_t i = 0; i < len; i++) {
                EditRecord record;
                if (!loadEditRecord(rt_seq_get(edits, i), record, diagnostics, i)) {
                    ok = false;
                    continue;
                }
                if (record.newText.size() > kWorkspaceEditMaxReplacementBytes - replacementBytes) {
                    pushDiagnostic(diagnostics,
                                   "workspace edit batch exceeds the replacement-byte limit",
                                   record.file,
                                   i,
                                   "edit.limit");
                    ok = false;
                    continue;
                }
                replacementBytes += record.newText.size();
                records.push_back(std::move(record));
            }
        }
        if (ok && !validateEditRecords(records, contents, identities, diagnostics, roots))
            ok = false;
    }
    rt_map_set_bool(result, rt_const_cstr("success"), ok ? 1 : 0);
    rt_map_set_int(result, rt_const_cstr("editCount"), static_cast<int64_t>(records.size()));
    rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
    releaseObject(diagnostics);
    return result;
}

/// @brief Validate a batch and discard its reusable native snapshot.
/// @param edits Runtime Seq of edit maps.
/// @param roots Optional canonical workspace roots.
/// @return Fresh validation result map.
static void *workspace_edit_validate_impl(void *edits, const std::vector<fs::path> *roots) {
    std::vector<EditRecord> records;
    std::unordered_map<std::string, std::string> contents;
    std::unordered_map<std::string, WorkspaceEditFileIdentity> identities;
    return workspace_edit_validate_into(edits, roots, records, contents, identities);
}

/// @brief One explicitly owned, one-shot validated workspace-edit transaction.
/// @details Native records and original bytes remain private so callers cannot
///          mutate validation state between prepare and commit. The result map
///          is cloned for each public observation.
struct PreparedWorkspaceEdit {
    std::vector<EditRecord> records;
    std::unordered_map<std::string, std::string> contents;
    std::unordered_map<std::string, WorkspaceEditFileIdentity> identities;
    std::vector<fs::path> roots;
    bool rooted{false};
    void *validation{nullptr};
    bool consumed{false};

    ~PreparedWorkspaceEdit() {
        releaseObject(validation);
    }
};

/// @brief Prepare one reusable native transaction without changing files.
/// @param edits Runtime Seq of edit maps.
/// @param roots Optional canonical workspace roots.
/// @return Owned explicit transaction handle, including invalid results.
static PreparedWorkspaceEdit *workspace_edit_prepare_impl(void *edits,
                                                          const std::vector<fs::path> *roots) {
    auto prepared = std::make_unique<PreparedWorkspaceEdit>();
    if (roots) {
        prepared->roots = *roots;
        prepared->rooted = true;
    }
    prepared->validation = workspace_edit_validate_into(
        edits, roots, prepared->records, prepared->contents, prepared->identities);
    return prepared.release();
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
    std::string tempLeaf;
    std::string backupLeaf;
    std::unique_ptr<WorkspaceEditTargetAccess> access;
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
static bool reserveWorkspaceEditBackup(WorkspaceEditTargetAccess &access,
                                       std::string &reserved,
                                       std::string &reservedLeaf) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        fs::path candidate = workspaceEditTempPath(access.file, ".bak");
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
            reservedLeaf = candidate.filename().string();
            return true;
        }
#else
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        const std::string leaf = candidate.filename().string();
        int fd = openat(access.parentFd, leaf.c_str(), flags, S_IRUSR | S_IWUSR);
        if (fd >= 0) {
            close(fd);
            reserved = candidate.string();
            reservedLeaf = leaf;
            return true;
        }
#endif
    }
    return false;
}

#if RT_PLATFORM_WINDOWS
/// @brief Rename one already-open file to a fully qualified destination path.
/// @details Win32's `SetFileInformationByHandle` does not honor
///          `FILE_RENAME_INFO::RootDirectory`; a non-null handle there fails the
///          call with `ERROR_INVALID_PARAMETER`, because handle-relative renames
///          exist only in the native API. The destination is therefore an
///          absolute path in the preferred (backslash) form. That reopens no race
///          window: `WorkspaceEditTargetAccess` keeps every directory component
///          open without `FILE_SHARE_DELETE`, so no component can be renamed or
///          replaced mid-transaction.
/// @param file Open handle carrying DELETE access to the file being renamed.
/// @param destination Absolute path the file is renamed to.
/// @param replaceExisting Whether an existing destination may be replaced.
/// @return True when the rename completed.
static bool renameWorkspaceEditHandleWindows(HANDLE file,
                                             const fs::path &destination,
                                             bool replaceExisting) {
    if (file == INVALID_HANDLE_VALUE)
        return false;
    std::error_code ec;
    fs::path resolved = destination.is_absolute() ? destination : fs::absolute(destination, ec);
    if (ec || !resolved.is_absolute())
        return false;
    resolved.make_preferred();
    const std::wstring target = resolved.wstring();
    const size_t nameBytes = target.size() * sizeof(wchar_t);
    if (nameBytes == 0)
        return false;
    // One trailing wide NUL keeps the buffer terminated for the kernel's own copy
    // of the name; the length field still counts only the name itself.
    const size_t allocation = offsetof(FILE_RENAME_INFO, FileName) + nameBytes + sizeof(wchar_t);
    if (allocation > std::numeric_limits<DWORD>::max())
        return false;
    std::vector<unsigned char> storage(allocation, 0);
    auto *renameInfo = reinterpret_cast<FILE_RENAME_INFO *>(storage.data());
    renameInfo->ReplaceIfExists = replaceExisting ? TRUE : FALSE;
    renameInfo->RootDirectory = nullptr;
    renameInfo->FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(renameInfo->FileName, target.data(), nameBytes);
    return SetFileInformationByHandle(
               file, FileRenameInfo, renameInfo, static_cast<DWORD>(allocation)) != 0;
}

/// @brief Mark one already-open sidecar for deletion.
static bool deleteWorkspaceEditHandleWindows(HANDLE file) {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    return file != INVALID_HANDLE_VALUE &&
           SetFileInformationByHandle(
               file, FileDispositionInfo, &disposition, sizeof(disposition)) != 0;
}
#endif

/// @brief Move an edit target over its exclusively reserved backup placeholder.
/// @details POSIX rename replaces the placeholder atomically. Windows' ordinary
///          filesystem rename rejects an existing destination, so use the native
///          replace-existing operation there. Both paths keep the target and
///          backup in one directory and therefore on one filesystem.
/// @param target Existing workspace file to preserve.
/// @param backup Empty path created by @ref reserveWorkspaceEditBackup.
/// @return True when @p target was moved into @p backup.
static bool moveWorkspaceTargetToReservedBackup(WorkspaceEditTargetAccess &access,
                                                const std::string &backupLeaf) {
#if RT_PLATFORM_WINDOWS
    if (access.directoryHandles.empty() || access.fileHandle == INVALID_HANDLE_VALUE)
        return false;
    return renameWorkspaceEditHandleWindows(access.fileHandle, access.parent / backupLeaf, true);
#else
    return renameat(access.parentFd, access.leaf.c_str(), access.parentFd, backupLeaf.c_str()) == 0;
#endif
}

/// @brief Move a staged temp into the target leaf without path re-resolution.
static bool moveWorkspaceTempToTarget(WorkspaceEditTargetAccess &access,
                                      const fs::path &temp,
                                      const std::string &tempLeaf) {
#if RT_PLATFORM_WINDOWS
    (void)tempLeaf;
    if (access.directoryHandles.empty())
        return false;
    HANDLE tempHandle = CreateFileW(temp.wstring().c_str(),
                                    DELETE | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                    nullptr);
    if (tempHandle == INVALID_HANDLE_VALUE)
        return false;
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    bool ok = GetFileInformationByHandleEx(
                  tempHandle, FileAttributeTagInfo, &attributes, sizeof(attributes)) != 0 &&
              (attributes.FileAttributes &
               (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
              renameWorkspaceEditHandleWindows(tempHandle, access.file, false);
    CloseHandle(tempHandle);
    return ok;
#else
    (void)temp;
    return renameat(access.parentFd, tempLeaf.c_str(), access.parentFd, access.leaf.c_str()) == 0;
#endif
}

/// @brief Write a string to a newly created temporary file.
/// @details Opens @p path with exclusive-create semantics so an existing file,
///          symlink, or racing creator cannot be overwritten. The write loop
///          chunks large strings to platform API limits before flushing and
///          closing the handle.
/// @param path Temporary file path to write.
/// @param text Complete replacement file contents.
/// @return `true` when the file was written and flushed successfully.
static bool writeWorkspaceEditTemp(WorkspaceEditTargetAccess &access,
                                   const fs::path &path,
                                   const std::string &tempLeaf,
                                   const std::string &text) {
#if RT_PLATFORM_WINDOWS
    (void)tempLeaf;
    // CopyFile preserves alternate streams, compression/encryption state,
    // object metadata, and platform attributes. Rewrite only the unnamed data
    // stream so those properties survive the eventual handle-relative rename.
    if (!CopyFileW(access.file.wstring().c_str(), path.wstring().c_str(), TRUE))
        return false;
    DWORD copiedAttributes = GetFileAttributesW(path.wstring().c_str());
    if (copiedAttributes != INVALID_FILE_ATTRIBUTES &&
        (copiedAttributes & FILE_ATTRIBUTE_READONLY) != 0)
        (void)SetFileAttributesW(path.wstring().c_str(),
                                 copiedAttributes & ~FILE_ATTRIBUTE_READONLY);
    HANDLE handle = CreateFileW(path.wstring().c_str(),
                                GENERIC_WRITE | FILE_WRITE_ATTRIBUTES,
                                0,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(handle, beginning, nullptr, FILE_BEGIN) || !SetEndOfFile(handle)) {
        CloseHandle(handle);
        return false;
    }
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
    int fd = openat(access.parentFd, tempLeaf.c_str(), flags, S_IRUSR | S_IWUSR);
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
static bool flushWorkspaceEditDirectory(WorkspaceEditTargetAccess &access) {
#if RT_PLATFORM_WINDOWS
    if (access.directoryHandles.empty())
        return false;
    return FlushFileBuffers(access.directoryHandles.back()) != 0;
#else
    return access.parentFd >= 0 && fsync(access.parentFd) == 0;
#endif
}

/// Maximum extended metadata copied with one workspace target.
constexpr size_t kWorkspaceEditMaxMetadataBytes = 1024u * 1024u;

#if RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
/// @brief List extended-attribute names through the host's descriptor signature.
static ssize_t listWorkspaceEditXattrs(int fd, char *names, size_t size) {
#if RT_PLATFORM_MACOS
    return flistxattr(fd, names, size, 0);
#else
    return flistxattr(fd, names, size);
#endif
}

/// @brief Read one extended attribute through the host's descriptor signature.
static ssize_t getWorkspaceEditXattr(int fd, const char *name, void *value, size_t size) {
#if RT_PLATFORM_MACOS
    return fgetxattr(fd, name, value, size, 0, 0);
#else
    return fgetxattr(fd, name, value, size);
#endif
}

/// @brief Write one extended attribute through the host's descriptor signature.
static int setWorkspaceEditXattr(int fd, const char *name, const void *value, size_t size) {
#if RT_PLATFORM_MACOS
    return fsetxattr(fd, name, value, size, 0, 0);
#else
    return fsetxattr(fd, name, value, size, 0);
#endif
}

/// @brief Copy every bounded extended attribute to an edit staging file.
/// @details ACL, security, resource-fork, quarantine, and caller namespaces are
///          all metadata. Existing equal values are retained so a platform
///          policy label that was inherited correctly does not require a
///          privileged redundant write.
static bool copyWorkspaceEditXattrs(int sourceFd, int tempFd) {
    errno = 0;
    const ssize_t rawNameBytes = listWorkspaceEditXattrs(sourceFd, nullptr, 0);
    if (rawNameBytes < 0)
        return errno == ENOTSUP || errno == EOPNOTSUPP;
    const size_t nameBytes = static_cast<size_t>(rawNameBytes);
    if (nameBytes > kWorkspaceEditMaxMetadataBytes)
        return false;
    std::vector<char> names(nameBytes);
    if (nameBytes > 0 &&
        listWorkspaceEditXattrs(sourceFd, names.data(), names.size()) != rawNameBytes)
        return false;

    size_t copiedBytes = nameBytes;
    size_t offset = 0;
    while (offset < names.size()) {
        const char *name = names.data() + offset;
        const size_t remaining = names.size() - offset;
        const void *terminator = std::memchr(name, '\0', remaining);
        if (!terminator)
            return false;
        const size_t length = static_cast<const char *>(terminator) - name;
        if (length == 0)
            return false;
        offset += length + 1;
        const ssize_t rawValueBytes = getWorkspaceEditXattr(sourceFd, name, nullptr, 0);
        if (rawValueBytes < 0)
            return false;
        const size_t valueBytes = static_cast<size_t>(rawValueBytes);
        if (valueBytes > kWorkspaceEditMaxMetadataBytes - copiedBytes)
            return false;
        std::vector<unsigned char> value(valueBytes);
        if (valueBytes > 0 &&
            getWorkspaceEditXattr(sourceFd, name, value.data(), value.size()) != rawValueBytes)
            return false;
        const ssize_t rawExistingBytes = getWorkspaceEditXattr(tempFd, name, nullptr, 0);
        bool equal = rawExistingBytes == rawValueBytes;
        if (equal && valueBytes > 0) {
            std::vector<unsigned char> existing(valueBytes);
            equal = getWorkspaceEditXattr(tempFd, name, existing.data(), existing.size()) ==
                        rawExistingBytes &&
                    existing == value;
        }
        if (!equal && setWorkspaceEditXattr(tempFd, name, value.data(), value.size()) != 0)
            return false;
        copiedBytes += valueBytes;
    }
    return true;
}
#endif

/// @brief Preserve complete writable metadata on a staged replacement.
/// @details Windows staging begins with CopyFileW, retaining alternate streams,
///          extended/resource attributes, compression, encryption, object
///          metadata, and timestamps; this helper explicitly restores owner,
///          group, DACL, and DOS attributes after rewriting the unnamed stream.
///          POSIX copies owner/group, permission and special-mode bits, every
///          bounded xattr (including ACL/security namespaces), and platform file
///          flags through already-open descriptors. Any unsupported discovered
///          metadata fails the transaction before commit rather than stripping it.
static bool preserveWorkspaceEditMetadata(WorkspaceEditTargetAccess &access,
                                          const fs::path &temp,
                                          const std::string &tempLeaf) {
#if RT_PLATFORM_WINDOWS
    (void)tempLeaf;
    const std::wstring sourceText = access.file.wstring();
    const std::wstring tempText = temp.wstring();
    const SECURITY_INFORMATION securityInformation =
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION;
    DWORD needed = 0;
    GetFileSecurityW(sourceText.c_str(), securityInformation, nullptr, 0, &needed);
    if (needed == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        return false;
    std::vector<unsigned char> security(needed);
    if (!GetFileSecurityW(
            sourceText.c_str(), securityInformation, security.data(), needed, &needed) ||
        !SetFileSecurityW(tempText.c_str(), securityInformation, security.data()))
        return false;

    const DWORD sourceAttributes = GetFileAttributesW(sourceText.c_str());
    if (sourceAttributes == INVALID_FILE_ATTRIBUTES)
        return false;
    return SetFileAttributesW(tempText.c_str(), sourceAttributes) != 0;
#else
    (void)temp;
    int flags = O_RDWR;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int tempFd = openat(access.parentFd, tempLeaf.c_str(), flags);
    if (tempFd < 0)
        return false;
    struct stat sourceStatus{};
    struct stat tempStatus{};
    bool ok = fstat(access.fileFd, &sourceStatus) == 0 && S_ISREG(sourceStatus.st_mode) &&
              fstat(tempFd, &tempStatus) == 0 && S_ISREG(tempStatus.st_mode);
    if (ok &&
        (sourceStatus.st_uid != tempStatus.st_uid || sourceStatus.st_gid != tempStatus.st_gid))
        ok = fchown(tempFd, sourceStatus.st_uid, sourceStatus.st_gid) == 0;
    if (ok)
        ok = fchmod(tempFd, sourceStatus.st_mode & 07777) == 0;
#if RT_PLATFORM_LINUX || RT_PLATFORM_MACOS
    if (ok)
        ok = copyWorkspaceEditXattrs(access.fileFd, tempFd);
#endif
#if RT_PLATFORM_LINUX && defined(FS_IOC_GETFLAGS) && defined(FS_IOC_SETFLAGS)
    if (ok) {
        int sourceFlags = 0;
        int tempFlags = 0;
        errno = 0;
        if (ioctl(access.fileFd, FS_IOC_GETFLAGS, &sourceFlags) != 0) {
            if (errno != ENOTTY && errno != EOPNOTSUPP)
                ok = false;
        } else if (ioctl(tempFd, FS_IOC_GETFLAGS, &tempFlags) != 0) {
            if (errno != ENOTTY && errno != EOPNOTSUPP)
                ok = false;
        } else {
#ifdef FS_FL_USER_MODIFIABLE
            constexpr int userFlags = FS_FL_USER_MODIFIABLE;
#else
            constexpr int userFlags = 0x000380FF;
#endif
            int desiredFlags = (tempFlags & ~userFlags) | (sourceFlags & userFlags);
            if (desiredFlags != tempFlags && ioctl(tempFd, FS_IOC_SETFLAGS, &desiredFlags) != 0)
                ok = false;
        }
    }
#elif RT_PLATFORM_MACOS
    if (ok)
        ok = fchflags(tempFd, sourceStatus.st_flags) == 0;
#endif
    if (close(tempFd) != 0)
        ok = false;
    return ok;
#endif
}

/// @brief Remove staged temp files and restore backups after an apply failure.
/// @details Any replacement already moved into place is removed before its
///          backup is renamed back. A backup is never deleted after a failed
///          restoration: it remains beside the target as the recoverable original.
///          Rollback diagnostics are appended after the primary apply failure.
/// @param writes Pending write records accumulated for this apply attempt.
/// @param diagnostics Owning runtime sequence receiving rollback failures.
/// @return True when every staged artifact was removed or restored.
static bool removeWorkspaceEditEntry(WorkspaceEditTargetAccess &access,
                                     const std::string &fullPath,
                                     const std::string &leaf) {
#if RT_PLATFORM_WINDOWS
    (void)access;
    (void)leaf;
    if (DeleteFileW(fs::path(fullPath).wstring().c_str()) != 0)
        return true;
    const DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
#else
    (void)fullPath;
    return unlinkat(access.parentFd, leaf.c_str(), 0) == 0 || errno == ENOENT;
#endif
}

static bool restoreWorkspaceEditBackup(PendingWorkspaceWrite &write) {
    if (!write.access)
        return false;
#if RT_PLATFORM_WINDOWS
    if (write.access->directoryHandles.empty())
        return false;
    return renameWorkspaceEditHandleWindows(write.access->fileHandle, write.access->file, false);
#else
    return renameat(write.access->parentFd,
                    write.backupLeaf.c_str(),
                    write.access->parentFd,
                    write.access->leaf.c_str()) == 0;
#endif
}

static bool rollbackWorkspaceWrites(std::vector<PendingWorkspaceWrite> &writes, void *diagnostics) {
    bool complete = true;
    for (auto it = writes.rbegin(); it != writes.rend(); ++it) {
        if (it->backupCreated) {
            if (!it->access || !removeWorkspaceEditEntry(*it->access, it->file, it->access->leaf)) {
                pushDiagnostic(diagnostics,
                               "cannot remove replacement during workspace edit rollback; "
                               "original preserved at " +
                                   it->backup,
                               it->file,
                               0,
                               "edit.rollback");
                complete = false;
            } else {
                if (!restoreWorkspaceEditBackup(*it)) {
                    pushDiagnostic(diagnostics,
                                   "cannot restore workspace edit target; original preserved at " +
                                       it->backup,
                                   it->file,
                                   0,
                                   "edit.rollback");
                    complete = false;
                }
            }
        } else if (it->backupReserved) {
            // Reserved but never used: remove the empty placeholder we created
            // (VDOC-196) so a failed apply leaves no stale sidecar.
            if (!it->access || !removeWorkspaceEditEntry(*it->access, it->backup, it->backupLeaf)) {
                pushDiagnostic(diagnostics,
                               "cannot remove reserved workspace edit backup",
                               it->backup,
                               0,
                               "edit.rollback");
                complete = false;
            }
        }
        if (it->access && !removeWorkspaceEditEntry(*it->access, it->temp, it->tempLeaf)) {
            pushDiagnostic(diagnostics,
                           "cannot remove staged workspace edit file",
                           it->temp,
                           0,
                           "edit.rollback");
            complete = false;
        }
    }
    return complete;
}

/// @brief Apply a prepared workspace edit batch with best-effort rollback.
/// @details This routine consumes one validated snapshot, applies edits in
///          descending range order per file, stages every new file image into a
///          same-directory temporary file, then commits via rename. Immediately
///          before replacing each live target it re-reads the file and confirms
///          it still matches the content validated earlier (optimistic
///          concurrency), aborting the whole batch with a rollback if an
///          external write changed it during staging, so newer content is never
///          silently overwritten (VDOC-195).
///          If any backup or replacement fails, earlier replacements are
///          restored from their backups and staged temps are removed.
/// @param prepared Owned transaction whose original bytes and identities were
///                 captured together by the prepare step.
/// @return Result map containing validation fields plus `appliedFiles`.
static void *workspace_edit_apply_prepared_impl(PreparedWorkspaceEdit *prepared) {
    if (!prepared || !prepared->validation) {
        void *result = rt_map_new();
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(
            diagnostics, "prepared workspace edit is invalid", "", 0, "edit.prepared.invalid");
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("editCount"), 0);
        rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
        rt_map_set(result, rt_const_cstr("diagnostics"), diagnostics);
        releaseObject(diagnostics);
        return result;
    }

    void *result = rt_map_clone(prepared->validation);
    void *diagnostics = rt_map_get(result, rt_const_cstr("diagnostics"));
    if (prepared->consumed) {
        pushDiagnostic(diagnostics,
                       "prepared workspace edit was already consumed",
                       "",
                       0,
                       "edit.prepared.consumed");
        rt_map_set_bool(result, rt_const_cstr("success"), 0);
        rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
        return result;
    }
    prepared->consumed = true;
    if (!rt_map_get_bool(result, rt_const_cstr("success"))) {
        rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
        return result;
    }

    const std::vector<EditRecord> &records = prepared->records;
    std::unordered_map<std::string, std::string> contents = prepared->contents;
    const auto &validatedOriginals = prepared->contents;
    const auto &validatedIdentities = prepared->identities;
    const std::vector<fs::path> *transactionRoots = prepared->rooted ? &prepared->roots : nullptr;

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
        write.tempLeaf = fs::path(write.temp).filename().string();
        write.access = openWorkspaceEditTarget(file, transactionRoots, true);
        if (!write.access) {
            pushDiagnostic(diagnostics,
                           "cannot securely open edit target for commit",
                           file,
                           0,
                           "edit.version");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes, diagnostics);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        // Exclusively reserve the backup name up front so no stale artifact or
        // racing process can make the commit-time rename clobber unrelated data
        // (VDOC-196). The reserved empty file is replaced by the target during
        // the commit rename.
        if (!reserveWorkspaceEditBackup(*write.access, write.backup, write.backupLeaf)) {
            pushDiagnostic(
                diagnostics, "cannot reserve backup for edit target", file, 0, "edit.write");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes, diagnostics);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        write.backupReserved = true;
        // Publish the reserved backup to rollback ownership before creating the
        // content temp. A partial or failed temp write must clean both artifacts.
        writes.push_back(std::move(write));
        PendingWorkspaceWrite &pending = writes.back();
        if (!writeWorkspaceEditTemp(*pending.access, pending.temp, pending.tempLeaf, text)) {
            pushDiagnostic(
                diagnostics, "cannot write temporary edit target", file, 0, "edit.write");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes, diagnostics);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        if (!preserveWorkspaceEditMetadata(*pending.access, pending.temp, pending.tempLeaf)) {
            pushDiagnostic(diagnostics,
                           "cannot preserve workspace edit target metadata",
                           file,
                           0,
                           "edit.metadata");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes, diagnostics);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
    }

    int64_t applied = 0;
    for (auto &write : writes) {
        // Optimistic-concurrency recheck: confirm the live target still matches
        // the content we validated, immediately before replacing it. An editor,
        // formatter, or external process that wrote the file during staging
        // would otherwise be silently overwritten (VDOC-195).
        {
            WorkspaceEditFileIdentity currentIdentity;
            std::string current;
            int64_t modifiedSeconds = -1;
            int64_t fileSize = -1;
            const bool readCurrent = readWorkspaceEditTarget(write.file,
                                                             transactionRoots,
                                                             0,
                                                             diagnostics,
                                                             current,
                                                             currentIdentity,
                                                             modifiedSeconds,
                                                             fileSize);
            auto expected = validatedOriginals.find(write.file);
            auto expectedIdentity = validatedIdentities.find(write.file);
            if (!readCurrent || expected == validatedOriginals.end() ||
                current != expected->second || expectedIdentity == validatedIdentities.end() ||
                !workspaceEditIdentityEqual(currentIdentity, expectedIdentity->second)) {
                pushDiagnostic(diagnostics,
                               "edit target changed since validation",
                               write.file,
                               0,
                               "edit.version");
                rt_map_set_bool(result, rt_const_cstr("success"), 0);
                rollbackWorkspaceWrites(writes, diagnostics);
                rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
                return result;
            }
        }
        if (!moveWorkspaceTargetToReservedBackup(*write.access, write.backupLeaf)) {
            pushDiagnostic(diagnostics, "cannot back up edit target", write.file, 0, "edit.write");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes, diagnostics);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        write.backupCreated = true;
        if (!moveWorkspaceTempToTarget(*write.access, write.temp, write.tempLeaf)) {
            pushDiagnostic(diagnostics, "cannot replace edit target", write.file, 0, "edit.write");
            rt_map_set_bool(result, rt_const_cstr("success"), 0);
            rollbackWorkspaceWrites(writes, diagnostics);
            rt_map_set_int(result, rt_const_cstr("appliedFiles"), 0);
            return result;
        }
        flushWorkspaceEditDirectory(*write.access);
        applied++;
    }
    for (auto &write : writes) {
#if RT_PLATFORM_WINDOWS
        const bool backupRemoved =
            write.access && deleteWorkspaceEditHandleWindows(write.access->fileHandle);
#else
        const bool backupRemoved =
            write.access && unlinkat(write.access->parentFd, write.backupLeaf.c_str(), 0) == 0;
#endif
        if (!backupRemoved) {
            pushDiagnostic(diagnostics,
                           "workspace edit applied, but its backup could not be removed; "
                           "stale backup preserved at " +
                               write.backup,
                           write.file,
                           0,
                           "edit.cleanup");
        }
        if (write.access)
            flushWorkspaceEditDirectory(*write.access);
    }
    rt_map_set_int(result, rt_const_cstr("appliedFiles"), applied);
    return result;
}

/// @brief Prepare and immediately consume a compatibility edit request.
/// @details Legacy Apply entry points now perform exactly one validation read;
///          explicit Prepare callers may inspect the result before committing
///          without causing a second validation pass.
static void *workspace_edit_apply_impl(void *edits, const std::vector<fs::path> *roots) {
    PreparedWorkspaceEdit *prepared = workspace_edit_prepare_impl(edits, roots);
    void *result = workspace_edit_apply_prepared_impl(prepared);
    delete prepared;
    return result;
}

/// @brief Construct an invalid prepared transaction from owned diagnostics.
/// @param diagnostics Borrowed owning sequence retained by the result map.
/// @return Explicit transaction handle whose validation result is unsuccessful.
static PreparedWorkspaceEdit *workspace_edit_prepare_failure(void *diagnostics) {
    auto *prepared = new PreparedWorkspaceEdit();
    prepared->validation = rt_map_new();
    rt_map_set_bool(prepared->validation, rt_const_cstr("success"), 0);
    rt_map_set_int(prepared->validation, rt_const_cstr("editCount"), 0);
    rt_map_set(prepared->validation, rt_const_cstr("diagnostics"), diagnostics);
    return prepared;
}

} // namespace

extern "C" {

/// @brief Prepare an unrooted workspace edit transaction without changing files.
/// @details Reads each target once and retains canonical records, original bytes,
///          and stable identities until Apply or Destroy. The returned handle is
///          explicit-lifetime and must be destroyed by the caller.
void *rt_workspace_edit_prepare(void *edits) {
    try {
        return workspace_edit_prepare_impl(edits, nullptr);
    } catch (...) {
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit prepare failed", "", 0, "edit.exception");
        PreparedWorkspaceEdit *prepared = workspace_edit_prepare_failure(diagnostics);
        releaseObject(diagnostics);
        return prepared;
    }
}

/// @brief Prepare an edit transaction constrained to one workspace root.
void *rt_workspace_edit_prepare_in_root(void *edits, rt_string root) {
    try {
        void *diagnostics = rt_seq_new_owned();
        fs::path resolvedRoot;
        if (!workspaceEditRootFromString(root, diagnostics, resolvedRoot)) {
            PreparedWorkspaceEdit *prepared = workspace_edit_prepare_failure(diagnostics);
            releaseObject(diagnostics);
            return prepared;
        }
        releaseObject(diagnostics);
        std::vector<fs::path> roots;
        roots.push_back(std::move(resolvedRoot));
        return workspace_edit_prepare_impl(edits, &roots);
    } catch (...) {
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit prepare failed", "", 0, "edit.exception");
        PreparedWorkspaceEdit *prepared = workspace_edit_prepare_failure(diagnostics);
        releaseObject(diagnostics);
        return prepared;
    }
}

/// @brief Prepare an edit transaction constrained to multiple workspace roots.
void *rt_workspace_edit_prepare_in_roots(void *edits, void *roots) {
    try {
        void *diagnostics = rt_seq_new_owned();
        std::vector<fs::path> resolvedRoots;
        if (!workspaceEditRootsFromSequence(roots, diagnostics, resolvedRoots)) {
            PreparedWorkspaceEdit *prepared = workspace_edit_prepare_failure(diagnostics);
            releaseObject(diagnostics);
            return prepared;
        }
        releaseObject(diagnostics);
        return workspace_edit_prepare_impl(edits, &resolvedRoots);
    } catch (...) {
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(diagnostics, "workspace edit prepare failed", "", 0, "edit.exception");
        PreparedWorkspaceEdit *prepared = workspace_edit_prepare_failure(diagnostics);
        releaseObject(diagnostics);
        return prepared;
    }
}

/// @brief Return whether an explicit prepared transaction can still be applied.
int8_t rt_workspace_edit_prepared_is_valid(void *handle) {
    auto *prepared = static_cast<PreparedWorkspaceEdit *>(handle);
    return prepared && prepared->validation && !prepared->consumed &&
                   rt_map_get_bool(prepared->validation, rt_const_cstr("success"))
               ? 1
               : 0;
}

/// @brief Clone the immutable validation result for a prepared transaction.
void *rt_workspace_edit_prepared_result(void *handle) {
    auto *prepared = static_cast<PreparedWorkspaceEdit *>(handle);
    if (!prepared || !prepared->validation) {
        void *diagnostics = rt_seq_new_owned();
        pushDiagnostic(
            diagnostics, "prepared workspace edit is invalid", "", 0, "edit.prepared.invalid");
        PreparedWorkspaceEdit *failure = workspace_edit_prepare_failure(diagnostics);
        releaseObject(diagnostics);
        void *result = rt_map_clone(failure->validation);
        delete failure;
        return result;
    }
    void *result = rt_map_clone(prepared->validation);
    rt_map_set_bool(result, rt_const_cstr("consumed"), prepared->consumed ? 1 : 0);
    return result;
}

/// @brief Atomically consume and apply one prepared transaction.
void *rt_workspace_edit_prepared_apply(void *handle) {
    try {
        return workspace_edit_apply_prepared_impl(static_cast<PreparedWorkspaceEdit *>(handle));
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

/// @brief Destroy an explicit prepared transaction; NULL is accepted.
void rt_workspace_edit_prepared_destroy(void *handle) {
    delete static_cast<PreparedWorkspaceEdit *>(handle);
}

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
