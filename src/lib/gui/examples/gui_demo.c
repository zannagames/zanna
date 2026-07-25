//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: gui_demo.c
// Purpose: Demo application for ZannaGUI widget library.
//
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Graphics library
#include "vgfx.h"

// GUI library headers
#include "vg_event.h"
#include "vg_font.h"
#include "vg_ide_widgets.h"
#include "vg_layout.h"
#include "vg_theme.h"
#include "vg_widget.h"
#include "vg_widgets.h"

/// @file
/// @brief Demonstrates basic ZannaGUI label, button, text-input, and checkbox widgets.
/// @details The example manually renders the widgets and performs simple event translation,
/// hit testing, focus handling, text editing, and live theme switching.

//=============================================================================
// Demo State
//=============================================================================

/// @brief Resources, interactive widgets, and loop state owned by the demo.
typedef struct {
    /// @brief Native graphics window used for events and drawing.
    vgfx_window_t window;

    /// @brief Optional platform font used for all visible text.
    vg_font_t *font;

    // Root widget
    /// @brief Root container that owns the widget tree.
    vg_widget_t *root;

    // Widgets for interaction
    /// @brief Label updated by button and text-input callbacks.
    vg_label_t *status_label;

    /// @brief Single-line editable text widget.
    vg_textinput_t *text_input;

    /// @brief Checkbox that selects the global light or dark theme.
    vg_checkbox_t *dark_mode_checkbox;

    /// @brief Primary button incrementing @ref click_count.
    vg_button_t *click_button;

    /// @brief Number of button activations observed so far.
    int click_count;

    // Running flag
    /// @brief True while the application main loop should continue.
    bool running;
} demo_state_t;

static demo_state_t g_demo;

//=============================================================================
// Callbacks
//=============================================================================

/// @brief Increment the click counter and publish it through the status label.
/// @param button Button that originated the callback.
/// @param user_data Owning @ref demo_state_t.
static void on_button_click(vg_widget_t *button, void *user_data) {
    demo_state_t *demo = (demo_state_t *)user_data;
    demo->click_count++;

    char buf[64];
    snprintf(buf, sizeof(buf), "Button clicked %d times!", demo->click_count);
    vg_label_set_text(demo->status_label, buf);
    vg_widget_invalidate((vg_widget_t *)demo->status_label);
}

/// @brief Switch the process-wide widget theme when the checkbox changes.
/// @param checkbox Checkbox that originated the callback.
/// @param checked True to select the dark theme; false for the light theme.
/// @param user_data Callback context, unused by this handler.
static void on_dark_mode_change(vg_widget_t *checkbox, bool checked, void *user_data) {
    (void)checkbox;
    (void)user_data;
    if (checked) {
        vg_theme_set_current(vg_theme_dark());
    } else {
        vg_theme_set_current(vg_theme_light());
    }
}

/// @brief Mirror the text input's current contents in the status label.
/// @param input Text-input widget that originated the callback.
/// @param text Current UTF-8 text, or NULL.
/// @param user_data Owning @ref demo_state_t.
static void on_text_change(vg_widget_t *input, const char *text, void *user_data) {
    (void)input;
    demo_state_t *demo = (demo_state_t *)user_data;

    char buf[256];
    snprintf(buf, sizeof(buf), "Text: %.200s", text ? text : "");
    vg_label_set_text(demo->status_label, buf);
    vg_widget_invalidate((vg_widget_t *)demo->status_label);
}

//=============================================================================
// Widget Drawing Helpers
//=============================================================================

// Draw a filled rectangle using vgfx
/// @brief Fill a floating-point widget rectangle through the integer graphics API.
/// @param window Target graphics window.
/// @param x Left edge in window coordinates.
/// @param y Top edge in window coordinates.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param color Packed color whose alpha byte is ignored.
static void draw_rect(vgfx_window_t window, float x, float y, float w, float h, uint32_t color) {
    // Convert ARGB to RGB for vgfx
    uint32_t rgb = color & 0x00FFFFFF;
    vgfx_fill_rect(window, (int)x, (int)y, (int)w, (int)h, rgb);
}

