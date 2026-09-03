//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_ide_widgets_editor.h
// Purpose: CodeEditor, FindReplaceBar, and Minimap widget declarations —
//          syntax-highlighted multi-cursor editing with undo/redo, folding,
//          IntelliSense completion, and an integrated minimap overview.
// Key invariants:
//   - All create functions return NULL on allocation failure.
//   - String parameters are copied internally unless documented otherwise.
//   - Line and column indices are zero-based throughout the editor API.
// Ownership/Lifetime:
//   - Widgets are owned by their parent in the widget tree.
// Links: lib/gui/include/vg_ide_widgets_common.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vg_ide_widgets_common.h"

/// @file
/// @brief Declares the IDE code editor, find/replace bar, and minimap APIs.
/// @details The editor surface covers line storage, multi-cursor editing, undo history,
/// incremental change journals, syntax and semantic overlays, folding, inlay hints, scrolling,
/// clipboard actions, search/replace, and bounded minimap caching.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// CodeEditor Widget
//=============================================================================

/// @brief Edit operation types for undo/redo
typedef enum vg_edit_op_type {
    VG_EDIT_INSERT, ///< Text inserted
    VG_EDIT_DELETE, ///< Text deleted
    VG_EDIT_REPLACE ///< Text replaced (delete + insert)
} vg_edit_op_type_t;

/// @brief Single edit operation for undo/redo history
typedef struct vg_edit_op {
    vg_edit_op_type_t type; ///< Operation type
    int cursor_id;          ///< 0 = primary cursor, 1+ = extra cursor slot

    // Position info
    int start_line; ///< Start line
    int start_col;  ///< Start column
    int end_line;   ///< End line
    int end_col;    ///< End column

    // Text data
    char *old_text; ///< Text before operation (for DELETE/REPLACE)
    char *new_text; ///< Text after operation (for INSERT/REPLACE)

    // Cursor position to restore
    int cursor_line_before;
    int cursor_col_before;
    int cursor_line_after;
    int cursor_col_after;

    // Grouping for compound operations
    uint32_t group_id; ///< Non-zero if part of a group
} vg_edit_op_t;

/// @brief Undo/redo history
typedef struct vg_edit_history {
    vg_edit_op_t **operations; ///< Array of edit operations
    size_t count;              ///< Number of operations
    size_t capacity;           ///< Allocated capacity
    size_t current_index;      ///< Points to next redo operation
    uint32_t next_group_id;    ///< Counter for grouping
    bool is_grouping;          ///< Currently recording a group
    uint32_t current_group;    ///< Active group ID

    // Typing coalescing: consecutive single-codepoint inserts that continue the
    // previous op within a short time window are merged into one undo unit, so
    // Ctrl+Z reverts words/bursts instead of individual characters.
    bool coalesce_enabled;        ///< Master switch (default true; tests may disable).
    bool coalesce_break;          ///< When set, the next insert starts a fresh op.
    uint64_t last_insert_time_ms; ///< Editor typing-clock time of the last coalesced insert.
} vg_edit_history_t;

/// @brief Forward content delta for incremental language-service sync (plan 08).
/// @details One content mutation on the hot editing path: the pre-edit byte range
///          [(start_line,start_col) .. (end_line,end_col)) was replaced by @p text
///          (empty for a pure delete; start == end for a pure insert). Positions
///          are 0-based byte line/column, matching the widget and
///          codeeditor_materialize_lines() (lines joined by '\n', no trailing
///          newline). Deltas are captured in application order, so a document
///          mirror that replays them in order against its own text stays byte-exact.
typedef struct vg_edit_delta {
    uint64_t revision; ///< Editor content revision AFTER this delta.
    int start_line;    ///< 0-based start line of the replaced range.
    int start_col;     ///< 0-based start byte column.
    int end_line;      ///< 0-based end line of the replaced range.
    int end_col;       ///< 0-based end byte column.
    char *text;        ///< Owned replacement text ("" for a pure delete).
} vg_edit_delta_t;

/// @brief Capacity of the per-editor delta ring; overflow forces a full re-sync.
#define VG_EDIT_JOURNAL_CAP 256

/// @brief Line information
typedef struct vg_code_line {
    char *text;                       ///< Line text (owned)
    size_t length;                    ///< Text length
    size_t capacity;                  ///< Buffer capacity
    uint32_t *colors;                 ///< Per-character colors (owned, optional)
    size_t colors_capacity;           ///< Allocated entries in colors array
    uint64_t highlight_generation;    ///< Syntax-cache generation represented by colors.
    uint64_t syntax_state_generation; ///< Generation for cached language state.
    int syntax_state_in;              ///< Cached language state before this line.
    int syntax_state_out;             ///< Cached language state after this line.
    bool modified;                    ///< Line modified since last save
} vg_code_line_t;

/// @brief Selection range
typedef struct vg_selection {
    int start_line;
    int start_col;
    int end_line;
    int end_col;
} vg_selection_t;

/// @brief Low-level CodeEditor performance counters for tests and diagnostics.
typedef struct vg_codeeditor_perf_stats {
    uint64_t total_height_linear_scans;     ///< Lines visited while summing document height.
    uint64_t total_visual_row_linear_scans; ///< Lines visited while summing visual rows.
    uint64_t visual_row_linear_scans;       ///< Lines visited while mapping position to visual row.
    uint64_t locate_visual_row_linear_scans; ///< Lines visited while mapping visual row to line.
    uint64_t line_highlight_calls;           ///< Syntax highlighter invocations from paint.
    uint64_t syntax_state_line_scans;        ///< Lines scanned to compute cached syntax state.
    uint64_t highlight_span_checks;          ///< Highlight spans inspected while painting.
    uint64_t full_text_copies;               ///< Full-document text materializations.
    uint64_t full_text_copy_bytes;           ///< Bytes copied by full-document materializations.
} vg_codeeditor_perf_stats_t;

/// @brief Populate per-character colors for one editor line.
/// @param editor Code editor widget requesting highlighting.
/// @param line_num Zero-based line number.
/// @param text Borrowed NUL-terminated line text.
/// @param colors Mutable per-character color array owned by the editor.
/// @param user_data Opaque context registered with the syntax callback.
typedef void (*vg_syntax_callback_t)(
    vg_widget_t *editor, int line_num, const char *text, uint32_t *colors, void *user_data);

