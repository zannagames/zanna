//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_dir.c
// Purpose: Directory operations: existence test, create (incl. make-all), remove,
//   recursive remove (with protected-path guard), current-directory get/set,
//   and move/rename. Platform path helpers live in rt_dir_internal.h.
// Key invariants:
//   - Recursive deletion never traverses reparse points and fails closed for protected paths.
//   - Platform-native paths are converted strictly at the runtime boundary.
// Ownership/Lifetime:
//   - Temporary native path buffers and returned runtime strings have explicit ownership.
//   - Recursive helpers release every search handle and child path on all exits.
// Links: src/runtime/io/rt_dir.h,
//        src/runtime/io/rt_dir_internal.h,
//        src/runtime/io/rt_dir_list.c,
//        src/runtime/io/rt_dir_page.cpp
//
//===----------------------------------------------------------------------===//

#include "rt_dir.h"
#include "rt_dir_internal.h"
#include "rt_file_path.h"
#include "rt_path.h"
#include "rt_seq.h"

/// @brief Return 1 if `cpath` is a protected target that must not be deleted.
///
/// Blocks RemoveAll on empty strings, root paths, `"."`, `".."`, the process's
/// current working directory, and an existing ancestor of that directory — all of which would be
/// catastrophic to wipe. Used as a safety gate before recursive deletion.
/// @param cpath Native path proposed as the recursive-deletion root.
/// @return 1 when deletion must be refused; otherwise 0.
static int rt_dir_remove_all_target_is_protected(const char *cpath) {
    if (!cpath || cpath[0] == '\0')
        return 1;

    size_t len = strlen(cpath);
    size_t root_len = rt_dir_root_prefix_len(cpath, len);
    while (len > root_len && rt_dir_is_sep_char(cpath[len - 1]))
        --len;

    if (root_len > 0 && len == root_len)
        return 1;
    if (len == 1 && cpath[0] == '.')
        return 1;
    if (len == 2 && cpath[0] == '.' && cpath[1] == '.')
        return 1;

#ifdef _WIN32
    if (rt_dir_win_path_matches_cwd_or_ancestor(cpath))
        return 1;
#else
    struct stat st;
    if (lstat(cpath, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        char resolved[PATH_MAX];
        if (realpath(cpath, resolved)) {
            if (strcmp(resolved, "/") == 0)
                return 1;
            char cwd[PATH_MAX];
            if (getcwd(cwd, sizeof(cwd))) {
                size_t resolved_len = strlen(resolved);
                size_t cwd_len = strlen(cwd);
                while (resolved_len > 1 && resolved[resolved_len - 1] == '/')
                    --resolved_len;
                while (cwd_len > 1 && cwd[cwd_len - 1] == '/')
                    --cwd_len;
                if (resolved_len == cwd_len && strncmp(resolved, cwd, resolved_len) == 0)
                    return 1;
                if (resolved_len < cwd_len && strncmp(resolved, cwd, resolved_len) == 0 &&
                    cwd[resolved_len] == '/')
                    return 1;
            }
        }
    }
#endif

    return 0;
}

/// @brief Check if a directory exists at the specified path.
///
/// Tests whether a directory exists and is accessible at the given path.
/// This function distinguishes directories from files - a path that points
/// to a regular file will return false.
///
/// **Usage example:**
/// ```
/// ' Check before creating
/// If Not Dir.Exists("output") Then
///     Dir.Make("output")
/// End If
///
/// ' Validate user input
/// Dim folder = InputBox("Enter folder path:")
/// If Dir.Exists(folder) Then
///     ProcessFolder(folder)
/// Else
///     Print "Directory not found: " & folder
/// End If
/// ```
///
/// **Behavior:**
/// - Returns true only for directories, not regular files
/// - Follows symbolic links (checks the target)
/// - Returns false for inaccessible directories (permission denied)
/// - Returns false for non-existent paths
///
/// @param path Directory path to check.
///
/// @return 1 (true) if path is an existing directory, 0 (false) otherwise.
///
/// @note O(1) time complexity (single stat() call).
/// @note Does not create the directory - use rt_dir_make for that.
/// @note Returns false rather than trapping on errors.
///
/// @see rt_dir_make For creating directories
/// @see rt_file_exists For checking file existence
int64_t rt_dir_exists(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath)
        return 0;

#ifdef _WIN32
    return rt_dir_win_exists_dir(cpath);
#else
    struct stat st;
    if (stat(cpath, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
#endif
}

/// @brief Create a single directory at the specified path.
///
/// Creates a new directory at the given path. The parent directory must
/// already exist. If the directory already exists, the function succeeds
/// silently (idempotent operation).
///
/// **Usage example:**
/// ```
/// ' Create output directory
/// Dir.Make("output")
///
/// ' Create in existing parent
/// Dir.Make("existing_parent/new_folder")
///
/// ' Safe creation (already exists is OK)
/// Dir.Make("logs")  ' Creates if needed
/// Dir.Make("logs")  ' Succeeds again (no error)
/// ```
///
/// **Permission mode (Unix):**
/// New directories are created with mode 0755:
/// - Owner: read, write, execute (rwx)
/// - Group: read, execute (r-x)
/// - Others: read, execute (r-x)
///
/// **Common errors:**
/// | Error                | Cause                                    |
/// |----------------------|------------------------------------------|
/// | Invalid path         | NULL or malformed path string            |
/// | Parent not found     | Parent directory doesn't exist           |
/// | Permission denied    | No write permission in parent directory  |
/// | Disk full            | No space available on filesystem         |
///
/// @param path Path where the directory should be created.
///
/// @note O(1) time complexity.
/// @note Traps on failure (except when directory already exists).
/// @note Use rt_dir_make_all to create parent directories automatically.
/// @note Does not modify existing directories.
///
/// @see rt_dir_make_all For creating nested directories
/// @see rt_dir_exists For checking if directory exists
/// @see rt_dir_remove For removing directories
void rt_dir_make(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath) {
        rt_dir_trap_domain("Dir.Make: invalid path");
        return;
    }

#ifdef _WIN32
    if (!rt_dir_win_create_dir(cpath)) {
        rt_dir_trap_io("Dir.Make: failed to create directory");
    }
#else
    if (mkdir(cpath, 0755) != 0) {
        if (errno == EEXIST && rt_dir_posix_path_is_dir(cpath))
            return;
        rt_dir_trap_io("Dir.Make: failed to create directory");
    }
#endif
}

/// @brief Create a directory and all missing parent directories.
///
/// Creates the target directory and any intermediate directories that don't
/// exist along the path. This is similar to the Unix `mkdir -p` command.
/// If the full path already exists, the function succeeds silently.
///
/// **Usage example:**
/// ```
/// ' Create nested directory structure
/// Dir.MakeAll("output/reports/2024/january")
/// ' Creates: output/, output/reports/, output/reports/2024/,
/// '          output/reports/2024/january/
///
/// ' Create path with existing parents (only missing parts created)
/// Dir.MakeAll("existing/new/deep/path")
///
/// ' Safe to call multiple times
/// Dir.MakeAll("logs/app")
/// Dir.MakeAll("logs/app")  ' No error
/// ```
///
/// **Creation order:**
/// ```
/// Path: "a/b/c/d"
///
/// Step 1: Check/Create "a"
/// Step 2: Check/Create "a/b"
/// Step 3: Check/Create "a/b/c"
/// Step 4: Check/Create "a/b/c/d"
/// ```
///
/// **Permission mode (Unix):**
/// Each created directory uses mode 0755 (rwxr-xr-x).
///
/// **Edge cases:**
/// - Trailing slashes are automatically removed
/// - Empty path is a no-op
/// - Absolute paths (starting with / or drive letter) work correctly
///
/// @param path Full path to create, including all intermediate directories.
///
/// @note O(n) time complexity where n is the path depth.
/// @note Traps on failure (permission denied, disk full, etc.).
/// @note Idempotent - safe to call multiple times with same path.
/// @note Handles both forward and backslashes as separators.
///
/// @see rt_dir_make For creating a single directory
/// @see rt_dir_remove_all For recursively removing directories
void rt_dir_make_all(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath) {
        rt_dir_trap_domain("Dir.MakeAll: invalid path");
        return;
    }

    size_t len = strlen(cpath);
    if (len == 0)
        return;

    // Make a mutable copy
    char *tmp = (char *)malloc(len + 1);
    if (!tmp) {
        rt_dir_trap_runtime("Dir.MakeAll: out of memory");
        return;
    }
    memcpy(tmp, cpath, len + 1);
    size_t root_len = rt_dir_root_prefix_len(tmp, len);

    // Remove trailing separators
    while (len > root_len && rt_dir_is_sep_char(tmp[len - 1])) {
        tmp[--len] = '\0';
    }
    if (len == 0 || (root_len > 0 && len == root_len)) {
        free(tmp);
        return;
    }

    // Create each level
    for (char *p = tmp + root_len; *p; p++) {
        if (rt_dir_is_sep_char(*p)) {
            char sep = *p;
            *p = '\0';

            // Check if this level exists
#ifdef _WIN32
            if (!rt_dir_win_exists_dir(tmp)) {
                if (!rt_dir_win_create_dir(tmp)) {
                    free(tmp);
                    rt_dir_trap_io("Dir.MakeAll: failed to create intermediate directory");
                    return;
                }
            }
#else
            struct stat st;
            if (stat(tmp, &st) == 0) {
                if (!S_ISDIR(st.st_mode)) {
                    free(tmp);
                    rt_dir_trap_io("Dir.MakeAll: path component exists and is not a directory");
                    return;
                }
            } else {
                if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                    free(tmp);
                    rt_dir_trap_io("Dir.MakeAll: failed to create intermediate directory");
                    return;
                }
                if (!rt_dir_posix_path_is_dir(tmp)) {
                    free(tmp);
                    rt_dir_trap_io("Dir.MakeAll: path component exists and is not a directory");
                    return;
                }
            }
#endif

            *p = sep;
        }
    }

    // Create the final directory