// Draw a rectangle outline
/// @brief Stroke a floating-point widget rectangle through the integer graphics API.
/// @param window Target graphics window.
/// @param x Left edge in window coordinates.
/// @param y Top edge in window coordinates.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param color Packed color whose alpha byte is ignored.
static void draw_rect_outline(
    vgfx_window_t window, float x, float y, float w, float h, uint32_t color) {
    uint32_t rgb = color & 0x00FFFFFF;
    vgfx_rect(window, (int)x, (int)y, (int)w, (int)h, rgb);
}

//=============================================================================
// Simple Widget Rendering (before vtable painting is fully hooked up)
//=============================================================================

/// @brief Render one visible label at its computed screen position.
/// @param window Target graphics window.
/// @param label Label to render; null or hidden labels are ignored.
static void render_label(vgfx_window_t window, vg_label_t *label) {
    if (!label || !label->base.visible)
        return;

    float sx, sy;
    vg_widget_get_screen_bounds(&label->base, &sx, &sy, NULL, NULL);

    // Get theme colors
    vg_theme_t *theme = vg_theme_get_current();
    uint32_t text_color = label->text_color ? label->text_color : theme->colors.fg_primary;

    // Draw text if font is available
    if (label->font && label->text) {
        vg_font_draw_text(window,
                          label->font,
                          label->font_size,
                          sx,
                          sy + label->font_size,
                          label->text,
                          text_color);
    }
}

/// @brief Render a button using its theme, style, and interaction state.
/// @param window Target graphics window.
/// @param button Button to render; null or hidden buttons are ignored.
static void render_button(vgfx_window_t window, vg_button_t *button) {
    if (!button || !button->base.visible)
        return;

    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&button->base, &sx, &sy, &sw, &sh);

    // Get theme
    vg_theme_t *theme = vg_theme_get_current();

    // Determine colors based on state
    uint32_t bg_color = theme->colors.bg_secondary;
    uint32_t fg_color = theme->colors.fg_primary;
    uint32_t border_color = theme->colors.border_primary;

    if (button->base.state & VG_STATE_HOVERED) {
        bg_color = theme->colors.bg_hover;
    }
    if (button->base.state & VG_STATE_PRESSED) {
        bg_color = theme->colors.bg_active;
    }
    if (button->style == VG_BUTTON_STYLE_PRIMARY) {
        bg_color = theme->colors.accent_primary;
        fg_color = 0xFFFFFFFF;
    }

    // Draw background
    draw_rect(window, sx, sy, sw, sh, bg_color);

    // Draw border
    draw_rect_outline(window, sx, sy, sw, sh, border_color);

    // Draw text centered
    if (button->font && button->text) {
        vg_text_metrics_t metrics;
        vg_font_measure_text(button->font, button->font_size, button->text, &metrics);
        float tx = sx + (sw - metrics.width) / 2;
        float ty = sy + (sh + button->font_size) / 2 - 2;
        vg_font_draw_text(window, button->font, button->font_size, tx, ty, button->text, fg_color);
    }
}

/// @brief Render a text input, placeholder, focus border, and caret.
/// @param window Target graphics window.
/// @param input Text input to render; null or hidden inputs are ignored.
static void render_textinput(vgfx_window_t window, vg_textinput_t *input) {
    if (!input || !input->base.visible)
        return;

    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&input->base, &sx, &sy, &sw, &sh);

    vg_theme_t *theme = vg_theme_get_current();

    uint32_t bg_color = theme->colors.bg_primary;
    uint32_t border_color = theme->colors.border_primary;
    uint32_t text_color = theme->colors.fg_primary;

    if (input->base.state & VG_STATE_FOCUSED) {
        border_color = theme->colors.border_focus;
    }

    // Draw background
    draw_rect(window, sx, sy, sw, sh, bg_color);

    // Draw border
    draw_rect_outline(window, sx, sy, sw, sh, border_color);

    // Draw text or placeholder
    const char *display_text = input->text;
    if ((!display_text || display_text[0] == '\0') && input->placeholder) {
        display_text = input->placeholder;
        text_color = theme->colors.fg_placeholder;
    }

    if (input->font && display_text) {
        float tx = sx + 4;
        float ty = sy + (sh + input->font_size) / 2 - 2;
        vg_font_draw_text(window, input->font, input->font_size, tx, ty, display_text, text_color);
    }

    // Draw cursor if focused
    if ((input->base.state & VG_STATE_FOCUSED) && input->font) {
        float cursor_x = sx + 4;
        if (input->text && input->cursor_pos > 0) {
            cursor_x += vg_font_get_cursor_x(
                input->font, input->font_size, input->text, (int)input->cursor_pos);
        }
        float cursor_y1 = sy + 4;
        float cursor_y2 = sy + sh - 4;
        vgfx_line(window,
                  (int)cursor_x,
                  (int)cursor_y1,
                  (int)cursor_x,
                  (int)cursor_y2,
                  text_color & 0x00FFFFFF);
    }
}

