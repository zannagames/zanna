//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_ide_widgets_dialog.h
// Purpose: Dialog and FileDialog widget declarations — modal dialogs with
//          preset button configurations and native file-open/save panels.
// Key invariants:
//   - All create functions return NULL on allocation failure.
//   - String parameters are copied internally unless documented otherwise.
// Ownership/Lifetime:
//   - Dialogs may be created without a parent and must be explicitly destroyed.
// Links: lib/gui/include/vg_ide_widgets_common.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vg_ide_widgets_common.h"

/// @file
/// @brief Declares modal message dialogs and configurable file-selection dialogs.
/// @details Message dialogs support preset or custom buttons, icons, sizing, and result
/// callbacks. File dialogs extend that surface with directory listings, filters, bookmarks,
/// open/save/folder modes, persistent results, and optional blocking host integration.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Dialog Widget
//=============================================================================

/// @brief Dialog button presets
typedef enum vg_dialog_buttons {
    VG_DIALOG_BUTTONS_NONE,
    VG_DIALOG_BUTTONS_OK,
    VG_DIALOG_BUTTONS_OK_CANCEL,
    VG_DIALOG_BUTTONS_YES_NO,
    VG_DIALOG_BUTTONS_YES_NO_CANCEL,
    VG_DIALOG_BUTTONS_RETRY_CANCEL,
    VG_DIALOG_BUTTONS_CUSTOM
} vg_dialog_buttons_t;

/// @brief Dialog result codes
typedef enum vg_dialog_result {
    VG_DIALOG_RESULT_NONE, ///< Still open
    VG_DIALOG_RESULT_OK,
    VG_DIALOG_RESULT_CANCEL,
    VG_DIALOG_RESULT_YES,
    VG_DIALOG_RESULT_NO,
    VG_DIALOG_RESULT_RETRY,
    VG_DIALOG_RESULT_CUSTOM_1,
    VG_DIALOG_RESULT_CUSTOM_2,
    VG_DIALOG_RESULT_CUSTOM_3
} vg_dialog_result_t;

/// @brief Dialog icon presets
typedef enum vg_dialog_icon {
    VG_DIALOG_ICON_NONE,
    VG_DIALOG_ICON_INFO,
    VG_DIALOG_ICON_WARNING,
    VG_DIALOG_ICON_ERROR,
    VG_DIALOG_ICON_QUESTION,
    VG_DIALOG_ICON_CUSTOM
} vg_dialog_icon_t;

/// @brief Custom button definition
typedef struct vg_dialog_button_def {
    char *label;               ///< Button label
    vg_dialog_result_t result; ///< Result code when clicked
    bool is_default;           ///< Activated on Enter
    bool is_cancel;            ///< Activated on Escape
} vg_dialog_button_def_t;

/// @brief Dialog widget structure
typedef struct vg_dialog {
    vg_widget_t base;

    // Title bar
    char *title;            ///< Dialog title
    bool show_close_button; ///< Show close button
    bool draggable;         ///< Can be dragged

    // Content
    vg_widget_t *content;  ///< Content widget
    vg_dialog_icon_t icon; ///< Icon preset
    vg_icon_t custom_icon; ///< Custom icon
    char *message;         ///< Simple text message

    // Buttons
    vg_dialog_buttons_t button_preset;      ///< Button preset
    vg_dialog_button_def_t *custom_buttons; ///< Custom buttons
    size_t custom_button_count;             ///< Number of custom buttons

    // Sizing
    uint32_t min_width;  ///< Minimum width
    uint32_t min_height; ///< Minimum height
    uint32_t max_width;  ///< Maximum width
    uint32_t max_height; ///< Maximum height
    bool resizable;      ///< Can be resized

    // Modal behavior
    bool modal;                ///< Block input to parent
    vg_widget_t *modal_parent; ///< Parent to block

    // Font
    vg_font_t *font;       ///< Font for text
    float font_size;       ///< Font size
    float title_font_size; ///< Title font size

    // Colors
    uint32_t bg_color;           ///< Background color
    uint32_t title_bg_color;     ///< Title bar background
    uint32_t title_text_color;   ///< Title text color
    uint32_t text_color;         ///< Content text color
    uint32_t button_bg_color;    ///< Button background
    uint32_t button_hover_color; ///< Button hover color
    uint32_t overlay_color;      ///< Modal overlay color

    // State
    vg_dialog_result_t result; ///< Dialog result
    bool is_open;              ///< Is dialog open
    bool is_dragging;          ///< Is being dragged
    int drag_offset_x;         ///< Drag offset X
    int drag_offset_y;         ///< Drag offset Y
    int hovered_button;        ///< Currently hovered button (-1 = none)
    bool closing_in_progress;  ///< Re-entrancy guard for vg_dialog_close
    float content_scroll_y;    ///< Scroll offset for content taller than the body region.

    // Callbacks
    void *user_data;           ///< Legacy user data (= on_result_user_data; kept for ABI)
    void *on_result_user_data; ///< User data passed to on_result
    void *on_close_user_data;  ///< User data passed to on_close
    void (*on_result)(struct vg_dialog *, vg_dialog_result_t, void *); ///< Result callback
    void (*on_close)(struct vg_dialog *, void *);                      ///< Close callback
} vg_dialog_t;