#ifdef _WIN32
    if (!rt_dir_win_exists_dir(tmp) && !rt_dir_win_create_dir(tmp)) {
        free(tmp);
        rt_dir_trap_io("Dir.MakeAll: failed to create directory");
        return;
    }
#else
    struct stat st;
    if (stat(tmp, &st) == 0) {
        if (!S_ISDIR(st.st_mode)) {
            free(tmp);
            rt_dir_trap_io("Dir.MakeAll: path exists and is not a directory");
            return;
        }
    } else {
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            free(tmp);
            rt_dir_trap_io("Dir.MakeAll: failed to create directory");
            return;
        }
        if (!rt_dir_posix_path_is_dir(tmp)) {
            free(tmp);
            rt_dir_trap_io("Dir.MakeAll: path exists and is not a directory");
            return;
        }
    }
#endif

    free(tmp);
}

/// @brief Remove an empty directory.
///
/// Deletes a directory that contains no files or subdirectories. The directory
/// must be empty - use rt_dir_remove_all to delete directories with contents.
///
/// **Usage example:**
/// ```
/// ' Remove temporary directory after use
/// Dir.Remove("temp_work")
///
/// ' Clean up after processing
/// For Each file In Dir.Files("staging")
///     File.Delete(Path.Join("staging", file))
/// Next
/// Dir.Remove("staging")  ' Now empty, can remove
/// ```
///
/// **Requirements:**
/// - Directory must exist
/// - Directory must be empty (no files or subdirectories)
/// - Caller must have write permission to parent directory
/// - Directory must not be in use (current working directory, open handles)
///
/// **Common errors:**
/// | Error                | Cause                                    |
/// |----------------------|------------------------------------------|
/// | Invalid path         | NULL or malformed path string            |
/// | Directory not empty  | Contains files or subdirectories         |
/// | Permission denied    | No write permission to parent directory  |
/// | Directory in use     | Is current working directory or has open handles |
///
/// @param path Path to the empty directory to remove.
///
/// @note O(1) time complexity.
/// @note Traps on failure.
/// @note Does not remove parent directories (even if empty after removal).
///
/// @see rt_dir_remove_all For removing directories with contents
/// @see rt_dir_make For creating directories
/// @see rt_dir_exists For checking if directory exists
void rt_dir_remove(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath) {
        rt_dir_trap_domain("Dir.Remove: invalid path");
        return;
    }