/// @brief Stable syntax token classification shared by the lexical tokenizers
///        and (later) the semantic-token overlay. Indices are an ABI contract:
///        only append, never renumber. The first six match the original
///        SetTokenColor order, so existing callers are unaffected. See
///        docs/adr/0007-codeeditor-syntax-surface-expansion.md.
typedef enum vg_syntax_token_type {
    VG_SYN_TOKEN_DEFAULT = 0,    ///< Plain text / unclassified.
    VG_SYN_TOKEN_KEYWORD = 1,    ///< Language keyword.
    VG_SYN_TOKEN_TYPE = 2,       ///< Type / class name.
    VG_SYN_TOKEN_STRING = 3,     ///< String literal.
    VG_SYN_TOKEN_COMMENT = 4,    ///< Comment.
    VG_SYN_TOKEN_NUMBER = 5,     ///< Numeric literal.
    VG_SYN_TOKEN_FUNCTION = 6,   ///< Function / method call.
    VG_SYN_TOKEN_OPERATOR = 7,   ///< Operator punctuation (+ - * / = < > etc.).
    VG_SYN_TOKEN_BRACKET = 8,    ///< Bracket / delimiter ( ) [ ] { }.
    VG_SYN_TOKEN_PARAMETER = 9,  ///< Parameter binding (semantic).
    VG_SYN_TOKEN_PROPERTY = 10,  ///< Field / property access (semantic).
    VG_SYN_TOKEN_CONSTANT = 11,  ///< Constant / enum member (semantic).
    VG_SYN_TOKEN_DECORATOR = 12, ///< Attribute / decorator (semantic).
    VG_SYN_TOKEN_COUNT = 13      ///< Number of token types (array sizing).
} vg_syntax_token_type;

/// @brief Whitespace-marker rendering modes for CodeEditor.
/// @details Controls rendering of faint dots (spaces) and arrows (tabs). The
///          integer values are an ABI contract shared with the Zia runtime
///          binding Zanna.GUI.CodeEditor.SetWhitespaceMode.
typedef enum vg_whitespace_mode {
    VG_WHITESPACE_NONE = 0,     ///< No whitespace markers (default; zero-cost).
    VG_WHITESPACE_BOUNDARY = 1, ///< Only leading and trailing whitespace on each line.
    VG_WHITESPACE_ALL = 2       ///< Every space and tab.
} vg_whitespace_mode;