/// @brief Create a new dialog widget.
/// @param title Title bar text (copied internally).
/// @return New dialog widget or NULL on failure.
vg_dialog_t *vg_dialog_create(const char *title);

/// @brief Destroy a dialog widget and free all resources.
/// @param dialog Dialog widget to destroy (may be NULL).
void vg_dialog_destroy(vg_dialog_t *dialog);

/// @brief Set the dialog title bar text.
/// @param dialog Dialog widget.
/// @param title  New title (copied internally).
void vg_dialog_set_title(vg_dialog_t *dialog, const char *title);

/// @brief Set the dialog's main content widget.
/// @param dialog  Dialog widget.
/// @param content Widget to embed in the dialog body (ownership transfers to dialog).
void vg_dialog_set_content(vg_dialog_t *dialog, vg_widget_t *content);

/// @brief Set a plain text message displayed in the dialog body.
/// @param dialog  Dialog widget.
/// @param message Message text (copied internally).
void vg_dialog_set_message(vg_dialog_t *dialog, const char *message);

/// @brief Set the built-in icon preset shown beside the message.
/// @param dialog Dialog widget.
/// @param icon   One of VG_DIALOG_ICON_INFO, WARNING, ERROR, QUESTION, or NONE.
void vg_dialog_set_icon(vg_dialog_t *dialog, vg_dialog_icon_t icon);

/// @brief Set a custom icon (overrides any icon preset).
/// @param dialog Dialog widget.
/// @param icon   Icon specification (the dialog takes a deep copy).
void vg_dialog_set_custom_icon(vg_dialog_t *dialog, vg_icon_t icon);

/// @brief Set the button row using a preset.
/// @param dialog  Dialog widget.
/// @param buttons One of the VG_DIALOG_BUTTONS_* presets.
void vg_dialog_set_buttons(vg_dialog_t *dialog, vg_dialog_buttons_t buttons);

/// @brief Replace the button row with a fully custom set of buttons.
/// @param dialog  Dialog widget.
/// @param buttons Array of button definitions (copied internally).
/// @param count   Number of entries in @p buttons.
void vg_dialog_set_custom_buttons(vg_dialog_t *dialog,
                                  vg_dialog_button_def_t *buttons,
                                  size_t count);

/// @brief Control whether the dialog can be resized by dragging its edges.
/// @param dialog    Dialog widget.
/// @param resizable true to enable resize handles.
void vg_dialog_set_resizable(vg_dialog_t *dialog, bool resizable);

/// @brief Set minimum and maximum size constraints.
/// @param dialog Dialog widget.
/// @param min_w  Minimum width in pixels.
/// @param min_h  Minimum height in pixels.
/// @param max_w  Maximum width in pixels (0 = unconstrained).
/// @param max_h  Maximum height in pixels (0 = unconstrained).
void vg_dialog_set_size_constraints(
    vg_dialog_t *dialog, uint32_t min_w, uint32_t min_h, uint32_t max_w, uint32_t max_h);

/// @brief Set whether the dialog blocks input to its parent (modal behaviour).
/// @param dialog Dialog widget.
/// @param modal  true to make the dialog modal.
/// @param parent Parent widget whose input is blocked while the dialog is open.
void vg_dialog_set_modal(vg_dialog_t *dialog, bool modal, vg_widget_t *parent);

/// @brief Make the dialog visible at its last position.
/// @param dialog Dialog widget.
void vg_dialog_show(vg_dialog_t *dialog);

/// @brief Make the dialog visible, centred over another widget.
/// @param dialog      Dialog widget.
/// @param relative_to Widget whose centre is used as the reference point.
void vg_dialog_show_centered(vg_dialog_t *dialog, vg_widget_t *relative_to);