#ifdef _WIN32
    if (!rt_dir_win_remove_dir(cpath)) {
        rt_dir_trap_io("Dir.Remove: failed to remove directory");
    }
#else
    if (rmdir(cpath) != 0) {
        rt_dir_trap_io("Dir.Remove: failed to remove directory");
    }
#endif
}

#if !defined(_WIN32)
/// @brief Open one descriptor-relative directory without following symlinks.
/// @details Validates the child with `fstatat(AT_SYMLINK_NOFOLLOW)`, opens it
/// with directory/no-follow flags when available, then validates the opened
/// descriptor again.
/// @param parent_fd Trusted parent-directory descriptor.
/// @param name Single child entry name.
/// @return Open child-directory descriptor, or -1 with `errno` describing the
/// failure.
static int rt_dir_posix_open_dir_at(int parent_fd, const char *name) {
    struct stat lst;
    if (fstatat(parent_fd, name, &lst, AT_SYMLINK_NOFOLLOW) != 0)
        return -1;
    if (S_ISLNK(lst.st_mode)) {
        errno = ELOOP;
        return -1;
    }
    if (!S_ISDIR(lst.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = openat(parent_fd, name, flags);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
            close(fd);
            errno = ENOTDIR;
            return -1;
        }
    }
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    if (fd >= 0) {
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
#endif
    return fd;
}

/// @brief Recursively remove one descriptor-relative filesystem entry.
/// @details Directories are traversed through verified descriptors; symlinks
/// and other non-directories are unlinked without following them. Missing
/// entries are treated as success.
/// @param parent_fd Descriptor for the trusted parent directory.
/// @param name Child entry to remove.
/// @return 1 when the entry is absent or completely removed; otherwise 0.
static int rt_dir_remove_all_at(int parent_fd, const char *name) {
    int fd = rt_dir_posix_open_dir_at(parent_fd, name);
    if (fd < 0) {
        int err = errno;
        if (err == ENOENT)
            return 1;
        if (err == ENOTDIR || err == ELOOP) {
            return unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT;
        }
        struct stat st;
        if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0 && !S_ISDIR(st.st_mode))
            return unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT;
        return 0;
    }

    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return 0;
    }

    int ok = 1;
    int dir_fd = dirfd(dir);
    struct dirent *ent;
    for (;;) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            if (errno != 0)
                ok = 0;
            break;
        }
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (!rt_dir_remove_all_at(dir_fd, ent->d_name))
            ok = 0;
    }

    if (closedir(dir) != 0)
        ok = 0;
    if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT)
        ok = 0;
    return ok;
}

