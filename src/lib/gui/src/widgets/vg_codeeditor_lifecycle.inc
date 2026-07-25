//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
///
/// @file vg_codeeditor_lifecycle.inc
/// @brief CodeEditor creation, initialization, disposal, invalidation, sizing,
/// and lifecycle hooks.
///
/// @details This include fragment is consumed only by vg_codeeditor.c and
/// centralizes the code that owns editor allocation, resource release, and GUI
/// widget lifecycle integration.
///
// File: src/lib/gui/src/widgets/vg_codeeditor_lifecycle.inc
// Purpose: Widget creation, destruction, measurement, and owned editor resource cleanup.
//
// Key invariants:
//   - Included only by src/lib/gui/src/widgets/vg_codeeditor.c.
//   - Fragment order preserves the original static helper visibility and
//     vtable wiring from the monolithic source file.
//   - Public CodeEditor behavior and exported symbols remain unchanged.
//
// Ownership/Lifetime:
//   - Ownership follows vg_codeeditor_t rules from vg_ide_widgets_editor.h;
//     text lines, history, highlights, fold regions, inlay hints, and extra
//     cursor storage are freed by the lifecycle fragment.
//
// Links: src/lib/gui/src/widgets/vg_codeeditor.c,
//        src/lib/gui/include/vg_ide_widgets_editor.h
//
//===----------------------------------------------------------------------===//

//=============================================================================
// CodeEditor Implementation
//=============================================================================

/// @brief Create a code editor widget with a single empty line and default theme colours.
///
/// @details The editor supports syntax highlighting (set via vg_codeeditor_set_syntax),
///          word wrap, line numbers, folding gutter, and a scrollbar.  All features
///          are disabled or hidden by default; enable them via the corresponding setters.
///
/// @param parent Widget to attach to as a child (may be NULL).
/// @return Newly allocated vg_codeeditor_t, or NULL on allocation failure.
vg_codeeditor_t *vg_codeeditor_create(vg_widget_t *parent) {
    vg_codeeditor_t *editor = calloc(1, sizeof(vg_codeeditor_t));
    if (!editor)
        return NULL;

    // Initialize base widget
    vg_widget_init(&editor->base, VG_WIDGET_CODEEDITOR, &g_codeeditor_vtable);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Allocate initial lines
    editor->lines = calloc(INITIAL_LINE_CAPACITY, sizeof(vg_code_line_t));
    if (!editor->lines) {
        vg_widget_destroy(&editor->base);
        return NULL;
    }
    editor->line_capacity = INITIAL_LINE_CAPACITY;

    // Create first empty line
    editor->line_count = 1;
    editor->lines[0].text = malloc(INITIAL_TEXT_CAPACITY);
    if (!editor->lines[0].text) {
        vg_widget_destroy(&editor->base);
        return NULL;
    }
    editor->lines[0].text[0] = '\0';
    editor->lines[0].length = 0;
    editor->lines[0].capacity = INITIAL_TEXT_CAPACITY;

    // Create shared text-engine mirror.
    editor->document_buffer = zanna_text_buffer_new();
    if (!editor->document_buffer ||
        !zanna_text_buffer_load_bytes((zanna_text_buffer_t *)editor->document_buffer, "", 0)) {
        vg_widget_destroy(&editor->base);
        return NULL;
    }

    // Cursor and selection
    editor->cursor_line = 0;
    editor->cursor_col = 0;
    editor->has_selection = false;
    memset(&editor->selection, 0, sizeof(editor->selection));

    // Scroll
    editor->scroll_x = 0;
    editor->scroll_y = 0;
    editor->visible_first_line = 0;
    editor->visible_line_count = 0;

    // Font
    editor->font = NULL;
    editor->font_size = theme->typography.size_normal;
    editor->char_width = 8.0f; // Default, updated when font is set
    editor->line_height = 18.0f;
    editor->font_pinned = false; // Until a caller sets the font explicitly, the
                                 // editor follows the app default font.

    // Gutter
    editor->show_line_numbers = true;
    editor->ligatures_enabled = true;
    editor->gutter_width = 50.0f;
    editor->line_number_width_override = 0.0f;
    editor->gutter_bg = theme->colors.bg_secondary;
    editor->line_number_color = theme->colors.fg_tertiary;
    editor->gutter_clicked = false;
    editor->gutter_click_read = false;
    editor->gutter_clicked_line = -1;
    editor->gutter_clicked_slot = -1;

    // Appearance
    editor->bg_color = theme->colors.bg_primary;
    editor->text_color = theme->colors.fg_primary;
    editor->cursor_color = theme->colors.fg_primary;
    editor->selection_color = theme->colors.bg_selected;
    // A clearly visible blue-tinted active-line band (bg_tertiary was almost
    // indistinguishable from the editor background).
    editor->current_line_bg =
        vg_color_blend(theme->colors.bg_primary, theme->colors.accent_primary, 0.18f);

    // Syntax highlighting
    editor->syntax_highlighter = NULL;
    editor->syntax_data = NULL;

    // Editing options
    editor->read_only = false;
    editor->insert_mode = true;
    editor->tab_width = 4;
    editor->use_spaces = true;
    editor->auto_indent = true;
    editor->word_wrap = false;

    // Display options
    editor->show_indent_guides = true;
    editor->render_whitespace_mode = VG_WHITESPACE_NONE;
    editor->whitespace_color = 0; // 0 = derive a faint default from the text color at paint time
    editor->indent_guide_color = 0; // 0 = derive a muted default at paint time

    // State
    editor->cursor_visible = true;
    editor->cursor_blink_time = 0;
    editor->modified = false;
    editor->revision = 1;
    editor->highlight_generation = 1;
    editor->layout_generation = 1;
    editor->has_folded_lines = false;
    editor->layout_cache_valid = false;
    editor->layout_cache_generation = 0;
    editor->layout_cache_content_width = 0.0f;
    editor->layout_cache_word_wrap = false;
    editor->layout_cache_line_count = 0;
    editor->layout_cache_total_visual_rows = 0;
    editor->layout_cache_total_height = 0.0f;
    editor->layout_cache_prefix_rows = NULL;
    editor->layout_cache_capacity = 0;
    editor->runtime_content_width_cache_valid = false;
    editor->runtime_content_width_generation = 0;
    editor->runtime_content_width_base_width = 0.0f;
    editor->runtime_content_width_viewport_height = 0.0f;
    editor->runtime_content_width_word_wrap = false;
    editor->runtime_content_width = 0.0f;
    editor->highlight_spans_sorted = true;
    editor->highlight_line_index_valid = false;
    editor->highlight_line_index_line_count = 0;
    editor->highlight_line_index_span_count = 0;
    editor->highlight_line_offsets = NULL;
    editor->highlight_line_offsets_cap = 0;
    editor->highlight_line_span_indices = NULL;
    editor->highlight_line_span_indices_cap = 0;
    editor->inlay_hints_sorted = true;

    // Create undo/redo history
    editor->history = edit_history_create();
    if (!editor->history) {
        vg_widget_destroy(&editor->base);
        return NULL;
    }

    // Add to parent
    if (parent) {
        vg_widget_add_child(parent, &editor->base);
    }

    return editor;
}