/// @brief Hide the dialog without producing a result.
/// @param dialog Dialog widget.
void vg_dialog_hide(vg_dialog_t *dialog);

/// @brief Close the dialog and record the result code.
///
/// @details Fires the on_result callback (if set) then hides the dialog.
///          Re-entrant calls while @c closing_in_progress is set are ignored.
///
/// @param dialog Dialog widget.
/// @param result Result code to store (e.g. VG_DIALOG_RESULT_OK).
void vg_dialog_close(vg_dialog_t *dialog, vg_dialog_result_t result);

/// @brief Get the result code produced when the dialog last closed.
/// @param dialog Dialog widget.
/// @return Last result code; VG_DIALOG_RESULT_NONE if the dialog is still open.
vg_dialog_result_t vg_dialog_get_result(vg_dialog_t *dialog);

/// @brief Check whether the dialog is currently visible.
/// @param dialog Dialog widget.
/// @return true if the dialog is open.
bool vg_dialog_is_open(vg_dialog_t *dialog);

/// @brief Set the callback fired when the dialog closes with a result.
/// @param dialog    Dialog widget.
/// @param callback  Handler function (receives the dialog, result, and user_data).
/// @param user_data User data passed to the handler.
void vg_dialog_set_on_result(vg_dialog_t *dialog,
                             void (*callback)(vg_dialog_t *, vg_dialog_result_t, void *),
                             void *user_data);

/// @brief Set the callback fired when the dialog is hidden (any cause).
/// @param dialog    Dialog widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_dialog_set_on_close(vg_dialog_t *dialog,
                            void (*callback)(vg_dialog_t *, void *),
                            void *user_data);

/// @brief Set the font used for message text and button labels.
/// @param dialog Dialog widget.
/// @param font   Font handle.
/// @param size   Font size in pixels.
void vg_dialog_set_font(vg_dialog_t *dialog, vg_font_t *font, float size);

/// @brief Create a preconfigured message dialog.
/// @param title   Title bar text.
/// @param message Body text.
/// @param icon    Icon preset.
/// @param buttons Button preset.
/// @return New dialog widget or NULL on failure.
vg_dialog_t *vg_dialog_message(const char *title,
                               const char *message,
                               vg_dialog_icon_t icon,
                               vg_dialog_buttons_t buttons);

/// @brief Create a YES/NO confirmation dialog with a single confirm callback.
/// @param title      Title bar text.
/// @param message    Body text.
/// @param on_confirm Callback invoked when the user confirms (may be NULL).
/// @param user_data  User data passed to @p on_confirm.
/// @return New dialog widget or NULL on failure.
vg_dialog_t *vg_dialog_confirm(const char *title,
                               const char *message,
                               void (*on_confirm)(void *),
                               void *user_data);

//=============================================================================
// FileDialog Widget
//=============================================================================

/// @brief File dialog mode
typedef enum vg_filedialog_mode {
    VG_FILEDIALOG_OPEN,         ///< Select existing file(s)
    VG_FILEDIALOG_SAVE,         ///< Select location to save
    VG_FILEDIALOG_SELECT_FOLDER ///< Select directory
} vg_filedialog_mode_t;

/// @brief File filter
typedef struct vg_file_filter {
    char *name;    ///< Display name (e.g., "Zanna Files")
    char *pattern; ///< Glob pattern (e.g., "*.zanna;*.vpr")
} vg_file_filter_t;

/// @brief File/directory entry
typedef struct vg_file_entry {
    char *name;             ///< File name
    char *full_path;        ///< Full path
    bool is_directory;      ///< Is this a directory
    uint64_t size;          ///< File size in bytes
    uint64_t modified_time; ///< Unix timestamp
} vg_file_entry_t;

/// @brief Bookmark entry
typedef struct vg_bookmark {
    char *name;     ///< Display name
    char *path;     ///< Full path
    vg_icon_t icon; ///< Optional icon
} vg_bookmark_t;

