//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/tests/test_win32_console.c
// Purpose: Verify that a graphical Win32 process releases a console only it uses,
//          and that its standard streams stay safe afterwards.
// Key invariants:
//   - A private console is released and every std slot and CRT descriptor 0-2
//     is left on an open, non-console handle that later handles cannot alias,
//     in every one of repeated runs (handle values recycle nondeterministically).
//   - A console shared with another process is kept, with the std handles intact.
//   - Redirected standard streams survive the release and keep receiving output.
// Ownership/Lifetime:
//   - The test re-launches itself in child roles; every child reports through a
//     result file in a per-run temporary directory that the parent removes.
// Links: src/lib/graphics/src/vgfx_platform_win32_console.c,
//        docs/adr/0361-release-private-console-for-windows-graphical-programs.md
//
//===----------------------------------------------------------------------===//

#include "vgfx_platform_win32_console.h"

#include <windows.h>

#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { CHILD_TIMEOUT_MS = 60000, HANDLE_PROBES = 64, PRIVATE_ROUNDS = 25 };

static const DWORD k_slots[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};

static FILE *g_report = NULL;
static int g_failures = 0;

/// @brief Record one check in the child's result file.
static void check(int condition, const char *message) {
    if (!condition)
        g_failures++;
    if (g_report) {
        fprintf(g_report, "%s %s\n", condition ? "ok  " : "FAIL", message);
        fflush(g_report);
    }
}

static int open_report(const wchar_t *path) {
    g_report = _wfopen(path, L"w");
    return g_report != NULL;
}

static int close_report(void) {
    if (g_report) {
        fprintf(g_report, "done failures=%d\n", g_failures);
        fclose(g_report);
    }
    return g_failures == 0 ? 0 : 1;
}

static DWORD console_clients(void) {
    DWORD clients[4];
    return GetConsoleProcessList(clients, 4);
}

static int is_console_handle(HANDLE handle) {
    DWORD mode = 0;
    return handle != NULL && handle != INVALID_HANDLE_VALUE &&
           GetFileType(handle) == FILE_TYPE_CHAR && GetConsoleMode(handle, &mode);
}

static int is_open_handle(HANDLE handle) {
    DWORD flags = 0;
    return handle != NULL && handle != INVALID_HANDLE_VALUE && GetHandleInformation(handle, &flags);
}

/// @brief Whether a newly opened handle ever takes a value a std stream still uses.
static int std_handles_alias_new_handles(void) {
    HANDLE opened[HANDLE_PROBES];
    int aliased = 0;
    for (int i = 0; i < HANDLE_PROBES; ++i) {
        opened[i] = CreateFileW(L"NUL",
                                GENERIC_WRITE,
                                FILE_SHARE_WRITE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                NULL);
        for (int d = 0; d < 3; ++d) {
            if (opened[i] != INVALID_HANDLE_VALUE &&
                (opened[i] == GetStdHandle(k_slots[d]) || opened[i] == (HANDLE)_get_osfhandle(d)))
                aliased = 1;
        }
    }
    for (int i = 0; i < HANDLE_PROBES; ++i) {
        if (opened[i] != INVALID_HANDLE_VALUE)
            CloseHandle(opened[i]);
    }
    return aliased;
}

/// @brief Child role: sole client of a windowless console.
static int child_private(const wchar_t *report) {
    if (!open_report(report))
        return 2;
    check(console_clients() == 1, "the child is its console's only client");
    check(is_console_handle(GetStdHandle(STD_OUTPUT_HANDLE)), "stdout starts on the console");

    check(vgfx_win32_release_private_console() == 1, "the private console is released");
    check(console_clients() == 0, "the process has no console afterwards");
    for (int d = 0; d < 3; ++d) {
        HANDLE slot = GetStdHandle(k_slots[d]);
        HANDLE crt = (HANDLE)_get_osfhandle(d);
        check(is_open_handle(slot) && !is_console_handle(slot),
              "std slot holds an open non-console handle");
        check(is_open_handle(crt) && !is_console_handle(crt),
              "CRT descriptor holds an open non-console handle");
    }
    DWORD written = 0;
    check(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), "x\n", 2, &written, NULL) && written == 2,
          "Win32 writes to stdout succeed");
    check(printf("printf after release\n") > 0 && fflush(stdout) == 0 && !ferror(stdout),
          "CRT writes to stdout succeed");
    check(fprintf(stderr, "stderr after release\n") > 0 && fflush(stderr) == 0 && !ferror(stderr),
          "CRT writes to stderr succeed");
    check(!std_handles_alias_new_handles(), "no std stream aliases a newly opened handle");
    check(vgfx_win32_release_private_console() == 0, "a second release is a no-op");
    return close_report();
}

