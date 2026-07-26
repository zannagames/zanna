//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/io/rt_dir_list.c
// Purpose: Immediate-child directory enumeration for all entries, files-only,
//   or directories-only results, including trapping and empty-on-error Seq
//   variants. Platform path helpers live in rt_dir_internal.h.
// Key invariants:
//   - Synthetic dot entries are excluded and conversion failures never fabricate names.
//   - A failed platform scan does not expose a partially populated result.
// Ownership/Lifetime:
//   - Returned sequences own their retained runtime-string elements.
//   - Native directory handles and path buffers are released on every exit.
// Links: src/runtime/io/rt_dir.h,
//        src/runtime/io/rt_dir_internal.h,
//        src/runtime/io/rt_dir.c,
//        src/runtime/io/rt_dir_page.cpp
//
//===----------------------------------------------------------------------===//

#include "rt_dir.h"
#include "rt_dir_internal.h"
#include "rt_file_path.h"
#include "rt_path.h"
#include "rt_platform.h"
#include "rt_seq.h"

#if RT_PLATFORM_WINDOWS
/// @brief Convert and append one Win32 directory name without fabricating an empty entry.
/// @param result Element-owning runtime Seq receiving the converted name.
/// @param wide_name NUL-terminated UTF-16 directory entry name.
/// @return 1 after a successful strict conversion and append; otherwise 0.
static int rt_dir_win_push_name(void *result, const wchar_t *wide_name) {
    rt_string name = NULL;
    if (!result || !rt_dir_win_wide_to_string_checked(wide_name, &name))
        return 0;
    rt_seq_push(result, (void *)name);
    rt_string_unref(name);
    return 1;
}
#endif

/// @brief List all entries (files and subdirectories) in a directory.
///
/// Returns a sequence containing the names of all files and subdirectories
/// in the specified directory. The special entries "." and ".." are excluded.
/// Entry names are returned without the directory path prefix.
///
/// **Usage example:**
/// ```
/// ' List all items in current directory
/// Dim entries = Dir.List(".")
/// For Each entry In entries
///     Print entry
/// Next
///
/// ' Process each entry
/// Dim items = Dir.List("downloads")
/// Print "Found " & items.Len() & " items"
///
/// ' Check if directory is empty
/// If Dir.List("folder").Len() = 0 Then
///     Print "Directory is empty"
/// End If
/// ```
///
/// **Return format:**
/// ```
/// Directory: project/
/// ├── main.bas
/// ├── utils.bas
/// └── docs/
///
/// Returns: ["main.bas", "utils.bas", "docs"]
/// (Names only, no "project/" prefix)
/// ```
///
/// **Ordering:**
/// The order of entries is filesystem-dependent and not guaranteed:
/// - May be alphabetical on some systems
/// - May be insertion order on others
/// - Use sorting if consistent order is needed
///
/// @param path Directory path to list.
///
/// @return Seq containing entry names as strings. Returns empty Seq on error.
///
/// @note O(n) time complexity where n is number of entries.
/// @note Returns empty sequence on error (does not trap).
/// @note Does not recurse into subdirectories.
/// @note Hidden files (starting with .) are included.
///
/// @see rt_dir_files For listing only files
/// @see rt_dir_dirs For listing only subdirectories
/// @see rt_dir_entries_seq For trapping version
void *rt_dir_list(rt_string path) {
    void *result = rt_seq_new();
    rt_seq_set_owns_elements(result, 1);

    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath)
        return result;

#if RT_PLATFORM_WINDOWS
    wchar_t *pattern = rt_dir_win_make_pattern(cpath);
    if (!pattern)
        return result;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        free(pattern);
        return result;
    }

    int scan_ok = 1;
    for (;;) {
        if (!rt_dir_win_is_dot_name(fd.cFileName)) {
            if (!rt_dir_win_push_name(result, fd.cFileName)) {
                scan_ok = 0;
                break;
            }
        }
        if (!FindNextFileW(h, &fd)) {
            if (GetLastError() != ERROR_NO_MORE_FILES)
                scan_ok = 0;
            break;
        }
    }

    if (!scan_ok)
        rt_seq_clear(result);
    FindClose(h);
    free(pattern);