/// @brief FileDialog widget structure
typedef struct vg_filedialog {
    vg_dialog_t base; ///< Inherit from dialog

    vg_filedialog_mode_t mode; ///< Dialog mode

    // Current state
    char *current_path;        ///< Current directory
    vg_file_entry_t **entries; ///< Files in current directory
    size_t entry_count;        ///< Number of entries
    size_t entry_capacity;     ///< Entry array capacity

    // Selection
    int *selected_indices;     ///< Selected file indices
    size_t selection_count;    ///< Number of selected files
    size_t selection_capacity; ///< Selection array capacity
    bool multi_select;         ///< Allow multiple selection (open mode only)

    // Filters
    vg_file_filter_t *filters; ///< File filters
    size_t filter_count;       ///< Number of filters
    size_t filter_capacity;    ///< Filter array capacity
    size_t active_filter;      ///< Currently selected filter

    // Bookmarks
    vg_bookmark_t *bookmarks; ///< Bookmarks
    size_t bookmark_count;    ///< Number of bookmarks
    size_t bookmark_capacity; ///< Bookmark array capacity

    // Configuration
    bool show_hidden;        ///< Show hidden files
    bool confirm_overwrite;  ///< Ask before overwriting (save mode)
    char *default_filename;  ///< Pre-filled filename (save mode)
    char *default_extension; ///< Auto-add extension

    // Child widget state (widgets created during show)
    void *path_input;           ///< Path text input
    void *file_list;            ///< File listing
    void *filename_input;       ///< Filename input (save mode)
    void *filter_dropdown;      ///< Filter selector
    void *bookmark_list;        ///< Sidebar bookmarks
    bool filename_active;       ///< True when the inline save-name field has focus
    float file_scroll_y;        ///< Vertical scroll position for the file list
    float bookmark_scroll_y;    ///< Vertical scroll position for the bookmark list
    size_t filename_cursor_pos; ///< Byte offset cursor for inline save-name editing

    // Result
    char **selected_files;      ///< Dialog-owned result array of selected paths.
    size_t selected_file_count; ///< Number of selected files

    // Callbacks
    void *user_data; ///< User data
    void (*on_select)(struct vg_filedialog *dialog,
                      char **paths,
                      size_t count,
                      void *user_data);                               ///< Selection callback
    void (*on_cancel)(struct vg_filedialog *dialog, void *user_data); ///< Cancel callback
} vg_filedialog_t;

/// @brief Create a new file dialog widget.
/// @param mode VG_FILEDIALOG_OPEN, SAVE, or SELECT_FOLDER.
/// @return New file dialog or NULL on failure.
vg_filedialog_t *vg_filedialog_create(vg_filedialog_mode_t mode);

/// @brief Destroy a file dialog and free all resources.
/// @param dialog File dialog to destroy (may be NULL).
void vg_filedialog_destroy(vg_filedialog_t *dialog);

/// @brief Set the title bar text.
/// @param dialog File dialog.
/// @param title  Title string (copied internally).
void vg_filedialog_set_title(vg_filedialog_t *dialog, const char *title);

/// @brief Set the directory displayed when the dialog opens.
/// @param dialog File dialog.
/// @param path   Absolute directory path (copied internally).
void vg_filedialog_set_initial_path(vg_filedialog_t *dialog, const char *path);

/// @brief Pre-fill the filename field (save mode only).
/// @param dialog   File dialog.
/// @param filename Suggested filename without directory (copied internally).
void vg_filedialog_set_filename(vg_filedialog_t *dialog, const char *filename);

/// @brief Allow the user to select multiple files (open mode only).
/// @param dialog File dialog.
/// @param multi  true to enable multi-selection.
void vg_filedialog_set_multi_select(vg_filedialog_t *dialog, bool multi);

/// @brief Control whether hidden files and directories are listed.
/// @param dialog File dialog.
/// @param show   true to show hidden entries.
void vg_filedialog_set_show_hidden(vg_filedialog_t *dialog, bool show);

/// @brief Ask the user to confirm before overwriting an existing file (save mode).
/// @param dialog  File dialog.
/// @param confirm true to show the overwrite confirmation prompt.
void vg_filedialog_set_confirm_overwrite(vg_filedialog_t *dialog, bool confirm);

/// @brief Add a file-type filter to the filter dropdown.
/// @param dialog  File dialog.
/// @param name    Display name (e.g. "Zanna Files").
/// @param pattern Semicolon-separated glob patterns (e.g. "*.zanna;*.vpr");
///                empty tokens are ignored.
void vg_filedialog_add_filter(vg_filedialog_t *dialog, const char *name, const char *pattern);

/// @brief Remove all file-type filters.
/// @param dialog File dialog.
void vg_filedialog_clear_filters(vg_filedialog_t *dialog);

/// @brief Set an extension automatically appended to the filename in save mode.
/// @param dialog File dialog.
/// @param ext    Extension including the leading dot (e.g. ".zanna").
void vg_filedialog_set_default_extension(vg_filedialog_t *dialog, const char *ext);