/// @brief Duplicate a POSIX path after removing redundant trailing separators.
/// @param path NUL-terminated path.
/// @return Heap-allocated normalized copy owned by the caller, or `NULL` for
/// empty/allocation-invalid input.
static char *rt_dir_strip_trailing_seps_dup(const char *path) {
    size_t len = strlen(path);
    while (len > 1 && rt_dir_is_sep_char(path[len - 1]))
        len--;
    if (len == 0)
        return NULL;
    char *copy = (char *)malloc(len + 1);
    if (!copy)
        return NULL;
    memcpy(copy, path, len);
    copy[len] = '\0';
    return copy;
}

/// @brief Remove a POSIX tree through descriptor-relative, no-follow traversal.
/// @details Opens the target's parent once, then delegates to
/// rt_dir_remove_all_at(). A top-level symlink is unlinked directly and an
/// already-missing target succeeds.
/// @param cpath Native path already approved by the protected-target guard.
/// @return 1 on complete removal or prior absence; otherwise 0.
static int rt_dir_remove_all_posix_safe(const char *cpath) {
    char *path = rt_dir_strip_trailing_seps_dup(cpath);
    if (!path)
        return 0;

    struct stat root_st;
    if (lstat(path, &root_st) != 0) {
        int ok = errno == ENOENT ? 1 : 0;
        free(path);
        return ok;
    }
    if (S_ISLNK(root_st.st_mode)) {
        int ok = unlink(path) == 0 || errno == ENOENT;
        free(path);
        return ok;
    }
    if (!S_ISDIR(root_st.st_mode)) {
        free(path);
        return 0;
    }

    char *last = strrchr(path, '/');
    const char *parent_path = ".";
    const char *leaf = path;
    if (last) {
        leaf = last + 1;
        if (last == path) {
            parent_path = "/";
        } else {
            *last = '\0';
            parent_path = path;
        }
    }
    if (!leaf || leaf[0] == '\0') {
        free(path);
        return 0;
    }

    int flags = O_RDONLY;
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int parent_fd = open(parent_path, flags);
    if (parent_fd < 0) {
        free(path);
        return errno == ENOENT ? 1 : 0;
    }
#if defined(FD_CLOEXEC) && !defined(O_CLOEXEC)
    int fd_flags = fcntl(parent_fd, F_GETFD);
    if (fd_flags >= 0)
        (void)fcntl(parent_fd, F_SETFD, fd_flags | FD_CLOEXEC);
#endif

    int ok = rt_dir_remove_all_at(parent_fd, leaf);
    if (close(parent_fd) != 0)
        ok = 0;
    free(path);
    return ok;
}
#endif