/// @brief CodeEditor widget structure
typedef struct vg_codeeditor {
    vg_widget_t base;

    // Document
    vg_code_line_t *lines; ///< Array of lines
    int line_count;        ///< Number of lines
    int line_capacity;     ///< Allocated capacity
    void *document_buffer; ///< Shared text-engine mirror used for full-document queries

    // Cursor and selection
    int cursor_line;          ///< Cursor line (0-based)
    int cursor_col;           ///< Cursor column (0-based)
    vg_selection_t selection; ///< Current selection
    bool has_selection;       ///< Is there an active selection

    // Scroll
    float scroll_x;         ///< Horizontal scroll
    float scroll_y;         ///< Vertical scroll
    bool ligatures_enabled; ///< Ligature shaping for this editor (default on)
    float smooth_target_x;  ///< Wheel easing destination (smooth scrolling)
    float smooth_target_y;  ///< Wheel easing destination (smooth scrolling)
    bool smooth_animating;  ///< True while wheel easing is in flight
    int visible_first_line; ///< First visible line
    int visible_line_count; ///< Number of visible lines

    // Font
    vg_font_t *font;   ///< Monospace font
    float font_size;   ///< Font size
    float char_width;  ///< Character width (monospace)
    float line_height; ///< Line height
    bool font_pinned;  ///< True once a caller set the font explicitly; suppresses
                       ///< app-wide default/chrome font propagation so the editor
                       ///< keeps its monospace font (see rt_gui_apply_font_to_widget).

    // Gutter
    bool show_line_numbers;           ///< Show line number gutter
    float gutter_width;               ///< Gutter width
    float line_number_width_override; ///< Explicit line-number gutter width in character units (0 =
                                      ///< auto)
    uint32_t gutter_bg;               ///< Gutter background color
    uint32_t line_number_color;       ///< Line number color

    // Appearance
    uint32_t bg_color;        ///< Background color
    uint32_t text_color;      ///< Default text color
    uint32_t cursor_color;    ///< Cursor color
    uint32_t selection_color; ///< Selection color
    uint32_t current_line_bg; ///< Current line highlight

    // Display options
    bool show_indent_guides;     ///< Draw faint vertical guides at indent levels.
    int render_whitespace_mode;  ///< Whitespace markers: 0=none, 1=boundary, 2=all
                                 ///< (vg_whitespace_mode).
    uint32_t whitespace_color;   ///< Whitespace marker color (0 = derive faint from text color).
    uint32_t indent_guide_color; ///< Indent-guide line color (0 = derive from gutter).

    // Syntax highlighting
    vg_syntax_callback_t syntax_highlighter;
    void *syntax_data;

    // Last Zia block-comment nesting depth observed by the syntax callback.
    // The callback derives depth from prior buffer lines for each highlighted
    // line, so rendering remains correct when the viewport jumps into the
    // middle of a block comment.
    int zia_block_comment_depth;

    // Token colors (overridable per-editor; indexed by vg_syntax_token_type).
    // Zero means "use theme default". The first six indices keep their original
    // SetTokenColor meaning; FUNCTION and the semantic types are now overridable.
    uint32_t token_colors[VG_SYN_TOKEN_COUNT];

    // Custom keywords (comma-separated list parsed into array; checked after
    // language keywords in the syntax callback). Owned by the editor.
    char **custom_keywords;
    int custom_keyword_count;

    // Auto fold detection
    bool auto_fold_detection; ///< Detect fold regions from indentation

    // Editing options
    bool read_only;   ///< Read-only mode
    bool insert_mode; ///< Insert vs overwrite mode
    int tab_width;    ///< Tab width in spaces
    bool use_spaces;  ///< Use spaces for tabs
    bool auto_indent; ///< Auto-indent on enter
    bool word_wrap;   ///< Word wrapping

    // State
    bool cursor_visible;     ///< Cursor blink state
    float cursor_blink_time; ///< Cursor blink timer
    uint64_t
        typing_clock_ms; ///< Monotonic ms clock advanced by vg_codeeditor_tick; drives
                         ///< undo coalescing time windows deterministically (tests control dt).
    bool modified;       ///< Document modified since last save
    uint64_t revision;   ///< Monotonic content revision; cursor/scroll changes do not affect it
    uint64_t highlight_generation;         ///< Monotonic syntax-cache generation.
    vg_codeeditor_perf_stats_t perf_stats; ///< Low-level performance counters.
    uint64_t layout_generation;            ///< Monotonic visual-layout generation.
    bool has_folded_lines;                 ///< True when at least one fold region is collapsed.

    // Cached source-line -> visual-row mapping for wrapped or folded layout.
    bool layout_cache_valid;
    uint64_t layout_cache_generation;
    float layout_cache_content_width;
    bool layout_cache_word_wrap;
    int layout_cache_line_count;
    int layout_cache_total_visual_rows;
    float layout_cache_total_height;
    int *layout_cache_prefix_rows; ///< line_count + 1 row-prefix entries.
    int layout_cache_capacity;     ///< Allocated prefix entry count.

    // Runtime-wrapper content-width cache used by hit-testing and cursor pixel queries.
    bool runtime_content_width_cache_valid;      ///< True when the runtime width cache is usable.
    uint64_t runtime_content_width_generation;   ///< Layout generation captured with the cache.
    float runtime_content_width_base_width;      ///< Base text width key used for the cache.
    float runtime_content_width_viewport_height; ///< Viewport height key used for the cache.
    bool runtime_content_width_word_wrap;        ///< Word-wrap state captured with the cache.
    float runtime_content_width;                 ///< Cached converged text width in pixels.

    // Undo/redo history
    vg_edit_history_t *history; ///< Edit history for undo/redo

    // Incremental sync journal (plan 08): bounded ring of forward content deltas
    // captured on the hot editing path so language services update a document
    // mirror by delta instead of re-copying the whole buffer per keystroke. Cold
    // mutations (undo/redo, SetText) set journal_overflowed to force a full sync.
    vg_edit_delta_t journal[VG_EDIT_JOURNAL_CAP]; ///< Ring of retained deltas.
    int journal_head;                             ///< Ring index of the oldest retained delta.
    int journal_count;           ///< Deltas currently retained (<= VG_EDIT_JOURNAL_CAP).
    uint64_t journal_oldest_rev; ///< Revision of the oldest retained delta (0 = none).
    bool journal_overflowed;     ///< A cold mutation or ring wrap requires a full re-sync.

    // Scrollbar drag state
    bool scrollbar_dragging;           ///< True while user is dragging the scroll thumb
    float scrollbar_drag_offset;       ///< Mouse Y at drag start (widget-relative)
    float scrollbar_drag_start_scroll; ///< scroll_y value at drag start

    // Horizontal scrollbar drag state + longest-line cache for the scroll range.
    bool hscrollbar_dragging;              ///< True while dragging the horizontal thumb.
    float hscrollbar_drag_offset;          ///< Mouse X at drag start (widget-relative).
    float hscrollbar_drag_start_scroll;    ///< scroll_x value at drag start.
    int longest_line_cols_cache;           ///< Cached widest line in visual columns.
    uint64_t longest_line_cols_generation; ///< layout_generation the cache was computed for.

    // Pointer selection drag state
    bool selection_drag_pending;  ///< True after mouse-down before movement crosses drag threshold.
    bool selection_dragging;      ///< True while a content-area pointer drag is selecting text
    float selection_drag_start_x; ///< Mouse-down X used to distinguish click from drag.
    float selection_drag_start_y; ///< Mouse-down Y used to distinguish click from drag.
    int selection_anchor_line;    ///< Selection anchor line for pointer drag
    int selection_anchor_col;     ///< Selection anchor column for pointer drag
    float selection_last_drag_x;  ///< Last pointer X seen during the active drag (widget-local).
    float selection_last_drag_y;  ///< Last pointer Y seen during the active drag (widget-local).
    float
        selection_autoscroll_accum_y; ///< Fractional vertical autoscroll pixels pending this drag.
    float selection_autoscroll_accum_x; ///< Fractional horizontal autoscroll pixels pending this
                                        ///< drag.

    // Matching-pair highlight cache. Columns are byte offsets because the
    // native editor stores cursor positions as UTF-8 byte columns.
    bool pair_match_cache_valid;
    uint64_t pair_match_revision;
    int pair_match_cursor_line;
    int pair_match_cursor_col;
    bool pair_match_active;
    int pair_anchor_line;
    int pair_anchor_col;
    int pair_peer_line;
    int pair_peer_col;

    // Highlight spans (arbitrary colored regions overlaid on text)
    struct vg_highlight_span {
        int start_line, start_col; ///< Inclusive start position
        int end_line, end_col;     ///< Exclusive end position
        uint32_t color;            ///< Background highlight color (0x00RRGGBB)
    } *highlight_spans;            ///< Owned array; NULL when unused

    int highlight_span_count;    ///< Active span count
    int highlight_span_cap;      ///< Allocated capacity
    bool highlight_spans_sorted; ///< True when highlight_spans is ordered by start line/column.

    // Semantic token overlay: per-identifier foreground colors supplied by the
    // compiler (Phase 5), applied on top of the lexical highlighter inside
    // highlight_line(). See docs/adr/0007-codeeditor-syntax-surface-expansion.md.
    struct vg_semantic_token {
        int line;       ///< 0-based line.
        int start_col;  ///< 0-based start column (inclusive).
        int end_col;    ///< 0-based end column (exclusive).
        uint32_t color; ///< Resolved ARGB foreground color.
    } *semantic_tokens; ///< Owned array; NULL when unused.

    int semantic_token_count;    ///< Active token count.
    int semantic_token_cap;      ///< Allocated capacity.
    bool semantic_tokens_sorted; ///< True when tokens are ordered by line/start/end.

    // Per-line highlight index built from highlight_spans for paint.
    bool highlight_line_index_valid;
    int highlight_line_index_line_count;
    int highlight_line_index_span_count;
    int *highlight_line_offsets;      ///< line_count + 1 offsets into highlight_line_span_indices.
    int highlight_line_offsets_cap;   ///< Allocated offset entry count.
    int *highlight_line_span_indices; ///< Span indices touching each line, grouped by line.
    int highlight_line_span_indices_cap;
    int *
        highlight_line_write_offsets; ///< Reusable scratch offsets while rebuilding the line index.
    int highlight_line_write_offsets_cap; ///< Allocated scratch offset entry count.

    // Inlay hints (display-only ghost text anchored to source positions)
    struct vg_inlay_hint {
        int line;       ///< Zero-based source line.
        int col;        ///< Zero-based source column anchor.
        char *text;     ///< Owned hint text.
        uint32_t color; ///< ARGB text color.
    } *inlay_hints;     ///< Owned array; NULL when unused.

    int inlay_hint_count;    ///< Active hint count.
    int inlay_hint_cap;      ///< Allocated capacity.
    bool inlay_hints_sorted; ///< True when hints are ordered by line/column for paint.

    // Gutter icons (breakpoints, diagnostics, etc.)
    struct vg_gutter_icon {
        int line;        ///< 0-based line number
        int type;        ///< 0=breakpoint, 1=warning, 2=error, 3=info; also the slot key.
        int style;       ///< 0=disc/image (default), 1=change bar at the gutter left edge.
        uint32_t color;  ///< Icon color (0x00RRGGBB)
        vg_icon_t image; ///< Optional RGBA icon; falls back to colored disc when absent.
    } *gutter_icons;     ///< Owned array; NULL when unused

    int gutter_icon_count; ///< Active icon count
    int gutter_icon_cap;   ///< Allocated capacity

    // Per-editor gutter click state (edge-triggered; payload is cleared after read)
    bool gutter_clicked;     ///< A gutter click is latched for this editor.
    bool gutter_click_read;  ///< True after the current click edge is consumed.
    int gutter_clicked_line; ///< Line that was clicked (-1 if none).
    int gutter_clicked_slot; ///< Slot that was clicked (-1 if none).

    // Fold gutter & regions
    bool show_fold_gutter; ///< Show fold indicators in gutter

    struct vg_fold_region {
        int start_line; ///< First line of the foldable block
        int end_line;   ///< Last line of the foldable block (inclusive)
        bool folded;    ///< Whether the region is currently collapsed
    } *fold_regions;    ///< Owned array; NULL when unused

    int fold_region_count; ///< Active region count
    int fold_region_cap;   ///< Allocated capacity

    // Extra cursors (multi-cursor editing state)
    struct vg_extra_cursor {
        int line;                 ///< 0-based line number
        int col;                  ///< 0-based column number
        vg_selection_t selection; ///< Per-cursor selection range
        bool has_selection;       ///< Whether this cursor owns an active selection
    } *extra_cursors;             ///< Owned array; NULL when unused

    int extra_cursor_count; ///< Active extra cursor count
    int extra_cursor_cap;   ///< Allocated capacity
} vg_codeeditor_t;

