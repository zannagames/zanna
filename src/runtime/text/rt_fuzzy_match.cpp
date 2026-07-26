//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/text/rt_fuzzy_match.cpp
// Purpose: Implements greedy fuzzy-subsequence scoring, detailed match
//          metadata, and quick-open highlight ranges.
//
// Key invariants:
//   - Comparisons fold ASCII case only and report candidate byte offsets.
//   - Successful scores are clamped to zero or above; -1 denotes a miss.
//   - Matching greedily consumes the earliest candidate byte for each query
//     byte and rewards boundaries, exact case, and consecutive positions.
//   - No C++ exception is allowed to cross an exported C ABI boundary.
//
// Ownership/Lifetime:
//   - Runtime string inputs are borrowed and copied into temporary std::string
//     values before matching.
//   - Exported detailed results are fresh Maps that own their nested strings,
//     range Seq, and range Maps.
//
// Links: src/runtime/text/rt_fuzzy_match.h (public C ABI),
//        src/runtime/rt_map.h (result Maps),
//        src/runtime/rt_seq.h (highlight-range sequence)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_fuzzy_match.cpp
 * @brief Implements greedy fuzzy-subsequence ranking and highlight metadata.
 * @details Matching folds ASCII case, rewards boundaries, exact case, and
 *          consecutive bytes, penalizes gaps, and records half-open candidate
 *          byte ranges. Exported C functions contain C++ exceptions and
 *          construct caller-owned managed detail Maps transactionally.
 */

#include "rt_fuzzy_match.h"
#include "rt_ascii.h"

#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace {

/// @brief Copy a runtime string into a standard byte string.
/// @details Null strings, inaccessible buffers, and non-positive lengths are
///          normalized to an empty string. The explicit runtime length is
///          honored, so embedded NUL bytes are preserved.
/// @param s Borrowed runtime string.
/// @return Independent standard-library copy of the runtime bytes.
std::string toStd(rt_string s) {
    if (!s)
        return {};
    const char *data = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!data || len <= 0)
        return {};
    return std::string(data, static_cast<size_t>(len));
}

/// @brief Release one locally owned runtime-object reference.
/// @details Frees the object when the released reference was its last one.
///          Null pointers are ignored.
/// @param obj Runtime object reference owned by the current scope.
void releaseObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Store a copied byte string under a constant-text map key.
/// @details Temporary runtime strings for the key and value are released after
///          `rt_map_set_str` has recorded them.
/// @param map Borrowed runtime Map to update.
/// @param key NUL-terminated key text to copy.
/// @param value Byte string to copy into a runtime string value.
void mapSetStr(void *map, const char *key, const std::string &value) {
    rt_string k = rt_const_cstr(key);
    rt_string s = rt_string_from_bytes(value.data(), value.size());
    rt_map_set_str(map, k, s);
    rt_string_unref(s);
    rt_string_unref(k);
}

/// @brief Store an integer under a constant-text map key.
/// @param map Borrowed runtime Map to update.
/// @param key NUL-terminated key text to copy.
/// @param value Integer value to store.
void mapSetInt(void *map, const char *key, int64_t value) {
    rt_string k = rt_const_cstr(key);
    rt_map_set_int(map, k, value);
    rt_string_unref(k);
}

/// @brief Store a runtime Boolean under a constant-text map key.
/// @param map Borrowed runtime Map to update.
/// @param key NUL-terminated key text to copy.
/// @param value Boolean representation accepted by `rt_map_set_bool`.
void mapSetBool(void *map, const char *key, int8_t value) {
    rt_string k = rt_const_cstr(key);
    rt_map_set_bool(map, k, value);
    rt_string_unref(k);
}

/// @brief Store a runtime object under a constant-text map key.
/// @details The caller retains its own reference to @p value and may release
///          that reference after the map setter retains the object.
/// @param map Borrowed runtime Map to update.
/// @param key NUL-terminated key text to copy.
/// @param value Borrowed runtime object to store.
void mapSetObj(void *map, const char *key, void *value) {
    rt_string k = rt_const_cstr(key);
    rt_map_set(map, k, value);
    rt_string_unref(k);
}

/// @brief Determine whether a byte begins a favored token or path segment.
/// @details The first byte is always a boundary. Other boundaries follow a
///          slash, backslash, underscore, hyphen, period, or space, or occur
///          at an ASCII lower-to-upper camel-case transition.
/// @param s Candidate byte string.
/// @param i Valid byte index in @p s.
/// @return `true` when the byte at @p i begins a scoring boundary.
bool isBoundary(const std::string &s, size_t i) {
    if (i == 0)
        return true;
    char prev = s[i - 1];
    char cur = s[i];
    if (prev == '/' || prev == '\\' || prev == '_' || prev == '-' || prev == '.' || prev == ' ')
        return true;
    return rt_ascii_islower(static_cast<unsigned char>(prev)) &&
           rt_ascii_isupper(static_cast<unsigned char>(cur));
}

/// Result of one greedy, case-insensitive subsequence comparison.
struct MatchResult {
    bool matched{false};            ///< Whether every query byte was found.
    int64_t score{-1};              ///< Non-negative rank, or `-1` for a miss.
    std::vector<int64_t> positions; ///< Matched candidate byte offsets.
};

