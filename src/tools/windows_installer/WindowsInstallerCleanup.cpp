//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tools/windows_installer/WindowsInstallerCleanup.cpp
// Purpose: Remove exact installer maintenance-cache targets after their owner exits.
// Key invariants:
//   - Every deletion target passes the fail-closed cleanup policy and is deduplicated.
//   - Directories are removed non-recursively and reparse points are never traversed.
// Ownership/Lifetime:
//   - Command-line storage and Win32 handles are released on every completed path.
//   - The detached helper owns its copied executable and marks it for self-deletion.
// Links: src/tools/windows_installer/WindowsInstallerCleanupPolicy.hpp
//
//===----------------------------------------------------------------------===//

#include "WindowsInstallerCleanupPolicy.hpp"

#include <windows.h>

#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr DWORD kExitInvalidArguments = 2;
constexpr DWORD kExitSelfOpenFailed = 10;
constexpr DWORD kExitSelfRenameFailed = 11;
constexpr DWORD kExitSelfReopenFailed = 12;
constexpr DWORD kExitSelfDeleteFailed = 13;
constexpr DWORD kExitParentWaitFailed = 14;
constexpr DWORD kExitTargetCleanupFailed = 15;
constexpr size_t kMaximumFiles = 64;
constexpr size_t kMaximumDirectories = 64;
constexpr int kMaximumArguments = 3 + static_cast<int>((kMaximumFiles + kMaximumDirectories) * 2);

/// @brief Wait a bounded interval for the owning installer process to exit.
/// @param processId Nonzero Windows process identifier.
/// @return ERROR_SUCCESS when the process exits or no longer exists; otherwise
///         a wait/open error or WAIT_TIMEOUT.
DWORD waitForParent(DWORD processId) {
    if (processId == 0)
        return ERROR_INVALID_PARAMETER;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) {
        const DWORD error = GetLastError();
        return error == ERROR_INVALID_PARAMETER ? ERROR_SUCCESS : error;
    }
    const DWORD result = WaitForSingleObject(process, 5U * 60U * 1000U);
    const DWORD error = result == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(process);
    if (result == WAIT_OBJECT_0)
        return ERROR_SUCCESS;
    if (result == WAIT_TIMEOUT)
        return WAIT_TIMEOUT;
    return error ? error : ERROR_INVALID_FUNCTION;
}

/// @brief Rename the running helper's primary stream and mark its base file deleted.
/// @return ERROR_SUCCESS on complete self-unlink, otherwise a stable helper exit code.
/// @details Uses a named-stream rename followed by FileDispositionInfoEx so the
///          mapped image can disappear without a second process or reboot action.
DWORD markSelfForDeletion() {
    std::vector<wchar_t> path(512);
    while (path.size() <= 32768U) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0)
            return GetLastError();
        if (length < path.size()) {
            path.resize(length + 1U);
            break;
        }
        const size_t next = path.size() * 2U;
        if (next <= path.size() || next > 32768U)
            return ERROR_BUFFER_OVERFLOW;
        path.resize(next);
    }
    if (path.empty() || path.back() != L'\0')
        return ERROR_BUFFER_OVERFLOW;

    HANDLE file = CreateFileW(path.data(),
                              DELETE | SYNCHRONIZE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return kExitSelfOpenFailed;
    constexpr wchar_t kDeletedStream[] = L":zanna-cleanup-deleted";
    constexpr DWORD kStreamBytes = sizeof(kDeletedStream) - sizeof(wchar_t);
    std::vector<unsigned char> renameBytes(offsetof(FILE_RENAME_INFO, FileName) +
                                           sizeof(kDeletedStream));
    auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(renameBytes.data());
    rename->ReplaceIfExists = TRUE;
    rename->RootDirectory = nullptr;
    rename->FileNameLength = kStreamBytes;
    std::memcpy(rename->FileName, kDeletedStream, sizeof(kDeletedStream));
    const bool renamed =
        SetFileInformationByHandle(
            file, FileRenameInfo, rename, static_cast<DWORD>(renameBytes.size())) != FALSE;
    CloseHandle(file);
    if (!renamed)
        return kExitSelfRenameFailed;

    file = CreateFileW(path.data(),
                       DELETE | SYNCHRONIZE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL,
                       nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return kExitSelfReopenFailed;
    FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE |
                                         FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                                         FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE};
    const bool deleted =
        SetFileInformationByHandle(
            file, FileDispositionInfoEx, &disposition, sizeof(disposition)) != FALSE;
    CloseHandle(file);
    return deleted ? ERROR_SUCCESS : kExitSelfDeleteFailed;
}

/// @brief Delete one exact non-directory, non-reparse-point file with retries.
/// @param path Validated absolute target path.
/// @return @c true when deleted or already absent; @c false for unsafe attributes,
///         nontransient failures, or retry exhaustion.
bool deleteFileWithRetry(const std::wstring &path) {
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
            return false;
        const bool clearedReadOnly = (attributes & FILE_ATTRIBUTE_READONLY) != 0;
        if (clearedReadOnly) {
            if (!SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY))
                return false;
        }
        if (DeleteFileW(path.c_str()))
            return true;
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return true;
        if (clearedReadOnly && !SetFileAttributesW(path.c_str(), attributes)) {
            const DWORD restoreError = GetLastError();
            if (restoreError == ERROR_FILE_NOT_FOUND || restoreError == ERROR_PATH_NOT_FOUND)
                return true;
            return false;
        }
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
            error != ERROR_LOCK_VIOLATION) {
            return false;
        }
        Sleep(50);
    }
    return false;
}