/// @brief Detachable per-document editor state (text, undo history, cursor,
///        scroll, folds, semantic/diagnostic overlays). A CodeEditor holds one
///        document's worth of this state in its own fields; a vg_editor_buffer_t
///        is a *detached* snapshot that can be swapped in and out so multiple
///        documents can share a single editor widget without losing undo history
///        or view state on every switch. Fields mirror the swappable subset of
///        vg_codeeditor_t exactly; view-only state (font, colors, layout caches)
///        stays on the widget and is invalidated on swap.
typedef struct vg_editor_buffer {
    // Text
    vg_code_line_t *lines;
    int line_count;
    int line_capacity;
    void *document_buffer;
    // History
    vg_edit_history_t *history;
    // Caret / selection / scroll
    int cursor_line;
    int cursor_col;
    vg_selection_t selection;
    bool has_selection;
    float scroll_x;
    float scroll_y;
    struct vg_extra_cursor *extra_cursors;
    int extra_cursor_count;
    int extra_cursor_cap;
    // Folds
    struct vg_fold_region *fold_regions;
    int fold_region_count;
    int fold_region_cap;
    bool has_folded_lines;
    // Semantic token overlay
    struct vg_semantic_token *semantic_tokens;
    int semantic_token_count;
    int semantic_token_cap;
    bool semantic_tokens_sorted;
    // Diagnostic highlight spans
    struct vg_highlight_span *highlight_spans;
    int highlight_span_count;
    int highlight_span_cap;
    bool highlight_spans_sorted;
    // Identity / dirty
    bool modified;
    uint64_t revision;
    uint64_t highlight_generation;
    uint64_t typing_clock_ms;
} vg_editor_buffer_t;

/// @brief Create a detached editor buffer initialised from @p text.
/// @param text Null-terminated UTF-8 (NULL/"" yields a single empty line).
/// @return New buffer, or NULL on allocation failure.
vg_editor_buffer_t *vg_editor_buffer_create(const char *text);

/// @brief Destroy a detached buffer and all state it owns.
/// @param buf Buffer to free (may be NULL). Must not be currently attached.
void vg_editor_buffer_destroy(vg_editor_buffer_t *buf);

/// @brief Full text of a detached buffer as a newly allocated string.
/// @param buf Buffer to read (may be NULL → NULL).
/// @return Caller-owned string, or NULL.
char *vg_editor_buffer_get_text(vg_editor_buffer_t *buf);

/// @brief Apply an undoable complete-text replacement to a detached buffer.
/// @details Uses the same state-preserving contract as
///          vg_codeeditor_replace_all_text() without consuming the buffer shell.
/// @param buf Detached buffer to update.
/// @param text Null-terminated UTF-8 replacement; NULL means empty text.
/// @return True when applied or already matched; false on invalid/allocation failure.
bool vg_editor_buffer_replace_all_text(vg_editor_buffer_t *buf, const char *text);

/// @brief Modified flag of a detached buffer.
/// @param buf Detached editor buffer to query.
/// @return True when the buffer has changed since its modified flag was cleared.
bool vg_editor_buffer_is_modified(const vg_editor_buffer_t *buf);

/// @brief Clear the modified flag of a detached buffer.
/// @param buf Detached editor buffer to mark unmodified; NULL is ignored.
void vg_editor_buffer_clear_modified(vg_editor_buffer_t *buf);

/// @brief Content revision of a detached buffer.
/// @param buf Detached editor buffer to query.
/// @return Monotonic content revision, or zero for NULL.
uint64_t vg_editor_buffer_get_revision(const vg_editor_buffer_t *buf);