/// @brief Score a query as a case-insensitive subsequence of a candidate.
/// @details The matcher greedily selects the first available occurrence of
///          each query byte. It rewards exact case, token boundaries, and
///          consecutive matches, while penalizing gaps, an early unmatched
///          prefix, and excess candidate length. Successful scores are clamped
///          to zero so `-1` remains the unique no-match sentinel. An empty
///          query matches every candidate with score zero.
/// @param query Query bytes to locate in order.
/// @param candidate Candidate bytes to search.
/// @return Match state, clamped score, and selected candidate byte offsets.
MatchResult computeMatch(const std::string &query, const std::string &candidate) {
    MatchResult result;
    if (query.empty()) {
        result.matched = true;
        result.score = 0;
        return result;
    }
    size_t qi = 0;
    int64_t score = 0;
    int64_t last = -2;
    for (size_t ci = 0; ci < candidate.size() && qi < query.size(); ci++) {
        char qc = static_cast<char>(rt_ascii_tolower(static_cast<unsigned char>(query[qi])));
        char cc = static_cast<char>(rt_ascii_tolower(static_cast<unsigned char>(candidate[ci])));
        if (qc != cc)
            continue;
        result.positions.push_back(static_cast<int64_t>(ci));
        score += 16;
        if (query[qi] == candidate[ci])
            score += 2;
        if (isBoundary(candidate, ci))
            score += 8;
        if (static_cast<int64_t>(ci) == last + 1)
            score += 12;
        else
            score -= static_cast<int64_t>(ci) - last - 1;
        last = static_cast<int64_t>(ci);
        qi++;
    }
    if (qi != query.size())
        return result;
    score -= static_cast<int64_t>(candidate.size() - query.size()) / 4;
    score -= result.positions.empty() ? 0 : result.positions.front();
    result.matched = true;
    // Penalties are unbounded, so clamp successful matches to zero: a
    // non-negative score always means "matched" and -1 stays a reliable
    // miss sentinel (VDOC-059).
    result.score = score < 0 ? 0 : score;
    return result;
}

/// @brief Coalesce matched byte offsets into half-open highlight ranges.
/// @details Consecutive positions become one Map with integer `"start"` and
///          `"end"` members, where end is exclusive. The returned owning Seq
///          contains the maps in ascending candidate order.
/// @param positions Ordered candidate byte offsets selected by the matcher.
/// @return Newly allocated element-owning Seq of newly allocated range Maps.
void *makeRanges(const std::vector<int64_t> &positions) {
    void *ranges = rt_seq_new_owned();
    if (positions.empty())
        return ranges;
    int64_t start = positions[0];
    int64_t prev = positions[0];
    /// @brief Append one half-open highlight interval to the owning result Seq.
    /// @param s Inclusive candidate byte offset.
    /// @param e Exclusive candidate byte offset.
    auto pushRange = [&](int64_t s, int64_t e) {
        void *range = rt_map_new();
        mapSetInt(range, "start", s);
        mapSetInt(range, "end", e);
        rt_seq_push(ranges, range);
        releaseObject(range);
    };
    for (size_t i = 1; i < positions.size(); i++) {
        if (positions[i] == prev + 1) {
            prev = positions[i];
            continue;
        }
        pushRange(start, prev + 1);
        start = prev = positions[i];
    }
    pushRange(start, prev + 1);
    return ranges;
}

} // namespace

extern "C" {

/// @brief Rank a candidate using greedy case-insensitive subsequence matching.
/// @details Null inputs are normalized to empty strings. A successful match
///          returns a non-negative score and a miss returns `-1`. To prevent
///          C++ exceptions from crossing the C ABI, any exception is caught
///          and reported as the neutral score zero.
/// @param query Borrowed query string.
/// @param candidate Borrowed candidate string.
/// @return Match score, `-1` for a normal miss, or zero after an exception.
int64_t rt_fuzzy_match_score(rt_string query, rt_string candidate) {
    try {
        return computeMatch(toStd(query), toStd(candidate)).score;
    } catch (...) {
        return 0;
    }
}

/// @brief Return detailed fuzzy-match metadata for a query and candidate.
/// @details The returned Map contains Boolean `"matched"`, integer `"score"`,
///          copied `"query"` and `"candidate"` strings, and an owning
///          `"ranges"` Seq of half-open `{start, end}` byte-offset Maps.
///          A caught C++ exception produces a newly allocated fallback Map
///          with `matched=false`, score zero, empty strings, and no ranges.
/// @param query Borrowed query string; null is treated as empty.
/// @param candidate Borrowed candidate string; null is treated as empty.
/// @return Newly allocated runtime Map owned by the caller.
void *rt_fuzzy_match_match(rt_string query, rt_string candidate) {
    try {
        std::string q = toStd(query);
        std::string c = toStd(candidate);
        MatchResult m = computeMatch(q, c);
        void *result = rt_map_new();
        mapSetBool(result, "matched", m.matched ? 1 : 0);
        mapSetInt(result, "score", m.score);
        mapSetStr(result, "query", q);
        mapSetStr(result, "candidate", c);
        void *ranges = makeRanges(m.positions);
        mapSetObj(result, "ranges", ranges);
        releaseObject(ranges);
        return result;
    } catch (...) {
        void *result = rt_map_new();
        mapSetBool(result, "matched", 0);
        mapSetInt(result, "score", 0);
        mapSetStr(result, "query", "");
        mapSetStr(result, "candidate", "");
        void *ranges = rt_seq_new_owned();
        mapSetObj(result, "ranges", ranges);
        releaseObject(ranges);
        return result;
    }
}

} // extern "C"
