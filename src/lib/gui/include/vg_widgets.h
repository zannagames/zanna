//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_widgets.h
// Purpose: Core widget library — concrete implementations of standard UI
//          controls: Label, Button, TextInput, Checkbox, ScrollView, ListBox,
//          Dropdown, Slider, ProgressBar, RadioButton, Image, Spinner,
//          ColorSwatch, ColorPalette, and ColorPicker.
// Key invariants:
//   - Widget create functions return NULL on allocation failure.
//   - String parameters (text, placeholder) are copied internally.
//   - Callback user_data pointers are stored but never dereferenced by the
//     widget; the application owns the pointed-to data.
//   - Every widget struct's first member is vg_widget_t base, enabling safe
//     pointer up-casts for generic tree operations.
//   - ListBox overflow is visibly ellipsized while its full row text remains
//     available through the shared hover-tooltip manager.
//   - ScrollView descendant reveals retain one live-ID-guarded target until
//     settled layout geometry can make it visible.
// Ownership/Lifetime:
//   - Created widgets are owned by their parent once added; destroying the
//     parent destroys all children.
//   - Strings passed to setters are copied; the caller may free the original.
// Links: lib/gui/include/vg_widget.h,
//        lib/gui/include/vg_layout.h,
//        lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_font.h,
//        lib/gui/include/vg_ide_widgets.h,
//        docs/adr/0165-scrollview-descendant-reveal.md,
//        docs/adr/0167-spinner-mixed-value-state.md
//
//===----------------------------------------------------------------------===//
#ifndef VG_WIDGETS_H
#define VG_WIDGETS_H

#include "vg_font.h"

/// @file
/// @brief Declares the standard concrete control library built on @ref vg_widget_t.
/// @details The API covers text, buttons, editing, toggles, scrolling, lists, selection,
/// numeric controls, images, and color tools. Constructors return typed structs whose embedded
/// base participates in the retained widget tree, and string setters copy their input.
#include "vg_layout.h"
#include "vg_widget.h"

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Label Widget
//=============================================================================

/// @brief Label widget structure
typedef struct vg_label {
    vg_widget_t base;

    char *text;              ///< Text content (owned, null-terminated)
    vg_font_t *font;         ///< Font for rendering
    float font_size;         ///< Font size in pixels
    uint32_t text_color;     ///< Text color (ARGB)
    vg_h_align_t h_align;    ///< Horizontal text alignment
    vg_v_align_t v_align;    ///< Vertical text alignment
    bool word_wrap;          ///< Enable word wrapping
    int max_lines;           ///< Maximum lines (0 = unlimited)
    bool ellipsis;           ///< Show an ellipsis when visible text is truncated
    bool selectable;         ///< Allow pointer/keyboard text selection
    int32_t icon_vector_id;  ///< Vector icon before the text (non-wrapped labels); negative = none.
    bool selection_dragging; ///< Pointer drag currently extends the selection
    size_t selection_anchor; ///< Fixed byte endpoint for shift/drag selection
    size_t selection_start;  ///< First selection endpoint as a UTF-8 byte offset
    size_t selection_end;    ///< Second selection endpoint as a UTF-8 byte offset

    /* Word-wrap line cache — populated by measure, consumed by paint.
     * Private; do not access from outside vg_label.c. */
    char **wrap_line_bufs;        ///< malloc'd array of malloc'd line strings
    size_t *wrap_source_starts;   ///< source byte offset corresponding to each cached line
    size_t *wrap_source_ends;     ///< source byte end corresponding to each cached line
    int wrap_line_count;          ///< number of entries in wrap_line_bufs
    float wrap_cached_w;          ///< wrap_width for which cache is valid (-1 = invalid)
    bool wrap_truncated;          ///< max_lines omitted source text from the cache
    char *ellipsis_cache;         ///< cached single-line fitted display text
    float ellipsis_cached_w;      ///< width for which ellipsis_cache was built
    size_t ellipsis_source_bytes; ///< original source prefix represented by ellipsis_cache
} vg_label_t;

/// @brief Create a new label widget
/// @param parent Parent widget (can be NULL)
/// @param text Initial text content
/// @return New label widget or NULL on failure
vg_label_t *vg_label_create(vg_widget_t *parent, const char *text);

/// @brief Set label text
/// @param label Label widget
/// @param text New text (copied internally)
void vg_label_set_text(vg_label_t *label, const char *text);

/// @brief Enable or disable word wrapping for a label.
/// @param label   Label widget
/// @param enabled true to wrap text to the laid-out width; false for single line
void vg_label_set_word_wrap(vg_label_t *label, bool enabled);

/// @brief Set (or clear) a scalable vector icon drawn before the text (ADR 0137).
/// @details Rendered on non-wrapped labels only; word-wrapped labels ignore it.
/// @param label The label to configure.
/// @param vector_id Icon id from vg_icon_vector_find; negative clears it.
void vg_label_set_vector_icon(vg_label_t *label, int32_t vector_id);

/// @brief Get label text
/// @param label Label widget
/// @return Current text (read-only)
const char *vg_label_get_text(vg_label_t *label);

/// @brief Set label font
/// @param label Label widget
/// @param font Font to use
/// @param size Font size in pixels
void vg_label_set_font(vg_label_t *label, vg_font_t *font, float size);

/// @brief Set text color
/// @param label Label widget
/// @param color Text color (ARGB)
void vg_label_set_color(vg_label_t *label, uint32_t color);

/// @brief Set text alignment
/// @param label Label widget
/// @param h_align Horizontal alignment
/// @param v_align Vertical alignment
void vg_label_set_alignment(vg_label_t *label, vg_h_align_t h_align, vg_v_align_t v_align);

/// @brief Return a label's horizontal alignment.
/// @param label Label widget to inspect.
/// @return Left, center, or right alignment; NULL defaults to left.
vg_h_align_t vg_label_get_alignment(const vg_label_t *label);

/// @brief Enable or disable ellipsis rendering for truncated label text.
/// @details For a single-line label the text is fitted to the arranged width. For wrapped text,
///          the final visible line receives an ellipsis only when max_lines omits source content.
///          UTF-8 units are never split.
/// @param label Label widget to update; NULL is ignored.
/// @param enabled true to display an ellipsis when truncation occurs.
void vg_label_set_ellipsis(vg_label_t *label, bool enabled);

/// @brief Set the maximum number of wrapped lines rendered by a label.
/// @details Zero means unlimited. Negative values normalize to zero. The limit is meaningful when
///          word wrapping is enabled and participates in layout measurement.
/// @param label Label widget to update; NULL is ignored.
/// @param count Maximum visible line count, or zero for unlimited.
void vg_label_set_max_lines(vg_label_t *label, int count);

/// @brief Enable or disable read-only selection interaction on a label.
/// @details Selectable labels accept pointer drag, Shift-click, Ctrl/Cmd+A, Ctrl/Cmd+C, and Escape.
///          Disabling selection clears any active selection and releases input capture.
/// @param label Label widget to update; NULL is ignored.
/// @param enabled true to make displayed text focusable and selectable.
void vg_label_set_selectable(vg_label_t *label, bool enabled);

/// @brief Return the selected UTF-8 byte range without allocating.
/// @details Endpoints are ordered and clamped to the current source text. The returned pointer is
///          borrowed from the label and remains valid only until its text changes or it is
///          destroyed.
/// @param label Label widget to inspect.
/// @param out_length Receives the selected byte count when non-NULL.
/// @return Pointer to the first selected byte, or NULL when no selection exists.
const char *vg_label_get_selected_text(const vg_label_t *label, size_t *out_length);

//=============================================================================
// Button Widget
//=============================================================================

/// @brief Button callback function type
typedef void (*vg_button_callback_t)(vg_widget_t *button, void *user_data);

/// @brief Button style enumeration
typedef enum vg_button_style {
    VG_BUTTON_STYLE_DEFAULT,   ///< Standard button
    VG_BUTTON_STYLE_PRIMARY,   ///< Primary action button
    VG_BUTTON_STYLE_SECONDARY, ///< Secondary action
    VG_BUTTON_STYLE_DANGER,    ///< Destructive action
    VG_BUTTON_STYLE_TEXT,      ///< Text-only button
    VG_BUTTON_STYLE_ICON       ///< Icon button
} vg_button_style_t;

/// @brief Button widget structure
typedef struct vg_button {
    vg_widget_t base;

    char *text;                    ///< Button text (owned)
    vg_font_t *font;               ///< Font for text
    float font_size;               ///< Font size
    vg_button_style_t style;       ///< Button style
    vg_button_callback_t on_click; ///< Click callback
    void *user_data;               ///< User data for callback

    // Appearance
    uint32_t bg_color;     ///< Background color
    uint32_t fg_color;     ///< Text color
    uint32_t border_color; ///< Border color
    float border_radius;   ///< Corner radius

    // Icon
    char *icon_text;        ///< UTF-8 icon/emoji string (NULL = no icon, owned by button)
    int icon_pos;           ///< Icon position: 0 = left of text (default), 1 = right of text
    int32_t icon_vector_id; ///< Vector icon id (vg_icon_vector); negative = none.
} vg_button_t;

/// @brief Create a new button widget
/// @param parent Parent widget (can be NULL)
/// @param text Button text
/// @return New button widget or NULL on failure
vg_button_t *vg_button_create(vg_widget_t *parent, const char *text);

/// @brief Set button text
/// @param button Button widget
/// @param text New text (copied internally)
void vg_button_set_text(vg_button_t *button, const char *text);

/// @brief Get the current label text of a button.
/// @param button Button widget
/// @return Pointer to internal text string (valid until next vg_button_set_text call), or NULL
const char *vg_button_get_text(vg_button_t *button);

/// @brief Set button click callback
/// @param button Button widget
/// @param callback Click handler function
/// @param user_data User data passed to callback
void vg_button_set_on_click(vg_button_t *button, vg_button_callback_t callback, void *user_data);

/// @brief Set button style
/// @param button Button widget
/// @param style Button style
void vg_button_set_style(vg_button_t *button, vg_button_style_t style);

/// @brief Set button font
/// @param button Button widget
/// @param font Font to use
/// @param size Font size in pixels
void vg_button_set_font(vg_button_t *button, vg_font_t *font, float size);