/// @brief Swap the editor's current document state for @p incoming.
/// @details The editor's current state is moved into a freshly allocated buffer
///          (returned to the caller, who now owns it); @p incoming's state is
///          moved into the editor. @p incoming is consumed: its struct is freed
///          and it must not be used again. All view caches are invalidated and a
///          repaint/relayout is requested. Undo history is never touched.
/// @param editor   Editor to retarget (must be non-NULL).
/// @param incoming Buffer to display (must be non-NULL; consumed).
/// @return The editor's previous state as a new detached buffer (caller owns).
vg_editor_buffer_t *vg_codeeditor_swap_buffer(vg_codeeditor_t *editor,
                                              vg_editor_buffer_t *incoming);

/// @brief Advance the cursor blink timer; call each frame.
/// @param editor Code editor widget.
/// @param dt     Elapsed time in seconds since the last call.
void vg_codeeditor_tick(vg_codeeditor_t *editor, float dt);

/// @brief Create a new code editor widget.
/// @param parent Parent widget (can be NULL).
/// @return New code editor or NULL on failure.
vg_codeeditor_t *vg_codeeditor_create(vg_widget_t *parent);

/// @brief Replace the entire document with new text and clear undo/redo history.
/// @param editor Code editor widget.
/// @param text   Null-terminated UTF-8 string (copied internally).
void vg_codeeditor_set_text(vg_codeeditor_t *editor, const char *text);

/// @brief Replace complete text as one undoable edit while retaining view state.
/// @details Preserves and clamps primary/extra cursors, selections, scroll, fold
///          regions, and prior undo history. Unlike vg_codeeditor_set_text(), the
///          replacement itself becomes the newest undo unit. No-op for read-only
///          editors; an identical replacement succeeds without changing revision.
/// @param editor Code editor widget.
/// @param text Null-terminated UTF-8 replacement; NULL means empty text.
/// @return True when the request was applied or already matched; false on invalid,
///         read-only, or allocation failure.
bool vg_codeeditor_replace_all_text(vg_codeeditor_t *editor, const char *text);

/// @brief Replace the entire document with an explicit byte span and clear undo/redo history.
/// @param editor Code editor widget.
/// @param text   UTF-8 byte buffer (copied internally, may contain embedded NUL).
/// @param len    Number of bytes to read from @p text.
void vg_codeeditor_set_text_bytes(vg_codeeditor_t *editor, const char *text, size_t len);

/// @brief Return the full document text as a newly allocated string.
/// @param editor Code editor widget.
/// @return Caller-owned null-terminated string; must be freed with free().
char *vg_codeeditor_get_text(vg_codeeditor_t *editor);

/// @brief Advance wheel smooth-scroll easing by one frame (ADR 0137).
/// @param editor Editor to advance.
/// @param delta_ms Elapsed milliseconds this frame.
/// @return True while easing is still in flight (keeps the app animating).
bool vg_codeeditor_smooth_tick(vg_codeeditor_t *editor, float delta_ms);

/// @brief Enable or disable ligature shaping for this editor (ADR 0137).
/// @param editor Editor to configure.
/// @param enabled True renders liga/calt ligatures; false shows plain glyphs.
void vg_codeeditor_set_ligatures_enabled(vg_codeeditor_t *editor, bool enabled);

/// @brief Return whether this editor renders ligatures.
/// @param editor Code editor widget to query.
/// @return True when ligature shaping is enabled.
bool vg_codeeditor_get_ligatures_enabled(const vg_codeeditor_t *editor);

/// @brief Return the monotonic content revision.
/// @param editor Code editor widget.
/// @return Content revision, or 0 if editor is NULL.
uint64_t vg_codeeditor_get_revision(vg_codeeditor_t *editor);

/// @brief Take the forward content deltas recorded since @p since_revision as a
///        compact JSON array, pruning the consumed entries (plan 08).
/// @details Each element is {"r":revision,"sl","sc","el","ec","t":text}: the
///          pre-edit byte range [(sl,sc)..(el,ec)) was replaced by t. Returns the
///          literal string "overflow" when a cold mutation (undo/redo, SetText,
///          buffer swap) or a ring wrap means the caller must full-sync instead.
///          Caller owns and frees the returned string.
/// @param editor Code editor widget.
/// @param since_revision Last revision the caller has already applied.
/// @return Owned JSON array string, "overflow", or NULL on OOM/NULL editor.
char *vg_codeeditor_take_deltas_json(vg_codeeditor_t *editor, uint64_t since_revision);

/// @brief Serialize retained forward deltas without consuming the journal.
/// @param editor Code editor whose incremental journal is inspected.
/// @param since_revision Last revision already held by the caller.
/// @return Caller-owned JSON array or `overflow`; free with free().
char *vg_codeeditor_peek_deltas_json(vg_codeeditor_t *editor, uint64_t since_revision);

/// @brief Replay one byte-column edit into a read-only document mirror.
/// @details Updates text/layout while intentionally retaining no undo or sync journal.
bool vg_codeeditor_apply_mirror_edit(vg_codeeditor_t *editor,
                                     int start_line,
                                     int start_col,
                                     int end_line,
                                     int end_col,
                                     const char *text);

/// @brief Map a UTF-8 byte boundary to fixed-cell display columns.
size_t vg_codeeditor_display_column_for_byte(const vg_codeeditor_t *editor,
                                             int line,
                                             size_t byte_col);

/// @brief Map a fixed-cell display column to a UTF-8 byte boundary.
size_t vg_codeeditor_byte_for_display_column(const vg_codeeditor_t *editor,
                                             int line,
                                             size_t display_col);

/// @brief Map a fixed-cell display column to a Unicode scalar column.
size_t vg_codeeditor_character_column_for_display(const vg_codeeditor_t *editor,
                                                  int line,
                                                  size_t display_col);

/// @brief Return the fixed-cell display width of one logical line.
size_t vg_codeeditor_line_display_columns(const vg_codeeditor_t *editor, int line);

/// @brief Return the currently selected text as a newly allocated string.
/// @param editor Code editor widget.
/// @return Caller-owned null-terminated string, or NULL if no selection.
char *vg_codeeditor_get_selection(vg_codeeditor_t *editor);

/// @brief Move the primary cursor to a specific line and column.
/// @param editor Code editor widget.
/// @param line   Zero-based line index.
/// @param col    Zero-based column (character) index.
void vg_codeeditor_set_cursor(vg_codeeditor_t *editor, int line, int col);

/// @brief Retrieve the current primary cursor position.
/// @param editor   Code editor widget.
/// @param out_line Receives the zero-based line index (may be NULL).
/// @param out_col  Receives the zero-based column index (may be NULL).
void vg_codeeditor_get_cursor(vg_codeeditor_t *editor, int *out_line, int *out_col);