/// @brief Render a checkbox box, checked mark, and adjacent label text.
/// @param window Target graphics window.
/// @param checkbox Checkbox to render; null or hidden widgets are ignored.
static void render_checkbox(vgfx_window_t window, vg_checkbox_t *checkbox) {
    if (!checkbox || !checkbox->base.visible)
        return;

    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&checkbox->base, &sx, &sy, &sw, &sh);

    vg_theme_t *theme = vg_theme_get_current();

    float box_size = checkbox->box_size > 0 ? checkbox->box_size : 16;
    float box_y = sy + (sh - box_size) / 2;

    uint32_t box_bg = theme->colors.bg_primary;
    uint32_t box_border = theme->colors.border_primary;
    uint32_t check_color = theme->colors.accent_primary;

    if (checkbox->base.state & VG_STATE_HOVERED) {
        box_border = theme->colors.border_focus;
    }

    // Draw checkbox box
    draw_rect(window, sx, box_y, box_size, box_size, box_bg);
    draw_rect_outline(window, sx, box_y, box_size, box_size, box_border);

    // Draw checkmark if checked
    if (checkbox->checked) {
        float cx = sx + box_size / 2;
        float cy = box_y + box_size / 2;
        // Simple X checkmark
        int check_rgb = check_color & 0x00FFFFFF;
        vgfx_line(window, (int)(cx - 4), (int)(cy - 4), (int)(cx + 4), (int)(cy + 4), check_rgb);
        vgfx_line(window, (int)(cx + 4), (int)(cy - 4), (int)(cx - 4), (int)(cy + 4), check_rgb);
    }

    // Draw label text
    if (checkbox->font && checkbox->text) {
        float tx = sx + box_size + (checkbox->gap > 0 ? checkbox->gap : 8);
        float ty = sy + (sh + checkbox->font_size) / 2 - 2;
        vg_font_draw_text(window,
                          checkbox->font,
                          checkbox->font_size,
                          tx,
                          ty,
                          checkbox->text,
                          theme->colors.fg_primary);
    }
}

//=============================================================================
// Main Render Function
//=============================================================================

static bool g_debug_printed = false;

