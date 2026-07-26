//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/dialogs/vg_filedialog_native_win32.c
// Purpose: Native Windows file dialogs (plan 09) — raw COM
//          IFileOpenDialog/IFileSaveDialog implementations of the shared
//          vg_native_* dialog surface, mirroring the macOS panel semantics.
// Key invariants:
//   - Every public call establishes and balances COM on its calling thread.
//   - Returned paths are heap UTF-8 strings owned by the caller (free()).
//   - Text crossing the Win32 boundary is converted strictly in both directions.
// Ownership/Lifetime:
//   - COM interfaces and conversion buffers are local to one dialog invocation.
// Links: lib/gui/src/dialogs/vg_filedialog_native.h,
//        lib/gui/src/dialogs/vg_filedialog_native.m (macOS counterpart)
//
//===----------------------------------------------------------------------===//

#include "vg_filedialog_native.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements the native file-dialog surface with raw Windows COM interfaces.
/// @details Each public operation initializes a single-threaded COM apartment for its call,
/// strictly converts UTF-8 to UTF-16, applies common title/folder/filter options, and converts
/// filesystem shell items back into caller-owned UTF-8 paths.

//=============================================================================
// COM GUIDs
//=============================================================================

/* Define GUIDs we need so native Zanna links do not depend on uuid.lib. */
static const GUID VGFD_CLSID_FileOpenDialog = {
    0xDC1C5A9C, 0xE88A, 0x4DDE, {0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7}};
static const GUID VGFD_CLSID_FileSaveDialog = {
    0xC0B4E2F3, 0xBA21, 0x4773, {0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B}};
static const GUID VGFD_IID_IFileOpenDialog = {
    0xD57C7288, 0xD4AD, 0x4768, {0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60}};
static const GUID VGFD_IID_IFileSaveDialog = {
    0x84BCCD23, 0x5FDE, 0x4CDB, {0xAE, 0xA4, 0xAF, 0x64, 0xB8, 0x3D, 0x78, 0xAB}};
static const GUID VGFD_IID_IShellItem = {
    0x43826D1E, 0xE718, 0x42EE, {0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE}};

enum {
    /// Defensive ceiling for one multi-select result returned by a shell extension.
    VGFD_MAX_MULTISELECT_PATHS = 65536,
};

//=============================================================================
// UTF conversion helpers
//=============================================================================

/// @brief Convert UTF-8 to a caller-owned wide string (NULL-tolerant).
/// @param utf8 NUL-terminated UTF-8 input; NULL or empty returns NULL.
/// @return Heap-allocated UTF-16 string, or NULL for empty/invalid input or allocation failure.
static wchar_t *vgfd_wide(const char *utf8) {
    if (!utf8 || !*utf8)
        return NULL;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (count <= 0 || (size_t)count > SIZE_MAX / sizeof(wchar_t))
        return NULL;
    wchar_t *wide = (wchar_t *)malloc((size_t)count * sizeof(wchar_t));
    if (!wide)
        return NULL;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, count) != count) {
        free(wide);
        return NULL;
    }
    return wide;
}

/// @brief Convert a wide string to caller-owned UTF-8.
/// @param wide NUL-terminated UTF-16 input, or NULL.
/// @return Heap-allocated UTF-8 string, or NULL for invalid input or allocation failure.
static char *vgfd_utf8(const wchar_t *wide) {
    if (!wide)
        return NULL;
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (count <= 0)
        return NULL;
    char *utf8 = (char *)malloc((size_t)count);
    if (!utf8)
        return NULL;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8, count, NULL, NULL) !=
        count) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

//=============================================================================
// COM lifecycle
//=============================================================================

/// @brief Tracks whether one helper invocation must balance COM initialization.
typedef struct {
    /// @brief Nonzero after successful `CoInitializeEx`.
    int uninitialize;
} vgfd_com_scope;

/// @brief Enter a single-threaded COM apartment for one dialog operation.
/// @param scope Destination lifecycle state.
/// @return 1 when COM initialization succeeds, otherwise 0.
static int vgfd_com_enter(vgfd_com_scope *scope) {
    if (!scope)
        return 0;
    scope->uninitialize = 0;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr))
        return 0;
    scope->uninitialize = 1;
    return 1;
}