/// @brief Set the icon text shown on the button.
///
/// @details The string is copied internally. Pass NULL to remove the icon.
///          Common usage is to pass a UTF-8 emoji or icon font glyph.
///
/// @param button Button widget.
/// @param icon   UTF-8 icon string (copied), or NULL to clear.
void vg_button_set_icon(vg_button_t *button, const char *icon);

/// @brief Set (or clear) the button's scalable vector icon (ADR 0137).
/// @details Vector icons take precedence over icon text when both are set.
/// @param button The button to configure.
/// @param vector_id Icon id from vg_icon_vector_find; negative clears it.
void vg_button_set_vector_icon(vg_button_t *button, int32_t vector_id);

/// @brief Set which side of the label the icon appears on.
///
/// @param button Button widget.
/// @param pos    0 = left of label (default), 1 = right of label.
void vg_button_set_icon_position(vg_button_t *button, int pos);

//=============================================================================
// TextInput Widget
//=============================================================================

/// @brief Text input callback for text changes
typedef void (*vg_text_change_callback_t)(vg_widget_t *input, const char *text, void *user_data);

/// @brief Text input widget structure
typedef struct vg_textinput {
    vg_widget_t base;

    char *text;             ///< Current text content (owned)
    size_t text_len;        ///< Current text length in bytes
    size_t text_capacity;   ///< Allocated capacity in bytes
    size_t text_char_count; ///< Cached UTF-8 codepoint count for the current text.
    size_t text_line_count; ///< Cached logical line count; always at least one.
    size_t cursor_pos;      ///< Cursor position (UTF-8 codepoint index)
    size_t selection_start; ///< Selection start position (UTF-8 codepoint index)
    size_t selection_end;   ///< Selection end position (UTF-8 codepoint index)

    char *composition_text;          ///< Owned UTF-8 IME preedit text, or NULL when inactive.
    size_t composition_text_len;     ///< Preedit byte length, excluding the terminator.
    size_t composition_start;        ///< Replacement start in committed-text codepoints.
    size_t composition_end;          ///< Replacement end in committed-text codepoints.
    size_t composition_sel_start;    ///< IME selection start in preedit grapheme units.
    size_t composition_sel_length;   ///< IME selection length in preedit grapheme units.
    size_t composition_saved_cursor; ///< Cursor restored when composition is cancelled.
    size_t composition_saved_start;  ///< Selection anchor restored on cancellation.
    size_t composition_saved_end;    ///< Selection end restored on cancellation.
    bool composing;                  ///< true while an IME preedit session is active.

    char *placeholder; ///< Placeholder text (owned)
    vg_font_t *font;   ///< Font for rendering
    float font_size;   ///< Font size

    size_t max_length;  ///< Maximum grapheme length (0 = unlimited)
    bool password_mode; ///< Show dots instead of characters
    bool read_only;     ///< Prevent text modification
    bool multiline;     ///< Allow multiple lines

    // Appearance
    uint32_t text_color;        ///< Text color
    uint32_t placeholder_color; ///< Placeholder text color
    uint32_t selection_color;   ///< Selection highlight color
    uint32_t cursor_color;      ///< Cursor color
    uint32_t bg_color;          ///< Background color
    uint32_t border_color;      ///< Border color

    // Scrolling (for multiline)
    float scroll_x; ///< Horizontal scroll offset
    float scroll_y; ///< Vertical scroll offset

    // Callbacks
    vg_text_change_callback_t on_change;
    void *on_change_data;
    vg_text_change_callback_t on_commit; ///< Called when Enter is pressed (single-line)
    void *on_commit_data;

    // Internal state
    float cursor_blink_time; ///< Cursor blink timer
    bool cursor_visible;     ///< Cursor visibility state
    void *platform_window;   ///< Borrowed last-painted ZannaGFX window for native IME updates

    // Undo/redo ring buffer (max 32 snapshots)
    char *undo_stack[32];    ///< strdup'd text snapshots; NULL = empty slot
    size_t undo_cursors[32]; ///< Cursor position at time of each snapshot
    int undo_head;           ///< Index of the most-recent (top) snapshot (0..31)
    int undo_count;          ///< Total number of valid snapshots
    int undo_pos; ///< Current position in stack (for redo; equals undo_count after normal
                  ///< edits)

    uint64_t change_revision;          ///< Independent text-change edge revision.
    uint64_t reported_change_revision; ///< Last text-change revision consumed by WasChanged.
    uint64_t submit_revision;          ///< Independent submission edge revision.
    uint64_t reported_submit_revision; ///< Last submission revision consumed by WasSubmitted.
} vg_textinput_t;

/// @brief Create a new text input widget
/// @param parent Parent widget (can be NULL)
/// @return New text input widget or NULL on failure
vg_textinput_t *vg_textinput_create(vg_widget_t *parent);

/// @brief Set text input content
/// @param input Text input widget
/// @param text New text (copied internally)
void vg_textinput_set_text(vg_textinput_t *input, const char *text);

/// @brief Get text input content
/// @param input Text input widget
/// @return Current text (read-only)
const char *vg_textinput_get_text(vg_textinput_t *input);

/// @brief Set placeholder text
/// @param input Text input widget
/// @param placeholder Placeholder text (copied internally)
void vg_textinput_set_placeholder(vg_textinput_t *input, const char *placeholder);

/// @brief Set the maximum committed text length in extended grapheme clusters.
///
/// @details Zero removes the limit. Lowering a nonzero limit truncates the
///          current content at a Unicode grapheme boundary, records a change
///          edge only when content was actually removed, and resets undo
///          history to the resulting programmatic value.
/// @param input Text input widget; NULL is ignored.
/// @param max_length Maximum user-perceived characters, or zero for unlimited.
void vg_textinput_set_max_length(vg_textinput_t *input, size_t max_length);

/// @brief Return the configured extended-grapheme length limit.
/// @param input Text input widget; may be NULL.
/// @return Maximum grapheme count, or zero for unlimited/invalid input.
size_t vg_textinput_get_max_length(const vg_textinput_t *input);

/// @brief Enable or disable password masking without changing committed text.
///
/// @details Masking is a presentation property. Stored text, selection,
///          clipboard policy, and undo history remain intact. One mask glyph
///          is rendered per extended grapheme cluster.
/// @param input Text input widget; NULL is ignored.
/// @param password true to mask displayed text, false to display it normally.
void vg_textinput_set_password(vg_textinput_t *input, bool password);

/// @brief Return whether password masking is enabled.
/// @param input Text input widget; may be NULL.
/// @return true only for a valid input with password presentation enabled.
bool vg_textinput_is_password(const vg_textinput_t *input);

/// @brief Make committed text immutable or editable.
///
/// @details Read-only inputs retain keyboard selection/navigation and copying,
///          while insert, delete, undo, redo, and IME commit operations reject
///          mutation. Active composition is cancelled when read-only is enabled.
/// @param input Text input widget; NULL is ignored.
/// @param read_only true to reject edits, false to allow edits.
void vg_textinput_set_read_only(vg_textinput_t *input, bool read_only);

/// @brief Return whether text mutation is disabled.
/// @param input Text input widget; may be NULL.
/// @return true only for a valid read-only input.
bool vg_textinput_is_read_only(const vg_textinput_t *input);

/// @brief Select single-line submission or multiline editing behavior.
///
/// @details Enabling multiline permits newline insertion and requests layout;
///          disabling it removes existing carriage returns/newlines
///          deterministically, reports a text change if needed, and restores
///          single-line Enter submission behavior.
/// @param input Text input widget; NULL is ignored.
/// @param multiline true for multiple logical lines, false for one line.
void vg_textinput_set_multiline(vg_textinput_t *input, bool multiline);

/// @brief Return whether newline editing and multiline layout are enabled.
/// @param input Text input widget; may be NULL.
/// @return true only for a valid multiline input.
bool vg_textinput_is_multiline(const vg_textinput_t *input);

/// @brief Set text change callback
/// @param input Text input widget
/// @param callback Change handler function
/// @param user_data User data passed to callback
void vg_textinput_set_on_change(vg_textinput_t *input,
                                vg_text_change_callback_t callback,
                                void *user_data);

/// @brief Set commit callback (called when Enter is pressed in single-line mode)
/// @param input Text input widget
/// @param callback Commit handler function
/// @param user_data User data passed to callback
void vg_textinput_set_on_commit(vg_textinput_t *input,
                                vg_text_change_callback_t callback,
                                void *user_data);

/// @brief Set cursor position
/// @param input Text input widget
/// @param pos Cursor position (UTF-8 codepoint index)
void vg_textinput_set_cursor(vg_textinput_t *input, size_t pos);

/// @brief Move the cursor using the public extended-grapheme index contract.
///
/// @details The requested index is clamped to the grapheme count and converted
///          to the widget's legacy codepoint storage without splitting a
///          combining, emoji, flag, Hangul, or Indic cluster.
/// @param input Text input widget; NULL is ignored.
/// @param grapheme_index Zero-based user-perceived-character boundary.
void vg_textinput_set_cursor_grapheme(vg_textinput_t *input, size_t grapheme_index);

/// @brief Return the cursor boundary in extended-grapheme units.
/// @param input Text input widget; may be NULL.
/// @return Clamped grapheme index, or zero for invalid input.
size_t vg_textinput_get_cursor_grapheme(const vg_textinput_t *input);

/// @brief Select text range
/// @param input Text input widget
/// @param start Selection start
/// @param end Selection end
void vg_textinput_select(vg_textinput_t *input, size_t start, size_t end);

/// @brief Select a half-open range using extended-grapheme indices.
///
/// @details Both endpoints are clamped independently. The stored anchor keeps
///          the caller's direction and the cursor moves to @p end, while public
///          selection getters return ordered minimum/maximum boundaries.
/// @param input Text input widget; NULL is ignored.
/// @param start Grapheme selection anchor.
/// @param end Grapheme selection cursor/end.
void vg_textinput_select_graphemes(vg_textinput_t *input, size_t start, size_t end);

/// @brief Collapse the selection at the current cursor without changing text.
/// @param input Text input widget; NULL is ignored.
void vg_textinput_clear_selection(vg_textinput_t *input);

/// @brief Return the ordered inclusive selection start in grapheme units.
/// @param input Text input widget; may be NULL.
/// @return Minimum selection endpoint, or zero for invalid input.
size_t vg_textinput_get_selection_start_grapheme(const vg_textinput_t *input);

/// @brief Return the ordered exclusive selection end in grapheme units.
/// @param input Text input widget; may be NULL.
/// @return Maximum selection endpoint, or zero for invalid input.
size_t vg_textinput_get_selection_end_grapheme(const vg_textinput_t *input);

