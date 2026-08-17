//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/runtime/RTZiaCompletionStubTests.cpp
// Purpose: Verify the weak Zia completion bridge stubs report unavailable
//          interactive editor tooling while diagnostic checks stay quiet.
//
//===----------------------------------------------------------------------===//

#include "rt_map.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <cstring>

extern "C" {
rt_string rt_zia_complete(rt_string source, int64_t line, int64_t col);
rt_string rt_zia_complete_for_file(rt_string source,
                                   rt_string file_path,
                                   int64_t line,
                                   int64_t col);
rt_string rt_zia_signature_help(rt_string source, int64_t line, int64_t col);
rt_string rt_zia_signature_help_for_file(rt_string source,
                                         rt_string file_path,
                                         int64_t line,
                                         int64_t col);
rt_string rt_zia_check(rt_string source);
rt_string rt_zia_check_for_file(rt_string source, rt_string file_path);
void *rt_zia_toolchain_check(rt_string source);
void *rt_zia_toolchain_check_for_file(rt_string source, rt_string file_path);
void *rt_zia_toolchain_compile(rt_string source);
void *rt_zia_toolchain_compile_for_file(rt_string source, rt_string file_path);
rt_string rt_zia_hover(rt_string source, int64_t line, int64_t col);
rt_string rt_zia_hover_for_file(rt_string source, rt_string file_path, int64_t line, int64_t col);
rt_string rt_zia_symbols(rt_string source);
rt_string rt_zia_symbols_for_file(rt_string source, rt_string file_path);
void *rt_zia_completion_begin_analysis_for_file(rt_string source, rt_string file_path);
void *rt_zia_semantic_job_error_option(void *handle);
void rt_zia_completion_clear_cache(void);
void *rt_zia_completion_items(rt_string source, int64_t line, int64_t col);
void *rt_zia_signature_info(rt_string source, int64_t line, int64_t col);
int64_t rt_map_get_int_or(void *map, rt_string key, int64_t fallback);
void *rt_map_get(void *map, rt_string key);
int8_t rt_map_has(void *map, rt_string key);
int8_t rt_zia_service_available(void);
int8_t rt_zia_doc_has(rt_string path);
}

static void expect_contains(rt_string value, const char *needle) {
    assert(value != nullptr);
    const char *text = rt_string_cstr(value);
    assert(text != nullptr);
    assert(std::strstr(text, needle) != nullptr);
    rt_string_unref(value);
}

static void expect_empty(rt_string value) {
    assert(value != nullptr);
    assert(rt_str_len(value) == 0);
    const char *text = rt_string_cstr(value);
    assert(text != nullptr);
    assert(text[0] == '\0');
    rt_string_unref(value);
}

static void release_object(void *value) {
    if (value && rt_obj_release_check0(value))
        rt_obj_free(value);
}

static rt_string key(const char *text) {
    return rt_string_from_bytes(text, std::strlen(text));
}

static void expect_empty_seq(void *value) {
    assert(value != nullptr);
    assert(rt_seq_len(value) == 0);
    release_object(value);
}

static void expect_compile_unavailable_result(void *value) {
    assert(value != nullptr);

    rt_string success_key = key("success");
    assert(rt_map_get_bool(value, success_key) == 0);
    rt_string_unref(success_key);

    rt_string diagnostics_key = key("diagnostics");
    void *diagnostics = rt_map_get(value, diagnostics_key);
    rt_string_unref(diagnostics_key);
    assert(diagnostics != nullptr);
    assert(rt_seq_len(diagnostics) == 0);

    rt_string il_key = key("il");
    rt_string il = rt_map_get_str(value, il_key);
    rt_string_unref(il_key);
    expect_empty(il);

    rt_string output_path_key = key("outputPath");
    rt_string output_path = rt_map_get_str(value, output_path_key);
    rt_string_unref(output_path_key);
    expect_empty(output_path);

    release_object(value);
}

static void test_stub_schema_parity() {
    // VDOC-112: stub payloads carry every full-service schema field.
    rt_string src = rt_string_from_bytes("module X;", 9);

    void *items = rt_zia_completion_items(src, 1, 0);
    assert(rt_seq_len(items) == 1);
    void *item = rt_seq_get(items, 0);
    rt_string cursor_key = rt_string_from_bytes("cursorOffset", 12);
    assert(rt_map_has(item, cursor_key) == 1);
    assert(rt_map_get_int_or(item, cursor_key, 0) == -1);
    rt_string_unref(cursor_key);

    void *sig = rt_zia_signature_info(src, 1, 0);
    rt_string ov_key = rt_string_from_bytes("overloads", 9);
    assert(rt_map_has(sig, ov_key) == 1);
    void *overloads = rt_map_get(sig, ov_key);
    assert(overloads != nullptr && rt_seq_len(overloads) == 0);
    rt_string_unref(ov_key);
    rt_string_unref(src);
}

static void test_stub_availability_probe() {
    // VDOC-113: the weak bridge is distinguishable from a clean analysis.
    assert(rt_zia_service_available() == 0);
    rt_string p = rt_string_from_bytes("any.zia", 7);
    assert(rt_zia_doc_has(p) == 0);
    rt_string_unref(p);
}

int main() {
    test_stub_schema_parity();
    test_stub_availability_probe();
    rt_string source = rt_string_from_bytes("module app;\n", 12);
    rt_string path = rt_string_from_bytes("app.zia", 7);

    expect_contains(rt_zia_complete(source, 1, 0), "Zia completion unavailable\t\t8\t");
    expect_contains(rt_zia_complete_for_file(source, path, 1, 0),
                    "Zia completion unavailable\t\t8\t");
    expect_contains(rt_zia_signature_help(source, 1, 0), "link fe_zia");
    expect_contains(rt_zia_signature_help_for_file(source, path, 1, 0), "link fe_zia");
    expect_empty(rt_zia_check(source));
    expect_empty(rt_zia_check_for_file(source, path));
    expect_empty_seq(rt_zia_toolchain_check(source));
    expect_empty_seq(rt_zia_toolchain_check_for_file(source, path));
    expect_compile_unavailable_result(rt_zia_toolchain_compile(source));
    expect_compile_unavailable_result(rt_zia_toolchain_compile_for_file(source, path));
    expect_contains(rt_zia_hover(source, 1, 0), "link fe_zia");
    expect_contains(rt_zia_hover_for_file(source, path, 1, 0), "link fe_zia");
    expect_contains(rt_zia_symbols(source), "Zia completion unavailable\tstatus\t");
    expect_contains(rt_zia_symbols_for_file(source, path), "Zia completion unavailable\tstatus\t");
    assert(rt_zia_completion_begin_analysis_for_file(source, path) == nullptr);
    void *error_option = rt_zia_semantic_job_error_option(nullptr);
    assert(error_option != nullptr);
    assert(rt_option_is_none(error_option) == 1);
    release_object(error_option);

    rt_zia_completion_clear_cache();
    rt_string_unref(path);
    rt_string_unref(source);
    return 0;
}