/// @brief Add a named quick-access bookmark to the sidebar.
/// @param dialog File dialog.
/// @param name   Display name for the bookmark.
/// @param path   Absolute path the bookmark navigates to.
void vg_filedialog_add_bookmark(vg_filedialog_t *dialog, const char *name, const char *path);

/// @brief Add platform-standard bookmarks (Home, Desktop, Documents).
/// @param dialog File dialog.
void vg_filedialog_add_default_bookmarks(vg_filedialog_t *dialog);

/// @brief Remove all bookmarks from the sidebar.
/// @param dialog File dialog.
void vg_filedialog_clear_bookmarks(vg_filedialog_t *dialog);

/// @brief Show the file dialog.
/// @param dialog File dialog.
void vg_filedialog_show(vg_filedialog_t *dialog);

/// @brief Get all selected file paths after the dialog closes.
/// @param dialog File dialog.
/// @param count  Receives the number of paths in the returned array.
/// @return Array of path strings (owned by the dialog; valid until destroy or next show).
char **vg_filedialog_get_selected_paths(vg_filedialog_t *dialog, size_t *count);

/// @brief Get the single selected path (convenience wrapper for non-multi-select).
/// @param dialog File dialog.
/// @return The first selected path string, or NULL if nothing was selected.
char *vg_filedialog_get_selected_path(vg_filedialog_t *dialog);

/// @brief Set the callback fired when the user confirms a selection.
/// @param dialog    File dialog.
/// @param callback  Handler called with the dialog-owned path array, count, and user_data.
/// @param user_data User data passed to the handler.
void vg_filedialog_set_on_select(vg_filedialog_t *dialog,
                                 void (*callback)(vg_filedialog_t *, char **, size_t, void *),
                                 void *user_data);

/// @brief Set the callback fired when the user cancels the dialog.
/// @param dialog    File dialog.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_filedialog_set_on_cancel(vg_filedialog_t *dialog,
                                 void (*callback)(vg_filedialog_t *, void *),
                                 void *user_data);

/// @brief Host callback used by blocking convenience wrappers to run a file dialog.
/// @details The callback must dispatch/render until @p dialog closes and return
///          true when a selection was confirmed.
/// @param dialog Visible file dialog to service until it closes.
/// @param user_data Opaque context registered with @ref vg_filedialog_set_modal_runner.
/// @return true only when the user confirmed a valid selection.
typedef bool (*vg_filedialog_modal_runner_t)(vg_filedialog_t *dialog, void *user_data);

/// @brief Install the modal runner used by the blocking convenience wrappers.
/// @details The callback and context are borrowed process-wide; passing NULL disables blocking
/// convenience execution until another runner is installed.
/// @param runner Host event/render loop callback, or NULL.
/// @param user_data Opaque context supplied to @p runner.
void vg_filedialog_set_modal_runner(vg_filedialog_modal_runner_t runner, void *user_data);

/// @brief Convenience: show a blocking open-file dialog and return the chosen path.
/// @param title          Dialog title.
/// @param initial_path   Starting directory (may be NULL for the current directory).
/// @param filter_name    Filter display name (may be NULL to show all files).
/// @param filter_pattern Glob pattern (may be NULL).
/// @return Newly allocated path string that the caller must free, or NULL on cancel.
char *vg_filedialog_open_file(const char *title,
                              const char *initial_path,
                              const char *filter_name,
                              const char *filter_pattern);

/// @brief Convenience: show a blocking save-file dialog and return the chosen path.
/// @param title          Dialog title.
/// @param initial_path   Starting directory (may be NULL).
/// @param default_name   Pre-filled filename (may be NULL).
/// @param filter_name    Filter display name (may be NULL).
/// @param filter_pattern Glob pattern (may be NULL).
/// @return Newly allocated path string that the caller must free, or NULL on cancel.
char *vg_filedialog_save_file(const char *title,
                              const char *initial_path,
                              const char *default_name,
                              const char *filter_name,
                              const char *filter_pattern);

/// @brief Convenience: show a blocking folder-select dialog and return the chosen path.
/// @param title        Dialog title.
/// @param initial_path Starting directory (may be NULL).
/// @return Newly allocated path string that the caller must free, or NULL on cancel.
char *vg_filedialog_select_folder(const char *title, const char *initial_path);

#ifdef __cplusplus
}
#endif