/// @brief Balance a successful COM apartment entry.
/// @param scope Lifecycle state previously passed to @ref vgfd_com_enter.
static void vgfd_com_leave(vgfd_com_scope *scope) {
    if (scope && scope->uninitialize)
        CoUninitialize();
}

/// @copydoc vg_native_dialogs_available
int vg_native_dialogs_available(void) {
    vgfd_com_scope scope;
    if (!vgfd_com_enter(&scope))
        return 0;
    IFileOpenDialog *probe = NULL;
    HRESULT hr = CoCreateInstance(&VGFD_CLSID_FileOpenDialog,
                                  NULL,
                                  CLSCTX_INPROC_SERVER,
                                  &VGFD_IID_IFileOpenDialog,
                                  (void **)&probe);
    int available = SUCCEEDED(hr) && probe;
    if (probe)
        probe->lpVtbl->Release(probe);
    vgfd_com_leave(&scope);
    return available;
}

/// @brief Apply title/initial-folder/filter options to a common dialog.
/// @param dialog Open common-file-dialog interface.
/// @param title Optional UTF-8 title.
/// @param initial_path Optional UTF-8 initial directory.
/// @param filter_name Optional human-readable filter label.
/// @param filter_pattern Optional semicolon-delimited filter specification.
/// @param filter_name_w Receives an owned UTF-16 filter label retained through dialog display.
/// @param filter_spec_w Receives an owned UTF-16 filter pattern retained through dialog display.
/// @return 1 when every requested option is applied, otherwise 0.
static int vgfd_apply_common(IFileDialog *dialog,
                             const char *title,
                             const char *initial_path,
                             const char *filter_name,
                             const char *filter_pattern,
                             wchar_t **filter_name_w,
                             wchar_t **filter_spec_w) {
    if (!dialog || !filter_name_w || !filter_spec_w)
        return 0;
    wchar_t *title_w = vgfd_wide(title);
    if (title && *title && !title_w)
        return 0;
    if (title_w) {
        HRESULT hr = dialog->lpVtbl->SetTitle(dialog, title_w);
        free(title_w);
        if (FAILED(hr))
            return 0;
    }
    if (filter_pattern && *filter_pattern) {
        const char *effective_name = filter_name && *filter_name ? filter_name : "Files";
        *filter_name_w = vgfd_wide(effective_name);
        *filter_spec_w = vgfd_wide(filter_pattern);
        if (!*filter_name_w || !*filter_spec_w)
            return 0;
        COMDLG_FILTERSPEC specs[2];
        specs[0].pszName = *filter_name_w;
        specs[0].pszSpec = *filter_spec_w;
        specs[1].pszName = L"All Files";
        specs[1].pszSpec = L"*.*";
        if (FAILED(dialog->lpVtbl->SetFileTypes(dialog, 2, specs)))
            return 0;
    }
    wchar_t *initial_w = vgfd_wide(initial_path);
    if (initial_path && *initial_path && !initial_w)
        return 0;
    if (initial_w) {
        IShellItem *folder = NULL;
        HRESULT hr =
            SHCreateItemFromParsingName(initial_w, NULL, &VGFD_IID_IShellItem, (void **)&folder);
        if (SUCCEEDED(hr) && folder) {
            hr = dialog->lpVtbl->SetDefaultFolder(dialog, folder);
            folder->lpVtbl->Release(folder);
            if (FAILED(hr)) {
                free(initial_w);
                return 0;
            }
        } else if (folder) {
            folder->lpVtbl->Release(folder);
        }
        free(initial_w);
    }
    return 1;
}

/// @brief Extract a caller-owned UTF-8 filesystem path from a shell item.
/// @param item Shell item expected to expose a filesystem display name.
/// @return Heap-allocated UTF-8 path, or NULL when extraction or conversion fails.
static char *vgfd_item_path(IShellItem *item) {
    PWSTR wide = NULL;
    if (!item)
        return NULL;
    if (FAILED(item->lpVtbl->GetDisplayName(item, SIGDN_FILESYSPATH, &wide)) || !wide) {
        if (wide)
            CoTaskMemFree(wide);
        return NULL;
    }
    char *path = vgfd_utf8(wide);
    CoTaskMemFree(wide);
    return path;
}