/// @brief Select all text
/// @param input Text input widget
void vg_textinput_select_all(vg_textinput_t *input);

/// @brief Insert text at cursor position
/// @param input Text input widget
/// @param text Text to insert
void vg_textinput_insert(vg_textinput_t *input, const char *text);

/// @brief Insert committed UTF-8 text and record one undo snapshot.
///
/// @details Replaces the current selection, sanitizes malformed UTF-8, enforces
///          the grapheme limit without splitting a cluster, emits one change
///          edge, and creates exactly one undo step. Empty, read-only, or fully
///          limit-clamped insertions return false without modifying history.
/// @param input Text input widget; may be NULL.
/// @param text Borrowed NUL-terminated UTF-8 text; may be NULL.
/// @return true only when committed text changed.
bool vg_textinput_insert_text(vg_textinput_t *input, const char *text);

/// @brief Delete selected text
/// @param input Text input widget
void vg_textinput_delete_selection(vg_textinput_t *input);

/// @brief Delete the selected grapheme-safe range and record one undo snapshot.
/// @param input Text input widget; may be NULL.
/// @return true only when a non-empty editable selection was removed.
bool vg_textinput_delete_selection_checked(vg_textinput_t *input);

/// @brief Restore the preceding committed-text snapshot.
///
/// @details Active composition is cancelled first. A successful restore emits
///          one change edge but does not append another history entry.
/// @param input Text input widget; may be NULL.
/// @return true when an older snapshot was restored.
bool vg_textinput_undo(vg_textinput_t *input);

/// @brief Reapply the next committed-text snapshot.
///
/// @details Active composition is cancelled first. A successful restore emits
///          one change edge but does not append another history entry.
/// @param input Text input widget; may be NULL.
/// @return true when a newer snapshot was restored.
bool vg_textinput_redo(vg_textinput_t *input);

/// @brief Return whether an older undo snapshot is available.
/// @param input Text input widget; may be NULL.
/// @return true when vg_textinput_undo() can change committed text.
bool vg_textinput_can_undo(const vg_textinput_t *input);

/// @brief Return whether a newer redo snapshot is available.
/// @param input Text input widget; may be NULL.
/// @return true when vg_textinput_redo() can change committed text.
bool vg_textinput_can_redo(const vg_textinput_t *input);

/// @brief Consume the text-changed edge independently of submission edges.
/// @param input Text input widget; may be NULL.
/// @return true once after one or more changes since the preceding call.
bool vg_textinput_was_changed(vg_textinput_t *input);

/// @brief Consume the single-line submission edge independently of text changes.
/// @param input Text input widget; may be NULL.
/// @return true once after one or more Enter submissions since the preceding call.
bool vg_textinput_was_submitted(vg_textinput_t *input);

/// @brief Return the non-consuming widget state revision.
/// @param input Text input widget; may be NULL.
/// @return Monotonic revision, or zero for invalid input.
uint64_t vg_textinput_get_revision(const vg_textinput_t *input);

/// @brief Begin an IME preedit session over a grapheme replacement range.
///
/// @details Existing composition is cancelled. The committed text and undo
///          history remain unchanged; cursor/selection state is saved for
///          cancellation. Read-only inputs reject the operation.
/// @param input Text input widget; may be NULL.
/// @param replacement_start Start boundary in committed-text graphemes.
/// @param replacement_length Number of committed graphemes replaced on commit.
/// @return true when composition state was initialized.
bool vg_textinput_composition_start(vg_textinput_t *input,
                                    size_t replacement_start,
                                    size_t replacement_length);

/// @brief Replace the visible IME preedit text and its internal selection.
///
/// @details The UTF-8 preedit is sanitized and owned by the widget. Selection
///          indices use grapheme units and are clamped to the preedit count.
///          This operation repaints but does not change committed text,
///          revisions, change edges, or undo history.
/// @param input Text input widget with an active composition.
/// @param text Borrowed NUL-terminated UTF-8 preedit text; NULL becomes empty.
/// @param selection_start Grapheme selection/caret start within preedit.
/// @param selection_length Grapheme selection length within preedit.
/// @return true when active composition state was updated.
bool vg_textinput_composition_update(vg_textinput_t *input,
                                     const char *text,
                                     size_t selection_start,
                                     size_t selection_length);

/// @brief Commit an IME result as one atomic text edit and one undo record.
///
/// @details The saved replacement range is selected, @p text is inserted using
///          normal sanitization/limit rules, and composition state is cleared.
///          A rejected or empty commit still ends composition without changing
///          committed text.
/// @param input Text input widget with an active composition.
/// @param text Borrowed NUL-terminated UTF-8 commit string; NULL becomes empty.
/// @return true only when committed text changed.
bool vg_textinput_composition_commit(vg_textinput_t *input, const char *text);

/// @brief Cancel IME preedit and restore the saved cursor/selection state.
/// @param input Text input widget; may be NULL or not composing.
/// @return true when an active composition was cancelled.
bool vg_textinput_composition_cancel(vg_textinput_t *input);

/// @brief Return whether an IME preedit session is active.
/// @param input Text input widget; may be NULL.
/// @return true only while preedit state is active.
bool vg_textinput_is_composing(const vg_textinput_t *input);

/// @brief Return the owned current IME preedit text.
/// @param input Text input widget; may be NULL.
/// @return Borrowed NUL-terminated preedit text, or an empty static string.
const char *vg_textinput_get_composition_text(const vg_textinput_t *input);

/// @brief Return the committed-text insertion point for preedit in grapheme units.
/// @param input Text input widget; may be NULL.
/// @return Grapheme boundary at which composition will commit, or zero.
size_t vg_textinput_get_composition_start(const vg_textinput_t *input);

/// @brief Return the current preedit text length in grapheme units.
/// @param input Text input widget; may be NULL.
/// @return Number of user-perceived characters in preedit text.
size_t vg_textinput_get_composition_length(const vg_textinput_t *input);

/// @brief Copy the selected text into a newly allocated UTF-8 string.
/// @param input Text input widget
/// @return Newly allocated selection text, or NULL when nothing is selected.
char *vg_textinput_get_selection(vg_textinput_t *input);

/// @brief Set font for text input
/// @param input Text input widget
/// @param font Font to use; NULL keeps the existing font
/// @param size Finite positive font size in pixels; invalid values use the theme normal size
void vg_textinput_set_font(vg_textinput_t *input, vg_font_t *font, float size);

/// @brief Advance cursor blink timer; call each frame with elapsed seconds
/// @param input Text input widget
/// @param dt Elapsed time in seconds since last call; NaN, infinity, and negative values are
/// ignored
void vg_textinput_tick(vg_textinput_t *input, float dt);

//=============================================================================
// Checkbox Widget
//=============================================================================

/// @brief Checkbox state change callback
typedef void (*vg_checkbox_callback_t)(vg_widget_t *checkbox, bool checked, void *user_data);

/// @brief Checkbox widget structure
typedef struct vg_checkbox {
    vg_widget_t base;

    char *text;         ///< Label text (owned)
    vg_font_t *font;    ///< Font for label
    float font_size;    ///< Font size
    bool checked;       ///< Checked state
    bool indeterminate; ///< Indeterminate state (tri-state)

    // Appearance
    float box_size;       ///< Checkbox box size
    float gap;            ///< Gap between box and label
    uint32_t check_color; ///< Check mark color
    uint32_t box_color;   ///< Box background color
    uint32_t text_color;  ///< Text color

    // Callback
    vg_checkbox_callback_t on_change;
    void *on_change_data;
} vg_checkbox_t;

/// @brief Create a new checkbox widget
/// @param parent Parent widget (can be NULL)
/// @param text Checkbox label text
/// @return New checkbox widget or NULL on failure
vg_checkbox_t *vg_checkbox_create(vg_widget_t *parent, const char *text);

/// @brief Set checkbox checked state
/// @param checkbox Checkbox widget
/// @param checked New checked state
void vg_checkbox_set_checked(vg_checkbox_t *checkbox, bool checked);

/// @brief Get checkbox checked state
/// @param checkbox Checkbox widget
/// @return Current checked state
bool vg_checkbox_is_checked(vg_checkbox_t *checkbox);

/// @brief Toggle checkbox state
/// @param checkbox Checkbox widget
void vg_checkbox_toggle(vg_checkbox_t *checkbox);

/// @brief Set checkbox text
/// @param checkbox Checkbox widget
/// @param text New text (copied internally)
void vg_checkbox_set_text(vg_checkbox_t *checkbox, const char *text);

/// @brief Set state change callback
/// @param checkbox Checkbox widget
/// @param callback Change handler function
/// @param user_data User data passed to callback
void vg_checkbox_set_on_change(vg_checkbox_t *checkbox,
                               vg_checkbox_callback_t callback,
                               void *user_data);

/// @brief Set checkbox label font
/// @param checkbox Checkbox widget
/// @param font Font to use
/// @param size Font size in pixels
void vg_checkbox_set_font(vg_checkbox_t *checkbox, vg_font_t *font, float size);

/// @brief Set the indeterminate (tri-state) state of a checkbox.
/// @param checkbox     Checkbox widget.
/// @param indeterminate true to show dash (indeterminate); false to clear it.
void vg_checkbox_set_indeterminate(vg_checkbox_t *checkbox, bool indeterminate);

/// @brief Return true when the checkbox is currently in its indeterminate state.
bool vg_checkbox_is_indeterminate(vg_checkbox_t *checkbox);

//=============================================================================
// ScrollView Widget
//=============================================================================

/// @brief Scroll direction flags
typedef enum vg_scroll_direction {
    VG_SCROLL_HORIZONTAL = (1 << 0),
    VG_SCROLL_VERTICAL = (1 << 1),
    VG_SCROLL_BOTH = VG_SCROLL_HORIZONTAL | VG_SCROLL_VERTICAL
} vg_scroll_direction_t;