#else
    // Unix: use POSIX directory APIs.
    DIR *dir = opendir(cpath);
    if (!dir)
        return result;

    int ok = 1;
    struct dirent *ent;
    for (;;) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            if (errno != 0)
                ok = 0;
            break;
        }
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            rt_string name = rt_string_from_bytes(ent->d_name, strlen(ent->d_name));
            rt_seq_push(result, (void *)name);
            rt_string_unref(name);
        }
    }

    if (closedir(dir) != 0)
        ok = 0;
    if (!ok)
        rt_seq_clear(result);
#endif

    return result;
}

/// @brief List all entries in a directory as a Zanna.Collections.Seq.
///
/// Wrapper for rt_dir_list that returns entries as a Zanna.Collections.Seq.
/// Preserves the same behavior: entry name formatting, enumeration order,
/// and empty-on-error handling.
///
/// **Usage example:**
/// ```
/// Dim entries = Dir.ListSeq("src")
/// Print "Directory contains " & entries.Len() & " items"
/// ```
///
/// @param path Directory path to list.
///
/// @return Seq containing entry names. Returns empty Seq on error.
///
/// @note Delegates to rt_dir_list.
/// @note Same behavior as rt_dir_list.
///
/// @see rt_dir_list For implementation details
void *rt_dir_list_seq(rt_string path) {
    return rt_dir_list(path);
}

/// @brief List all directory entries with error trapping on failure.
///
/// Similar to rt_dir_list but traps (terminates with error) if the directory
/// does not exist or cannot be read. Use this when directory existence is
/// required, not optional.
///
/// **Usage example:**
/// ```
/// ' Will trap if "config" doesn't exist
/// Dim configs = Dir.Entries("config")
///
/// ' Use when directory must exist
/// Dim sources = Dir.Entries("src")
/// For Each src In sources
///     Compile(src)
/// Next
/// ```
///
/// **Difference from rt_dir_list:**
/// | Function       | Missing Dir | Permission Denied |
/// |----------------|-------------|-------------------|
/// | Dir.List       | Empty Seq   | Empty Seq         |
/// | Dir.Entries    | Trap        | Trap              |
///
/// **When to use which:**
/// - Dir.List: Directory may or may not exist, handle both cases
/// - Dir.Entries: Directory must exist, trap on absence is appropriate
///
/// @param path Directory path to list.
///
/// @return Zanna.Collections.Seq containing entry names as strings.
///
/// @note O(n) time complexity where n is number of entries.
/// @note Traps if directory doesn't exist or can't be opened.
/// @note Does not sort entries - order is filesystem-dependent.
///
/// @see rt_dir_list For non-trapping version
/// @see rt_dir_files For listing only files
/// @see rt_dir_dirs For listing only subdirectories
void *rt_dir_entries_seq(rt_string path) {
    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath)
        rt_dir_trap_domain("Zanna.IO.Dir.Entries: invalid directory path");

#if RT_PLATFORM_WINDOWS
    if (!rt_dir_win_exists_dir(cpath))
        rt_dir_trap_not_found("Zanna.IO.Dir.Entries: directory not found");
#else
    struct stat st;
    if (stat(cpath, &st) != 0)
        rt_dir_trap_not_found("Zanna.IO.Dir.Entries: directory not found");
    if (!S_ISDIR(st.st_mode))
        rt_dir_trap_not_found("Zanna.IO.Dir.Entries: directory not found");
#endif

    void *result = rt_seq_new();
    if (!result)
        return NULL;
    rt_seq_set_owns_elements(result, 1);

#if RT_PLATFORM_WINDOWS
    wchar_t *pattern = rt_dir_win_make_pattern(cpath);
    if (!pattern) {
        rt_dir_trap_io("Zanna.IO.Dir.Entries: failed to open directory");
        return result;
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND) {
            free(pattern);
            return result;
        }
        free(pattern);
        rt_dir_trap_io("Zanna.IO.Dir.Entries: failed to open directory");
        return result;
    }

    int scan_ok = 1;
    for (;;) {
        if (!rt_dir_win_is_dot_name(fd.cFileName)) {
            if (!rt_dir_win_push_name(result, fd.cFileName)) {
                scan_ok = 0;
                break;
            }
        }
        if (!FindNextFileW(h, &fd)) {
            if (GetLastError() != ERROR_NO_MORE_FILES)
                scan_ok = 0;
            break;
        }
    }

    if (!scan_ok) {
        FindClose(h);
        free(pattern);
        rt_seq_clear(result);
        rt_dir_trap_io("Zanna.IO.Dir.Entries: failed to read directory");
        return result;
    }
    FindClose(h);
    free(pattern);