/// @brief Set the selection range using UTF-8 codepoint columns.
/// @param editor     Code editor widget.
/// @param start_line Selection start line (zero-based, inclusive).
/// @param start_col  Selection start column (zero-based, inclusive).
/// @param end_line   Selection end line (zero-based, exclusive).
/// @param end_col    Selection end column (zero-based, exclusive).
void vg_codeeditor_set_selection(
    vg_codeeditor_t *editor, int start_line, int start_col, int end_line, int end_col);

/// @brief Insert text at the current cursor position, pushing it into undo history.
/// @details No-op when the editor is read-only.
/// @param editor Code editor widget.
/// @param text   Null-terminated UTF-8 string to insert.
void vg_codeeditor_insert_text(vg_codeeditor_t *editor, const char *text);

/// @brief Delete the currently selected text, pushing the operation into undo history.
/// @details No-op when the editor is read-only.
/// @param editor Code editor widget.
void vg_codeeditor_delete_selection(vg_codeeditor_t *editor);

/// @brief Scroll the view so that the specified line is visible.
/// @param editor Code editor widget.
/// @param line   Zero-based line index to scroll to.
void vg_codeeditor_scroll_to_line(vg_codeeditor_t *editor, int line);

/// @brief Return the source line currently nearest the top of the viewport.
/// @param editor Code editor widget.
/// @return Zero-based line index, or 0 when unavailable.
int vg_codeeditor_get_scroll_top_line(vg_codeeditor_t *editor);

/// @brief Scroll so that the given source line is nearest the top of the viewport.
/// @param editor Code editor widget.
/// @param line   Zero-based line index to place near the viewport top.
void vg_codeeditor_set_scroll_top_line(vg_codeeditor_t *editor, int line);

/// @brief Set the syntax highlighting callback invoked per visible line.
/// @param editor    Code editor widget.
/// @param callback  Function called with the line number, text, and a writable colour array.
/// @param user_data User data passed to the callback.
void vg_codeeditor_set_syntax(vg_codeeditor_t *editor,
                              vg_syntax_callback_t callback,
                              void *user_data);

/// @brief Undo the most recent edit operation.
/// @param editor Code editor widget.
void vg_codeeditor_undo(vg_codeeditor_t *editor);

/// @brief Re-apply the most recently undone operation.
/// @param editor Code editor widget.
void vg_codeeditor_redo(vg_codeeditor_t *editor);

/// @brief Copy the current selection to the system clipboard.
/// @param editor Code editor widget.
/// @return true if text was copied; false if nothing was selected.
bool vg_codeeditor_copy(vg_codeeditor_t *editor);

/// @brief Cut the current selection to the system clipboard.
/// @param editor Code editor widget.
/// @return true if text was cut; false if nothing was selected.
bool vg_codeeditor_cut(vg_codeeditor_t *editor);

/// @brief Insert clipboard text at the current cursor position.
/// @param editor Code editor widget.
/// @return true on success; false if the clipboard is empty or unavailable.
bool vg_codeeditor_paste(vg_codeeditor_t *editor);

/// @brief Select the entire document.
/// @param editor Code editor widget.
void vg_codeeditor_select_all(vg_codeeditor_t *editor);

/// @brief Set the monospace font used for code rendering.
/// @param editor Code editor widget.
/// @param font   Font handle (should be a monospace face).
/// @param size   Font size in pixels.
void vg_codeeditor_set_font(vg_codeeditor_t *editor, vg_font_t *font, float size);

/// @brief Get the total number of lines in the document.
/// @param editor Code editor widget.
/// @return Line count (always >= 1).
int vg_codeeditor_get_line_count(vg_codeeditor_t *editor);

/// @brief Check whether the document has unsaved modifications.
/// @param editor Code editor widget.
/// @return true if the document has been modified since the last save/clear.
bool vg_codeeditor_is_modified(vg_codeeditor_t *editor);

/// @brief Clear the modified flag (e.g., after a successful save).
/// @param editor Code editor widget.
void vg_codeeditor_clear_modified(vg_codeeditor_t *editor);

/// @brief Recompute gutter width and layout metrics after visual option changes.
/// @param editor Code editor widget.
void vg_codeeditor_refresh_layout_state(vg_codeeditor_t *editor);

/// @brief Reset CodeEditor performance counters to zero.
/// @param editor Code editor widget.
void vg_codeeditor_reset_perf_stats(vg_codeeditor_t *editor);

/// @brief Add display-only ghost text anchored to a source position.
/// @param editor Code editor widget.
/// @param line   Zero-based source line.
/// @param col    Zero-based source column.
/// @param text   Hint text (copied internally).
/// @param color  ARGB text color.
void vg_codeeditor_add_inlay_hint(
    vg_codeeditor_t *editor, int line, int col, const char *text, uint32_t color);

/// @brief Clear all inlay hints.
/// @param editor Code editor widget.
void vg_codeeditor_clear_inlay_hints(vg_codeeditor_t *editor);

/// @brief Return the active inlay hint count.
/// @param editor Code editor widget.
/// @return Number of retained inlay hints.
int vg_codeeditor_get_inlay_hint_count(const vg_codeeditor_t *editor);

/// @brief Copy current CodeEditor performance counters.
/// @param editor Code editor widget.
/// @return Counter snapshot; all fields zero when editor is NULL.
vg_codeeditor_perf_stats_t vg_codeeditor_get_perf_stats(const vg_codeeditor_t *editor);

//=============================================================================
// FindReplaceBar Widget
//=============================================================================

/// @brief Search options
typedef struct vg_search_options {
    bool case_sensitive; ///< Case-sensitive search
    bool whole_word;     ///< Match whole words only
    bool use_regex;      ///< Use regular expressions
    bool in_selection;   ///< Search within selection only
    bool wrap_around;    ///< Wrap to beginning when reaching end
} vg_search_options_t;

/// @brief Search match
typedef struct vg_search_match {
    uint32_t line;      ///< Line number (0-based)
    uint32_t start_col; ///< Start column
    uint32_t end_col;   ///< End column
} vg_search_match_t;