/// @brief ScrollView widget structure
typedef struct vg_scrollview {
    vg_widget_t base;

    float scroll_x;        ///< Horizontal scroll position
    float scroll_y;        ///< Vertical scroll position
    float smooth_target_x; ///< Wheel easing destination (smooth scrolling)
    float smooth_target_y; ///< Wheel easing destination (smooth scrolling)
    bool smooth_animating; ///< True while wheel easing is in flight
    float content_width;   ///< Effective content width after auto/explicit resolution
    float content_height;  ///< Effective content height after auto/explicit resolution
    float
        explicit_content_width; ///< Caller-provided content width, when has_explicit_content_width
    float explicit_content_height;    ///< Caller-provided content height, when
                                      ///< has_explicit_content_height
    bool has_explicit_content_width;  ///< Keep width fixed instead of deriving from children
    bool has_explicit_content_height; ///< Keep height fixed instead of deriving from children
    vg_scroll_direction_t direction;  ///< Scroll direction

    // Scrollbars
    bool show_h_scrollbar;     ///< Show horizontal scrollbar
    bool show_v_scrollbar;     ///< Show vertical scrollbar
    bool auto_hide_scrollbars; ///< Auto-hide scrollbars when not needed
    float scrollbar_width;     ///< Scrollbar width

    // Scrollbar appearance
    uint32_t track_color;       ///< Scrollbar track color
    uint32_t thumb_color;       ///< Scrollbar thumb color
    uint32_t thumb_hover_color; ///< Thumb color when hovered

    // State
    bool h_scrollbar_hovered;      ///< Is horizontal scrollbar hovered
    bool v_scrollbar_hovered;      ///< Is vertical scrollbar hovered
    bool h_scrollbar_dragging;     ///< Is horizontal scrollbar being dragged
    bool v_scrollbar_dragging;     ///< Is vertical scrollbar being dragged
    float drag_offset;             ///< Drag offset for scrollbar
    vg_widget_t *scroll_to_widget; ///< Pending descendant reveal target (non-owning)
    uint64_t scroll_to_widget_id;  ///< Immutable ID guarding the pending target
} vg_scrollview_t;

/// @brief Create a new scroll view widget
/// @param parent Parent widget (can be NULL)
/// @return New scroll view widget or NULL on failure
vg_scrollview_t *vg_scrollview_create(vg_widget_t *parent);

/// @brief Set scroll position
/// @param scroll Scroll view widget
/// @param x Horizontal scroll position
/// @param y Vertical scroll position
void vg_scrollview_set_scroll(vg_scrollview_t *scroll, float x, float y);

/// @brief Advance wheel smooth-scroll easing by one frame (ADR 0137).
/// @param scroll Scroll view to advance.
/// @param delta_ms Elapsed milliseconds this frame.
/// @return True while easing is still in flight (keeps the app animating).
bool vg_scrollview_tick(vg_scrollview_t *scroll, float delta_ms);

/// @brief Get scroll position
/// @param scroll Scroll view widget
/// @param out_x Pointer to receive X position (can be NULL)
/// @param out_y Pointer to receive Y position (can be NULL)
void vg_scrollview_get_scroll(vg_scrollview_t *scroll, float *out_x, float *out_y);

/// @brief Set content size
/// @param scroll Scroll view widget
/// @param width Content width (0 = auto)
/// @param height Content height (0 = auto)
void vg_scrollview_set_content_size(vg_scrollview_t *scroll, float width, float height);

/// @brief Scroll to make a descendant widget visible.
/// @details The request is retained through the next layout pass so callers
///          may reveal a descendant while an ancestor pane is being restored.
/// @param scroll Scroll view widget
/// @param child Descendant widget to scroll into view
void vg_scrollview_scroll_to_widget(vg_scrollview_t *scroll, vg_widget_t *child);

/// @brief Set scroll direction
/// @param scroll Scroll view widget
/// @param direction Scroll direction flags
void vg_scrollview_set_direction(vg_scrollview_t *scroll, vg_scroll_direction_t direction);

//=============================================================================
// ListBox Widget
//=============================================================================

/// @brief ListBox item structure
typedef struct vg_listbox_item {
    uint64_t magic;           ///< Live-item sentinel for stale handle detection
    struct vg_listbox *owner; ///< Owning listbox while the item is live
    char *text;               ///< Item text (owned)
    size_t text_len;          ///< Item text length in bytes
    uint32_t text_color;      ///< Optional text color override
    void *user_data;          ///< User data
    bool owns_user_data;      ///< Free user_data when the item is destroyed
    bool has_text_color;      ///< Whether text_color overrides the listbox text color
    bool selected;            ///< Is item selected
    struct vg_listbox_item *next;
    struct vg_listbox_item *prev;
    struct vg_listbox_item *retired_next; ///< Retired-item list link
} vg_listbox_item_t;

/// @brief ListBox selection callback
typedef void (*vg_listbox_callback_t)(vg_widget_t *listbox,
                                      vg_listbox_item_t *item,
                                      void *user_data);

// Forward declaration for icon
struct vg_icon;

/// @brief Virtual mode data provider callback
/// @param listbox ListBox widget
/// @param index Item index
/// @param text Output: item text (must NOT be freed by caller)
/// @param icon Output: item icon (optional, can be NULL)
/// @param user_data User data
typedef void (*vg_listbox_data_provider_t)(
    vg_widget_t *listbox, size_t index, const char **text, struct vg_icon *icon, void *user_data);

/// @brief Notification fired immediately before a virtual model is detached from a ListBox.
/// @details The callback is invoked at most once for each successful binding. It runs when the
///          binding is explicitly cleared, replaced, disabled through the legacy virtual-mode
///          API, or when the ListBox is destroyed. The callback must not destroy @p listbox and
///          must not attempt to clear the same binding recursively.
/// @param listbox ListBox whose binding is being detached; valid only for the duration of the
///                callback.
/// @param user_data Opaque model-owned pointer supplied to @ref vg_listbox_bind_virtual_model.
typedef void (*vg_listbox_virtual_unbind_callback_t)(vg_widget_t *listbox, void *user_data);

/// @brief Virtual item cache entry
typedef struct vg_listbox_cache_entry {
    char *text;    ///< Cached text (owned)
    bool selected; ///< Selection state
} vg_listbox_cache_entry_t;

/// @brief ListBox widget structure
typedef struct vg_listbox {
    vg_widget_t base;

    vg_listbox_item_t *first_item;      ///< First item
    vg_listbox_item_t *last_item;       ///< Last item
    int item_count;                     ///< Number of items
    vg_listbox_item_t *selected;        ///< Currently selected item
    vg_listbox_item_t *prev_selected;   ///< Previous selection (for change detection)
    vg_listbox_item_t *anchor_selected; ///< Range-selection anchor (non-virtual mode)
    vg_listbox_item_t *hovered;         ///< Currently hovered item
    vg_listbox_item_t *retired_items;   ///< Detached stale handles freed when listbox is destroyed
    char *saved_tooltip_text;           ///< Original widget tooltip during row hover
    char *ellipsis_scratch;             ///< Reusable fitted-row paint buffer
    size_t ellipsis_scratch_capacity;   ///< Allocated bytes in ellipsis_scratch
    bool hover_tooltip_active;          ///< Whether row text currently replaces base tooltip
    uint64_t selection_revision;        ///< Incremented whenever logical selection changes
    uint64_t reported_selection_revision; ///< Last selection revision reported to runtime callers

    vg_font_t *font;   ///< Font for rendering
    float font_size;   ///< Font size
    float item_height; ///< Height of each item
    float scroll_y;    ///< Vertical scroll position

    bool multi_select; ///< Allow multiple selection

    // Virtual scrolling mode
    bool virtual_mode;                                   ///< Enable virtual scrolling
    size_t total_item_count;                             ///< Total items (when virtual)
    vg_listbox_data_provider_t data_provider;            ///< Data provider callback
    void *data_provider_user_data;                       ///< Data provider user data
    vg_listbox_virtual_unbind_callback_t virtual_unbind; ///< Binding-lifetime notification.
    void *virtual_unbind_user_data; ///< Opaque pointer passed to @ref virtual_unbind.

    // Virtual mode visible range
    size_t visible_start; ///< First visible item index
    size_t visible_count; ///< Number of visible items

    // Virtual mode cache
    vg_listbox_cache_entry_t *visible_cache; ///< Cache for visible items
    size_t cache_capacity;                   ///< Cache capacity

    // Virtual mode selection (packed bitmap for large lists)
    uint8_t *selection_bitmap;        ///< Packed selection bits for virtual mode
    size_t selection_bitmap_size;     ///< Logical bitmap size
    size_t selection_bitmap_capacity; ///< Allocated bitmap capacity
    size_t selected_index;            ///< Currently selected index (virtual mode)
    size_t prev_selected_index;       ///< Previous selected index (virtual mode change detection)
    size_t anchor_selected_index;     ///< Range-selection anchor (virtual mode)
    size_t hovered_index;             ///< Currently hovered index (virtual mode)

    // Appearance
    uint32_t bg_color;     ///< Background color
    uint32_t item_bg;      ///< Item background
    uint32_t selected_bg;  ///< Selected item background
    uint32_t hover_bg;     ///< Hovered item background
    uint32_t text_color;   ///< Text color
    uint32_t border_color; ///< Border color

    // Callbacks
    vg_listbox_callback_t on_select;
    void *on_select_data;
    vg_listbox_callback_t on_activate; ///< Double-click
    void *on_activate_data;
} vg_listbox_t;

/// @brief Create a new listbox widget.
/// @param parent Parent widget (can be NULL).
/// @return New listbox widget or NULL on failure.
vg_listbox_t *vg_listbox_create(vg_widget_t *parent);

/// @brief Add an item to the listbox.
/// @param listbox  ListBox widget.
/// @param text     Item label text (copied internally).
/// @param user_data Caller-owned data associated with the item.
/// @return Pointer to the new item, or NULL on allocation failure.
vg_listbox_item_t *vg_listbox_add_item(vg_listbox_t *listbox, const char *text, void *user_data);

/// @brief Remove an item from the listbox and free it.
/// @param listbox ListBox widget.
/// @param item    Item to remove (must belong to @p listbox).
void vg_listbox_remove_item(vg_listbox_t *listbox, vg_listbox_item_t *item);

/// @brief Remove all items and reset selection; virtual mode is disabled and cleared.
/// @param listbox ListBox widget.
void vg_listbox_clear(vg_listbox_t *listbox);

/// @brief Reclaim one specific retired ListBox item tombstone.
/// @details Searches only the owning ListBox's retirement chain, unlinks @p item when present, and
///          releases its already-cleared payload record. The caller must ensure no external handle
///          can subsequently inspect @p item. Live items, foreign pointers, NULL inputs, and items
///          already reclaimed return false without side effects. This operation exists for managed
///          runtimes that retain removed records until the last wrapper is finalized.
/// @param listbox Live owner of the retired item.
/// @param item Candidate tombstone address; ownership is not transferred unless found.
/// @return true when the tombstone was found and freed, otherwise false.
bool vg_listbox_reclaim_retired_item(vg_listbox_t *listbox, vg_listbox_item_t *item);

