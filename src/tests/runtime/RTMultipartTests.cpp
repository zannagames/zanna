//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTMultipartTests.cpp
// Purpose: Validate multipart form-data builder/parser hardening.
// Key invariants:
//   - Strict parsing never publishes or retains a partial Multipart object.
//   - Serialization cleans native staging before managed allocation traps escape.
//   - Result wrappers consume the parser's initial payload reference.
// Ownership/Lifetime:
//   - New lifecycle probes release every managed value they create and compare
//     the GC tracked count before and after failure/success paths.
// Links: src/runtime/network/rt_multipart.c, docs/zannalib/network.md
//
//===----------------------------------------------------------------------===//

#include "rt_bytes.h"
#include "rt_gc.h"
#include "rt_internal.h"
#include "rt_multipart.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void test_result(const char *name, bool passed) {
    printf("  %s: %s\n", name, passed ? "PASS" : "FAIL");
    assert(passed);
}

// -- Trap interception (strict-parse tests, VDOC-146) ------------------------
static jmp_buf g_trap_jmp;
static bool g_trap_expected = false;
static const char *g_last_trap = nullptr;
static int g_alloc_fail_countdown = 0;

extern "C" void vm_trap(const char *msg) {
    g_last_trap = msg;
    if (g_trap_expected)
        longjmp(g_trap_jmp, 1);
    fprintf(stderr, "UNEXPECTED TRAP: %s\n", msg ? msg : "(null)");
    exit(1);
}

/// Expect `expr` to trap; capture the message and continue.
#define EXPECT_TRAP(expr)                                                                          \
    do {                                                                                           \
        g_trap_expected = true;                                                                    \
        g_last_trap = nullptr;                                                                     \
        if (setjmp(g_trap_jmp) == 0) {                                                             \
            expr;                                                                                  \
            assert(false && "Expected trap did not occur");                                        \
        }                                                                                          \
        g_trap_expected = false;                                                                   \
    } while (0)

