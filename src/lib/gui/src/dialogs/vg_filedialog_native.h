//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/dialogs/vg_filedialog_native.h
// Purpose: Platform-native file dialog wrappers — blocking open/save/folder
//          dialogs using the OS APIs (Win32, macOS NSPanel, GTK file chooser).
// Key invariants:
//   - All returned strings are heap-allocated and must be freed by the caller.
//   - A NULL return indicates the user cancelled the dialog.
//   - Filter patterns use semicolon-delimited globs (e.g. "*.c;*.h").
// Ownership/Lifetime:
//   - The returned char* is owned by the caller and must be freed with free().
// Links: lib/gui/include/vg_ide_widgets_dialog.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>

/// @file
/// @brief Declares blocking platform-native open, save, and folder-selection dialogs.
/// @details Paths cross the adapter boundary as UTF-8. Successful operations return heap-owned
/// strings, multi-selection returns a heap-owned array of heap-owned strings, and cancellation
/// or setup failure returns NULL.

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Return whether native OS dialogs are usable in this session.
/// @details macOS: always 1. Windows: probes COM once (IFileOpenDialog); a
///          failed probe routes callers to the drawn fallback dialog.
/// @return Nonzero when native dialog creation succeeds, otherwise zero.
int vg_native_dialogs_available(void);

/// @brief Show a native "Open File" dialog and return the selected file path.
///
/// @details Opens the operating system's native file-open dialog. The dialog
///          blocks until the user selects a file or cancels. The returned
///          string is dynamically allocated and must be freed by the caller.
///
/// @param title          Dialog window title (e.g. "Open File").
/// @param initial_path   Initial directory to display (may be NULL for default).
/// @param filter_name    Human-readable filter label (e.g. "C Source Files").
/// @param filter_pattern Semicolon-separated glob patterns (e.g. "*.c;*.h").
/// @return Heap-allocated UTF-8 path owned by the caller, or NULL on cancellation or failure.
char *vg_native_open_file(const char *title,
                          const char *initial_path,
                          const char *filter_name,
                          const char *filter_pattern);

/// @brief Show a native "Open Files" dialog and return the selected paths.
///
/// @details The returned array and each contained string are heap-allocated.
///          Free them with vg_native_free_paths(). On cancel, returns NULL and
///          stores 0 in out_count.
///
/// @param title          Dialog window title.
/// @param initial_path   Initial directory to display (may be NULL).
/// @param filter_name    Human-readable filter label.
/// @param filter_pattern Semicolon-separated glob patterns.
/// @param out_count      Receives the number of selected paths.
/// @return Heap-allocated path array, or NULL on cancel/allocation failure.
char **vg_native_open_files(const char *title,
                            const char *initial_path,
                            const char *filter_name,
                            const char *filter_pattern,
                            size_t *out_count);

/// @brief Free an array returned by vg_native_open_files().
/// @param paths Array of heap-owned path strings, or NULL.
/// @param count Number of readable entries in @p paths.
void vg_native_free_paths(char **paths, size_t count);

/// @brief Show a native "Save File" dialog and return the chosen save path.
///
/// @details Opens the operating system's native file-save dialog with an
///          optional pre-filled filename. The dialog blocks until the user
///          confirms a path or cancels. The returned string is dynamically
///          allocated and must be freed by the caller.
///
/// @param title          Dialog window title (e.g. "Save As").
/// @param initial_path   Initial directory to display (may be NULL for default).
/// @param default_name   Pre-filled filename suggestion (may be NULL).
/// @param filter_name    Human-readable filter label.
/// @param filter_pattern Semicolon-separated glob patterns.
/// @return Heap-allocated full file path, or NULL if the user cancelled.
char *vg_native_save_file(const char *title,
                          const char *initial_path,
                          const char *default_name,
                          const char *filter_name,
                          const char *filter_pattern);

/// @brief Show a native "Select Folder" dialog and return the chosen directory.
///
/// @details Opens the operating system's native folder-selection dialog. The
///          dialog blocks until the user selects a folder or cancels. The
///          returned string is dynamically allocated and must be freed by
///          the caller.
///
/// @param title        Dialog window title (e.g. "Select Folder").
/// @param initial_path Initial directory to display (may be NULL for default).
/// @return Heap-allocated full directory path, or NULL if the user cancelled.
char *vg_native_select_folder(const char *title, const char *initial_path);

#ifdef __cplusplus
}
#endif