/// @brief Select an item; passing NULL clears all non-virtual selection flags.
/// @param listbox ListBox widget.
/// @param item    Item to select, or NULL to deselect all.
void vg_listbox_select(vg_listbox_t *listbox, vg_listbox_item_t *item);

/// @brief Get the currently selected item.
/// @param listbox ListBox widget.
/// @return Selected item pointer, or NULL if nothing is selected.
vg_listbox_item_t *vg_listbox_get_selected(vg_listbox_t *listbox);

/// @brief Return true when an item handle still belongs to a live listbox.
/// @param item Item handle to test.
/// @return true if the item is still live; false if it has been removed or the listbox destroyed.
bool vg_listbox_item_is_live(const vg_listbox_item_t *item);

/// @brief Override the rendered text color for one item.
/// @param item  Live ListBox item.
/// @param color RGB/RGBA color value interpreted by the active backend.
void vg_listbox_item_set_text_color(vg_listbox_item_t *item, uint32_t color);

/// @brief Set the font used for item text rendering.
/// @param listbox ListBox widget.
/// @param font    Font handle.
/// @param size    Font size in pixels.
void vg_listbox_set_font(vg_listbox_t *listbox, vg_font_t *font, float size);

/// @brief Set the callback fired when the selection changes.
/// @param listbox   ListBox widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_listbox_set_on_select(vg_listbox_t *listbox,
                              vg_listbox_callback_t callback,
                              void *user_data);

// --- Virtual Scrolling ---

/// @brief Enable virtual scrolling mode
/// @param listbox ListBox widget
/// @param enabled Enable virtual mode
/// @param total_count Total number of items
/// @param item_height Fixed item height (required for virtual mode)
void vg_listbox_set_virtual_mode(vg_listbox_t *listbox,
                                 bool enabled,
                                 size_t total_count,
                                 float item_height);

/// @brief Set data provider for virtual mode
/// @param listbox ListBox widget
/// @param provider Data provider callback
/// @param user_data User data
void vg_listbox_set_data_provider(vg_listbox_t *listbox,
                                  vg_listbox_data_provider_t provider,
                                  void *user_data);

/// @brief Atomically attach an external viewport-backed model to a ListBox.
/// @details On success the ListBox enters virtual mode, owns only a packed selection bitmap and a
///          viewport-sized text cache, and calls @p provider only for rows in the painted
///          viewport. An existing external binding is detached after all allocations for the new
///          binding have succeeded. The model and ListBox do not retain or own each other.
/// @param listbox ListBox to bind; NULL is rejected.
/// @param total_count Logical number of model rows. Counts that overflow bitmap sizing are
///                    rejected.
/// @param item_height Positive finite physical row height used by the lower renderer.
/// @param provider Callback that supplies borrowed row text; NULL is rejected.
/// @param user_data Opaque model pointer passed to @p provider and @p on_unbind.
/// @param on_unbind Callback used to invalidate the model's non-owning ListBox pointer. May be
///                  NULL when no lifetime notification is required.
/// @return true when the new binding is installed; false on invalid input or allocation failure,
///         leaving an existing binding unchanged.
bool vg_listbox_bind_virtual_model(vg_listbox_t *listbox,
                                   size_t total_count,
                                   float item_height,
                                   vg_listbox_data_provider_t provider,
                                   void *user_data,
                                   vg_listbox_virtual_unbind_callback_t on_unbind);

/// @brief Detach an external virtual model and return the ListBox to ordinary non-virtual mode.
/// @details Invokes the registered unbind callback exactly once before releasing virtual cache and
///          selection storage. Ordinary non-virtual items remain intact. NULL is ignored.
/// @param listbox ListBox whose non-owning model binding should be cleared.
void vg_listbox_clear_virtual_model(vg_listbox_t *listbox);

/// @brief Update total count (e.g., after filtering) in virtual mode
/// @param listbox ListBox widget
/// @param count New total count
void vg_listbox_set_total_count(vg_listbox_t *listbox, size_t count);

/// @brief Invalidate all cached items (force refresh)
/// @param listbox ListBox widget
void vg_listbox_invalidate_items(vg_listbox_t *listbox);

/// @brief Invalidate a specific item in the cache
/// @param listbox ListBox widget
/// @param index Item index to invalidate
void vg_listbox_invalidate_item(vg_listbox_t *listbox, size_t index);

/// @brief Return the first virtual row intersecting the current viewport.
/// @details The value is computed in O(1) from scroll offset and row height and does not invoke the
///          data provider. For a non-virtual, empty, or NULL ListBox the result is zero.
/// @param listbox ListBox to inspect.
/// @return Zero-based first visible row index.
size_t vg_listbox_get_visible_first(vg_listbox_t *listbox);

/// @brief Return the number of virtual rows materialized for the current viewport.
/// @details Includes the renderer's trailing safety rows and is capped at the logical item count.
///          The value is computed without invoking the model data provider.
/// @param listbox ListBox to inspect.
/// @return Visible/cache row count, or zero for a non-virtual, empty, or NULL ListBox.
size_t vg_listbox_get_visible_count(vg_listbox_t *listbox);

/// @brief Select item by index (virtual mode).
/// @param listbox ListBox widget
/// @param index Item index, or SIZE_MAX to clear virtual selection.
void vg_listbox_select_index(vg_listbox_t *listbox, size_t index);

/// @brief Get selected index (virtual mode)
/// @param listbox ListBox widget
/// @return Selected index or SIZE_MAX if none
size_t vg_listbox_get_selected_index(vg_listbox_t *listbox);

/// @brief Scroll to the first row without changing selection.
/// @param listbox ListBox widget.
void vg_listbox_scroll_to_top(vg_listbox_t *listbox);

/// @brief Scroll to the last row without changing selection.
/// @param listbox ListBox widget.
void vg_listbox_scroll_to_bottom(vg_listbox_t *listbox);

//=============================================================================
// Dropdown/ComboBox Widget
//=============================================================================

/// @brief Dropdown selection callback
typedef void (*vg_dropdown_callback_t)(vg_widget_t *dropdown,
                                       int index,
                                       const char *text,
                                       void *user_data);

/// @brief Dropdown widget structure
typedef struct vg_dropdown {
    vg_widget_t base;

    char **items;       ///< Array of item strings (owned)
    int item_count;     ///< Number of items
    int item_capacity;  ///< Allocated capacity
    int selected_index; ///< Currently selected index (-1 = none)

    vg_font_t *font;   ///< Font for rendering
    float font_size;   ///< Font size
    char *placeholder; ///< Placeholder when nothing selected (owned)

    bool open;             ///< Is dropdown list open
    int hovered_index;     ///< Hovered item index
    float dropdown_height; ///< Max height of dropdown list
    float scroll_y;        ///< Scroll position when list is long

    // Appearance
    uint32_t bg_color;     ///< Background color
    uint32_t text_color;   ///< Text color
    uint32_t border_color; ///< Border color
    uint32_t dropdown_bg;  ///< Dropdown list background
    uint32_t hover_bg;     ///< Hovered item background
    uint32_t selected_bg;  ///< Selected item in list background
    float arrow_size;      ///< Dropdown arrow size

    // Callbacks
    vg_dropdown_callback_t on_change;
    void *on_change_data;
} vg_dropdown_t;

/// @brief Create a new dropdown widget.
/// @param parent Parent widget (can be NULL).
/// @return New dropdown widget or NULL on failure.
vg_dropdown_t *vg_dropdown_create(vg_widget_t *parent);

/// @brief Add an item to the dropdown.
/// @param dropdown Dropdown widget.
/// @param text     Item label text (copied internally).
/// @return Index of the newly added item.
int vg_dropdown_add_item(vg_dropdown_t *dropdown, const char *text);

/// @brief Remove an item by index.
/// @param dropdown Dropdown widget.
/// @param index    Zero-based index of the item to remove.
void vg_dropdown_remove_item(vg_dropdown_t *dropdown, int index);

/// @brief Remove and free all items from the dropdown.
/// @param dropdown Dropdown widget.
void vg_dropdown_clear(vg_dropdown_t *dropdown);

/// @brief Set the currently selected item by index.
/// @param dropdown Dropdown widget.
/// @param index    Zero-based item index, or -1 to show the placeholder. Other invalid
///                 indices are ignored.
void vg_dropdown_set_selected(vg_dropdown_t *dropdown, int index);

/// @brief Get the currently selected item index.
/// @param dropdown Dropdown widget.
/// @return Zero-based index of the selected item, or -1 if nothing is selected.
int vg_dropdown_get_selected(vg_dropdown_t *dropdown);

/// @brief Get the text of the currently selected item.
/// @param dropdown Dropdown widget.
/// @return Read-only pointer to the selected item's text, or NULL if nothing is selected.
const char *vg_dropdown_get_selected_text(vg_dropdown_t *dropdown);

/// @brief Set placeholder text shown when no item is selected.
/// @param dropdown Dropdown widget.
/// @param text     Placeholder text (copied internally; NULL clears it).
void vg_dropdown_set_placeholder(vg_dropdown_t *dropdown, const char *text);

/// @brief Set the font used for item text rendering.
/// @param dropdown Dropdown widget.
/// @param font     Font handle.
/// @param size     Font size in pixels.
void vg_dropdown_set_font(vg_dropdown_t *dropdown, vg_font_t *font, float size);

/// @brief Set the callback fired when the selection changes.
/// @param dropdown  Dropdown widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_dropdown_set_on_change(vg_dropdown_t *dropdown,
                               vg_dropdown_callback_t callback,
                               void *user_data);

//=============================================================================
// Slider Widget
//=============================================================================

/// @brief Slider orientation
typedef enum vg_slider_orientation {
    VG_SLIDER_HORIZONTAL,
    VG_SLIDER_VERTICAL
} vg_slider_orientation_t;

/// @brief Slider value change callback
typedef void (*vg_slider_callback_t)(vg_widget_t *slider, float value, void *user_data);