/// @brief Draw one complete demo frame and emit one-time rendering diagnostics.
/// @details On the first frame with a font, the function reports logical and framebuffer
/// dimensions, one glyph record, and font metrics. It then clears the window and renders the
/// diagnostic primitives and interactive widget set.
/// @param demo Initialized demo state.
static void render_demo(demo_state_t *demo) {
    vgfx_window_t window = demo->window;
    vg_theme_t *theme = vg_theme_get_current();

    // Debug: print window size once
    if (!g_debug_printed && demo->font) {
        int32_t w, h;
        vgfx_get_size(window, &w, &h);
        printf("Window (logical) size: %d x %d\n", w, h);

        // Check framebuffer dimensions
        vgfx_framebuffer_t fb;
        if (vgfx_get_framebuffer(window, &fb)) {
            printf("Framebuffer size: %d x %d, stride: %d bytes\n", fb.width, fb.height, fb.stride);
        }

        // Test glyph retrieval
        const vg_glyph_t *glyph = vg_font_get_glyph(demo->font, 24.0f, 'T');
        if (glyph) {
            printf("Glyph 'T': width=%d height=%d advance=%d bearing_x=%d bearing_y=%d bitmap=%p\n",
                   glyph->width,
                   glyph->height,
                   glyph->advance,
                   glyph->bearing_x,
                   glyph->bearing_y,
                   (void *)glyph->bitmap);
        } else {
            printf("ERROR: Could not get glyph for 'T'\n");
        }

        // Check font metrics
        vg_font_metrics_t metrics;
        vg_font_get_metrics(demo->font, 24.0f, &metrics);
        printf("Font metrics at 24px: ascent=%d descent=%d line_height=%d\n",
               metrics.ascent,
               metrics.descent,
               metrics.line_height);

        fflush(stdout);
        g_debug_printed = true;
    }

    // Clear background
    vgfx_cls(window, theme->colors.bg_primary & 0x00FFFFFF);

    // Debug: draw direct rectangles to verify rendering works
    vgfx_fill_rect(window, 20, 20, 200, 30, 0xFF0000); // Red rect at top
    vgfx_fill_rect(window, 20, 60, 200, 30, 0x00FF00); // Green rect below

    // Debug: try drawing text directly with the font
    if (demo->font) {
        // Draw with white color (0xFFFFFFFF) to ensure visibility
        vg_font_draw_text(window, demo->font, 24.0f, 250, 40, "TEST", 0xFFFFFFFF);

        // Try a simpler approach - just draw at baseline
        vg_font_draw_text(window, demo->font, 16.0f, 250, 80, "Hello", 0xFFFFFFFF);
    }

    // Draw title using theme color
    if (demo->font) {
        vg_font_draw_text(
            window, demo->font, 24.0f, 20, 130, "ZannaGUI Demo", theme->colors.fg_primary);
    }

    // Render widgets at their actual positions
    render_button(window, demo->click_button);
    render_textinput(window, demo->text_input);
    render_checkbox(window, demo->dark_mode_checkbox);
    render_label(window, demo->status_label);
}

//=============================================================================
// Event Handling
//=============================================================================