/// @brief Child role: attached to a console another process also uses.
static int child_shared_client(const wchar_t *report) {
    if (!open_report(report))
        return 2;
    HANDLE before[3];
    for (int d = 0; d < 3; ++d)
        before[d] = GetStdHandle(k_slots[d]);
    check(console_clients() >= 2, "the console has another client");
    check(vgfx_win32_release_private_console() == 0, "a shared console is kept");
    check(console_clients() >= 2, "the process is still attached");
    for (int d = 0; d < 3; ++d)
        check(GetStdHandle(k_slots[d]) == before[d], "std slots are untouched");
    check(is_console_handle(GetStdHandle(STD_OUTPUT_HANDLE)), "stdout is still the console");
    return close_report();
}

/// @brief Child role: sole console client whose std streams are redirected.
static int child_redirected(const wchar_t *report) {
    if (!open_report(report))
        return 2;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    check(console_clients() == 1, "the child is its console's only client");
    check(GetFileType(out) == FILE_TYPE_DISK, "stdout starts redirected to a file");
    check(vgfx_win32_release_private_console() == 1, "the private console is released");
    check(GetStdHandle(STD_OUTPUT_HANDLE) == out, "a redirected std slot is untouched");
    static const char marker[] = "win32-marker-after-release\n";
    DWORD written = 0;
    check(WriteFile(out, marker, (DWORD)(sizeof(marker) - 1), &written, NULL) &&
              written == sizeof(marker) - 1,
          "Win32 writes still reach the file");
    check(printf("crt-marker-after-release\n") > 0 && fflush(stdout) == 0,
          "CRT writes still reach the file");
    return close_report();
}

static int spawn_self(const wchar_t *role,
                      const wchar_t *report,
                      DWORD flags,
                      HANDLE redirect,
                      HANDLE nul_input,
                      DWORD *exit_code) {
    wchar_t exe[MAX_PATH];
    DWORD exe_length = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (exe_length == 0 || exe_length >= MAX_PATH)
        return 0;
    wchar_t command[3 * MAX_PATH];
    if (swprintf_s(command, 3 * MAX_PATH, L"\"%s\" %s \"%s\"", exe, role, report) < 0)
        return 0;
    STARTUPINFOW startup = {sizeof(startup)};
    BOOL inherit = FALSE;
    if (redirect) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nul_input;
        startup.hStdOutput = redirect;
        startup.hStdError = redirect;
        inherit = TRUE;
    }
    PROCESS_INFORMATION process = {0};
    if (!CreateProcessW(exe, command, NULL, NULL, inherit, flags, NULL, NULL, &startup, &process))
        return 0;
    DWORD waited = WaitForSingleObject(process.hProcess, CHILD_TIMEOUT_MS);
    if (waited != WAIT_OBJECT_0)
        TerminateProcess(process.hProcess, 3);
    const BOOL got = GetExitCodeProcess(process.hProcess, exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return waited == WAIT_OBJECT_0 && got;
}

/// @brief Child role: owns a windowless console and runs the shared client on it.
static int child_shared_host(const wchar_t *report) {
    DWORD code = 99;
    if (!spawn_self(L"--shared-client", report, 0, NULL, NULL, &code))
        return 2;
    return (int)code;
}

static int file_contains(const wchar_t *path, const char *needle) {
    FILE *file = _wfopen(path, L"rb");
    if (!file)
        return 0;
    char buffer[4096];
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';
    return strstr(buffer, needle) != NULL;
}

static void dump_file(const wchar_t *path) {
    FILE *file = _wfopen(path, L"rb");
    if (!file)
        return;
    char buffer[4096];
    size_t length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[length] = '\0';
    fprintf(stderr, "%s", buffer);
}