/// @brief Slider widget structure
typedef struct vg_slider {
    vg_widget_t base;

    float value;     ///< Current value
    float min_value; ///< Minimum value
    float max_value; ///< Maximum value
    float step;      ///< Step increment (0 = continuous)
    vg_slider_orientation_t orientation;

    // Appearance
    float track_thickness;      ///< Track thickness
    float thumb_size;           ///< Thumb diameter
    uint32_t track_color;       ///< Track color
    uint32_t fill_color;        ///< Filled portion color
    uint32_t thumb_color;       ///< Thumb color
    uint32_t thumb_hover_color; ///< Thumb hover color

    // Display
    bool show_value; ///< Show value label
    vg_font_t *font; ///< Font for value label
    float font_size; ///< Font size

    // State
    bool dragging;      ///< Is thumb being dragged
    bool thumb_hovered; ///< Is thumb hovered

    // Callbacks
    vg_slider_callback_t on_change;
    void *on_change_data;
} vg_slider_t;

/// @brief Create a new slider widget.
/// @param parent      Parent widget (can be NULL).
/// @param orientation VG_SLIDER_HORIZONTAL or VG_SLIDER_VERTICAL.
/// @return New slider widget or NULL on failure.
vg_slider_t *vg_slider_create(vg_widget_t *parent, vg_slider_orientation_t orientation);

/// @brief Set the current slider value (clamped to [min, max]).
/// @param slider Slider widget.
/// @param value  New value.
void vg_slider_set_value(vg_slider_t *slider, float value);

/// @brief Get the current slider value.
/// @param slider Slider widget.
/// @return Current value in [min, max].
float vg_slider_get_value(vg_slider_t *slider);

/// @brief Set the allowed value range.
/// @param slider  Slider widget.
/// @param min_val Minimum value.
/// @param max_val Maximum value.
void vg_slider_set_range(vg_slider_t *slider, float min_val, float max_val);

/// @brief Set the discrete step increment.
/// @param slider Slider widget.
/// @param step   Step size; pass 0 for continuous movement.
void vg_slider_set_step(vg_slider_t *slider, float step);

/// @brief Set the callback fired when the value changes.
/// @param slider    Slider widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_slider_set_on_change(vg_slider_t *slider, vg_slider_callback_t callback, void *user_data);

//=============================================================================
// ProgressBar Widget
//=============================================================================

/// @brief ProgressBar style
typedef enum vg_progress_style {
    VG_PROGRESS_BAR,          ///< Standard horizontal bar
    VG_PROGRESS_CIRCULAR,     ///< Circular progress
    VG_PROGRESS_INDETERMINATE ///< Indeterminate animation
} vg_progress_style_t;

/// @brief ProgressBar widget structure
typedef struct vg_progressbar {
    vg_widget_t base;

    float value;               ///< Current value (0-1)
    vg_progress_style_t style; ///< Progress style

    // Appearance
    uint32_t track_color; ///< Track/background color
    uint32_t fill_color;  ///< Fill/progress color
    float corner_radius;  ///< Corner radius for bar style

    // Display
    bool show_percentage; ///< Show percentage text
    vg_font_t *font;      ///< Font for percentage
    float font_size;      ///< Font size

    // Animation (for indeterminate)
    float animation_phase; ///< Current animation phase
} vg_progressbar_t;

/// @brief Create a new progress bar widget.
/// @param parent Parent widget (can be NULL).
/// @return New progress bar widget or NULL on failure.
vg_progressbar_t *vg_progressbar_create(vg_widget_t *parent);

/// @brief Set the progress value (clamped to [0, 1]).
/// @param progress Progress bar widget.
/// @param value    Normalised progress in the range [0.0, 1.0].
void vg_progressbar_set_value(vg_progressbar_t *progress, float value);

/// @brief Get the current progress value.
/// @param progress Progress bar widget.
/// @return Normalised progress in [0.0, 1.0].
float vg_progressbar_get_value(vg_progressbar_t *progress);

/// @brief Set the rendering style.
/// @param progress Progress bar widget.
/// @param style    One of VG_PROGRESS_BAR, VG_PROGRESS_CIRCULAR, or VG_PROGRESS_INDETERMINATE.
void vg_progressbar_set_style(vg_progressbar_t *progress, vg_progress_style_t style);

/// @brief Show or hide the percentage text label.
/// @param progress Progress bar widget.
/// @param show     true to overlay the percentage string on the bar.
void vg_progressbar_show_percentage(vg_progressbar_t *progress, bool show);

/// @brief Set the font used for the optional percentage label.
/// @param progress Progress bar widget.
/// @param font     Font handle.
/// @param size     Font size in pixels.
void vg_progressbar_set_font(vg_progressbar_t *progress, vg_font_t *font, float size);

/// @brief Advance indeterminate animation; call each frame with elapsed seconds
/// @param progress Progress bar widget
/// @param dt Elapsed time in seconds since last call
void vg_progressbar_tick(vg_progressbar_t *progress, float dt);

//=============================================================================
// RadioButton Widget
//=============================================================================

/// @brief RadioButton group - manages mutual exclusivity
typedef struct vg_radiogroup {
    struct vg_radiobutton **buttons;      ///< Array of buttons in group
    int button_count;                     ///< Number of buttons
    int button_capacity;                  ///< Allocated capacity
    int selected_index;                   ///< Currently selected index
    uint64_t revision;                    ///< Monotonic membership/selection revision
    uint64_t selection_revision;          ///< Monotonic selected-index transition revision
    uint64_t reported_selection_revision; ///< Last selection revision consumed by WasChanged
} vg_radiogroup_t;

/// @brief RadioButton callback
typedef void (*vg_radio_callback_t)(vg_widget_t *radio, bool selected, void *user_data);

/// @brief RadioButton widget structure
typedef struct vg_radiobutton {
    vg_widget_t base;

    char *text;             ///< Label text (owned)
    vg_font_t *font;        ///< Font for label
    float font_size;        ///< Font size
    bool selected;          ///< Is this button selected
    vg_radiogroup_t *group; ///< Group this button belongs to
    void *user_data;        ///< Opaque caller data
    bool owns_user_data;    ///< Whether destruction must free user_data

    // Appearance
    float circle_size;     ///< Radio circle size
    float gap;             ///< Gap between circle and label
    uint32_t circle_color; ///< Circle border color
    uint32_t fill_color;   ///< Selected fill color
    uint32_t text_color;   ///< Text color

    // Callback
    vg_radio_callback_t on_change;
    void *on_change_data;
} vg_radiobutton_t;

/// @brief Create a new radio group to manage mutual exclusivity.
/// @return Newly allocated radio group, or NULL on failure.
vg_radiogroup_t *vg_radiogroup_create(void);

/// @brief Destroy a radio group (does not destroy the member buttons).
/// @param group Radio group to destroy (may be NULL).
void vg_radiogroup_destroy(vg_radiogroup_t *group);

/// @brief Create a new radio button and register it in a group.
/// @param parent Parent widget (can be NULL).
/// @param text   Label text (copied internally).
/// @param group  Group that enforces mutual exclusivity (can be NULL for standalone).
/// @return New radio button widget or NULL on failure.
vg_radiobutton_t *vg_radiobutton_create(vg_widget_t *parent,
                                        const char *text,
                                        vg_radiogroup_t *group);

/// @brief Set this radio button's selected state and deselect siblings in its group.
/// @param radio    Radio button widget.
/// @param selected true to select; false to deselect.
void vg_radiobutton_set_selected(vg_radiobutton_t *radio, bool selected);

/// @brief Check whether this radio button is currently selected.
/// @param radio Radio button widget.
/// @return true if selected.
bool vg_radiobutton_is_selected(vg_radiobutton_t *radio);

/// @brief Get the index of the currently selected button within the group.
/// @param group Radio group.
/// @return Zero-based index of the selected button, or -1 if none is selected.
int vg_radiogroup_get_selected(vg_radiogroup_t *group);

/// @brief Select the button at the given index within the group.
/// @param group Radio group.
/// @param index Zero-based index of the button to select.
void vg_radiogroup_set_selected(vg_radiogroup_t *group, int index);

/// @brief Attempt to select a group member by index or clear the selection.
/// @details An index of -1 clears the selection. Values below -1 or at/above the member count are
///          rejected without changing state. Selecting the current index succeeds as a no-op.
/// @param group Radio group to update.
/// @param index Zero-based member index or -1 to select none.
/// @return true when the request is valid, otherwise false.
bool vg_radiogroup_try_set_selected(vg_radiogroup_t *group, int index);

/// @brief Return the number of live buttons registered with a radio group.
/// @param group Radio group to inspect.
/// @return Non-negative member count, or zero when @p group is NULL.
int vg_radiogroup_get_count(const vg_radiogroup_t *group);

/// @brief Consume the radio group's independent selected-index change edge.
/// @details Membership-only changes do not set this edge. Multiple unreported selection changes
///          coalesce into one true result and do not consume the general revision.
/// @param group Radio group to inspect.
/// @return true once after one or more unreported selected-index transitions.
bool vg_radiogroup_was_changed(vg_radiogroup_t *group);

/// @brief Return the radio group's non-consuming state revision.
/// @details The revision advances for membership and selected-index changes and saturates at
///          UINT64_MAX.
/// @param group Radio group to inspect.
/// @return Monotonic revision, or zero when @p group is NULL.
uint64_t vg_radiogroup_get_revision(const vg_radiogroup_t *group);

/// @brief Replace a radio button's visible label text atomically.
/// @details The text is copied before the previous value is released. NULL is treated as an empty
///          string. Allocation failure preserves the previous text.
/// @param radio Radio button widget to update; NULL is ignored.
/// @param text UTF-8 label text to copy.
void vg_radiobutton_set_text(vg_radiobutton_t *radio, const char *text);

/// @brief Return a radio button's current label text.
/// @param radio Radio button widget to inspect.
/// @return Borrowed null-terminated text, or NULL when @p radio is NULL.
const char *vg_radiobutton_get_text(const vg_radiobutton_t *radio);

/// @brief Store borrowed opaque caller data on a radio button.
/// @details Replacing an internally-owned prior payload releases it. The new pointer remains owned
///          by the caller and is never dereferenced by the widget.
/// @param radio Radio button widget to update; NULL is ignored.
/// @param data Borrowed pointer, or NULL to clear it.
void vg_radiobutton_set_data(vg_radiobutton_t *radio, void *data);

/// @brief Return the opaque data pointer stored on a radio button.
/// @param radio Radio button widget to inspect.
/// @return Borrowed opaque pointer, or NULL when absent or @p radio is NULL.
void *vg_radiobutton_get_data(const vg_radiobutton_t *radio);