/// @brief Drain native events and update widget hover, focus, press, theme, and text state.
/// @details Platform events are translated to GUI events for hit testing and editing. Close
/// or Escape stops the loop; focused text input handles backspace, arrows, and printable ASCII.
/// @param demo Mutable initialized demo state.
static void handle_events(demo_state_t *demo) {
    vgfx_event_t pe;

    while (vgfx_poll_event(demo->window, &pe)) {
        // Check for close event
        if (pe.type == VGFX_EVENT_CLOSE) {
            demo->running = false;
            return;
        }

        // Check for escape key
        if (pe.type == VGFX_EVENT_KEY_DOWN && pe.data.key.key == VGFX_KEY_ESCAPE) {
            demo->running = false;
            return;
        }

        // Translate to GUI event
        vg_event_t event = vg_event_from_platform(&pe);

        // Do basic hit testing and state management
        if (event.type == VG_EVENT_MOUSE_MOVE) {
            int32_t mx, my;
            vgfx_mouse_pos(demo->window, &mx, &my);

            // Clear all hover states
            demo->click_button->base.state &= ~VG_STATE_HOVERED;
            demo->text_input->base.state &= ~VG_STATE_HOVERED;
            demo->dark_mode_checkbox->base.state &= ~VG_STATE_HOVERED;

            // Check button hover
            float bx, by, bw, bh;
            vg_widget_get_screen_bounds(&demo->click_button->base, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                demo->click_button->base.state |= VG_STATE_HOVERED;
            }

            // Check textinput hover
            vg_widget_get_screen_bounds(&demo->text_input->base, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                demo->text_input->base.state |= VG_STATE_HOVERED;
            }

            // Check checkbox hover
            vg_widget_get_screen_bounds(&demo->dark_mode_checkbox->base, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                demo->dark_mode_checkbox->base.state |= VG_STATE_HOVERED;
            }
        }

        if (event.type == VG_EVENT_MOUSE_DOWN) {
            int32_t mx, my;
            vgfx_mouse_pos(demo->window, &mx, &my);

            // Clear all focus
            demo->text_input->base.state &= ~VG_STATE_FOCUSED;

            // Check button click
            float bx, by, bw, bh;
            vg_widget_get_screen_bounds(&demo->click_button->base, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                demo->click_button->base.state |= VG_STATE_PRESSED;
                on_button_click(&demo->click_button->base, demo);
            }

            // Check textinput click (focus)
            vg_widget_get_screen_bounds(&demo->text_input->base, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                demo->text_input->base.state |= VG_STATE_FOCUSED;
            }

            // Check checkbox click
            vg_widget_get_screen_bounds(&demo->dark_mode_checkbox->base, &bx, &by, &bw, &bh);
            if (mx >= bx && mx < bx + bw && my >= by && my < by + bh) {
                vg_checkbox_toggle(demo->dark_mode_checkbox);
                on_dark_mode_change(
                    &demo->dark_mode_checkbox->base, demo->dark_mode_checkbox->checked, demo);
            }
        }

        if (event.type == VG_EVENT_MOUSE_UP) {
            demo->click_button->base.state &= ~VG_STATE_PRESSED;
        }

        // Handle text input
        if (event.type == VG_EVENT_KEY_DOWN && (demo->text_input->base.state & VG_STATE_FOCUSED)) {
            vg_key_t key = event.key.key;

            if (key == VG_KEY_BACKSPACE) {
                // Delete character before cursor
                if (demo->text_input->cursor_pos > 0 && demo->text_input->text_len > 0) {
                    memmove(demo->text_input->text + demo->text_input->cursor_pos - 1,
                            demo->text_input->text + demo->text_input->cursor_pos,
                            demo->text_input->text_len - demo->text_input->cursor_pos + 1);
                    demo->text_input->cursor_pos--;
                    demo->text_input->text_len--;
                    on_text_change(&demo->text_input->base, demo->text_input->text, demo);
                }
            } else if (key == VG_KEY_LEFT) {
                if (demo->text_input->cursor_pos > 0) {
                    demo->text_input->cursor_pos--;
                }
            } else if (key == VG_KEY_RIGHT) {
                if (demo->text_input->cursor_pos < demo->text_input->text_len) {
                    demo->text_input->cursor_pos++;
                }
            } else if (key >= VG_KEY_SPACE && key <= 127) {
                // Insert printable character
                char ch = (char)key;
                // Handle shift for uppercase
                if (key >= VG_KEY_A && key <= VG_KEY_Z) {
                    if (!(event.modifiers & VG_MOD_SHIFT)) {
                        ch = ch - 'A' + 'a';
                    }
                }
                vg_textinput_insert(demo->text_input, (char[]){ch, '\0'});
                on_text_change(&demo->text_input->base, demo->text_input->text, demo);
            }
        }
    }
}

//=============================================================================
// Initialization
//=============================================================================