/// @brief FindReplaceBar widget structure
typedef struct vg_findreplacebar {
    vg_widget_t base;

    // Mode
    bool show_replace; ///< Show replace controls

    // Child widgets (void* to avoid circular deps)
    void *find_input;        ///< Find text input
    void *replace_input;     ///< Replace text input
    void *find_prev_btn;     ///< Find previous button
    void *find_next_btn;     ///< Find next button
    void *replace_btn;       ///< Replace button
    void *replace_all_btn;   ///< Replace all button
    void *close_btn;         ///< Close button
    void *case_sensitive_cb; ///< Case sensitive checkbox
    void *whole_word_cb;     ///< Whole word checkbox
    void *regex_cb;          ///< Regex checkbox

    // Search state
    vg_search_options_t options;     ///< Search options
    vg_search_match_t *matches;      ///< All matches in document
    size_t match_count;              ///< Number of matches
    size_t match_capacity;           ///< Match array capacity
    size_t current_match;            ///< Index of current match
    bool search_pending;             ///< Query edit is waiting for its debounce interval.
    bool search_truncated;           ///< Last scan stopped at its byte or result budget.
    float search_debounce_remaining; ///< Seconds until a pending interactive scan runs.
    size_t search_scanned_bytes;     ///< Source bytes inspected by the last completed scan.

    // Target editor
    struct vg_codeeditor *target_editor; ///< Editor to search in

    // Result display
    char result_text[64]; ///< "3 of 42" or "No results"

    // Font
    vg_font_t *font; ///< Font for text
    float font_size; ///< Font size

    // Colors
    uint32_t bg_color;          ///< Background color
    uint32_t border_color;      ///< Border color
    uint32_t match_highlight;   ///< Match highlight color
    uint32_t current_highlight; ///< Current match highlight

    // Callbacks
    void *user_data; ///< User data
    void (*on_find)(struct vg_findreplacebar *bar,
                    const char *query,
                    vg_search_options_t *options,
                    void *user_data);
    void (*on_replace)(struct vg_findreplacebar *bar,
                       const char *find,
                       const char *replace,
                       void *user_data);
    void (*on_replace_all)(struct vg_findreplacebar *bar,
                           const char *find,
                           const char *replace,
                           void *user_data);
    void (*on_close)(struct vg_findreplacebar *bar, void *user_data);
} vg_findreplacebar_t;

/// @brief Create a new find/replace bar widget (not yet attached to a parent).
/// @return New find/replace bar or NULL on failure.
vg_findreplacebar_t *vg_findreplacebar_create(void);

/// @brief Destroy a find/replace bar and free all resources.
/// @param bar Find/replace bar to destroy (may be NULL).
void vg_findreplacebar_destroy(vg_findreplacebar_t *bar);

/// @brief Set the editor that the find/replace bar operates on.
/// @param bar    Find/replace bar.
/// @param editor Target code editor.
void vg_findreplacebar_set_target(vg_findreplacebar_t *bar, struct vg_codeeditor *editor);

/// @brief Show or hide the replace row of controls.
/// @param bar  Find/replace bar.
/// @param show true to reveal the replace input and buttons.
void vg_findreplacebar_set_show_replace(vg_findreplacebar_t *bar, bool show);

/// @brief Apply new search option flags.
/// @param bar     Find/replace bar.
/// @param options Pointer to options struct (values are copied).
void vg_findreplacebar_set_options(vg_findreplacebar_t *bar, vg_search_options_t *options);

/// @brief Run a search with a new query string and rebuild the match list.
/// @param bar   Find/replace bar.
/// @param query Null-terminated search string.
void vg_findreplacebar_find(vg_findreplacebar_t *bar, const char *query);

/// @brief Navigate to the next match, wrapping if @c wrap_around is set.
/// @param bar Find/replace bar.
void vg_findreplacebar_find_next(vg_findreplacebar_t *bar);

/// @brief Navigate to the previous match, wrapping if @c wrap_around is set.
/// @param bar Find/replace bar.
void vg_findreplacebar_find_prev(vg_findreplacebar_t *bar);

/// @brief Replace the current match with the replacement text.
/// @param bar Find/replace bar.
/// @return true when a replacement was applied.
bool vg_findreplacebar_replace_current(vg_findreplacebar_t *bar);

/// @brief Replace every match in the document with the replacement text.
/// @param bar Find/replace bar.
/// @return Number of replacements applied.
size_t vg_findreplacebar_replace_all(vg_findreplacebar_t *bar);

/// @brief Get the total number of matches found by the last search.
/// @param bar Find/replace bar.
/// @return Number of matches.
size_t vg_findreplacebar_get_match_count(vg_findreplacebar_t *bar);

/// @brief Get the zero-based index of the currently highlighted match.
/// @param bar Find/replace bar.
/// @return Current match index.
size_t vg_findreplacebar_get_current_match(vg_findreplacebar_t *bar);

/// @brief Move keyboard focus to the find text input.
/// @param bar Find/replace bar.
void vg_findreplacebar_focus(vg_findreplacebar_t *bar);

/// @brief Programmatically set the find input text.
/// @param bar  Find/replace bar.
/// @param text Text to put in the find field (copied internally).
void vg_findreplacebar_set_find_text(vg_findreplacebar_t *bar, const char *text);

/// @brief Advance a pending debounced interactive search.
/// @param bar Find/replace bar to advance.
/// @param dt Elapsed seconds; invalid or negative values are ignored.
/// @return true while another tick is needed, otherwise false.
bool vg_findreplacebar_tick(vg_findreplacebar_t *bar, float dt);

/// @brief Tick @p widget only when it is a live FindReplaceBar instance.
/// @details Lets generic widget-tree schedulers service the custom-typed bar
///          without allocating a public widget-type enum value.
/// @param widget Candidate widget.
/// @param dt Elapsed seconds.
/// @return true while a matching bar still has pending work.
bool vg_findreplacebar_tick_widget(vg_widget_t *widget, float dt);

/// @brief Set the callback fired when the user closes the find bar.
/// @param bar       Find/replace bar.
/// @param callback  Handler function.
/// @param user_data User data passed to the handler.
void vg_findreplacebar_set_on_close(vg_findreplacebar_t *bar,
                                    void (*callback)(vg_findreplacebar_t *, void *),
                                    void *user_data);

/// @brief Set the font used in the find and replace input fields.
/// @param bar  Find/replace bar.
/// @param font Font handle.
/// @param size Font size in pixels.
void vg_findreplacebar_set_font(vg_findreplacebar_t *bar, vg_font_t *font, float size);