#else
    // Unix: use POSIX directory APIs.
    DIR *dir = opendir(cpath);
    if (!dir)
        rt_dir_trap_io("Zanna.IO.Dir.Entries: failed to open directory");

    struct dirent *ent;
    for (;;) {
        errno = 0;
        ent = readdir(dir);
        if (!ent) {
            if (errno != 0) {
                (void)closedir(dir);
                rt_dir_trap_io("Zanna.IO.Dir.Entries: failed to read directory");
            }
            break;
        }
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            rt_string name = rt_string_from_bytes(ent->d_name, strlen(ent->d_name));
            rt_seq_push(result, (void *)name);
            rt_string_unref(name);
        }
    }

    if (closedir(dir) != 0)
        rt_dir_trap_io("Zanna.IO.Dir.Entries: failed to close directory");
#endif

    return result;
}

/// @brief List only regular files in a directory (excludes subdirectories).
///
/// Returns a sequence containing the names of all regular files in the
/// specified directory. Subdirectories, symbolic links, and special files
/// are excluded from the results.
///
/// **Usage example:**
/// ```
/// ' List all files to process
/// Dim files = Dir.Files("input")
/// For Each file In files
///     ProcessFile(Path.Join("input", file))
/// Next
///
/// ' Count source files
/// Dim sources = Dir.Files("src")
/// Print "Found " & sources.Len() & " source files"
///
/// ' Filter by extension (manual)
/// For Each f In Dir.Files("docs")
///     If Path.Ext(f) = ".txt" Then
///         Print f
///     End If
/// Next
/// ```
///
/// **What is included vs excluded:**
/// | Entry Type       | Included? |
/// |------------------|-----------|
/// | Regular files    | Yes       |
/// | Subdirectories   | No        |
/// | Symbolic links   | No*       |
/// | Device files     | No        |
/// | Named pipes      | No        |
///
/// *On Unix, symlinks to files are excluded; on Windows behavior varies.
///
/// @param path Directory path to scan.
///
/// @return Seq containing file names as strings. Returns empty Seq on error.
///
/// @note O(n) time complexity where n is number of entries.
/// @note Returns empty sequence on error (does not trap).
/// @note Does not recurse into subdirectories.
/// @note File names are returned without directory prefix.
///
/// @see rt_dir_dirs For listing only subdirectories
/// @see rt_dir_list For listing all entries
/// @see rt_dir_files_seq For wrapper version
void *rt_dir_files(rt_string path) {
    void *result = rt_seq_new();
    rt_seq_set_owns_elements(result, 1);

    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath)
        return result;

#if RT_PLATFORM_WINDOWS
    wchar_t *pattern = rt_dir_win_make_pattern(cpath);
    if (!pattern)
        return result;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        free(pattern);
        return result;
    }

    int scan_ok = 1;
    for (;;) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (!rt_dir_win_push_name(result, fd.cFileName)) {
                scan_ok = 0;
                break;
            }
        }
        if (!FindNextFileW(h, &fd)) {
            if (GetLastError() != ERROR_NO_MORE_FILES)
                scan_ok = 0;
            break;
        }
    }

    if (!scan_ok)
        rt_seq_clear(result);
    FindClose(h);
    free(pattern);
#else
    // Unix: use POSIX directory APIs.
    DIR *dir = opendir(cpath);
    if (!dir)
        return result;

    int ok = 1;
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

        char *full = rt_dir_join_child_alloc(cpath, ent->d_name);
        if (!full) {
            ok = 0;
            break;
        }
        struct stat st;
        if (lstat(full, &st) != 0) {
            ok = 0;
            free(full);
            break;
        }
        if (S_ISREG(st.st_mode)) {
            rt_string name = rt_string_from_bytes(ent->d_name, strlen(ent->d_name));
            rt_seq_push(result, (void *)name);
            rt_string_unref(name);
        }
        free(full);
    }

    if (closedir(dir) != 0)
        ok = 0;
    if (!ok)
        rt_seq_clear(result);
#endif

    return result;
}

/// @brief List only files in a directory as a Zanna.Collections.Seq.
///
/// Wrapper for rt_dir_files that returns file names as a Zanna.Collections.Seq.
/// Preserves the same filtering behavior (only regular files, no directories).
///
/// **Usage example:**
/// ```
/// Dim files = Dir.FilesSeq("data")
/// Print "Found " & files.Len() & " data files"
/// ```
///
/// @param path Directory path to scan.
///
/// @return Seq containing file names. Returns empty Seq on error.
///
/// @note Delegates to rt_dir_files.
///
/// @see rt_dir_files For implementation details
void *rt_dir_files_seq(rt_string path) {
    return rt_dir_files(path);
}