/// @brief Create the demo window, optional font, theme, and widget tree.
/// @details Font discovery tries common macOS, Linux, and Windows paths and is nonfatal.
/// Failure to create the native window or root widget aborts initialization.
/// @param demo Zero-initialized destination state.
/// @return True when resources required by the main loop are ready.
static bool init_demo(demo_state_t *demo) {
    // Create window
    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = 800;
    params.height = 600;
    params.title = "ZannaGUI Demo";
    params.resizable = 1;
    params.fps = 60;

    demo->window = vgfx_create_window(&params);
    if (!demo->window) {
        fprintf(stderr, "Failed to create window: %s\n", vgfx_get_last_error());
        return false;
    }

    // Try to load a font - look in common system locations
    const char *font_paths[] = {"/System/Library/Fonts/SFNSMono.ttf",              // macOS
                                "/System/Library/Fonts/Menlo.ttc",                 // macOS
                                "/System/Library/Fonts/Monaco.ttf",                // macOS
                                "/System/Library/Fonts/Supplemental/Arial.ttf",    // macOS
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", // Linux
                                "/usr/share/fonts/TTF/DejaVuSans.ttf",             // Arch Linux
                                "C:\\Windows\\Fonts\\arial.ttf",                   // Windows
                                NULL};

    for (int i = 0; font_paths[i] != NULL; i++) {
        demo->font = vg_font_load_file(font_paths[i]);
        if (demo->font) {
            printf("Loaded font: %s\n", font_paths[i]);
            fflush(stdout);
            break;
        }
    }

    if (!demo->font) {
        fprintf(stderr, "Warning: No font could be loaded. Text will not display.\n");
        fflush(stderr);
    }

    // Set up default theme
    vg_theme_set_current(vg_theme_dark());

    // Create root container
    demo->root = vg_widget_create(VG_WIDGET_CONTAINER);
    if (!demo->root) {
        fprintf(stderr, "Failed to create root widget\n");
        return false;
    }

    // Create status label
    demo->status_label = vg_label_create(demo->root, "Welcome to ZannaGUI!");
    if (demo->status_label && demo->font) {
        vg_label_set_font(demo->status_label, demo->font, 16.0f);
        demo->status_label->base.x = 20;
        demo->status_label->base.y = 60;
        demo->status_label->base.width = 400;
        demo->status_label->base.height = 24;
    }

    // Create click button
    demo->click_button = vg_button_create(demo->root, "Click Me!");
    if (demo->click_button && demo->font) {
        vg_button_set_font(demo->click_button, demo->font, 14.0f);
        vg_button_set_style(demo->click_button, VG_BUTTON_STYLE_PRIMARY);
        demo->click_button->base.x = 20;
        demo->click_button->base.y = 100;
        demo->click_button->base.width = 120;
        demo->click_button->base.height = 36;
    }
    demo->click_count = 0;

    // Create text input
    demo->text_input = vg_textinput_create(demo->root);
    if (demo->text_input && demo->font) {
        vg_textinput_set_font(demo->text_input, demo->font, 14.0f);
        vg_textinput_set_placeholder(demo->text_input, "Type something...");
        demo->text_input->base.x = 20;
        demo->text_input->base.y = 150;
        demo->text_input->base.width = 300;
        demo->text_input->base.height = 32;
    }

    // Create dark mode checkbox
    demo->dark_mode_checkbox = vg_checkbox_create(demo->root, "Dark Mode");
    if (demo->dark_mode_checkbox && demo->font) {
        demo->dark_mode_checkbox->font = demo->font;
        demo->dark_mode_checkbox->font_size = 14.0f;
        demo->dark_mode_checkbox->box_size = 18;
        demo->dark_mode_checkbox->gap = 8;
        vg_checkbox_set_checked(demo->dark_mode_checkbox, true); // Start in dark mode
        demo->dark_mode_checkbox->base.x = 20;
        demo->dark_mode_checkbox->base.y = 200;
        demo->dark_mode_checkbox->base.width = 150;
        demo->dark_mode_checkbox->base.height = 24;
    }

    demo->running = true;
    return true;
}

/// @brief Release the widget tree, font, and graphics window owned by the demo.
/// @param demo Demo state whose initialized resources should be destroyed.
static void cleanup_demo(demo_state_t *demo) {
    if (demo->root) {
        vg_widget_destroy(demo->root);
        demo->root = NULL;
    }

    if (demo->font) {
        vg_font_destroy(demo->font);
        demo->font = NULL;
    }

    if (demo->window) {
        vgfx_destroy_window(demo->window);
        demo->window = NULL;
    }
}

//=============================================================================
// Main
//=============================================================================

/// @brief Run the interactive ZannaGUI widget demonstration.
/// @param argc Process argument count; currently ignored.
/// @param argv Process argument vector; currently ignored.
/// @return Zero after normal completion, or one when initialization fails.
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("ZannaGUI Demo\n");
    printf("=============\n");
    printf("Press ESC to exit\n\n");
    fflush(stdout);

    memset(&g_demo, 0, sizeof(g_demo));

    if (!init_demo(&g_demo)) {
        fprintf(stderr, "Failed to initialize demo\n");
        return 1;
    }

    // Main loop
    while (g_demo.running) {
        handle_events(&g_demo);
        render_demo(&g_demo);

        if (!vgfx_update(g_demo.window)) {
            break;
        }
    }

    cleanup_demo(&g_demo);

    printf("Demo exited cleanly.\n");
    return 0;
}