/// @brief Remove one exact ordinary directory with bounded retries.
/// @param path Validated absolute directory path.
/// @param allowNonEmpty Whether a persistently nonempty directory counts as success.
/// @return @c true when removed, absent, or allowed to remain nonempty.
/// @details Never traverses contents and rejects reparse points.
bool removeEmptyDirectoryWithRetry(const std::wstring &path, bool allowNonEmpty) {
    DWORD lastError = ERROR_SUCCESS;
    for (unsigned attempt = 0; attempt < 600; ++attempt) {
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
        if (RemoveDirectoryW(path.c_str()))
            return true;
        const DWORD error = GetLastError();
        lastError = error;
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return true;
        // A successfully unlinked executable can remain visible briefly while
        // the image section and directory enumeration state drain.  Treating
        // ERROR_DIR_NOT_EMPTY as immediate success left an empty per-package
        // cache directory behind after otherwise successful uninstalls.
        if (error == ERROR_DIR_NOT_EMPTY) {
            if (allowNonEmpty)
                return true;
            Sleep(50);
            continue;
        }
        if (error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION)
            return false;
        Sleep(50);
    }
    return allowNonEmpty && lastError == ERROR_DIR_NOT_EMPTY;
}

struct DirectoryRequest {
    std::wstring path;
    bool allowNonEmpty{false};
};

/// @brief Test whether a cleanup target duplicates an earlier validated target.
/// @param targets Previously accepted absolute paths.
/// @param candidate Candidate path.
/// @return @c true for a case- and separator-insensitive exact match.
bool containsTarget(const std::vector<std::wstring> &targets, const std::wstring &candidate) {
    /// @brief Compare one accepted cleanup target with the candidate path.
    /// @param target Previously accepted absolute path.
    /// @return `true` when `target` denotes the same Windows path as `candidate`.
    return std::any_of(targets.begin(), targets.end(), [&candidate](const std::wstring &target) {
        return zanna::installer::cleanup::pathsEqual(target, candidate);
    });
}

} // namespace

/// @brief Parse cleanup requests and run the detached deletion sequence.
/// @details Accepts one parent PID plus bounded @c /delete, @c /rmdir, and
///          @c /rmdir-if-empty pairs. The WinMain parameters are intentionally
///          unnamed because parsing uses GetCommandLineW for exact Unicode input.
/// @return ERROR_SUCCESS on complete cleanup, otherwise a stable validation,
///         self-delete, parent-wait, or target-cleanup exit code.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return kExitInvalidArguments;
    if (argc == 2 &&
        (lstrcmpiW(argv[1], L"/selftest") == 0 || lstrcmpiW(argv[1], L"/self-test") == 0)) {
        LocalFree(argv);
        return ERROR_SUCCESS;
    }

    DWORD parentId = 0;
    bool sawParent = false;
    std::vector<std::wstring> files;
    std::vector<DirectoryRequest> directories;
    std::vector<std::wstring> targets;
    bool valid = true;
    if (argc < 2 || argc > kMaximumArguments)
        valid = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring option(argv[i]);
        const bool isParent = _wcsicmp(option.c_str(), L"/parent") == 0;
        const bool isDelete = _wcsicmp(option.c_str(), L"/delete") == 0;
        const bool isRemoveDirectory = _wcsicmp(option.c_str(), L"/rmdir") == 0;
        const bool isRemoveIfEmpty = _wcsicmp(option.c_str(), L"/rmdir-if-empty") == 0;
        if ((isParent || isDelete || isRemoveDirectory || isRemoveIfEmpty) && i + 1 < argc) {
            const std::wstring value(argv[++i]);
            if (isParent) {
                std::uint32_t parsed = 0;
                const bool parsedParent =
                    !sawParent && zanna::installer::cleanup::parseProcessId(value, parsed);
                valid = valid && parsedParent;
                if (parsedParent) {
                    parentId = parsed;
                    sawParent = true;
                }
            } else if (!zanna::installer::cleanup::isSafeAbsolutePath(value) ||
                       containsTarget(targets, value)) {
                valid = false;
            } else if (isDelete) {
                if (files.size() >= kMaximumFiles) {
                    valid = false;
                    continue;
                }
                files.push_back(value);
                targets.push_back(value);
            } else {
                if (directories.size() >= kMaximumDirectories) {
                    valid = false;
                    continue;
                }
                directories.push_back({value, isRemoveIfEmpty});
                targets.push_back(value);
            }
        } else {
            valid = false;
        }
    }
    LocalFree(argv);
    if (!valid || !sawParent || parentId == 0 || files.empty())
        return kExitInvalidArguments;
    if (const DWORD selfDeleteError = markSelfForDeletion(); selfDeleteError != ERROR_SUCCESS)
        return static_cast<int>(selfDeleteError);
    if (const DWORD waitError = waitForParent(parentId); waitError != ERROR_SUCCESS)
        return kExitParentWaitFailed;

    bool success = true;
    for (const std::wstring &file : files)
        success = deleteFileWithRetry(file) && success;
    for (const DirectoryRequest &directory : directories)
        success = removeEmptyDirectoryWithRetry(directory.path, directory.allowNonEmpty) && success;
    return success ? ERROR_SUCCESS : kExitTargetCleanupFailed;
}
