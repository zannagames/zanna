//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/test_vg_filedialog_platform_win32.c
// Purpose: Verify Windows widget file-dialog path and environment adapters.
//
// Key invariants:
//   - Drive, UNC, and extended UNC roots remain intact when navigating upward.
//   - Path joining adds exactly one separator and malformed UTF-8 fails closed.
//   - USERPROFILE is used only when it resolves to an existing directory.
//
// Ownership/Lifetime:
//   - Every platform-returned string is released by the test.
//   - Process environment values are restored before the test exits.
//
// Links: src/lib/gui/src/widgets/vg_filedialog_platform_win32.c
//
//===----------------------------------------------------------------------===//

#include "vg_filedialog_platform.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

static wchar_t *snapshot_environment(const wchar_t *name) {
    DWORD required = GetEnvironmentVariableW(name, NULL, 0);
    wchar_t *value;
    DWORD length;
    if (required == 0)
        return NULL;
    value = (wchar_t *)malloc((size_t)required * sizeof(wchar_t));
    assert(value != NULL);
    length = GetEnvironmentVariableW(name, value, required);
    assert(length > 0 && length < required);
    return value;
}

static void expect_parent(const char *path, const char *expected) {
    char *parent = vg_filedialog_platform_parent_dir(path);
    assert(parent != NULL);
    assert(strcmp(parent, expected) == 0);
    free(parent);
}

static void test_root_preserving_parent_navigation(void) {
    expect_parent("C:\\", "C:\\");
    expect_parent("C:\\Zanna", "C:\\");
    expect_parent("\\\\server\\share", "\\\\server\\share");
    expect_parent("\\\\server\\share\\", "\\\\server\\share");
    expect_parent("\\\\server\\share\\folder", "\\\\server\\share");
    expect_parent("\\\\?\\C:\\Zanna", "\\\\?\\C:\\");
    expect_parent("\\\\?\\UNC\\server\\share", "\\\\?\\UNC\\server\\share");
    expect_parent("\\\\?\\UNC\\server\\share\\folder", "\\\\?\\UNC\\server\\share");
}

static void test_join_and_utf8_guards(void) {
    char *joined = vg_filedialog_platform_join_path("C:\\Zanna", "project.zia");
    assert(joined != NULL);
    assert(strcmp(joined, "C:\\Zanna\\project.zia") == 0);
    free(joined);

    joined = vg_filedialog_platform_join_path("C:\\Zanna\\", "project.zia");
    assert(joined != NULL);
    assert(strcmp(joined, "C:\\Zanna\\project.zia") == 0);
    free(joined);

    assert(vg_filedialog_platform_is_absolute_path("C:\\Zanna"));
    assert(vg_filedialog_platform_is_absolute_path("\\\\server\\share\\Zanna"));
    assert(vg_filedialog_platform_is_absolute_path("\\\\?\\C:\\Zanna"));
    assert(vg_filedialog_platform_is_absolute_path("\\\\?\\UNC\\server\\share\\Zanna"));
    assert(!vg_filedialog_platform_is_absolute_path("Zanna\\project.zia"));
    assert(!vg_filedialog_platform_is_absolute_path("\\\\server"));
    assert(!vg_filedialog_platform_is_absolute_path("\\\\.\\PhysicalDrive0"));
    assert(!vg_filedialog_platform_is_absolute_path("\\\\?\\GLOBALROOT\\Device"));
    assert(!vg_filedialog_platform_path_is_dir("\xC0\xAF"));
}

static void test_home_requires_existing_directory(void) {
    wchar_t *savedProfile = snapshot_environment(L"USERPROFILE");
    wchar_t *savedDrive = snapshot_environment(L"HOMEDRIVE");
    wchar_t *savedPath = snapshot_environment(L"HOMEPATH");
    wchar_t temporary[MAX_PATH];
    wchar_t directory[MAX_PATH];
    DWORD tempLength = GetTempPathW(MAX_PATH, temporary);
    assert(tempLength > 0 && tempLength < MAX_PATH);
    int written = swprintf_s(
        directory, MAX_PATH, L"%szanna-filedialog-home-%lu", temporary, GetCurrentProcessId());
    assert(written > 0 && written < MAX_PATH);
    assert(CreateDirectoryW(directory, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);

    assert(SetEnvironmentVariableW(L"USERPROFILE", directory));
    assert(SetEnvironmentVariableW(L"HOMEDRIVE", L"Z:"));
    assert(SetEnvironmentVariableW(L"HOMEPATH", L"\\definitely-missing-zanna-home"));
    char *home = vg_filedialog_platform_home_dir();
    assert(home != NULL);
    assert(vg_filedialog_platform_path_is_dir(home));
    free(home);

    assert(SetEnvironmentVariableW(L"USERPROFILE", L"Z:\\definitely-missing-zanna-profile"));
    home = vg_filedialog_platform_home_dir();
    assert(home != NULL);
    assert(strcmp(home, "Z:\\definitely-missing-zanna-profile") != 0);
    free(home);

    assert(SetEnvironmentVariableW(L"USERPROFILE", L"."));
    home = vg_filedialog_platform_home_dir();
    assert(home != NULL);
    assert(strcmp(home, ".") != 0);
    assert(vg_filedialog_platform_is_absolute_path(home));
    free(home);

    assert(SetEnvironmentVariableW(L"USERPROFILE", savedProfile));
    assert(SetEnvironmentVariableW(L"HOMEDRIVE", savedDrive));
    assert(SetEnvironmentVariableW(L"HOMEPATH", savedPath));
    free(savedProfile);
    free(savedDrive);
    free(savedPath);
    assert(RemoveDirectoryW(directory));
}

int main(void) {
    test_root_preserving_parent_navigation();
    test_join_and_utf8_guards();
    test_home_requires_existing_directory();
    return 0;
}