//=============================================================================
// Image Widget
//=============================================================================

/// @brief Image scaling mode
typedef enum vg_image_scale {
    VG_IMAGE_SCALE_NONE,   ///< No scaling (original size)
    VG_IMAGE_SCALE_FIT,    ///< Scale to fit, maintain aspect ratio
    VG_IMAGE_SCALE_FILL,   ///< Scale to fill, may crop
    VG_IMAGE_SCALE_STRETCH ///< Stretch to fill (distorts)
} vg_image_scale_t;

/// @brief Image sampling filter used whenever source pixels are resized.
typedef enum vg_image_filter {
    VG_IMAGE_FILTER_NEAREST = 0, ///< Deterministic nearest-neighbour sampling.
    VG_IMAGE_FILTER_BILINEAR = 1 ///< Four-sample bilinear interpolation.
} vg_image_filter_t;

/// @brief Image widget structure
typedef struct vg_image {
    vg_widget_t base;

    uint8_t *pixels;             ///< Pixel data (RGBA, owned).
    size_t pixel_capacity;       ///< Allocated byte capacity of @ref pixels.
    int img_width;               ///< Original image width.
    int img_height;              ///< Original image height.
    vg_image_scale_t scale_mode; ///< Scaling mode.
    vg_image_filter_t filter;    ///< Sampling filter for resized output.
    bool pixels_opaque;          ///< True when every source alpha byte is 255.

    uint8_t *scaled_pixels;          ///< Reusable resized RGBA cache (owned).
    size_t scaled_capacity;          ///< Allocated byte capacity of @ref scaled_pixels.
    int scaled_width;                ///< Width represented by the resized cache.
    int scaled_height;               ///< Height represented by the resized cache.
    uint64_t content_revision;       ///< Saturating source-content revision.
    uint64_t scaled_revision;        ///< Source revision represented by the cache.
    vg_image_filter_t scaled_filter; ///< Filter represented by the cache.

    // Appearance
    uint32_t bg_color;   ///< Background color (shown if image doesn't fill)
    float opacity;       ///< Image opacity (0-1)
    float corner_radius; ///< Corner radius for rounded images
} vg_image_t;

/// @brief Create a new image widget with no initial pixel data.
/// @param parent Parent widget (can be NULL).
/// @return New image widget or NULL on failure.
vg_image_t *vg_image_create(vg_widget_t *parent);

/// @brief Set image pixel data from an RGBA buffer (copied internally).
/// @param image  Image widget.
/// @param pixels RGBA pixel data (4 bytes per pixel).
/// @param width  Image width in pixels.
/// @param height Image height in pixels.
void vg_image_set_pixels(vg_image_t *image, const uint8_t *pixels, int width, int height);

/// @brief Atomically replace image pixels with a copied RGBA buffer.
/// @details Validation and any required allocation complete before the old image state is
///          replaced. Existing storage is reused when its capacity is sufficient. Unlike
///          @ref vg_image_set_pixels, invalid input never clears the current image.
/// @param image Image widget to update; NULL is rejected.
/// @param pixels Source RGBA bytes in row-major order; NULL is rejected.
/// @param width Positive source width in pixels.
/// @param height Positive source height in pixels.
/// @return true when all pixels were copied; false on invalid dimensions or allocation failure.
bool vg_image_try_set_pixels(vg_image_t *image, const uint8_t *pixels, int width, int height);

/// @brief Atomically copy a rectangular RGBA source region into the current image.
/// @details Both rectangles must fit completely in their respective buffers. Validation failure
///          leaves every destination byte unchanged. Overlapping source and destination storage
///          is supported; temporary storage is allocated before mutation when overlap requires it.
/// @param image Image widget whose existing pixel buffer receives the update.
/// @param pixels Source RGBA byte buffer.
/// @param source_width Positive full width of @p pixels.
/// @param source_height Positive full height of @p pixels.
/// @param source_x Zero-based source-region left coordinate.
/// @param source_y Zero-based source-region top coordinate.
/// @param width Positive region width.
/// @param height Positive region height.
/// @param dest_x Zero-based destination-region left coordinate.
/// @param dest_y Zero-based destination-region top coordinate.
/// @return true on a complete copy; false on invalid input or temporary-allocation failure.
bool vg_image_update_region(vg_image_t *image,
                            const uint8_t *pixels,
                            int source_width,
                            int source_height,
                            int source_x,
                            int source_y,
                            int width,
                            int height,
                            int dest_x,
                            int dest_y);

/// @brief Load image pixel data from a file (PNG, JPEG, or BMP).
/// @param image Image widget.
/// @param path  Filesystem path to the image file (UTF-8).
/// @return true on success; false if the file cannot be read or decoded.
bool vg_image_load_file(vg_image_t *image, const char *path);

/// @brief Free the current pixel data and reset the image to an empty state.
/// @param image Image widget.
void vg_image_clear(vg_image_t *image);

/// @brief Set the scaling mode used when the widget size differs from the image size.
/// @param image Image widget.
/// @param mode  One of VG_IMAGE_SCALE_NONE, FIT, FILL, or STRETCH.
void vg_image_set_scale_mode(vg_image_t *image, vg_image_scale_t mode);

/// @brief Select the sampling filter used when the image is resized.
/// @details Invalid enum values are normalized to @ref VG_IMAGE_FILTER_NEAREST. Changing the
///          filter invalidates the reusable scaled cache and schedules repaint without changing
///          the source pixels.
/// @param image Image widget to configure; NULL is ignored.
/// @param filter Requested nearest or bilinear filter.
void vg_image_set_filter(vg_image_t *image, vg_image_filter_t filter);

/// @brief Return the image sampling filter.
/// @param image Image widget to inspect; may be NULL.
/// @return Current filter, or @ref VG_IMAGE_FILTER_NEAREST for NULL.
vg_image_filter_t vg_image_get_filter(const vg_image_t *image);

/// @brief Set the rendering opacity.
/// @param image   Image widget.
/// @param opacity Opacity in the range [0.0, 1.0] (0 = transparent, 1 = opaque).
void vg_image_set_opacity(vg_image_t *image, float opacity);

//=============================================================================
// Spinner/NumberInput Widget
//=============================================================================

/// @brief Spinner value change callback
typedef void (*vg_spinner_callback_t)(vg_widget_t *spinner, double value, void *user_data);

/// @brief Spinner widget structure
typedef struct vg_spinner {
    vg_widget_t base;

    double value;       ///< Current value
    double min_value;   ///< Minimum value
    double max_value;   ///< Maximum value
    double step;        ///< Step increment
    int decimal_places; ///< Decimal places to display
    bool indeterminate; ///< Mixed group value; numeric value remains an editing seed

    vg_font_t *font;   ///< Font for value display
    float font_size;   ///< Font size
    char *text_buffer; ///< Text buffer for display
    bool editing;      ///< Is user editing the text
    size_t cursor_pos; ///< Caret position inside the edit buffer

    // Appearance
    uint32_t bg_color;     ///< Background color
    uint32_t text_color;   ///< Text color
    uint32_t border_color; ///< Border color
    uint32_t button_color; ///< Up/down button color
    float button_width;    ///< Width of up/down buttons

    // State
    bool up_hovered;   ///< Is up button hovered
    bool down_hovered; ///< Is down button hovered
    bool up_pressed;   ///< Is up button pressed
    bool down_pressed; ///< Is down button pressed

    // Callbacks
    vg_spinner_callback_t on_change;
    void *on_change_data;
} vg_spinner_t;

/// @brief Create a new spinner widget with default range [0, 100] and step 1.
/// @param parent Parent widget (can be NULL).
/// @return New spinner widget or NULL on failure.
vg_spinner_t *vg_spinner_create(vg_widget_t *parent);

/// @brief Set the current numeric value (clamped to [min, max]).
/// @param spinner Spinner widget.
/// @param value   New value.
void vg_spinner_set_value(vg_spinner_t *spinner, double value);

/// @brief Get the current numeric value.
/// @param spinner Spinner widget.
/// @return Current value in [min, max].
double vg_spinner_get_value(vg_spinner_t *spinner);

/// @brief Set whether the spinner represents a mixed group value.
/// @details The committed numeric value is retained as an editing seed. Setting
///          a concrete value or editing the control clears this state.
/// @param spinner Spinner widget.
/// @param indeterminate true to present a mixed value; false to reveal the
///        retained concrete value.
void vg_spinner_set_indeterminate(vg_spinner_t *spinner, bool indeterminate);

/// @brief Return whether the spinner currently represents a mixed group value.
/// @param spinner Spinner widget.
/// @return true only while the mixed-value presentation is active.
bool vg_spinner_is_indeterminate(const vg_spinner_t *spinner);

/// @brief Set the allowed value range.
/// @param spinner Spinner widget.
/// @param min_val Minimum allowed value.
/// @param max_val Maximum allowed value.
void vg_spinner_set_range(vg_spinner_t *spinner, double min_val, double max_val);

/// @brief Set the increment applied by the up/down buttons.
/// @param spinner Spinner widget.
/// @param step    Step size (must be > 0).
void vg_spinner_set_step(vg_spinner_t *spinner, double step);

/// @brief Set the number of decimal places shown in the text field.
/// @param spinner  Spinner widget.
/// @param decimals Number of digits after the decimal point (0 = integer display).
void vg_spinner_set_decimals(vg_spinner_t *spinner, int decimals);

/// @brief Set the font used to display the numeric value.
/// @param spinner Spinner widget.
/// @param font    Font handle.
/// @param size    Font size in pixels.
void vg_spinner_set_font(vg_spinner_t *spinner, vg_font_t *font, float size);

/// @brief Set the callback fired when the value changes.
/// @param spinner   Spinner widget.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_spinner_set_on_change(vg_spinner_t *spinner,
                              vg_spinner_callback_t callback,
                              void *user_data);

//=============================================================================
// ColorSwatch Widget
//=============================================================================

/// @brief ColorSwatch callback - called when color is selected
typedef void (*vg_colorswatch_callback_t)(vg_widget_t *swatch, uint32_t color, void *user_data);