//=============================================================================
// Public dialog surface
//=============================================================================

/// @copydoc vg_native_open_file
char *vg_native_open_file(const char *title,
                          const char *initial_path,
                          const char *filter_name,
                          const char *filter_pattern) {
    vgfd_com_scope scope;
    if (!vgfd_com_enter(&scope))
        return NULL;
    IFileOpenDialog *dialog = NULL;
    HRESULT hr = CoCreateInstance(&VGFD_CLSID_FileOpenDialog,
                                  NULL,
                                  CLSCTX_INPROC_SERVER,
                                  &VGFD_IID_IFileOpenDialog,
                                  (void **)&dialog);
    wchar_t *fname = NULL, *fspec = NULL;
    char *result = NULL;
    if (FAILED(hr) || !dialog)
        goto cleanup;
    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(dialog->lpVtbl->GetOptions(dialog, &options)) ||
        FAILED(dialog->lpVtbl->SetOptions(dialog,
                                          options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                                              FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                                              FOS_DONTADDTORECENT)))
        goto cleanup;
    if (!vgfd_apply_common((IFileDialog *)dialog,
                           title,
                           initial_path,
                           filter_name,
                           filter_pattern,
                           &fname,
                           &fspec))
        goto cleanup;
    if (SUCCEEDED(dialog->lpVtbl->Show(dialog, NULL))) {
        IShellItem *item = NULL;
        hr = dialog->lpVtbl->GetResult(dialog, &item);
        if (SUCCEEDED(hr) && item)
            result = vgfd_item_path(item);
        if (item)
            item->lpVtbl->Release(item);
    }
cleanup:
    if (dialog)
        dialog->lpVtbl->Release(dialog);
    free(fname);
    free(fspec);
    vgfd_com_leave(&scope);
    return result;
}

/// @copydoc vg_native_open_files
char **vg_native_open_files(const char *title,
                            const char *initial_path,
                            const char *filter_name,
                            const char *filter_pattern,
                            size_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (!out_count)
        return NULL;
    vgfd_com_scope scope;
    if (!vgfd_com_enter(&scope))
        return NULL;
    IFileOpenDialog *dialog = NULL;
    HRESULT hr = CoCreateInstance(&VGFD_CLSID_FileOpenDialog,
                                  NULL,
                                  CLSCTX_INPROC_SERVER,
                                  &VGFD_IID_IFileOpenDialog,
                                  (void **)&dialog);
    wchar_t *fname = NULL, *fspec = NULL;
    char **paths = NULL;
    if (FAILED(hr) || !dialog)
        goto cleanup;
    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(dialog->lpVtbl->GetOptions(dialog, &options)) ||
        FAILED(dialog->lpVtbl->SetOptions(dialog,
                                          options | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM |
                                              FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST |
                                              FOS_NOCHANGEDIR | FOS_DONTADDTORECENT)))
        goto cleanup;
    if (!vgfd_apply_common((IFileDialog *)dialog,
                           title,
                           initial_path,
                           filter_name,
                           filter_pattern,
                           &fname,
                           &fspec))
        goto cleanup;
    if (SUCCEEDED(dialog->lpVtbl->Show(dialog, NULL))) {
        IShellItemArray *items = NULL;
        hr = dialog->lpVtbl->GetResults(dialog, &items);
        if (SUCCEEDED(hr) && items) {
            DWORD count = 0;
            if (SUCCEEDED(items->lpVtbl->GetCount(items, &count)) && count > 0 &&
                count <= VGFD_MAX_MULTISELECT_PATHS && (size_t)count <= SIZE_MAX / sizeof(char *)) {
                paths = (char **)calloc(count, sizeof(char *));
                if (paths) {
                    size_t written = 0;
                    for (DWORD i = 0; i < count; ++i) {
                        IShellItem *item = NULL;
                        hr = items->lpVtbl->GetItemAt(items, i, &item);
                        if (SUCCEEDED(hr) && item) {
                            char *path = vgfd_item_path(item);
                            if (path)
                                paths[written++] = path;
                        }
                        if (item)
                            item->lpVtbl->Release(item);
                    }
                    *out_count = written;
                    if (written == 0) {
                        free(paths);
                        paths = NULL;
                    }
                }
            }
        }
        if (items)
            items->lpVtbl->Release(items);
    }