//=============================================================================
// Minimap Widget
//=============================================================================

/// @brief One direct-mapped cached line summary used by minimap rasterization.
/// @details Entries cache the trimmed visible byte-column span for one source line and one scan
///          width. A line-number modulo cache-capacity mapping provides allocation-free O(1)
///          lookup and bounded memory regardless of document size.
typedef struct vg_minimap_line_cache_entry {
    int line;         ///< Source line represented by this slot.
    int scan_columns; ///< Maximum byte columns represented by the summary.
    int first_column; ///< First non-whitespace byte column, or -1 for blank.
    int last_column;  ///< Last non-whitespace byte column, or -1 for blank.
    bool valid;       ///< True when all fields describe current source content.
} vg_minimap_line_cache_entry_t;

/// @brief Minimap widget structure
typedef struct vg_minimap {
    vg_widget_t base;

    // Source editor
    vg_codeeditor_t *editor; ///< Editor to display

    // Rendering
    uint32_t char_width;  ///< Width per character (1-2 pixels)
    uint32_t line_height; ///< Height per line (1-2 pixels)
    bool show_viewport;   ///< Show visible region indicator
    float scale;          ///< Scale factor (default: 0.1)

    // Cached render
    uint8_t *render_buffer; ///< RGBA pixels
    uint32_t buffer_width;
    uint32_t buffer_height;
    bool buffer_dirty;                         ///< Needs re-render
    vg_minimap_line_cache_entry_t *line_cache; ///< Owned direct-mapped line-summary cache.
    uint32_t maximum_cached_lines;             ///< Allocated/cacheable slot count.
    uint32_t cached_line_count;                ///< Number of currently valid slots.

    uint64_t source_revision;          ///< Saturating combined observed-source revision.
    uint64_t observed_text_revision;   ///< Last editor content revision observed by sync.
    uint64_t observed_layout_revision; ///< Last editor layout generation observed by sync.
    float observed_scroll_x;           ///< Last editor horizontal scroll position.
    float observed_scroll_y;           ///< Last editor vertical scroll position.
    int observed_visible_first_line;   ///< Last editor viewport start line.
    int observed_visible_line_count;   ///< Last editor viewport line count.
    int observed_line_count;           ///< Last editor document line count.

    // Viewport indicator
    uint32_t viewport_start_line;
    uint32_t viewport_end_line;
    uint32_t viewport_color;

    // Styling
    uint32_t bg_color;
    uint32_t text_color;

    // Interaction
    bool dragging;    ///< Dragging viewport
    int drag_start_y; ///< Drag start Y position

    // Markers (colored horizontal lines overlaid on the minimap)
    struct vg_minimap_marker {
        int line;       ///< Source editor line number (0-based)
        uint32_t color; ///< Marker color (0x00RRGGBB)
        int type;       ///< Marker type (user-defined category)
    } *markers;         ///< Owned array; NULL when unused

    int marker_count; ///< Active marker count
    int marker_cap;   ///< Allocated capacity
} vg_minimap_t;

/// @brief Create a new minimap widget linked to a code editor.
/// @param editor Code editor whose content the minimap reflects.
/// @return New minimap widget or NULL on failure.
vg_minimap_t *vg_minimap_create(vg_codeeditor_t *editor);

/// @brief Destroy a minimap widget and free its render buffer.
/// @param minimap Minimap widget to destroy (may be NULL).
void vg_minimap_destroy(vg_minimap_t *minimap);

/// @brief Change the editor the minimap is linked to.
/// @param minimap Minimap widget.
/// @param editor  New source editor.
void vg_minimap_set_editor(vg_minimap_t *minimap, vg_codeeditor_t *editor);

/// @brief Set the pixel scale used to render the code overview.
/// @param minimap Minimap widget.
/// @param scale   Scale factor (e.g. 0.1 = each source pixel maps to 0.1 display pixels).
void vg_minimap_set_scale(vg_minimap_t *minimap, float scale);

/// @brief Show or hide the visible-region rectangle overlay.
/// @param minimap Minimap widget.
/// @param show    true to draw the viewport indicator.
void vg_minimap_set_show_viewport(vg_minimap_t *minimap, bool show);

/// @brief Mark the entire minimap render buffer as dirty (needs full re-render).
/// @param minimap Minimap widget.
void vg_minimap_invalidate(vg_minimap_t *minimap);

/// @brief Mark a range of source lines as dirty in the minimap buffer.
/// @param minimap     Minimap widget.
/// @param start_line  First dirty source line (zero-based, inclusive).
/// @param end_line    Last dirty source line (zero-based, inclusive).
void vg_minimap_invalidate_lines(vg_minimap_t *minimap, uint32_t start_line, uint32_t end_line);

/// @brief Synchronize cached text/layout/scroll observations from the bound editor.
/// @details Content, layout, or line-count changes invalidate cached line summaries; scroll-only
///          changes invalidate paint for the viewport overlay without discarding line summaries.
///          Any observed transition advances the combined source revision exactly once.
/// @param minimap Minimap widget to synchronize; NULL is ignored.
/// @return true when source or viewport state changed, otherwise false.
bool vg_minimap_sync_source(vg_minimap_t *minimap);

/// @brief Return the combined observed editor source revision.
/// @details The source is synchronized before reading. The revision covers content, layout,
///          viewport, and binding transitions and saturates at UINT64_MAX.
/// @param minimap Minimap widget to inspect.
/// @return Non-consuming revision, or zero for NULL.
uint64_t vg_minimap_get_source_revision(vg_minimap_t *minimap);

/// @brief Atomically resize the bounded direct-mapped line-summary cache.
/// @details Zero disables caching and releases existing storage. A failed non-zero allocation
///          preserves the previous cache and limit. Resizing invalidates cached summaries but not
///          source pixels or the observed-source revision.
/// @param minimap Minimap widget to configure.
/// @param maximum_lines Maximum cache slots; zero disables caching.
/// @return true after the requested configuration is installed, otherwise false.
bool vg_minimap_set_maximum_cached_lines(vg_minimap_t *minimap, uint32_t maximum_lines);

/// @brief Return the number of valid cached line summaries.
/// @param minimap Minimap widget to inspect.
/// @return Valid entry count, always no greater than the configured maximum.
uint32_t vg_minimap_get_cached_line_count(const vg_minimap_t *minimap);

#ifdef __cplusplus
}
#endif
