//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/zia/test_zia_doc_mirror.cpp
// Purpose: Verify atomic, strict parsing and application of editor mirror deltas.
// Key invariants:
//   - Malformed batches never alter mirror text or revision.
//   - Valid JSON escapes are decoded to their UTF-8 document representation.
// Ownership/Lifetime:
//   - Tests borrow runtime strings for the duration of each bridge call.
//   - Each test closes every document mirror it creates.
// Links: src/frontends/zia/rt_zia_completion.cpp
//
//===----------------------------------------------------------------------===//
///
/// @file test_zia_doc_mirror.cpp
/// @brief Unit tests for the Zia document mirror delta path (VDOC-109).
///
/// @details Validates that rt_zia_doc_sync_delta enforces revision ordering
///          and range validity: stale/backwards revisions, out-of-range
///          coordinates, and reversed ranges are rejected (returning 0 so the
///          caller full-syncs) instead of silently diverging the mirror.
///
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include <cstring>
#include <string>

extern "C" {
typedef struct rt_string_impl *rt_string;
rt_string rt_string_from_bytes(const char *bytes, size_t len);
const char *rt_string_cstr(rt_string s);
void rt_zia_doc_sync_full(rt_string path, rt_string text, int64_t revision);
int8_t rt_zia_doc_sync_delta(rt_string path, rt_string deltas_json, int64_t end_revision);
rt_string rt_zia_doc_text(rt_string path);
void rt_zia_doc_close(rt_string path);
rt_string rt_zia_symbols_for_file(rt_string source, rt_string file_path);
int8_t rt_zia_doc_has(rt_string path);
int8_t rt_zia_service_available(void);
}

namespace {
rt_string S(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

std::string mirrorText(const char *path) {
    rt_string t = rt_zia_doc_text(S(path));
    const char *c = rt_string_cstr(t);
    return c ? std::string(c) : std::string();
}
} // namespace

TEST(ZiaDocMirror, StaleRevisionRejected) {
    const char *path = "/mirror/stale.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);

    // Revision 1 is behind the mirror's revision 10: must be rejected.
    int8_t ok = rt_zia_doc_sync_delta(
        S(path), S(R"([{"r":1,"sl":0,"sc":0,"el":0,"ec":1,"t":"X"}])"), 1);
    ASSERT_TRUE(ok == 0);
    ASSERT_TRUE(mirrorText(path) == "abc");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, OutOfRangeCoordinatesRejected) {
    const char *path = "/mirror/oob.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);

    // Line/column 99 are far past the buffer: reject instead of clamping.
    int8_t ok = rt_zia_doc_sync_delta(
        S(path), S(R"([{"r":11,"sl":99,"sc":99,"el":99,"ec":99,"t":"Y"}])"), 11);
    ASSERT_TRUE(ok == 0);
    ASSERT_TRUE(mirrorText(path) == "abc");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, ReversedRangeRejected) {
    const char *path = "/mirror/reversed.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);

    int8_t ok = rt_zia_doc_sync_delta(
        S(path), S(R"([{"r":11,"sl":0,"sc":2,"el":0,"ec":0,"t":"Z"}])"), 11);
    ASSERT_TRUE(ok == 0);
    ASSERT_TRUE(mirrorText(path) == "abc");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, ValidDeltaApplies) {
    const char *path = "/mirror/valid.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);

    int8_t ok = rt_zia_doc_sync_delta(
        S(path), S(R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"X"}])"), 11);
    ASSERT_TRUE(ok == 1);
    ASSERT_TRUE(mirrorText(path) == "Xbc");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocSymbols, OnlyActiveFileSymbols) {
    // VDOC-110: document symbols cover the active file only — no
    // runtime-registry rows and no imported/foreign declarations.
    const char *source = "module Test;\n\nclass Cat {\n}\n\nfunc start() {\n}\n";
    rt_string rows = rt_zia_symbols_for_file(S(source), S("symbols_probe.zia"));
    std::string out(rt_string_cstr(rows));

    ASSERT_TRUE(out.find("Cat\t") != std::string::npos);
    ASSERT_TRUE(out.find("start\t") != std::string::npos);
    ASSERT_TRUE(out.find("Zanna.") == std::string::npos);

    // Small payload: a registry leak would produce thousands of rows.
    size_t lines = 0;
    for (char c : out)
        if (c == '\n')
            lines++;
    ASSERT_TRUE(lines < 20);
}