/// @brief VTable destroy: frees all lines, history, custom keywords, highlight spans, gutter icons,
/// and fold regions.
/// @details Also releases the shared text-buffer mirror, incremental journal strings, semantic
/// tokens, layout-prefix cache, inlay hints, and secondary cursors.
/// @param widget Code-editor base widget being destroyed.
static void codeeditor_destroy(vg_widget_t *widget) {
    vg_codeeditor_t *editor = (vg_codeeditor_t *)widget;

    for (int i = 0; i < editor->line_count; i++) {
        free_line(&editor->lines[i]);
    }
    free(editor->lines);
    editor->lines = NULL;
    editor->line_count = 0;
    editor->line_capacity = 0;

    zanna_text_buffer_free((zanna_text_buffer_t *)editor->document_buffer);
    editor->document_buffer = NULL;

    // Free undo/redo history
    edit_history_destroy(editor->history);
    editor->history = NULL;

    // Free retained sync-journal delta text (plan 08).
    codeeditor_journal_reset(editor);

    if (editor->custom_keywords) {
        for (int i = 0; i < editor->custom_keyword_count; i++)
            free(editor->custom_keywords[i]);
        free(editor->custom_keywords);
        editor->custom_keywords = NULL;
        editor->custom_keyword_count = 0;
    }

    codeeditor_clear_highlight_spans(editor);
    codeeditor_free_inlay_hint_storage(editor);

    free(editor->semantic_tokens);
    editor->semantic_tokens = NULL;
    editor->semantic_token_count = 0;
    editor->semantic_token_cap = 0;
    editor->semantic_tokens_sorted = true;

    if (editor->gutter_icons) {
        for (int i = 0; i < editor->gutter_icon_count; i++)
            vg_icon_destroy(&editor->gutter_icons[i].image);
        free(editor->gutter_icons);
    }
    editor->gutter_icons = NULL;
    editor->gutter_icon_count = 0;
    editor->gutter_icon_cap = 0;

    free(editor->fold_regions);
    editor->fold_regions = NULL;
    editor->fold_region_count = 0;
    editor->fold_region_cap = 0;
    editor->has_folded_lines = false;

    free(editor->layout_cache_prefix_rows);
    editor->layout_cache_prefix_rows = NULL;
    editor->layout_cache_capacity = 0;
    editor->layout_cache_valid = false;

    free(editor->extra_cursors);
    editor->extra_cursors = NULL;
    editor->extra_cursor_count = 0;
    editor->extra_cursor_cap = 0;
}

/// @brief VTable measure: fills all available space, clamped to min_width/min_height constraints.
/// @param widget Code-editor base widget whose measured dimensions are updated.
/// @param available_width Parent-provided width, or a 400-pixel fallback when non-positive.
/// @param available_height Parent-provided height, or a 300-pixel fallback when non-positive.
static void codeeditor_measure(vg_widget_t *widget, float available_width, float available_height) {
    // Code editor fills available space
    widget->measured_width = available_width > 0 ? available_width : 400;
    widget->measured_height = available_height > 0 ? available_height : 300;

    // Apply constraints
    if (widget->measured_width < widget->constraints.min_width) {
        widget->measured_width = widget->constraints.min_width;
    }
    if (widget->measured_height < widget->constraints.min_height) {
        widget->measured_height = widget->constraints.min_height;
    }
}

/// @brief Blits a gutter icon's RGBA pixel data into the framebuffer at (dst_x, dst_y) with
/// clipping applied.