/// @brief Recursively delete a directory tree rooted at the C-string `cpath`.
///
/// Cross-platform recursive delete: Windows uses `FindFirstFileW` /
/// `FindNextFileW` with reparse-point awareness; POSIX uses `openat`,
/// `fstatat`, and descriptor-relative traversal. Neither path follows a
/// symlink/reparse-point child into an unrelated tree.
/// @param cpath Native path already approved by the protected-target guard.
/// @return 1 on complete removal or prior absence; otherwise 0.
static int rt_dir_remove_all_cpath(const char *cpath) {
#ifdef _WIN32
    wchar_t *dir_path = rt_dir_win_prepare_path(cpath);
    wchar_t *pattern = rt_dir_win_make_pattern(cpath);
    if (!dir_path || !pattern) {
        free(dir_path);
        free(pattern);
        return 0;
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        int ok = 1;
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            ok = 1;
        } else if (!rt_dir_win_remove_directory_w(dir_path)) {
            ok = 0;
        }
        free(pattern);
        free(dir_path);
        return ok;
    }

    int ok = 1;
    do {
        if (rt_dir_win_is_dot_name(fd.cFileName))
            continue;

        wchar_t *full_path = rt_dir_win_join(dir_path, fd.cFileName);
        if (!full_path) {
            ok = 0;
            continue;
        }

        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                if (!rt_dir_win_remove_directory_w(full_path))
                    ok = 0;
            } else {
                rt_string sub = NULL;
                if (!rt_dir_win_wide_to_string_checked(full_path, &sub)) {
                    ok = 0;
                } else {
                    const char *sub_cpath = rt_string_cstr(sub);
                    if (!sub_cpath || !sub_cpath[0] || !rt_dir_remove_all_cpath(sub_cpath))
                        ok = 0;
                    rt_string_unref(sub);
                }
            }
        } else {
            if (!rt_dir_win_delete_file_w(full_path) && GetLastError() != ERROR_FILE_NOT_FOUND)
                ok = 0;
        }

        free(full_path);
    } while (FindNextFileW(h, &fd));

    DWORD find_err = GetLastError();
    if (find_err != ERROR_NO_MORE_FILES)
        ok = 0;
    FindClose(h);
    if (!rt_dir_win_remove_directory_w(dir_path))
        ok = 0;
    free(pattern);
    free(dir_path);
    return ok;
#else
    return rt_dir_remove_all_posix_safe(cpath);
#endif
}

/// @brief Recursively remove a directory and all its contents.
///
/// Deletes a directory along with all files and subdirectories it contains.
/// This is equivalent to the Unix `rm -rf` command. Use with caution as this
/// operation is irreversible.
///
/// **Usage example:**
/// ```
/// ' Clean up temporary build directory
/// Dir.RemoveAll("build/temp")
///
/// ' Remove project and all contents
/// If Confirm("Delete project folder?") Then
///     Dir.RemoveAll("my_project")
/// End If
///
/// ' Safe cleanup pattern
/// If Dir.Exists("cache") Then
///     Dir.RemoveAll("cache")
/// End If
/// ```
///
/// **Deletion order (depth-first):**
/// ```
/// project/
/// ├── src/
/// │   ├── main.bas  ← deleted first (file)
/// │   └── utils.bas ← deleted second (file)
/// │   └── (src/ deleted after contents)
/// └── docs/
///     └── readme.txt ← deleted
///     └── (docs/ deleted after contents)
/// └── (project/ deleted last)
/// ```
///
/// **Warning:** This function:
/// - Permanently deletes files (no recycle bin)
/// - Cannot be undone
/// - Removes encountered symlinks/reparse points themselves without following their targets
/// - Traps if any child cannot be removed
///
/// **Edge cases:**
/// - Non-existent directory: silently succeeds
/// - Empty directory: removes it directly
/// - Read-only files: deletion may trap on some platforms
///
/// @param path Path to the directory to remove recursively.
///
/// @note O(n) time complexity where n is total number of files/directories.
/// @note Does not follow symbolic links into other directories.
/// @note Traps on deletion failures except for an already-missing top-level path.
///
/// @see rt_dir_remove For removing empty directories only
/// @see rt_dir_make_all For creating nested directories
void rt_dir_remove_all(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath) {
        rt_dir_trap_domain("Dir.RemoveAll: invalid path");
        return;
    }
    if (rt_dir_remove_all_target_is_protected(cpath)) {
        rt_dir_trap_domain("Dir.RemoveAll: refusing to remove protected directory");
        return;
    }

    if (!rt_dir_remove_all_cpath(cpath))
        rt_dir_trap_io("Dir.RemoveAll: failed to remove directory tree");
}