TEST(ZiaDocMirror, HasDistinguishesAbsentFromEmpty) {
    // VDOC-113: Has() separates "no mirror" from "mirror with empty text".
    const char *path = "/mirror/empty.zia";
    ASSERT_TRUE(rt_zia_doc_has(S(path)) == 0);
    rt_zia_doc_sync_full(S(path), S(""), 1);
    ASSERT_TRUE(rt_zia_doc_has(S(path)) == 1);
    ASSERT_TRUE(mirrorText(path).empty());
    rt_zia_doc_close(S(path));
    ASSERT_TRUE(rt_zia_doc_has(S(path)) == 0);

    // The full service reports itself available.
    ASSERT_TRUE(rt_zia_service_available() == 1);
}

TEST(ZiaDocMirror, OversizedNumericLiteralRejected) {
    // VDOC-115: absurdly long numeric literals are malformed, not values.
    const char *path = "/mirror/hugeint.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);
    int8_t ok = rt_zia_doc_sync_delta(
        S(path),
        S(R"([{"r":11,"sl":99999999999999999999999999,"sc":0,"el":0,"ec":0,"t":"X"}])"),
        11);
    ASSERT_TRUE(ok == 0);
    ASSERT_TRUE(mirrorText(path) == "abc");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, MalformedJsonIsRejectedAtomically) {
    const char *path = "/mirror/atomic.zia";
    rt_zia_doc_sync_full(S(path), S("abcd"), 10);

    const char *cases[] = {
        R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"X"}] trailing)",
        R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"X"},])",
        R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"X",}])",
        R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"X","extra":0}])",
        R"([{"r":11,"r":12,"sl":0,"sc":0,"el":0,"ec":1,"t":"X"}])",
        R"([{"r":11,"sl":0,"sl":0,"sc":0,"el":0,"ec":1,"t":"X"}])",
        R"([{"sl":0,"sc":0,"el":0,"ec":1,"t":"X"}])",
        R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1}])",
        R"([{"r":11,"sl":-1,"sc":0,"el":0,"ec":1,"t":"X"}])",
        R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"\q"}])",
        R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"\uD800"}])",
        R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"X"},{"r":12}])",
    };
    for (const char *json : cases) {
        EXPECT_EQ(rt_zia_doc_sync_delta(S(path), S(json), 12), 0);
        EXPECT_EQ(mirrorText(path), "abcd");
    }
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, RevisionMustReachBatchEnd) {
    const char *path = "/mirror/revision-end.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);
    EXPECT_EQ(rt_zia_doc_sync_delta(
                  S(path), S(R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"X"}])"), 12),
              0);
    EXPECT_EQ(mirrorText(path), "abc");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, EmptyBatchCannotAdvanceRevision) {
    const char *path = "/mirror/empty-batch.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);
    EXPECT_EQ(rt_zia_doc_sync_delta(S(path), S("[]"), 11), 0);
    EXPECT_EQ(mirrorText(path), "abc");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, NegativeFullRevisionIsIgnored) {
    const char *path = "/mirror/negative-full.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);
    rt_zia_doc_sync_full(S(path), S("wrong"), -1);
    EXPECT_EQ(mirrorText(path), "abc");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, UnicodeEscapesDecodeAsUtf8) {
    const char *path = "/mirror/unicode.zia";
    rt_zia_doc_sync_full(S(path), S("x"), 1);
    EXPECT_EQ(
        rt_zia_doc_sync_delta(
            S(path),
            S(R"([{"r":2,"sl":0,"sc":0,"el":0,"ec":1,"t":"\u03A9 \uD83D\uDE00"}])"),
            2),
        1);
    EXPECT_EQ(mirrorText(path), "\xCE\xA9 \xF0\x9F\x98\x80");
    rt_zia_doc_close(S(path));
}

TEST(ZiaDocMirror, MultiDeltaBatchUsesUpdatedCoordinates) {
    const char *path = "/mirror/multi.zia";
    rt_zia_doc_sync_full(S(path), S("abc"), 10);
    EXPECT_EQ(
        rt_zia_doc_sync_delta(
            S(path),
            S(R"([{"r":11,"sl":0,"sc":0,"el":0,"ec":1,"t":"XY"},{"r":12,"sl":0,"sc":2,"el":0,"ec":3,"t":"Z"}])"),
            12),
        1);
    EXPECT_EQ(mirrorText(path), "XYZc");
    rt_zia_doc_close(S(path));
}

int main() {
    return zanna_test::run_all_tests();
}