static int g_parent_failures = 0;

static void run_role(const wchar_t *directory,
                     const wchar_t *role,
                     const wchar_t *name,
                     DWORD flags) {
    wchar_t report[MAX_PATH];
    swprintf_s(report, MAX_PATH, L"%s\\%s.txt", directory, name);
    DWORD code = 99;
    const int ran = spawn_self(role, report, flags, NULL, NULL, &code);
    if (!ran || code != 0 || !file_contains(report, "done failures=0")) {
        fprintf(stderr, "FAIL: child role %ls (ran=%d exit=%lu)\n", role, ran, (unsigned long)code);
        dump_file(report);
        g_parent_failures++;
    }
    DeleteFileW(report);
}

static void run_redirected_role(const wchar_t *directory) {
    wchar_t report[MAX_PATH];
    wchar_t output[MAX_PATH];
    swprintf_s(report, MAX_PATH, L"%s\\redirected.txt", directory);
    swprintf_s(output, MAX_PATH, L"%s\\redirected.out", directory);
    SECURITY_ATTRIBUTES inheritable = {sizeof(inheritable), NULL, TRUE};
    HANDLE file = CreateFileW(output,
                              GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &inheritable,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              NULL);
    HANDLE nul = CreateFileW(L"NUL",
                             GENERIC_READ,
                             FILE_SHARE_READ,
                             &inheritable,
                             OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL,
                             NULL);
    DWORD code = 99;
    int ran = 0;
    if (file != INVALID_HANDLE_VALUE && nul != INVALID_HANDLE_VALUE)
        ran = spawn_self(L"--redirected", report, CREATE_NO_WINDOW, file, nul, &code);
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    if (nul != INVALID_HANDLE_VALUE)
        CloseHandle(nul);
    if (!ran || code != 0 || !file_contains(report, "done failures=0") ||
        !file_contains(output, "win32-marker-after-release") ||
        !file_contains(output, "crt-marker-after-release")) {
        fprintf(stderr, "FAIL: redirected child (ran=%d exit=%lu)\n", ran, (unsigned long)code);
        dump_file(report);
        g_parent_failures++;
    }
    DeleteFileW(report);
    DeleteFileW(output);
}

int wmain(int argc, wchar_t **argv) {
    if (argc >= 3) {
        if (wcscmp(argv[1], L"--private") == 0)
            return child_private(argv[2]);
        if (wcscmp(argv[1], L"--shared-host") == 0)
            return child_shared_host(argv[2]);
        if (wcscmp(argv[1], L"--shared-client") == 0)
            return child_shared_client(argv[2]);
        if (wcscmp(argv[1], L"--redirected") == 0)
            return child_redirected(argv[2]);
    }

    wchar_t temporary[MAX_PATH];
    wchar_t directory[MAX_PATH];
    DWORD temp_length = GetTempPathW(MAX_PATH, temporary);
    if (temp_length == 0 || temp_length >= MAX_PATH ||
        swprintf_s(
            directory, MAX_PATH, L"%szanna-win32-console-%lu", temporary, GetCurrentProcessId()) <
            0 ||
        (!CreateDirectoryW(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)) {
        fprintf(stderr, "FAIL: cannot create the temporary directory\n");
        return 1;
    }

    // CREATE_NO_WINDOW gives each role a console of its own without showing it.
    // Windows recycles freed handle values nondeterministically, so the private
    // role repeats: a release that opened a handle after retiring the console
    // handles fails only when FreeConsole meets a recycled value.
    for (int round = 0; round < PRIVATE_ROUNDS; ++round)
        run_role(directory, L"--private", L"private", CREATE_NO_WINDOW);
    run_role(directory, L"--shared-host", L"shared", CREATE_NO_WINDOW);
    run_redirected_role(directory);

    RemoveDirectoryW(directory);
    if (g_parent_failures != 0) {
        fprintf(stderr, "%d console release run(s) failed\n", g_parent_failures);
        return 1;
    }
    printf("win32 console release: %d private, 1 shared and 1 redirected run passed\n",
           PRIVATE_ROUNDS);
    return 0;
}