/// @brief Get the current working directory path.
///
/// Returns the absolute path of the process's current working directory.
/// This is the directory used as the base for resolving relative paths in
/// file operations.
///
/// **Usage example:**
/// ```
/// ' Display current location
/// Print "Working in: " & Dir.Current()
///
/// ' Save and restore working directory
/// Dim original = Dir.Current()
/// Dir.SetCurrent("subdir")
/// ' ... do work in subdir ...
/// Dir.SetCurrent(original)  ' Restore
///
/// ' Build absolute path from relative
/// Dim absPath = Path.Join(Dir.Current(), "file.txt")
/// ```
///
/// **Return format:**
/// - Windows: `C:\Users\name\project`
/// - Unix: `/home/user/project`
///
/// **Important notes:**
/// - The current directory is process-wide, not per-thread
/// - Changing directories in one thread affects all threads
/// - Relative paths are resolved against this directory
///
/// @return Absolute path to the current working directory.
///
/// @note O(1) time complexity.
/// @note Traps if the current directory cannot be determined.
/// @note Returns newly allocated string (caller manages memory).
/// @note Windows uses wide Win32 APIs and supports extended-length paths.
///
/// @see rt_dir_set_current For changing the working directory
/// @see rt_path_absolute For converting relative paths to absolute
rt_string rt_dir_current(void) {
#ifdef _WIN32
    wchar_t *wide = rt_dir_win_current_directory_alloc();
    if (!wide) {
        rt_dir_trap_io("Dir.Current: failed to get current directory");
        return rt_str_empty();
    }

    rt_string result = NULL;
    if (!rt_dir_win_wide_to_string_checked(wide, &result)) {
        free(wide);
        rt_dir_trap_io("Dir.Current: failed to convert current directory");
        return rt_str_empty();
    }
    free(wide);
    return result;
#else
    // Unix: allocate dynamically so very long cwd paths work.
    char *buffer = getcwd(NULL, 0);
    if (buffer == NULL) {
        rt_dir_trap_io("Dir.Current: failed to get current directory");
        return rt_str_empty();
    }
    rt_string result = rt_string_from_bytes(buffer, strlen(buffer));
    free(buffer);
    return result;
#endif
}

/// @brief Change the current working directory.
///
/// Changes the process's current working directory to the specified path.
/// After this call, relative paths in file operations will be resolved
/// relative to the new directory.
///
/// **Usage example:**
/// ```
/// ' Change to project directory
/// Dir.SetCurrent("/home/user/project")
///
/// ' Now relative paths work from project/
/// Dim files = Dir.Files("src")  ' Lists project/src/
///
/// ' Use with pattern: save, change, restore
/// Dim saved = Dir.Current()
/// Dir.SetCurrent("build")
/// RunBuildCommands()
/// Dir.SetCurrent(saved)
/// ```
///
/// **Valid paths:**
/// - Absolute paths: `/home/user/dir` or `C:\Users\dir`
/// - Relative paths: `subdir` or `../sibling`
/// - Home expansion: NOT supported (use full path)
///
/// **Common errors:**
/// | Error                | Cause                              |
/// |----------------------|------------------------------------|
/// | Invalid path         | NULL or malformed path string      |
/// | Directory not found  | Path does not exist                |
/// | Not a directory      | Path points to a file              |
/// | Permission denied    | No execute permission on directory |
///
/// **Warning:** Changing the current directory affects ALL threads in the
/// process. Avoid changing directories in multi-threaded applications or
/// use proper synchronization.
///
/// @param path Path to the new working directory.
///
/// @note O(1) time complexity.
/// @note Traps on failure (directory not found, permission denied).
/// @note Process-wide effect - affects all threads.
///
/// @see rt_dir_current For getting the current working directory
/// @see rt_dir_exists For checking if directory exists before changing
void rt_dir_set_current(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath) {
        rt_dir_trap_domain("Dir.SetCurrent: invalid path");
        return;
    }

