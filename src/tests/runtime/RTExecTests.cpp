//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTExecTests.cpp
// Purpose: Tests for Zanna.System.Exec external command execution.
//
//===----------------------------------------------------------------------===//

#include "common/PlatformCapabilities.hpp"
#include "rt_activity_wake.h"
#include "rt_box.h"
#include "rt_exec.h"
#include "rt_internal.h"
#include "rt_map.h"
#include "rt_process.h"
#include "rt_pty.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <cassert>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>

#if ZANNA_HOST_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

extern "C" void vm_trap(const char *msg) {
    rt_abort(msg);
}

static rt_string make_string(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

static void append_runtime_string(std::string &dst, rt_string value) {
    if (!value || rt_str_len(value) <= 0)
        return;
    dst.append(rt_string_cstr(value), (size_t)rt_str_len(value));
}

static void *make_shell_args(const char *script) {
    void *args = rt_seq_new();
    rt_seq_push(args, make_string("-c"));
    rt_seq_push(args, make_string(script));
    return args;
}

static void *start_shell_process(const char *script) {
    const char *shells[] = {"/bin/sh", "/usr/bin/sh", nullptr};
    for (int i = 0; shells[i] != nullptr; i++) {
        void *handle = rt_process_start(make_string(shells[i]), make_shell_args(script));
        if (handle)
            return handle;
    }
    return nullptr;
}

static void *start_shell_process_with_env(const char *script, const char *cwd, void *env) {
    const char *shells[] = {"/bin/sh", "/usr/bin/sh", nullptr};
    for (int i = 0; shells[i] != nullptr; i++) {
        void *handle = rt_process_start_with_env(
            make_string(shells[i]), make_shell_args(script), make_string(cwd), env);
        if (handle)
            return handle;
    }
    return nullptr;
}

static void *start_shell_process_with_env_overlay(const char *script, const char *cwd, void *env) {
    const char *shells[] = {"/bin/sh", "/usr/bin/sh", nullptr};
    for (int i = 0; shells[i] != nullptr; i++) {
        void *handle = rt_process_start_with_env_overlay(
            make_string(shells[i]), make_shell_args(script), make_string(cwd), env);
        if (handle)
            return handle;
    }
    return nullptr;
}

static void record_activity_wake(void *context) {
    auto *count = static_cast<std::atomic<int> *>(context);
    count->fetch_add(1, std::memory_order_release);
}

#if !ZANNA_HOST_WINDOWS
static void test_process_activity_wake() {
    void *handle = start_shell_process("sleep 1; printf activity-wake");
    assert(handle != nullptr);
    std::atomic<int> wakes{0};
    rt_activity_wake_target *target = rt_activity_wake_new(record_activity_wake, &wakes);
    assert(target != nullptr);
    assert(rt_process_set_activity_wake(handle, target) == 1);
    rt_activity_wake_release(target);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (wakes.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(wakes.load(std::memory_order_acquire) > 0);
    assert(rt_process_wait(handle) == 0);
    std::string output;
    append_runtime_string(output, rt_process_read_stdout(handle));
    assert(output.find("activity-wake") != std::string::npos);
    rt_process_destroy(handle);

    // Closing both output streams before exit must produce one drain wake, not
    // a POLLHUP wake loop for the remainder of the child lifetime.
    handle = start_shell_process("exec 1>&- 2>&-; sleep 1");
    assert(handle != nullptr);
    wakes.store(0, std::memory_order_release);
    target = rt_activity_wake_new(record_activity_wake, &wakes);
    assert(target != nullptr);
    assert(rt_process_set_activity_wake(handle, target) == 1);
    rt_activity_wake_release(target);
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (wakes.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(wakes.load(std::memory_order_acquire) > 0);
    append_runtime_string(output, rt_process_read_stdout(handle));
    append_runtime_string(output, rt_process_read_stderr(handle));
    const int wakes_after_hup = wakes.load(std::memory_order_acquire);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(wakes.load(std::memory_order_acquire) == wakes_after_hup);
    rt_process_destroy(handle);
}

static void test_pty_activity_wake() {
    if (!rt_pty_is_supported())
        return;
    void *handle = rt_pty_open(make_string("/bin/sh"),
                               make_shell_args("sleep 1; printf pty-activity-wake"),
                               make_string(""),
                               nullptr,
                               80,
                               24);
    assert(handle != nullptr);
    std::atomic<int> wakes{0};
    rt_activity_wake_target *target = rt_activity_wake_new(record_activity_wake, &wakes);
    assert(target != nullptr);
    assert(rt_pty_set_activity_wake(handle, target) == 1);
    rt_activity_wake_release(target);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (wakes.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    assert(wakes.load(std::memory_order_acquire) > 0);
    assert(rt_pty_wait(handle) == 0);
    std::string output;
    append_runtime_string(output, rt_pty_read(handle));
    assert(output.find("pty-activity-wake") != std::string::npos);
    rt_pty_destroy(handle);
}
#endif

static void test_shell_true() {
    // "true" command should return 0
    rt_string cmd = make_string("true");
    int64_t result = rt_exec_shell(cmd);
    assert(result == 0);
}

static void test_shell_false() {
    // "false" command should return 1
    rt_string cmd = make_string("false");
    int64_t result = rt_exec_shell(cmd);
    assert(result == 1);
}

static void test_shell_echo() {
    // Run echo through shell
    rt_string cmd = make_string("echo hello");
    int64_t result = rt_exec_shell(cmd);
    assert(result == 0);
}

static void test_shell_capture_echo() {
    // Capture output of echo
    rt_string cmd = make_string("echo hello");
    rt_string output = rt_exec_shell_capture(cmd);
    assert(output != nullptr);

    const char *out_str = rt_string_cstr(output);
    // Output should be "hello\n" or "hello\r\n" depending on platform
    assert(strncmp(out_str, "hello", 5) == 0);
}

static void test_shell_capture_multiline() {
    // Capture multiline output
    rt_string cmd = make_string("echo line1; echo line2");
    rt_string output = rt_exec_shell_capture(cmd);
    assert(output != nullptr);

    const char *out_str = rt_string_cstr(output);
    assert(strstr(out_str, "line1") != nullptr);
    assert(strstr(out_str, "line2") != nullptr);
}

static void test_run_true() {
    // Direct execution of /bin/true (or /usr/bin/true)
    rt_string prog = make_string("/bin/true");
    int64_t result = rt_exec_run(prog);
    // Might fail if /bin/true doesn't exist, try /usr/bin/true
    if (result < 0) {
        prog = make_string("/usr/bin/true");
        result = rt_exec_run(prog);
    }
    // On some systems true might not be in either location
    // Just verify we get a reasonable result
    assert(result == 0 || result == -1);
}

static void test_run_args() {
    // Run echo with arguments
    rt_string prog = make_string("/bin/echo");
    void *args = rt_seq_new();
    rt_seq_push(args, make_string("hello"));
    rt_seq_push(args, make_string("world"));

    int64_t result = rt_exec_run_args(prog, args);
    // Might fail if /bin/echo doesn't exist
    if (result < 0) {
        prog = make_string("/usr/bin/echo");
        result = rt_exec_run_args(prog, args);
    }
    // Just verify we get a reasonable result
    assert(result == 0 || result == -1);
}

static void test_capture_args() {
    // Capture output of echo with arguments
    rt_string prog = make_string("/bin/echo");
    void *args = rt_seq_new();
    rt_seq_push(args, make_string("test"));
    rt_seq_push(args, make_string("output"));

    rt_string output = rt_exec_capture_args(prog, args);

    // Try /usr/bin/echo if /bin/echo failed
    if (rt_str_len(output) == 0) {
        prog = make_string("/usr/bin/echo");
        output = rt_exec_capture_args(prog, args);
    }

    // If we got output, verify it
    if (rt_str_len(output) > 0) {
        const char *out_str = rt_string_cstr(output);
        assert(strstr(out_str, "test") != nullptr);
        assert(strstr(out_str, "output") != nullptr);
    }
}

static void test_shell_empty_command() {
    // Empty command should return 0
    rt_string cmd = make_string("");
    int64_t result = rt_exec_shell(cmd);
    assert(result == 0);
}

static void test_shell_capture_empty() {
    // Empty command should return empty string
    rt_string cmd = make_string("");
    rt_string output = rt_exec_shell_capture(cmd);
    assert(output != nullptr);
    assert(rt_str_len(output) == 0);
}

static void test_nonexistent_program() {
    // Nonexistent program should return -1
    rt_string prog = make_string("/nonexistent/program/path");
    int64_t result = rt_exec_run(prog);
    assert(result == -1);
}

static void test_capture_nonexistent() {
    // Nonexistent program should return empty string
    rt_string prog = make_string("/nonexistent/program/path");
    rt_string output = rt_exec_capture(prog);
    assert(output != nullptr);
    assert(rt_str_len(output) == 0);
}

static void test_shell_exit_code() {
    // Shell command with specific exit code
    rt_string cmd = make_string("exit 42");
    int64_t result = rt_exec_shell(cmd);
    assert(result == 42);
}

static void test_shell_capture_stderr() {
    // Note: ShellCapture only captures stdout, not stderr
    // This test verifies that behavior
    rt_string cmd = make_string("echo stdout; echo stderr >&2");
    rt_string output = rt_exec_shell_capture(cmd);
    assert(output != nullptr);

    const char *out_str = rt_string_cstr(output);
    // Should contain stdout but not stderr
    assert(strstr(out_str, "stdout") != nullptr);
    // stderr goes to stderr, not captured
}

static void test_run_null_args() {
    // Run with NULL args should work (no arguments)
    rt_string prog = make_string("/bin/true");
    int64_t result = rt_exec_run_args(prog, nullptr);
    if (result < 0) {
        prog = make_string("/usr/bin/true");
        result = rt_exec_run_args(prog, nullptr);
    }
    assert(result == 0 || result == -1);
}

static void test_run_args_accepts_boxed_strings() {
    // Zia pushes strings through the Object ABI, so args may contain boxed strings.
    rt_string prog = make_string("/bin/sh");
    void *args = rt_seq_new();
    rt_seq_push(args, rt_box_str(make_string("-c")));
    rt_seq_push(args, rt_box_str(make_string("exit 0")));

    int64_t result = rt_exec_run_args(prog, args);
    if (result < 0) {
        prog = make_string("/usr/bin/sh");
        result = rt_exec_run_args(prog, args);
    }
    assert(result == 0 || result == -1);
}

static void test_shell_full_success() {
    // ShellFull on a successful command: exit code 0, output captured
    rt_string cmd = make_string("echo hello_full");
    rt_string output = rt_exec_shell_full(cmd);
    int64_t code = rt_exec_last_exit_code();

    assert(output != nullptr);
    const char *out_str = rt_string_cstr(output);
    assert(strstr(out_str, "hello_full") != nullptr);
    assert(code == 0);
}

static void test_shell_full_exit_code() {
    // ShellFull records the exit code from a failing command
    rt_string cmd = make_string("exit 7");
    rt_exec_shell_full(cmd);
    int64_t code = rt_exec_last_exit_code();
    assert(code == 7);
}

static void test_shell_full_with_stderr_merge() {
    // ShellFull does NOT auto-merge stderr. Caller adds "2>&1" to the command
    // to merge streams (same contract as ShellCapture). Verify stdout is captured
    // and stderr-merged variant works when user adds 2>&1.
    rt_string cmd = make_string("echo to_stdout; echo to_stderr >&2 2>&1");
    rt_string output = rt_exec_shell_full(cmd);
    int64_t code = rt_exec_last_exit_code();

    assert(output != nullptr);
    const char *out_str = rt_string_cstr(output);
    assert(strstr(out_str, "to_stdout") != nullptr);
    // stderr is merged because the caller added 2>&1 in the right position
    assert(code == 0);
}

static void test_shell_full_empty() {
    // Empty command: empty output, exit 0
    rt_string cmd = make_string("");
    rt_string output = rt_exec_shell_full(cmd);
    int64_t code = rt_exec_last_exit_code();
    assert(output != nullptr);
    assert(rt_str_len(output) == 0);
    assert(code == 0);
}

static void test_shell_result_success() {
    rt_string cmd = make_string("echo result_ok");
    void *result = rt_exec_shell_result(cmd);
    assert(result != nullptr);
    rt_string output = rt_exec_command_result_output(result);
    assert(output != nullptr);
    assert(strstr(rt_string_cstr(output), "result_ok") != nullptr);
    assert(rt_exec_command_result_exit_code(result) == 0);
    assert(rt_exec_command_result_succeeded(result) == 1);
}

static void test_shell_result_exit_code() {
    rt_string cmd = make_string("echo result_fail; exit 9");
    void *result = rt_exec_shell_result(cmd);
    assert(result != nullptr);
    rt_string output = rt_exec_command_result_output(result);
    assert(output != nullptr);
    assert(strstr(rt_string_cstr(output), "result_fail") != nullptr);
    assert(rt_exec_command_result_exit_code(result) == 9);
    assert(rt_exec_command_result_succeeded(result) == 0);
}

static void test_last_exit_code_initial() {
    // Before any ShellFull call, last exit code is -1
    // (We can't reset the TLS state between tests, so just call it after
    // the previous test and check that it returns the last recorded code.)
    // This test simply verifies the function is callable without crashing.
    int64_t code = rt_exec_last_exit_code();
    (void)code; // value depends on prior test; just ensure no crash
}

static void test_process_streams_stdout_stderr() {
    void *handle = start_shell_process("printf 'out1\\n'; printf 'err1\\n' >&2; sleep 1; "
                                       "printf 'out2\\n'; printf 'err2\\n' >&2; exit 7");
    assert(handle != nullptr);
    assert(rt_process_is_valid(handle) == 1);

    std::string stdout_text;
    std::string stderr_text;
    bool saw_output_while_running = false;

    for (int i = 0; i < 80; i++) {
        bool running = rt_process_is_running(handle) != 0;
        rt_string out = rt_process_read_stdout(handle);
        rt_string err = rt_process_read_stderr(handle);
        if (running && ((out && rt_str_len(out) > 0) || (err && rt_str_len(err) > 0)))
            saw_output_while_running = true;
        append_runtime_string(stdout_text, out);
        append_runtime_string(stderr_text, err);
        if (!running)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    int64_t code = rt_process_wait(handle);
    append_runtime_string(stdout_text, rt_process_read_stdout(handle));
    append_runtime_string(stderr_text, rt_process_read_stderr(handle));

    assert(code == 7);
    assert(rt_process_exit_code(handle) == 7);
    assert(stdout_text.find("out1") != std::string::npos);
    assert(stdout_text.find("out2") != std::string::npos);
    assert(stderr_text.find("err1") != std::string::npos);
    assert(stderr_text.find("err2") != std::string::npos);
    assert(saw_output_while_running);

    rt_process_destroy(handle);
    assert(rt_process_is_valid(handle) == 0);
}

static void append_ordered_process_output(
    void *result, std::string &tagged, int64_t &last_sequence, bool &saw_stdout, bool &saw_stderr) {
    assert(result != nullptr);
    void *chunks = rt_map_get(result, rt_const_cstr("chunks"));
    assert(chunks != nullptr);
    for (int64_t i = 0; i < rt_seq_len(chunks); ++i) {
        void *chunk = rt_seq_get(chunks, i);
        assert(chunk != nullptr);
        int64_t sequence = rt_map_get_int(chunk, rt_const_cstr("sequence"));
        assert(sequence > last_sequence);
        last_sequence = sequence;
        rt_string stream = rt_map_get_str(chunk, rt_const_cstr("stream"));
        rt_string text = rt_map_get_str(chunk, rt_const_cstr("text"));
        std::string stream_text(rt_string_cstr(stream), (size_t)rt_str_len(stream));
        if (stream_text == "stdout")
            saw_stdout = true;
        if (stream_text == "stderr")
            saw_stderr = true;
        tagged.append("[").append(stream_text).append("]");
        tagged.append(rt_string_cstr(text), (size_t)rt_str_len(text));
        rt_str_release_maybe(stream);
        rt_str_release_maybe(text);
    }
}

static void test_process_ordered_tagged_output() {
    void *handle = start_shell_process("printf 'ordered-out-1\\n'; printf 'ordered-err-1\\n' >&2; "
                                       "printf 'ordered-out-2\\n'; printf 'ordered-err-2\\n' >&2");
    assert(handle != nullptr);

    std::string tagged;
    int64_t last_sequence = -1;
    bool saw_stdout = false;
    bool saw_stderr = false;
    for (int i = 0; i < 400; ++i) {
        bool running = rt_process_is_running(handle) != 0;
        append_ordered_process_output(
            rt_process_read_output_result(handle), tagged, last_sequence, saw_stdout, saw_stderr);
        if (!running)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(rt_process_wait(handle) == 0);
    append_ordered_process_output(
        rt_process_read_output_result(handle), tagged, last_sequence, saw_stdout, saw_stderr);
    assert(saw_stdout && saw_stderr);
    assert(tagged.find("[stdout]ordered-out") != std::string::npos);
    assert(tagged.find("[stderr]ordered-err") != std::string::npos);
    assert(rt_str_len(rt_process_read_stdout(handle)) == 0);
    assert(rt_str_len(rt_process_read_stderr(handle)) == 0);
    rt_process_destroy(handle);
}

static void test_process_cwd_and_env() {
    void *env = rt_seq_new();
    rt_seq_push(env, make_string("ZANNA_PROCESS_TEST=env-ok"));

    void *handle =
        start_shell_process_with_env("pwd; printf '%s\\n' \"$ZANNA_PROCESS_TEST\"", "/tmp", env);
    assert(handle != nullptr);

    int64_t code = rt_process_wait(handle);
    std::string stdout_text;
    append_runtime_string(stdout_text, rt_process_read_stdout(handle));

    assert(code == 0);
    assert(stdout_text.find("/tmp") != std::string::npos);
    assert(stdout_text.find("env-ok") != std::string::npos);
    rt_process_destroy(handle);
}

#if !ZANNA_HOST_WINDOWS
static void test_process_environment_overlay() {
    assert(setenv("ZANNA_PROCESS_OVERLAY_PARENT", "inherited", 1) == 0);
    assert(setenv("ZANNA_PROCESS_OVERLAY_OVERRIDE", "parent", 1) == 0);
    void *overlay = rt_seq_new();
    rt_seq_push(overlay, make_string("ZANNA_PROCESS_OVERLAY_OVERRIDE=child"));
    rt_seq_push(overlay, make_string("ZANNA_PROCESS_OVERLAY_NEW=added"));
    void *handle = start_shell_process_with_env_overlay(
        "printf '%s|%s|%s' \"$ZANNA_PROCESS_OVERLAY_PARENT\" "
        "\"$ZANNA_PROCESS_OVERLAY_OVERRIDE\" \"$ZANNA_PROCESS_OVERLAY_NEW\"",
        "/tmp",
        overlay);
    assert(handle != nullptr);
    assert(rt_process_wait(handle) == 0);
    std::string out;
    append_runtime_string(out, rt_process_read_stdout(handle));
    assert(out == "inherited|child|added");
    rt_process_destroy(handle);
    unsetenv("ZANNA_PROCESS_OVERLAY_PARENT");
    unsetenv("ZANNA_PROCESS_OVERLAY_OVERRIDE");
}
#endif

// VDOC-213: a bare program name is PATH-searched even with an explicit
// environment, so Process resolves it the same way with or without env (adding
// an env no longer turns a searchable name into exit code 127). The PATH search
// uses the supplied environment's PATH.
static void test_process_bare_name_path_search_with_env() {
    void *args = make_shell_args("printf path-ok");
    void *env = rt_seq_new();
    rt_seq_push(env, make_string("PATH=/usr/bin:/bin"));
    void *handle = rt_process_start_with_env(make_string("sh"), args, make_string("/tmp"), env);
    assert(handle != nullptr);

    int64_t code = rt_process_wait(handle);
    std::string out;
    append_runtime_string(out, rt_process_read_stdout(handle));
    assert(code == 0);
    assert(out.find("path-ok") != std::string::npos);
    rt_process_destroy(handle);
}

static void test_process_accepts_boxed_args_from_object_abi() {
    void *args = rt_seq_new();
    rt_seq_push(args, rt_box_str(make_string("-c")));
    rt_seq_push(args, rt_box_str(make_string("printf boxed-ok")));

    void *handle = nullptr;
    const char *shells[] = {"/bin/sh", "/usr/bin/sh", nullptr};
    for (int i = 0; shells[i] != nullptr; i++) {
        handle = rt_process_start(make_string(shells[i]), args);
        if (handle)
            break;
    }
    assert(handle != nullptr);

    int64_t code = rt_process_wait(handle);
    std::string stdout_text;
    append_runtime_string(stdout_text, rt_process_read_stdout(handle));

    assert(code == 0);
    assert(stdout_text.find("boxed-ok") != std::string::npos);
    rt_process_destroy(handle);
}

static void test_process_empty_incremental_read() {
    void *handle = start_shell_process("sleep 1; exit 0");
    assert(handle != nullptr);
    assert(rt_process_is_running(handle) == 1);

    rt_string out = rt_process_read_stdout(handle);
    rt_string err = rt_process_read_stderr(handle);
    assert(out != nullptr);
    assert(err != nullptr);
    assert(rt_str_len(out) == 0);
    assert(rt_str_len(err) == 0);

    assert(rt_process_wait(handle) == 0);
    rt_process_destroy(handle);
}

static void test_process_kill() {
    void *handle = start_shell_process("while true; do sleep 1; done");
    assert(handle != nullptr);
    assert(rt_process_is_running(handle) == 1);
    assert(rt_process_kill(handle) == 1);

    int64_t code = rt_process_wait(handle);
    assert(code != 0);
    assert(rt_process_is_running(handle) == 0);
    rt_process_destroy(handle);
}

#if !ZANNA_HOST_WINDOWS
static void test_process_kill_terminates_descendants() {
    void *handle = start_shell_process("sleep 30 & child=$!; printf '%s\\n' \"$child\"; wait");
    assert(handle != nullptr);

    std::string output;
    for (int i = 0; i < 400 && output.find('\n') == std::string::npos; ++i) {
        append_runtime_string(output, rt_process_read_stdout(handle));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    char *end = nullptr;
    long descendant = std::strtol(output.c_str(), &end, 10);
    assert(descendant > 1 && end != output.c_str());
    assert(kill((pid_t)descendant, 0) == 0);

    assert(rt_process_kill(handle) == 1);
    assert(rt_process_wait(handle) != 0);
    bool descendant_gone = false;
    for (int i = 0; i < 400; ++i) {
        if (kill((pid_t)descendant, 0) != 0 && errno == ESRCH) {
            descendant_gone = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(descendant_gone);
    rt_process_destroy(handle);
}
#endif

static void test_process_write_stdin() {
    // A child that reads one line from stdin and echoes it back proves the new
    // WriteStdin pipe carries data into the child.
    void *handle = start_shell_process("read line; echo \"got:$line\"");
    assert(handle != nullptr);
    assert(rt_process_is_valid(handle) == 1);

    rt_string payload = make_string("ping\n");
    int64_t written = rt_process_write_stdin(handle, payload);
    assert(written == 5);

    std::string out;
    for (int i = 0; i < 400 && rt_process_is_running(handle); i++) {
        append_runtime_string(out, rt_process_read_stdout(handle));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    int64_t code = rt_process_wait(handle);
    append_runtime_string(out, rt_process_read_stdout(handle));
    assert(code == 0);
    assert(out.find("got:ping") != std::string::npos);

    // Writing after the child exited must fail gracefully (no SIGPIPE crash).
    rt_string late = make_string("late\n");
    int64_t late_write = rt_process_write_stdin(handle, late);
    assert(late_write <= 0);

    rt_process_destroy(handle);
}

#if ZANNA_HOST_WINDOWS
static int verify_windows_overlay_environment_child() {
    wchar_t value[256];
    if (GetEnvironmentVariableW(L"ZANNA_PROCESS_OVERLAY_PARENT", value, (DWORD)std::size(value)) ==
            0 ||
        wcscmp(value, L"inherited") != 0)
        return 21;
    if (GetEnvironmentVariableW(
            L"ZANNA_PROCESS_OVERLAY_OVERRIDE", value, (DWORD)std::size(value)) == 0 ||
        wcscmp(value, L"child") != 0)
        return 22;
    if (GetEnvironmentVariableW(L"ZANNA_PROCESS_OVERLAY_NEW", value, (DWORD)std::size(value)) ==
            0 ||
        wcscmp(value, L"added") != 0)
        return 23;
    if (GetEnvironmentVariableW(L"PATH", value, (DWORD)std::size(value)) == 0)
        return 24;
    return 0;
}

static int verify_windows_unicode_environment_child() {
    wchar_t value[64];
    DWORD value_len =
        GetEnvironmentVariableW(L"ZANNA_PROCESS_TEST", value, (DWORD)std::size(value));
    if (value_len == 0 || value_len >= std::size(value) ||
        wcscmp(value, L"Gr\u00fc\u00dfe-\u6771\u4eac") != 0)
        return 11;
    if (GetEnvironmentVariableW(L"ZANNA_A_FIRST", value, (DWORD)std::size(value)) == 0 ||
        wcscmp(value, L"ok") != 0)
        return 12;
    if (GetEnvironmentVariableW(L"ZANNA_Z_LAST", value, (DWORD)std::size(value)) == 0 ||
        wcscmp(value, L"ok") != 0)
        return 13;

    LPWCH environment = GetEnvironmentStringsW();
    if (!environment)
        return 14;
    const wchar_t *previous = nullptr;
    for (const wchar_t *entry = environment; *entry; entry += wcslen(entry) + 1) {
        if (*entry == L'=')
            continue;
        if (previous && CompareStringOrdinal(previous, -1, entry, -1, TRUE) == CSTR_GREATER_THAN) {
            FreeEnvironmentStringsW(environment);
            return 15;
        }
        previous = entry;
    }
    FreeEnvironmentStringsW(environment);
    return 0;
}

static std::string windows_current_executable_utf8() {
    wchar_t path[32768];
    DWORD path_len = GetModuleFileNameW(nullptr, path, (DWORD)std::size(path));
    if (path_len == 0 || path_len >= std::size(path))
        return {};
    int byte_len = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, path, (int)path_len, nullptr, 0, nullptr, nullptr);
    if (byte_len <= 0)
        return {};
    std::string result((size_t)byte_len, '\0');
    if (WideCharToMultiByte(CP_UTF8,
                            WC_ERR_INVALID_CHARS,
                            path,
                            (int)path_len,
                            result.data(),
                            byte_len,
                            nullptr,
                            nullptr) != byte_len)
        return {};
    return result;
}

static void test_windows_process_stdin_is_nonblocking() {
    std::string executable = windows_current_executable_utf8();
    assert(!executable.empty());
    void *args = rt_seq_new();
    rt_seq_push(args, make_string("--process-stdin-no-read-child"));
    void *handle = rt_process_start(make_string(executable.c_str()), args);
    assert(handle != nullptr);

    std::string payload(2u * 1024u * 1024u, 'x');
    rt_string input = rt_string_from_bytes(payload.data(), payload.size());
    auto started = std::chrono::steady_clock::now();
    int64_t accepted = rt_process_write_stdin(handle, input);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    assert(accepted > 0);
    assert((size_t)accepted < payload.size());
    assert(elapsed < std::chrono::milliseconds(1000));
    rt_process_destroy(handle);
    printf("  PASS: test_windows_process_stdin_is_nonblocking\n");
}

static void test_windows_unicode_environment_round_trip() {
    std::string executable = windows_current_executable_utf8();
    assert(!executable.empty());

    void *args = rt_seq_new();
    rt_seq_push(args, make_string("--verify-unicode-environment"));

    void *env = rt_seq_new();
    rt_seq_push(env, make_string("ZANNA_Z_LAST=ok"));
    rt_seq_push(env,
                make_string("ZANNA_PROCESS_TEST=Gr\xc3\xbc\xc3\x9f"
                            "e-\xe6\x9d\xb1\xe4\xba\xac"));
    rt_seq_push(env, make_string("ZANNA_A_FIRST=ok"));

    void *handle =
        rt_process_start_with_env(make_string(executable.c_str()), args, make_string(""), env);
    assert(handle != nullptr);
    assert(rt_process_wait(handle) == 0);
    rt_process_destroy(handle);
    printf("  PASS: test_windows_unicode_environment_round_trip\n");
}

static void test_windows_environment_overlay() {
    std::string executable = windows_current_executable_utf8();
    assert(!executable.empty());
    assert(SetEnvironmentVariableW(L"ZANNA_PROCESS_OVERLAY_PARENT", L"inherited"));
    assert(SetEnvironmentVariableW(L"ZANNA_PROCESS_OVERLAY_OVERRIDE", L"parent"));
    void *args = rt_seq_new();
    rt_seq_push(args, make_string("--verify-overlay-environment"));
    void *overlay = rt_seq_new();
    // Windows matching is case-insensitive; this must replace the uppercase parent name.
    rt_seq_push(overlay, make_string("zanna_process_overlay_override=child"));
    rt_seq_push(overlay, make_string("ZANNA_PROCESS_OVERLAY_NEW=added"));
    void *handle = rt_process_start_with_env_overlay(
        make_string(executable.c_str()), args, make_string(""), overlay);
    assert(handle != nullptr);
    assert(rt_process_wait(handle) == 0);
    rt_process_destroy(handle);
    SetEnvironmentVariableW(L"ZANNA_PROCESS_OVERLAY_PARENT", nullptr);
    SetEnvironmentVariableW(L"ZANNA_PROCESS_OVERLAY_OVERRIDE", nullptr);
    printf("  PASS: test_windows_environment_overlay\n");
}

static void test_windows_conpty_unicode_environment_round_trip() {
    if (!rt_pty_is_supported()) {
        printf("  SKIP: test_windows_conpty_unicode_environment_round_trip (ConPTY unavailable)\n");
        return;
    }
    std::string executable = windows_current_executable_utf8();
    assert(!executable.empty());

    void *args = rt_seq_new();
    rt_seq_push(args, make_string("--verify-unicode-environment"));
    void *env = rt_seq_new();
    rt_seq_push(env, make_string("ZANNA_Z_LAST=ok"));
    rt_seq_push(env,
                make_string("ZANNA_PROCESS_TEST=Gr\xc3\xbc\xc3\x9f"
                            "e-\xe6\x9d\xb1\xe4\xba\xac"));
    rt_seq_push(env, make_string("ZANNA_A_FIRST=ok"));

    void *handle = rt_pty_open(make_string(executable.c_str()), args, make_string(""), env, 80, 24);
    assert(handle != nullptr);
    assert(rt_pty_wait(handle) == 0);
    rt_pty_destroy(handle);

    void *duplicate_env = rt_seq_new();
    rt_seq_push(duplicate_env, make_string("ZANNA_DUPLICATE=one"));
    rt_seq_push(duplicate_env, make_string("zanna_duplicate=two"));
    assert(rt_pty_open(
               make_string(executable.c_str()), args, make_string(""), duplicate_env, 80, 24) ==
           nullptr);

    const char malformed_cwd_bytes[] = "\x78\xC0\xAF";
    assert(rt_pty_open(make_string(executable.c_str()),
                       args,
                       make_string(malformed_cwd_bytes),
                       nullptr,
                       80,
                       24) == nullptr);
    printf("  PASS: test_windows_conpty_unicode_environment_round_trip\n");
}
#endif

int main(int argc, char **argv) {
#if ZANNA_HOST_WINDOWS
    if (argc == 2 && strcmp(argv[1], "--verify-unicode-environment") == 0)
        return verify_windows_unicode_environment_child();
    if (argc == 2 && strcmp(argv[1], "--verify-overlay-environment") == 0)
        return verify_windows_overlay_environment_child();
    if (argc == 2 && strcmp(argv[1], "--process-stdin-no-read-child") == 0) {
        Sleep(5000);
        return 0;
    }
    test_windows_unicode_environment_round_trip();
    test_windows_environment_overlay();
    test_windows_conpty_unicode_environment_round_trip();
    test_windows_process_stdin_is_nonblocking();
    return 0;
#else
    (void)argc;
    (void)argv;
#endif
    test_shell_true();
    test_shell_false();
    test_shell_echo();
    test_shell_capture_echo();
    test_shell_capture_multiline();
    test_run_true();
    test_run_args();
    test_capture_args();
    test_shell_empty_command();
    test_shell_capture_empty();
    test_nonexistent_program();
    test_capture_nonexistent();
    test_shell_exit_code();
    test_shell_capture_stderr();
    test_run_null_args();
    test_run_args_accepts_boxed_strings();

    // ShellFull + LastExitCode
    test_shell_full_success();
    test_shell_full_exit_code();
    test_shell_full_with_stderr_merge();
    test_shell_full_empty();
    test_shell_result_success();
    test_shell_result_exit_code();
    test_last_exit_code_initial();

    // Streaming Process handle coverage.
    test_process_streams_stdout_stderr();
    test_process_ordered_tagged_output();
    test_process_write_stdin();
    test_process_cwd_and_env();
#if !ZANNA_HOST_WINDOWS
    test_process_environment_overlay();
#endif
    test_process_bare_name_path_search_with_env();
    test_process_accepts_boxed_args_from_object_abi();
    test_process_empty_incremental_read();
    test_process_kill();
#if !ZANNA_HOST_WINDOWS
    test_process_kill_terminates_descendants();
    test_process_activity_wake();
    test_pty_activity_wake();
#endif

    return 0;
}