/// @brief ColorSwatch widget structure - displays a single color
typedef struct vg_colorswatch {
    vg_widget_t base;

    uint32_t color;   ///< The color displayed (ARGB)
    bool selected;    ///< Is this swatch currently selected
    bool show_border; ///< Show border around swatch

    // Appearance
    float size;               ///< Swatch size (width and height)
    uint32_t border_color;    ///< Border color
    uint32_t selected_border; ///< Border color when selected
    float border_width;       ///< Border width
    float corner_radius;      ///< Corner radius

    // Callback
    vg_colorswatch_callback_t on_select;
    void *on_select_data;
} vg_colorswatch_t;

/// @brief Create a new color swatch widget
/// @param parent Parent widget (can be NULL)
/// @param color Initial color (ARGB)
/// @return New color swatch widget or NULL on failure
vg_colorswatch_t *vg_colorswatch_create(vg_widget_t *parent, uint32_t color);

/// @brief Set swatch color
/// @param swatch ColorSwatch widget
/// @param color New color (ARGB)
void vg_colorswatch_set_color(vg_colorswatch_t *swatch, uint32_t color);

/// @brief Get swatch color
/// @param swatch ColorSwatch widget
/// @return Current color (ARGB)
uint32_t vg_colorswatch_get_color(vg_colorswatch_t *swatch);

/// @brief Set selected state
/// @param swatch ColorSwatch widget
/// @param selected Selected state
void vg_colorswatch_set_selected(vg_colorswatch_t *swatch, bool selected);

/// @brief Check if swatch is selected
/// @param swatch ColorSwatch widget
/// @return true if selected
bool vg_colorswatch_is_selected(vg_colorswatch_t *swatch);

/// @brief Set selection callback
/// @param swatch ColorSwatch widget
/// @param callback Selection handler function
/// @param user_data User data passed to callback
void vg_colorswatch_set_on_select(vg_colorswatch_t *swatch,
                                  vg_colorswatch_callback_t callback,
                                  void *user_data);

/// @brief Set swatch size
/// @param swatch ColorSwatch widget
/// @param size Size (width and height)
void vg_colorswatch_set_size(vg_colorswatch_t *swatch, float size);

//=============================================================================
// ColorPalette Widget
//=============================================================================

/// @brief ColorPalette callback - called when a color is selected from palette
typedef void (*vg_colorpalette_callback_t)(vg_widget_t *palette,
                                           uint32_t color,
                                           int index,
                                           void *user_data);

/// @brief ColorPalette widget structure - grid of color swatches
typedef struct vg_colorpalette {
    vg_widget_t base;

    uint32_t *colors;   ///< Array of colors (owned)
    int color_count;    ///< Number of colors
    int columns;        ///< Number of columns in grid
    int selected_index; ///< Currently selected color index (-1 = none)

    // Appearance
    float swatch_size;        ///< Size of each swatch
    float gap;                ///< Gap between swatches
    uint32_t bg_color;        ///< Background color
    uint32_t border_color;    ///< Border around swatches
    uint32_t selected_border; ///< Border for selected swatch

    // Callback
    vg_colorpalette_callback_t on_select;
    void *on_select_data;
} vg_colorpalette_t;

/// @brief Create a new color palette widget
/// @param parent Parent widget (can be NULL)
/// @return New color palette widget or NULL on failure
vg_colorpalette_t *vg_colorpalette_create(vg_widget_t *parent);

/// @brief Set palette colors
/// @param palette ColorPalette widget
/// @param colors Array of colors (copied)
/// @param count Number of colors
void vg_colorpalette_set_colors(vg_colorpalette_t *palette, const uint32_t *colors, int count);

/// @brief Add a color to the palette
/// @param palette ColorPalette widget
/// @param color Color to add (ARGB)
void vg_colorpalette_add_color(vg_colorpalette_t *palette, uint32_t color);

/// @brief Remove one color by zero-based index.
/// @details Later colors shift left. The selected index follows the same logical color when a
///          preceding entry is removed and clears when the selected entry is removed. Invalid
///          indices leave the palette unchanged.
/// @param palette Color palette widget to update.
/// @param index Zero-based color index.
/// @return true when an entry was removed, otherwise false.
bool vg_colorpalette_remove_color(vg_colorpalette_t *palette, int index);

/// @brief Clear all colors from palette
/// @param palette ColorPalette widget
void vg_colorpalette_clear(vg_colorpalette_t *palette);

/// @brief Return the number of colors stored in a palette.
/// @param palette Color palette widget to inspect.
/// @return Non-negative color count, or zero when @p palette is NULL.
int vg_colorpalette_get_color_count(const vg_colorpalette_t *palette);

/// @brief Read one palette color by zero-based index.
/// @param palette Color palette widget to inspect.
/// @param index Zero-based color index.
/// @param out_color Receives the stored AARRGGBB value on success.
/// @return true when @p index exists and @p out_color is non-NULL.
bool vg_colorpalette_get_color_at(const vg_colorpalette_t *palette, int index, uint32_t *out_color);

/// @brief Set number of columns
/// @param palette ColorPalette widget
/// @param columns Number of columns
void vg_colorpalette_set_columns(vg_colorpalette_t *palette, int columns);

/// @brief Set selected color index
/// @param palette ColorPalette widget
/// @param index Selected index (-1 to deselect)
void vg_colorpalette_set_selected(vg_colorpalette_t *palette, int index);

/// @brief Get selected color index
/// @param palette ColorPalette widget
/// @return Selected index or -1 if none
int vg_colorpalette_get_selected(vg_colorpalette_t *palette);

/// @brief Get selected color
/// @param palette ColorPalette widget
/// @return Selected color or 0 if none selected
uint32_t vg_colorpalette_get_selected_color(vg_colorpalette_t *palette);

/// @brief Set selection callback
/// @param palette ColorPalette widget
/// @param callback Selection handler function
/// @param user_data User data passed to callback
void vg_colorpalette_set_on_select(vg_colorpalette_t *palette,
                                   vg_colorpalette_callback_t callback,
                                   void *user_data);

/// @brief Set swatch size
/// @param palette ColorPalette widget
/// @param size Swatch size
void vg_colorpalette_set_swatch_size(vg_colorpalette_t *palette, float size);

/// @brief Load standard 16-color palette
/// @param palette ColorPalette widget
void vg_colorpalette_load_standard_16(vg_colorpalette_t *palette);

//=============================================================================
// ColorPicker Widget
//=============================================================================

/// @brief ColorPicker callback - called when color changes
typedef void (*vg_colorpicker_callback_t)(vg_widget_t *picker, uint32_t color, void *user_data);

/// @brief ColorPicker widget structure - full color selection with RGB sliders
typedef struct vg_colorpicker {
    vg_widget_t base;

    uint32_t color;     ///< Current selected color (ARGB)
    uint8_t r, g, b, a; ///< Individual components

    // Child widgets (managed internally)
    vg_slider_t *slider_r; ///< Red slider
    vg_slider_t *slider_g; ///< Green slider
    vg_slider_t *slider_b; ///< Blue slider
    vg_slider_t *slider_a; ///< Alpha slider (optional)

    vg_colorswatch_t *preview;  ///< Color preview swatch
    vg_colorpalette_t *palette; ///< Quick color palette

    // Display options
    bool show_alpha;   ///< Show alpha slider
    bool show_palette; ///< Show quick palette
    bool show_labels;  ///< Show R/G/B labels
    bool show_values;  ///< Show numeric values

    vg_font_t *font; ///< Font for labels
    float font_size; ///< Font size

    // Callback
    vg_colorpicker_callback_t on_change;
    void *on_change_data;

    bool syncing_children; ///< Internal: suppress child slider callbacks during programmatic sync
    int active_channel;    ///< Keyboard-active channel: 0=red, 1=green, 2=blue, 3=alpha
} vg_colorpicker_t;

/// @brief Create a new color picker widget
/// @param parent Parent widget (can be NULL)
/// @return New color picker widget or NULL on failure
vg_colorpicker_t *vg_colorpicker_create(vg_widget_t *parent);

/// @brief Set picker color
/// @param picker ColorPicker widget
/// @param color Color (ARGB)
void vg_colorpicker_set_color(vg_colorpicker_t *picker, uint32_t color);

/// @brief Get picker color
/// @param picker ColorPicker widget
/// @return Current color (ARGB)
uint32_t vg_colorpicker_get_color(vg_colorpicker_t *picker);

/// @brief Set RGB components
/// @param picker ColorPicker widget
/// @param r Red component (0-255)
/// @param g Green component (0-255)
/// @param b Blue component (0-255)
void vg_colorpicker_set_rgb(vg_colorpicker_t *picker, uint8_t r, uint8_t g, uint8_t b);

/// @brief Get RGB components
/// @param picker ColorPicker widget
/// @param r Pointer to receive red (can be NULL)
/// @param g Pointer to receive green (can be NULL)
/// @param b Pointer to receive blue (can be NULL)
void vg_colorpicker_get_rgb(vg_colorpicker_t *picker, uint8_t *r, uint8_t *g, uint8_t *b);

/// @brief Set alpha component
/// @param picker ColorPicker widget
/// @param alpha Alpha (0-255)
void vg_colorpicker_set_alpha(vg_colorpicker_t *picker, uint8_t alpha);

/// @brief Get alpha component
/// @param picker ColorPicker widget
/// @return Alpha (0-255)
uint8_t vg_colorpicker_get_alpha(vg_colorpicker_t *picker);

/// @brief Show/hide alpha slider
/// @param picker ColorPicker widget
/// @param show true to show alpha slider
void vg_colorpicker_show_alpha(vg_colorpicker_t *picker, bool show);

/// @brief Return whether the alpha channel slider is enabled and visible.
/// @param picker Color picker widget to inspect.
/// @return true when alpha editing is enabled, otherwise false.
bool vg_colorpicker_is_alpha_enabled(const vg_colorpicker_t *picker);

/// @brief Show/hide quick palette
/// @param picker ColorPicker widget
/// @param show true to show palette
void vg_colorpicker_show_palette(vg_colorpicker_t *picker, bool show);

/// @brief Set change callback
/// @param picker ColorPicker widget
/// @param callback Change handler function
/// @param user_data User data passed to callback
void vg_colorpicker_set_on_change(vg_colorpicker_t *picker,
                                  vg_colorpicker_callback_t callback,
                                  void *user_data);

/// @brief Set font for labels
/// @param picker ColorPicker widget
/// @param font Font to use
/// @param size Font size
void vg_colorpicker_set_font(vg_colorpicker_t *picker, vg_font_t *font, float size);

#ifdef __cplusplus
}
#endif

#endif // VG_WIDGETS_H