#ifdef _WIN32
    wchar_t *wide = rt_dir_win_prepare_path(cpath);
    if (!wide) {
        rt_dir_trap_io("Dir.SetCurrent: failed to change directory");
        return;
    }
    if (!SetCurrentDirectoryW(wide)) {
        free(wide);
        rt_dir_trap_io("Dir.SetCurrent: failed to change directory");
        return;
    }
    free(wide);
#else
    if (chdir(cpath) != 0) {
        rt_dir_trap_io("Dir.SetCurrent: failed to change directory");
    }
#endif
}

/// @brief Move or rename a directory.
///
/// Moves a directory from one location to another, or renames it. This
/// operation is atomic on the same filesystem - either the entire directory
/// is moved or the operation fails (no partial moves).
///
/// **Usage example:**
/// ```
/// ' Rename a directory
/// Dir.Move("old_name", "new_name")
///
/// ' Move to different location
/// Dir.Move("project/temp", "archive/temp_backup")
///
/// ' Move with rename
/// Dir.Move("downloads/data", "processed/data_2024")
/// ```
///
/// **Operation modes:**
/// | Source           | Destination      | Result                      |
/// |------------------|------------------|-----------------------------|
/// | `dir1`           | `dir2`           | Rename dir1 to dir2         |
/// | `path/dir1`      | `other/dir1`     | Move to other location      |
/// | `path/dir1`      | `other/dir2`     | Move and rename             |
///
/// **Requirements:**
/// - Source directory must exist
/// - Destination must not exist (won't overwrite)
/// - Parent of destination must exist
/// - Both paths should be on same filesystem for atomic move
///
/// **Common errors:**
/// | Error                | Cause                                    |
/// |----------------------|------------------------------------------|
/// | Invalid path         | NULL or malformed source/dest path       |
/// | Source not found     | Source directory doesn't exist           |
/// | Dest already exists  | Destination path already exists          |
/// | Permission denied    | No write permission to source or dest    |
/// | Cross-device         | Moving across filesystems (may fail)     |
///
/// **Cross-filesystem moves:**
/// Some platforms don't support atomic moves across filesystems. In such
/// cases, use a copy-then-delete pattern instead:
/// ```
/// ' Manual cross-filesystem move
/// CopyDir(src, dst)
/// Dir.RemoveAll(src)
/// ```
///
/// @param src Source directory path.
/// @param dst Destination directory path.
///
/// @note O(1) time complexity for same-filesystem moves.
/// @note Traps on failure.
/// @note Uses the platform rename/move primitive internally (`MoveFileExW` on Windows,
///       `rename()` on Unix).
/// @note Not atomic across different filesystems.
///
/// @see rt_dir_make For creating directories
/// @see rt_dir_remove For removing directories
/// @see rt_file_move For moving files
void rt_dir_move(rt_string src, rt_string dst) {
    const char *csrc = NULL;
    const char *cdst = NULL;

    if (!rt_file_path_from_vstr(src, &csrc) || !csrc) {
        rt_dir_trap_domain("Dir.Move: invalid source path");
        return;
    }

    if (!rt_file_path_from_vstr(dst, &cdst) || !cdst) {
        rt_dir_trap_domain("Dir.Move: invalid destination path");
        return;
    }

#ifdef _WIN32
    if (!rt_dir_win_move_dir(csrc, cdst)) {
        rt_dir_trap_io("Dir.Move: failed to move directory");
    }
#else
    struct stat src_st;
    if (stat(csrc, &src_st) != 0 || !S_ISDIR(src_st.st_mode)) {
        rt_dir_trap_not_found("Dir.Move: source directory not found");
        return;
    }
    struct stat dst_st;
    if (lstat(cdst, &dst_st) == 0) {
        rt_dir_trap_io("Dir.Move: destination already exists");
        return;
    }
    if (errno != ENOENT) {
        rt_dir_trap_io("Dir.Move: failed to check destination");
        return;
    }

    if (rename(csrc, cdst) != 0) {
        rt_dir_trap_io("Dir.Move: failed to move directory");
    }
#endif
}