/// @brief List only subdirectories in a directory (excludes files).
///
/// Returns a sequence containing the names of all subdirectories in the
/// specified directory. Regular files and special entries (. and ..) are
/// excluded from the results.
///
/// **Usage example:**
/// ```
/// ' List all project subdirectories
/// Dim folders = Dir.Dirs("projects")
/// For Each folder In folders
///     Print "Project: " & folder
/// Next
///
/// ' Navigate directory structure
/// Sub ShowTree(path As String, indent As Integer)
///     Print Space(indent) & Path.Name(path)
///     For Each subdir In Dir.Dirs(path)
///         ShowTree(Path.Join(path, subdir), indent + 2)
///     Next
/// End Sub
///
/// ' Check for subdirectories
/// If Dir.Dirs("output").Len() > 0 Then
///     Print "Has subdirectories"
/// End If
/// ```
///
/// **What is included vs excluded:**
/// | Entry Type       | Included? |
/// |------------------|-----------|
/// | Subdirectories   | Yes       |
/// | Regular files    | No        |
/// | "." and ".."     | No        |
/// | Symbolic links   | Varies*   |
///
/// *On Unix, follows symlinks to check if target is directory.
///
/// @param path Directory path to scan.
///
/// @return Seq containing subdirectory names as strings. Returns empty Seq on error.
///
/// @note O(n) time complexity where n is number of entries.
/// @note Returns empty sequence on error (does not trap).
/// @note Does not recurse - returns only immediate children.
/// @note Directory names are returned without path prefix.
///
/// @see rt_dir_files For listing only files
/// @see rt_dir_list For listing all entries
/// @see rt_dir_dirs_seq For wrapper version
void *rt_dir_dirs(rt_string path) {
    void *result = rt_seq_new();
    rt_seq_set_owns_elements(result, 1);

    const char *cpath = NULL;
    if (!rt_file_path_from_vstr(path, &cpath) || !cpath)
        return result;

#if RT_PLATFORM_WINDOWS
    wchar_t *pattern = rt_dir_win_make_pattern(cpath);
    if (!pattern)
        return result;

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        free(pattern);
        return result;
    }

    int scan_ok = 1;
    for (;;) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            !rt_dir_win_is_dot_name(fd.cFileName)) {
            if (!rt_dir_win_push_name(result, fd.cFileName)) {
                scan_ok = 0;
                break;
            }
        }
        if (!FindNextFileW(h, &fd)) {
            if (GetLastError() != ERROR_NO_MORE_FILES)
                scan_ok = 0;
            break;
        }
    }

    if (!scan_ok)
        rt_seq_clear(result);
    FindClose(h);
    free(pattern);
#else
    // Unix: use POSIX directory APIs.
    DIR *dir = opendir(cpath);
    if (!dir)
        return result;

    int ok = 1;
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

        char *full = rt_dir_join_child_alloc(cpath, ent->d_name);
        if (!full) {
            ok = 0;
            break;
        }
        struct stat st;
        if (stat(full, &st) != 0) {
            ok = 0;
            free(full);
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            rt_string name = rt_string_from_bytes(ent->d_name, strlen(ent->d_name));
            rt_seq_push(result, (void *)name);
            rt_string_unref(name);
        }
        free(full);
    }

    if (closedir(dir) != 0)
        ok = 0;
    if (!ok)
        rt_seq_clear(result);
#endif

    return result;
}

/// @brief List only subdirectories as a Zanna.Collections.Seq.
///
/// Wrapper for rt_dir_dirs that returns subdirectory names as a
/// Zanna.Collections.Seq. Preserves the same filtering behavior.
///
/// **Usage example:**
/// ```
/// Dim subdirs = Dir.DirsSeq("src")
/// Print "Found " & subdirs.Len() & " subdirectories"
/// ```
///
/// @param path Directory path to scan.
///
/// @return Seq containing subdirectory names. Returns empty Seq on error.
///
/// @note Delegates to rt_dir_dirs.
///
/// @see rt_dir_dirs For implementation details
void *rt_dir_dirs_seq(rt_string path) {
    return rt_dir_dirs(path);
}