/// @brief Release one caller-owned managed test value.
/// @details Handles Strings and heap objects through the shared dynamic
///          release protocol; heap objects run their finalizer at zero.
/// @param value Managed reference, or NULL.
static void release_managed(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

/// @brief Fail one selected managed runtime allocation.
/// @details Native multipart staging continues to use `malloc`; routing only
///          `rt_alloc` through this hook deterministically targets the final
///          Bytes/Multipart/Result allocation without perturbing parser input.
/// @param bytes Requested allocation size.
/// @param next Default runtime allocator.
/// @return NULL at the selected countdown, otherwise @p next's result.
static void *fail_selected_allocation(int64_t bytes, void *(*next)(int64_t)) {
    if (g_alloc_fail_countdown > 0 && --g_alloc_fail_countdown == 0)
        return nullptr;
    return next(bytes);
}

/// @brief Exercise stable identity and atomic cleanup contracts before legacy probes leak literals.
/// @details Warms boundary generation, verifies wrong-class receivers cannot be
///          reinterpreted, injects a final Bytes allocation failure after Build
///          has populated native staging, checks malformed parsing releases its
///          allocated Multipart, and proves both ParseResult variants return to
///          the original tracked-object count after release.
static void test_identity_and_trap_safe_lifecycle() {
    printf("Testing Multipart identity and trap-safe lifecycle:\n");

    void *warmup = rt_multipart_new();
    release_managed(warmup);

    void *wrong = rt_seq_new();
    EXPECT_TRAP(rt_multipart_build(wrong));
    test_result("Build rejects a wrong-class managed receiver",
                g_last_trap && strstr(g_last_trap, "invalid receiver") != nullptr);
    rt_string wrong_name = rt_const_cstr("x");
    test_result("Predicates safely reject a wrong-class receiver",
                rt_multipart_has_field(wrong, wrong_name) == 0);
    rt_string_unref(wrong_name);
    release_managed(wrong);

    void *builder = rt_multipart_new();
    rt_string field_name = rt_const_cstr("field");
    rt_string field_value = rt_const_cstr("value");
    rt_multipart_add_field(builder, field_name, field_value);
    const int64_t build_baseline = rt_gc_tracked_count();
    volatile bool build_trapped = false;
    g_trap_expected = true;
    g_alloc_fail_countdown = 1;
    if (setjmp(g_trap_jmp) == 0) {
        rt_set_alloc_hook(fail_selected_allocation);
        (void)rt_multipart_build(builder);
        rt_set_alloc_hook(nullptr);
    } else {
        rt_set_alloc_hook(nullptr);
        build_trapped = true;
    }
    g_trap_expected = false;
    test_result("Build releases native staging after Bytes allocation trap",
                build_trapped && g_alloc_fail_countdown == 0 &&
                    rt_gc_tracked_count() == build_baseline);

    rt_string content_type = rt_const_cstr("multipart/form-data; boundary=abc");
    rt_string malformed_text =
        rt_const_cstr("--abc\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\ndata");
    void *malformed_body = rt_bytes_from_str(malformed_text);
    const int64_t parse_baseline = rt_gc_tracked_count();
    EXPECT_TRAP(rt_multipart_parse(content_type, malformed_body));
    test_result("Malformed parse releases its partial Multipart",
                rt_gc_tracked_count() == parse_baseline);

    rt_string valid_text = rt_const_cstr(
        "--abc\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nok\r\n--abc--\r\n");
    void *valid_body = rt_bytes_from_str(valid_text);
    const int64_t result_baseline = rt_gc_tracked_count();
    void *ok_result = rt_multipart_parse_result(content_type, valid_body);
    test_result("ParseResult success owns a complete Multipart",
                ok_result && rt_result_is_ok(ok_result) == 1 &&
                    rt_multipart_count(rt_result_unwrap(ok_result)) == 1);
    release_managed(ok_result);
    test_result("Releasing ParseResult drops its retained Multipart",
                rt_gc_tracked_count() == result_baseline);

    void *error_result = rt_multipart_parse_result(content_type, malformed_body);
    test_result("ParseResult converts malformed input to Err",
                error_result && rt_result_is_err(error_result) == 1);
    release_managed(error_result);
    test_result("Releasing error ParseResult restores tracked baseline",
                rt_gc_tracked_count() == result_baseline);

    release_managed(valid_body);
    release_managed(malformed_body);
    rt_string_unref(valid_text);
    rt_string_unref(malformed_text);
    rt_string_unref(content_type);
    rt_string_unref(field_value);
    rt_string_unref(field_name);
    release_managed(builder);
}

static void test_builder_escapes_header_params() {
    printf("Testing Multipart builder header escaping:\n");

    void *mp = rt_multipart_new();
    void *file_bytes = rt_bytes_from_str(rt_const_cstr("payload"));
    rt_multipart_add_field(mp, rt_const_cstr("alpha\"\r\nX-Evil: 1"), rt_const_cstr("value"));
    rt_multipart_add_file(
        mp, rt_const_cstr("upload"), rt_const_cstr("evil\"\r\nX-Injected: 1.txt"), file_bytes);

    void *body = rt_multipart_build(mp);
    rt_string text = rt_bytes_to_str(body);
    const char *cstr = rt_string_cstr(text);

    test_result("Field name quote is escaped",
                strstr(cstr, "name=\"alpha\\\"  X-Evil: 1\"") != nullptr);
    test_result("Filename quote is escaped",
                strstr(cstr, "filename=\"evil\\\"  X-Injected: 1.txt\"") != nullptr);
    test_result("Field name did not inject a new header", strstr(cstr, "\r\nX-Evil: 1") == nullptr);
    test_result("Filename did not inject a new header",
                strstr(cstr, "\r\nX-Injected: 1.txt") == nullptr);

    rt_string_unref(text);
}

static void test_parser_handles_quoted_and_escaped_params() {
    printf("\nTesting Multipart parser quoted parameter handling:\n");

    const char *raw_body =
        "--abc123\r\n"
        "Content-Disposition: form-data; name=\"field\\\"name\"\r\n"
        "\r\n"
        "value\r\n"
        "--abc123\r\n"
        "Content-Disposition: form-data; name=\"file\\\"field\"; filename=\"a\\\"b.txt\"\r\n"
        "Content-Type: application/octet-stream\r\n"
        "\r\n"
        "xyz\r\n"
        "--abc123--\r\n";

    void *body = rt_bytes_from_str(rt_const_cstr(raw_body));
    void *mp = rt_multipart_parse(
        rt_const_cstr("multipart/form-data; charset=utf-8; BOUNDARY=\"abc123\""), body);

    test_result("Multipart count matches", rt_multipart_count(mp) == 2);

    rt_string field = rt_multipart_get_field(mp, rt_const_cstr("field\"name"));
    test_result("Escaped quoted field name parsed", strcmp(rt_string_cstr(field), "value") == 0);
    rt_string_unref(field);

    void *file = rt_multipart_get_file(mp, rt_const_cstr("file\"field"));
    rt_string file_text = rt_bytes_to_str(file);
    test_result("Escaped quoted file field name parsed",
                strcmp(rt_string_cstr(file_text), "xyz") == 0);
    rt_string_unref(file_text);
}

static void test_parser_handles_bare_token_params() {
    printf("\nTesting Multipart parser bare token parameters:\n");

    const char *raw_body = "--plain\r\n"
                           "Content-Disposition: form-data; name=plain\r\n"
                           "\r\n"
                           "ok\r\n"
                           "--plain--\r\n";

    void *body = rt_bytes_from_str(rt_const_cstr(raw_body));
    void *mp = rt_multipart_parse(rt_const_cstr("multipart/form-data; boundary=plain"), body);

    test_result("Multipart count with bare token name matches", rt_multipart_count(mp) == 1);
    rt_string field = rt_multipart_get_field(mp, rt_const_cstr("plain"));
    test_result("Bare token field name parsed", strcmp(rt_string_cstr(field), "ok") == 0);
    rt_string_unref(field);
}

static void test_parser_handles_direct_boundary_param() {
    printf("\nTesting Multipart parser direct boundary parameter:\n");

    const char *raw_body = "--plain\r\n"
                           "Content-Disposition: form-data; name=plain\r\n"
                           "\r\n"
                           "ok\r\n"
                           "--plain--\r\n";

    void *body = rt_bytes_from_str(rt_const_cstr(raw_body));
    void *mp = rt_multipart_parse(rt_const_cstr("boundary=plain"), body);

    test_result("Direct boundary parameter parsed", rt_multipart_count(mp) == 1);
    rt_string field = rt_multipart_get_field(mp, rt_const_cstr("plain"));
    test_result("Direct boundary field parsed", strcmp(rt_string_cstr(field), "ok") == 0);
    rt_string_unref(field);
}

/// @brief Strict atomic parsing (VDOC-146): malformed, truncated, and invalid
///        inputs trap instead of yielding empty or partial objects, and
///        ParseResult/HasField/HasFile disambiguate the remaining cases.
static void test_parser_strict_and_distinguishable() {
    printf("\nTesting Multipart strict parsing and presence checks:\n");

    // Unnamed part is malformed form-data — the whole parse fails.
    const char *unnamed_body = "--abc\r\n"
                               "Content-Disposition: form-data; filename=\"x.txt\"\r\n"
                               "\r\n"
                               "ignored\r\n"
                               "--abc--\r\n";
    void *body = rt_bytes_from_str(rt_const_cstr(unnamed_body));
    EXPECT_TRAP(rt_multipart_parse(rt_const_cstr("multipart/form-data; boundary=abc"), body));
    test_result("Unnamed part traps as malformed",
                g_last_trap && strstr(g_last_trap, "malformed") != nullptr);

    // Truncated body (no closing boundary) is rejected atomically.
    const char *truncated_body = "--abc\r\n"
                                 "Content-Disposition: form-data; name=\"a\"\r\n"
                                 "\r\n"
                                 "prefix-data-without-terminator";
    body = rt_bytes_from_str(rt_const_cstr(truncated_body));
    EXPECT_TRAP(rt_multipart_parse(rt_const_cstr("multipart/form-data; boundary=abc"), body));
    test_result("Truncated body traps", g_last_trap && strstr(g_last_trap, "truncated") != nullptr);

    // Content-Type without a boundary parameter is rejected.
    body = rt_bytes_from_str(rt_const_cstr("--abc--\r\n"));
    EXPECT_TRAP(rt_multipart_parse(rt_const_cstr("multipart/form-data"), body));
    test_result("Missing boundary parameter traps",
                g_last_trap && strstr(g_last_trap, "boundary") != nullptr);

    // ParseResult converts the trap into Result.ErrStr...
    body = rt_bytes_from_str(rt_const_cstr("no delimiters here"));
    void *res = rt_multipart_parse_result(rt_const_cstr("multipart/form-data; boundary=abc"), body);
    test_result("ParseResult reports rejected input as Err", rt_result_is_err(res) == 1);

    // ...and wraps a valid parse in Result.Ok.
    const char *good_body = "--abc\r\n"
                            "Content-Disposition: form-data; name=\"empty\"\r\n"
                            "\r\n"
                            "\r\n"
                            "--abc--\r\n";
    body = rt_bytes_from_str(rt_const_cstr(good_body));
    res = rt_multipart_parse_result(rt_const_cstr("multipart/form-data; boundary=abc"), body);
    test_result("ParseResult wraps a valid parse in Ok", rt_result_is_ok(res) == 1);
    void *mp = rt_result_unwrap(res);

    // Presence checks distinguish present-but-empty from missing.
    test_result("HasField sees the present empty field",
                rt_multipart_has_field(mp, rt_const_cstr("empty")) == 1);
    test_result("HasField rejects a missing name",
                rt_multipart_has_field(mp, rt_const_cstr("missing")) == 0);
    test_result("HasFile distinguishes fields from files",
                rt_multipart_has_file(mp, rt_const_cstr("empty")) == 0);
    rt_string empty_val = rt_multipart_get_field(mp, rt_const_cstr("empty"));
    test_result("Present empty field still reads as empty string",
                rt_string_cstr(empty_val)[0] == '\0');
    rt_string_unref(empty_val);
}

static void test_builder_preserves_empty_parts_and_final_boundary() {
    printf("\nTesting Multipart empty parts and terminator:\n");

    void *mp = rt_multipart_new();
    void *empty_file = rt_bytes_new(0);
    rt_multipart_add_field(mp, rt_const_cstr("empty"), rt_const_cstr(""));
    rt_multipart_add_file(mp, rt_const_cstr("upload"), rt_const_cstr("empty.bin"), empty_file);

    rt_string content_type = rt_multipart_content_type(mp);
    void *body = rt_multipart_build(mp);
    rt_string text = rt_bytes_to_str(body);
    const char *ct = rt_string_cstr(content_type);
    const char *cstr = rt_string_cstr(text);
    const char *boundary = ct ? strstr(ct, "boundary=") : nullptr;
    char final_boundary[160];
    bool final_ok = false;
    if (boundary && cstr) {
        boundary += strlen("boundary=");
        snprintf(final_boundary, sizeof(final_boundary), "--%s--\r\n", boundary);
        int64_t text_len = rt_str_len(text);
        size_t final_len = strlen(final_boundary);
        final_ok = text_len >= (int64_t)final_len &&
                   memcmp(cstr + text_len - (int64_t)final_len, final_boundary, final_len) == 0;
    }
    test_result("Multipart body ends with final boundary", final_ok);

    void *parsed = rt_multipart_parse(content_type, body);
    test_result("Multipart parser keeps empty field and file", rt_multipart_count(parsed) == 2);
    rt_string field = rt_multipart_get_field(parsed, rt_const_cstr("empty"));
    test_result("Empty field value round-trips", rt_str_len(field) == 0);
    rt_string_unref(field);
    void *file = rt_multipart_get_file(parsed, rt_const_cstr("upload"));
    test_result("Empty file value round-trips", rt_bytes_len(file) == 0);

    rt_string_unref(text);
    rt_string_unref(content_type);
}

static void test_field_value_preserves_embedded_nul() {
    printf("\nTesting Multipart embedded-NUL field values:\n");

    const char value_bytes[] = {'a', '\0', 'b'};
    void *mp = rt_multipart_new();
    rt_string value = rt_string_from_bytes(value_bytes, sizeof(value_bytes));
    rt_multipart_add_field(mp, rt_const_cstr("field"), value);

    rt_string content_type = rt_multipart_content_type(mp);
    void *body = rt_multipart_build(mp);
    void *parsed = rt_multipart_parse(content_type, body);
    rt_string field = rt_multipart_get_field(parsed, rt_const_cstr("field"));

    test_result("Embedded-NUL field value round-trips",
                rt_str_len(field) == (int64_t)sizeof(value_bytes) &&
                    memcmp(rt_string_cstr(field), value_bytes, sizeof(value_bytes)) == 0);

    rt_string_unref(field);
    rt_string_unref(content_type);
    rt_string_unref(value);
}

int main() {
    printf("=== RT Multipart Tests ===\n\n");

    test_identity_and_trap_safe_lifecycle();
    test_builder_escapes_header_params();
    test_parser_handles_quoted_and_escaped_params();
    test_parser_handles_bare_token_params();
    test_parser_handles_direct_boundary_param();
    test_parser_strict_and_distinguishable();
    test_builder_preserves_empty_parts_and_final_boundary();
    test_field_value_preserves_embedded_nul();

    printf("\nAll Multipart tests passed.\n");
    return 0;
}