cleanup:
    if (dialog)
        dialog->lpVtbl->Release(dialog);
    free(fname);
    free(fspec);
    vgfd_com_leave(&scope);
    return paths;
}

/// @copydoc vg_native_free_paths
void vg_native_free_paths(char **paths, size_t count) {
    if (!paths)
        return;
    for (size_t i = 0; i < count; ++i)
        free(paths[i]);
    free(paths);
}

/// @copydoc vg_native_save_file
char *vg_native_save_file(const char *title,
                          const char *initial_path,
                          const char *default_name,
                          const char *filter_name,
                          const char *filter_pattern) {
    vgfd_com_scope scope;
    if (!vgfd_com_enter(&scope))
        return NULL;
    IFileSaveDialog *dialog = NULL;
    HRESULT hr = CoCreateInstance(&VGFD_CLSID_FileSaveDialog,
                                  NULL,
                                  CLSCTX_INPROC_SERVER,
                                  &VGFD_IID_IFileSaveDialog,
                                  (void **)&dialog);
    wchar_t *fname = NULL, *fspec = NULL;
    wchar_t *name_w = NULL;
    char *result = NULL;
    if (FAILED(hr) || !dialog)
        goto cleanup;
    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(dialog->lpVtbl->GetOptions(dialog, &options)) ||
        FAILED(dialog->lpVtbl->SetOptions(dialog,
                                          options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT |
                                              FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                                              FOS_DONTADDTORECENT)))
        goto cleanup;
    if (!vgfd_apply_common((IFileDialog *)dialog,
                           title,
                           initial_path,
                           filter_name,
                           filter_pattern,
                           &fname,
                           &fspec))
        goto cleanup;
    name_w = vgfd_wide(default_name);
    if (default_name && *default_name && !name_w)
        goto cleanup;
    if (name_w) {
        hr = dialog->lpVtbl->SetFileName(dialog, name_w);
        if (FAILED(hr))
            goto cleanup;
    }
    if (SUCCEEDED(dialog->lpVtbl->Show(dialog, NULL))) {
        IShellItem *item = NULL;
        hr = dialog->lpVtbl->GetResult(dialog, &item);
        if (SUCCEEDED(hr) && item)
            result = vgfd_item_path(item);
        if (item)
            item->lpVtbl->Release(item);
    }
cleanup:
    if (dialog)
        dialog->lpVtbl->Release(dialog);
    free(name_w);
    free(fname);
    free(fspec);
    vgfd_com_leave(&scope);
    return result;
}

/// @copydoc vg_native_select_folder
char *vg_native_select_folder(const char *title, const char *initial_path) {
    vgfd_com_scope scope;
    if (!vgfd_com_enter(&scope))
        return NULL;
    IFileOpenDialog *dialog = NULL;
    HRESULT hr = CoCreateInstance(&VGFD_CLSID_FileOpenDialog,
                                  NULL,
                                  CLSCTX_INPROC_SERVER,
                                  &VGFD_IID_IFileOpenDialog,
                                  (void **)&dialog);
    wchar_t *fname = NULL, *fspec = NULL;
    char *result = NULL;
    if (FAILED(hr) || !dialog)
        goto cleanup;
    FILEOPENDIALOGOPTIONS options = 0;
    if (FAILED(dialog->lpVtbl->GetOptions(dialog, &options)) ||
        FAILED(dialog->lpVtbl->SetOptions(dialog,
                                          options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                              FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                                              FOS_DONTADDTORECENT)))
        goto cleanup;
    if (!vgfd_apply_common((IFileDialog *)dialog, title, initial_path, NULL, NULL, &fname, &fspec))
        goto cleanup;
    if (SUCCEEDED(dialog->lpVtbl->Show(dialog, NULL))) {
        IShellItem *item = NULL;
        hr = dialog->lpVtbl->GetResult(dialog, &item);
        if (SUCCEEDED(hr) && item)
            result = vgfd_item_path(item);
        if (item)
            item->lpVtbl->Release(item);
    }
cleanup:
    if (dialog)
        dialog->lpVtbl->Release(dialog);
    free(fname);
    free(fspec);
    vgfd_com_leave(&scope);
    return result;
}
