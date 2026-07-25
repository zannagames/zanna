//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: color_widgets_demo.c
// Purpose: Demo application for ZannaGUI color widgets (ColorSwatch,
//          ColorPalette, ColorPicker).
//
//===----------------------------------------------------------------------===//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Graphics library
#include "vgfx.h"

// GUI library headers
#include "vg_event.h"
#include "vg_font.h"
#include "vg_layout.h"
#include "vg_theme.h"
#include "vg_widget.h"
#include "vg_widgets.h"

/// @file
/// @brief Demonstrates interactive color swatches, palettes, and RGB color picking.
/// @details The example creates a small manually rendered widget scene, translates raw mouse
/// events into widget selections, mirrors every selection in a preview and status label, and
/// exits automatically after a fixed timeout.

//=============================================================================
// Demo State
//=============================================================================

/// @brief Resources, widget references, and run-loop state owned by the demo.
typedef struct {
    /// @brief Native graphics window used for event polling and drawing.
    vgfx_window_t window;

    /// @brief Optional font selected from the platform fallback list.
    vg_font_t *font;

    // Root widget
    /// @brief Root container that owns the demo's widget tree.
    vg_widget_t *root;

    // Color widgets
    /// @brief Red selectable swatch.
    vg_colorswatch_t *swatch1;

    /// @brief Green selectable swatch.
    vg_colorswatch_t *swatch2;

    /// @brief Blue selectable swatch.
    vg_colorswatch_t *swatch3;

    /// @brief Larger swatch mirroring the latest selected color.
    vg_colorswatch_t *preview_swatch;

    /// @brief Standalone standard 16-color palette.
    vg_colorpalette_t *palette;

    /// @brief Composite RGB slider and palette picker.
    vg_colorpicker_t *picker;

    // Labels
    /// @brief Reserved title label reference.
    vg_label_t *title_label;

    /// @brief Heading for the standalone swatches.
    vg_label_t *swatch_label;

    /// @brief Heading for the standard palette.
    vg_label_t *palette_label;

    /// @brief Heading for the composite picker.
    vg_label_t *picker_label;

    /// @brief Dynamic description of the latest selection.
    vg_label_t *status_label;

    // Selected color
    /// @brief Latest selected packed ARGB color.
    uint32_t selected_color;

    // Running flag
    /// @brief True while the main loop should continue.
    bool running;

    // Timer
    /// @brief Wall-clock time at which the demo began.
    time_t start_time;

    /// @brief Maximum run time before automatic shutdown.
    int timeout_seconds;
} demo_state_t;

static demo_state_t g_demo;

//=============================================================================
// Callbacks
//=============================================================================

/// @brief Apply a standalone swatch selection to the shared demo state.
/// @param swatch Widget that originated the callback.
/// @param color Selected packed ARGB color.
/// @param user_data Owning @ref demo_state_t.
static void on_swatch_select(vg_widget_t *swatch, uint32_t color, void *user_data) {
    (void)swatch;
    demo_state_t *demo = (demo_state_t *)user_data;
    demo->selected_color = color;

    // Update preview swatch
    if (demo->preview_swatch) {
        vg_colorswatch_set_color(demo->preview_swatch, color);
    }

    // Update status
    if (demo->status_label) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Selected: #%06X", color & 0x00FFFFFF);
        vg_label_set_text(demo->status_label, buf);
    }

    printf("Swatch selected: 0x%08X\n", color);
}

/// @brief Apply a palette-cell selection to the preview and status text.
/// @param palette Palette widget that originated the callback.
/// @param color Selected packed ARGB color.
/// @param index Zero-based selected palette index.
/// @param user_data Owning @ref demo_state_t.
static void on_palette_select(vg_widget_t *palette, uint32_t color, int index, void *user_data) {
    (void)palette;
    demo_state_t *demo = (demo_state_t *)user_data;
    demo->selected_color = color;

    // Update preview swatch
    if (demo->preview_swatch) {
        vg_colorswatch_set_color(demo->preview_swatch, color);
    }

    // Update status
    if (demo->status_label) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Palette[%d]: #%06X", index, color & 0x00FFFFFF);
        vg_label_set_text(demo->status_label, buf);
    }

    printf("Palette color %d selected: 0x%08X\n", index, color);
}

/// @brief Apply a composite picker change and display its RGB components.
/// @param picker Picker widget that originated the callback.
/// @param color Newly selected packed ARGB color.
/// @param user_data Owning @ref demo_state_t.
static void on_picker_change(vg_widget_t *picker, uint32_t color, void *user_data) {
    (void)picker;
    demo_state_t *demo = (demo_state_t *)user_data;
    demo->selected_color = color;

    // Update preview swatch
    if (demo->preview_swatch) {
        vg_colorswatch_set_color(demo->preview_swatch, color);
    }

    // Update status
    if (demo->status_label) {
        uint8_t r = (color >> 16) & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = color & 0xFF;
        char buf[64];
        snprintf(buf, sizeof(buf), "RGB(%d, %d, %d)", r, g, b);
        vg_label_set_text(demo->status_label, buf);
    }

    printf("Picker changed: 0x%08X\n", color);
}

//=============================================================================
// Widget Drawing Helpers
//=============================================================================

/// @brief Fill a floating-point widget rectangle through the integer graphics API.
/// @param window Target graphics window.
/// @param x Left edge in window coordinates.
/// @param y Top edge in window coordinates.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param color Packed color whose alpha byte is ignored.
static void draw_rect(vgfx_window_t window, float x, float y, float w, float h, uint32_t color) {
    uint32_t rgb = color & 0x00FFFFFF;
    vgfx_fill_rect(window, (int)x, (int)y, (int)w, (int)h, rgb);
}

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
// Custom Widget Rendering
//=============================================================================

/// @brief Render one visible label at its computed screen position.
/// @param window Target graphics window.
/// @param label Label to render; null or hidden labels are ignored.
static void render_label(vgfx_window_t window, vg_label_t *label) {
    if (!label || !label->base.visible)
        return;

    float sx, sy;
    vg_widget_get_screen_bounds(&label->base, &sx, &sy, NULL, NULL);

    vg_theme_t *theme = vg_theme_get_current();
    uint32_t text_color = label->text_color ? label->text_color : theme->colors.fg_primary;

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

/// @brief Render a color swatch, transparency checkerboard, and selection border.
/// @param window Target graphics window.
/// @param swatch Swatch to render; null or hidden swatches are ignored.
static void render_colorswatch(vgfx_window_t window, vg_colorswatch_t *swatch) {
    if (!swatch || !swatch->base.visible)
        return;

    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&swatch->base, &sx, &sy, &sw, &sh);

    // Draw checkerboard for transparency (optional, simplified)
    uint8_t alpha = (swatch->color >> 24) & 0xFF;
    if (alpha < 255) {
        // Draw simple checkerboard
        int check_size = 4;
        for (int cy = 0; cy < (int)sh; cy += check_size) {
            for (int cx = 0; cx < (int)sw; cx += check_size) {
                uint32_t c = ((cx / check_size + cy / check_size) % 2) ? 0xCCCCCC : 0x999999;
                int cw = (cx + check_size > (int)sw) ? (int)sw - cx : check_size;
                int ch = (cy + check_size > (int)sh) ? (int)sh - cy : check_size;
                vgfx_fill_rect(window, (int)sx + cx, (int)sy + cy, cw, ch, c);
            }
        }
    }

    // Draw color
    draw_rect(window, sx, sy, sw, sh, swatch->color);

    // Draw border
    uint32_t border = swatch->selected ? swatch->selected_border : swatch->border_color;
    if (swatch->base.state & VG_STATE_HOVERED) {
        border = swatch->selected_border;
    }
    draw_rect_outline(window, sx, sy, sw, sh, border);

    // Draw selection indicator (inner white border when selected)
    if (swatch->selected) {
        draw_rect_outline(window, sx + 2, sy + 2, sw - 4, sh - 4, 0xFFFFFF);
    }
}

/// @brief Render every cell and the active selection of a color palette.
/// @param window Target graphics window.
/// @param palette Palette to render; invalid or hidden palettes are ignored.
static void render_colorpalette(vgfx_window_t window, vg_colorpalette_t *palette) {
    if (!palette || !palette->base.visible || !palette->colors)
        return;

    float sx, sy;
    vg_widget_get_screen_bounds(&palette->base, &sx, &sy, NULL, NULL);

    vg_theme_t *theme = vg_theme_get_current();

    for (int i = 0; i < palette->color_count; i++) {
        int col = i % palette->columns;
        int row = i / palette->columns;

        float swatch_x = sx + col * (palette->swatch_size + palette->gap);
        float swatch_y = sy + row * (palette->swatch_size + palette->gap);

        // Draw color
        draw_rect(window,
                  swatch_x,
                  swatch_y,
                  palette->swatch_size,
                  palette->swatch_size,
                  palette->colors[i]);

        // Draw border
        uint32_t border =
            (i == palette->selected_index) ? palette->selected_border : palette->border_color;
        draw_rect_outline(
            window, swatch_x, swatch_y, palette->swatch_size, palette->swatch_size, border);

        // Selection indicator
        if (i == palette->selected_index) {
            draw_rect_outline(window,
                              swatch_x + 1,
                              swatch_y + 1,
                              palette->swatch_size - 2,
                              palette->swatch_size - 2,
                              theme->colors.fg_primary);
        }
    }
}

/// @brief Render a labeled slider with its track, fill, thumb, and numeric value.
/// @param window Target graphics window.
/// @param slider Slider state to render.
/// @param label Optional short label displayed before the track.
/// @param font Optional font used for the label and value.
/// @param fill_color Track fill color representing the slider channel.
static void render_slider(vgfx_window_t window,
                          vg_slider_t *slider,
                          const char *label,
                          vg_font_t *font,
                          uint32_t fill_color) {
    if (!slider || !slider->base.visible)
        return;

    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&slider->base, &sx, &sy, &sw, &sh);

    vg_theme_t *theme = vg_theme_get_current();

    // Draw label
    if (font && label) {
        vg_font_draw_text(
            window, font, 12.0f, sx - 20, sy + sh / 2 + 4, label, theme->colors.fg_primary);
    }

    // Draw track
    float track_y = sy + sh / 2 - 2;
    draw_rect(window, sx, track_y, sw, 4, theme->colors.bg_tertiary);

    // Calculate fill width
    float range = slider->max_value - slider->min_value;
    float pct = (range > 0) ? (slider->value - slider->min_value) / range : 0;
    float fill_width = sw * pct;

    // Draw fill
    draw_rect(window, sx, track_y, fill_width, 4, fill_color);

    // Draw thumb
    float thumb_x = sx + fill_width - slider->thumb_size / 2;
    float thumb_y = sy + sh / 2 - slider->thumb_size / 2;
    draw_rect(
        window, thumb_x, thumb_y, slider->thumb_size, slider->thumb_size, theme->colors.fg_primary);
    draw_rect_outline(window,
                      thumb_x,
                      thumb_y,
                      slider->thumb_size,
                      slider->thumb_size,
                      theme->colors.border_primary);

    // Draw value
    if (font) {
        char val[8];
        snprintf(val, sizeof(val), "%d", (int)slider->value);
        vg_font_draw_text(
            window, font, 12.0f, sx + sw + 8, sy + sh / 2 + 4, val, theme->colors.fg_secondary);
    }
}

/// @brief Render the composite picker background and all visible child controls.
/// @param window Target graphics window.
/// @param picker Picker to render; null or hidden pickers are ignored.
/// @param font Optional font used by the child slider renderers.
static void render_colorpicker(vgfx_window_t window, vg_colorpicker_t *picker, vg_font_t *font) {
    if (!picker || !picker->base.visible)
        return;

    float sx, sy, sw, sh;
    vg_widget_get_screen_bounds(&picker->base, &sx, &sy, &sw, &sh);

    vg_theme_t *theme = vg_theme_get_current();

    // Draw background
    draw_rect(window, sx, sy, sw, sh, theme->colors.bg_secondary);
    draw_rect_outline(window, sx, sy, sw, sh, theme->colors.border_primary);

    // Draw preview swatch
    if (picker->preview) {
        render_colorswatch(window, picker->preview);
    }

    // Draw RGB sliders with color-coded fills
    if (picker->slider_r) {
        render_slider(window, picker->slider_r, "R", font, 0xFF0000);
    }
    if (picker->slider_g) {
        render_slider(window, picker->slider_g, "G", font, 0x00FF00);
    }
    if (picker->slider_b) {
        render_slider(window, picker->slider_b, "B", font, 0x0000FF);
    }
    if (picker->slider_a && picker->show_alpha) {
        render_slider(window, picker->slider_a, "A", font, 0x888888);
    }

    // Draw palette if shown
    if (picker->palette && picker->show_palette) {
        render_colorpalette(window, picker->palette);
    }
}

//=============================================================================
// Main Render Function
//=============================================================================

/// @brief Draw one complete frame of the color-widget demonstration.
/// @param demo Initialized demo state containing the current widget values.
static void render_demo(demo_state_t *demo) {
    vgfx_window_t window = demo->window;
    vg_theme_t *theme = vg_theme_get_current();

    // Clear background
    vgfx_cls(window, theme->colors.bg_primary & 0x00FFFFFF);

    // Draw title
    if (demo->font) {
        vg_font_draw_text(
            window, demo->font, 24.0f, 20, 35, "Color Widgets Demo", theme->colors.fg_primary);
    }

    // Draw section labels
    render_label(window, demo->swatch_label);
    render_label(window, demo->palette_label);
    render_label(window, demo->picker_label);
    render_label(window, demo->status_label);

    // Draw individual color swatches
    render_colorswatch(window, demo->swatch1);
    render_colorswatch(window, demo->swatch2);
    render_colorswatch(window, demo->swatch3);

    // Draw preview swatch (larger, shows selected color)
    if (demo->font) {
        vg_font_draw_text(
            window, demo->font, 12.0f, 220, 75, "Selected:", theme->colors.fg_secondary);
    }
    render_colorswatch(window, demo->preview_swatch);

    // Draw color palette
    render_colorpalette(window, demo->palette);

    // Draw color picker
    render_colorpicker(window, demo->picker, demo->font);

    // Draw countdown timer
    time_t now = time(NULL);
    int remaining = demo->timeout_seconds - (int)(now - demo->start_time);
    if (remaining < 0)
        remaining = 0;

    if (demo->font) {
        char timer_buf[32];
        snprintf(timer_buf, sizeof(timer_buf), "Closing in %d seconds", remaining);
        vg_font_draw_text(
            window, demo->font, 14.0f, 500, 35, timer_buf, theme->colors.fg_secondary);
    }
}

//=============================================================================
// Event Handling
//=============================================================================

/// @brief Test whether an integer point lies inside a half-open floating rectangle.
/// @param x Point x coordinate.
/// @param y Point y coordinate.
/// @param rx Rectangle left edge.
/// @param ry Rectangle top edge.
/// @param rw Rectangle width.
/// @param rh Rectangle height.
/// @return True when the point lies within the rectangle's left/top-inclusive bounds.
static bool point_in_rect(int x, int y, float rx, float ry, float rw, float rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

/// @brief Drain window events and update hover, selection, slider, and lifetime state.
/// @details Close and Escape events stop the demo. Mouse motion updates swatch hover flags,
/// while mouse presses select standalone or embedded palette cells and reposition RGB sliders.
/// The timeout is checked after the current event batch.
/// @param demo Mutable initialized demo state.
static void handle_events(demo_state_t *demo) {
    vgfx_event_t pe;

    while (vgfx_poll_event(demo->window, &pe)) {
        if (pe.type == VGFX_EVENT_CLOSE) {
            demo->running = false;
            return;
        }

        if (pe.type == VGFX_EVENT_KEY_DOWN && pe.data.key.key == VGFX_KEY_ESCAPE) {
            demo->running = false;
            return;
        }

        // Mouse handling
        if (pe.type == VGFX_EVENT_MOUSE_MOVE || pe.type == VGFX_EVENT_MOUSE_DOWN) {
            int32_t mx, my;
            vgfx_mouse_pos(demo->window, &mx, &my);

            // Clear hover states
            demo->swatch1->base.state &= ~VG_STATE_HOVERED;
            demo->swatch2->base.state &= ~VG_STATE_HOVERED;
            demo->swatch3->base.state &= ~VG_STATE_HOVERED;

            // Check swatch hover
            float sx, sy, sw, sh;

            vg_widget_get_screen_bounds(&demo->swatch1->base, &sx, &sy, &sw, &sh);
            if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                demo->swatch1->base.state |= VG_STATE_HOVERED;
            }

            vg_widget_get_screen_bounds(&demo->swatch2->base, &sx, &sy, &sw, &sh);
            if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                demo->swatch2->base.state |= VG_STATE_HOVERED;
            }

            vg_widget_get_screen_bounds(&demo->swatch3->base, &sx, &sy, &sw, &sh);
            if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                demo->swatch3->base.state |= VG_STATE_HOVERED;
            }
        }

        if (pe.type == VGFX_EVENT_MOUSE_DOWN) {
            int32_t mx, my;
            vgfx_mouse_pos(demo->window, &mx, &my);

            float sx, sy, sw, sh;

            // Check swatch clicks
            vg_widget_get_screen_bounds(&demo->swatch1->base, &sx, &sy, &sw, &sh);
            if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                on_swatch_select(&demo->swatch1->base, demo->swatch1->color, demo);
            }

            vg_widget_get_screen_bounds(&demo->swatch2->base, &sx, &sy, &sw, &sh);
            if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                on_swatch_select(&demo->swatch2->base, demo->swatch2->color, demo);
            }

            vg_widget_get_screen_bounds(&demo->swatch3->base, &sx, &sy, &sw, &sh);
            if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                on_swatch_select(&demo->swatch3->base, demo->swatch3->color, demo);
            }

            // Check palette click
            if (demo->palette && demo->palette->colors) {
                vg_widget_get_screen_bounds(&demo->palette->base, &sx, &sy, &sw, &sh);
                if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                    // Determine which swatch was clicked
                    float local_x = mx - sx;
                    float local_y = my - sy;
                    float cell_size = demo->palette->swatch_size + demo->palette->gap;
                    int col = (int)(local_x / cell_size);
                    int row = (int)(local_y / cell_size);

                    // Check we're within a swatch, not a gap
                    float cell_x = local_x - col * cell_size;
                    float cell_y = local_y - row * cell_size;

                    if (cell_x <= demo->palette->swatch_size &&
                        cell_y <= demo->palette->swatch_size && col < demo->palette->columns) {
                        int index = row * demo->palette->columns + col;
                        if (index < demo->palette->color_count) {
                            demo->palette->selected_index = index;
                            on_palette_select(
                                &demo->palette->base, demo->palette->colors[index], index, demo);
                        }
                    }
                }
            }

            // Check picker slider drags (simplified - handle drag start)
            if (demo->picker) {
                // R slider
                if (demo->picker->slider_r) {
                    vg_widget_get_screen_bounds(&demo->picker->slider_r->base, &sx, &sy, &sw, &sh);
                    if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                        float pct = (mx - sx) / sw;
                        if (pct < 0)
                            pct = 0;
                        if (pct > 1)
                            pct = 1;
                        float new_val = demo->picker->slider_r->min_value +
                                        pct * (demo->picker->slider_r->max_value -
                                               demo->picker->slider_r->min_value);
                        vg_slider_set_value(demo->picker->slider_r, new_val);
                        demo->picker->r = (uint8_t)new_val;
                        demo->picker->color = (0xFF << 24) | (demo->picker->r << 16) |
                                              (demo->picker->g << 8) | demo->picker->b;
                        on_picker_change(&demo->picker->base, demo->picker->color, demo);
                    }
                }

                // G slider
                if (demo->picker->slider_g) {
                    vg_widget_get_screen_bounds(&demo->picker->slider_g->base, &sx, &sy, &sw, &sh);
                    if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                        float pct = (mx - sx) / sw;
                        if (pct < 0)
                            pct = 0;
                        if (pct > 1)
                            pct = 1;
                        float new_val = demo->picker->slider_g->min_value +
                                        pct * (demo->picker->slider_g->max_value -
                                               demo->picker->slider_g->min_value);
                        vg_slider_set_value(demo->picker->slider_g, new_val);
                        demo->picker->g = (uint8_t)new_val;
                        demo->picker->color = (0xFF << 24) | (demo->picker->r << 16) |
                                              (demo->picker->g << 8) | demo->picker->b;
                        on_picker_change(&demo->picker->base, demo->picker->color, demo);
                    }
                }

                // B slider
                if (demo->picker->slider_b) {
                    vg_widget_get_screen_bounds(&demo->picker->slider_b->base, &sx, &sy, &sw, &sh);
                    if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                        float pct = (mx - sx) / sw;
                        if (pct < 0)
                            pct = 0;
                        if (pct > 1)
                            pct = 1;
                        float new_val = demo->picker->slider_b->min_value +
                                        pct * (demo->picker->slider_b->max_value -
                                               demo->picker->slider_b->min_value);
                        vg_slider_set_value(demo->picker->slider_b, new_val);
                        demo->picker->b = (uint8_t)new_val;
                        demo->picker->color = (0xFF << 24) | (demo->picker->r << 16) |
                                              (demo->picker->g << 8) | demo->picker->b;
                        on_picker_change(&demo->picker->base, demo->picker->color, demo);
                    }
                }

                // Picker's internal palette
                if (demo->picker->palette && demo->picker->show_palette) {
                    vg_widget_get_screen_bounds(&demo->picker->palette->base, &sx, &sy, &sw, &sh);
                    if (point_in_rect(mx, my, sx, sy, sw, sh)) {
                        float local_x = mx - sx;
                        float local_y = my - sy;
                        float cell_size =
                            demo->picker->palette->swatch_size + demo->picker->palette->gap;
                        int col = (int)(local_x / cell_size);
                        int row = (int)(local_y / cell_size);

                        if (col < demo->picker->palette->columns) {
                            int index = row * demo->picker->palette->columns + col;
                            if (index < demo->picker->palette->color_count) {
                                uint32_t color = demo->picker->palette->colors[index];
                                vg_colorpicker_set_color(demo->picker, color);
                                on_picker_change(&demo->picker->base, color, demo);
                            }
                        }
                    }
                }
            }
        }
    }

    // Check timeout
    time_t now = time(NULL);
    if (now - demo->start_time >= demo->timeout_seconds) {
        demo->running = false;
    }
}

//=============================================================================
// Initialization
//=============================================================================

/// @brief Create the demo window, optional font, widget tree, and initial color state.
/// @details Font loading tries a cross-platform fallback list. Missing font data is nonfatal,
/// while failure to create the window or root container aborts initialization.
/// @param demo Zero-initialized destination state.
/// @param timeout_seconds Number of seconds before automatic shutdown.
/// @return True when the resources required by the main loop are ready.
static bool init_demo(demo_state_t *demo, int timeout_seconds) {
    demo->timeout_seconds = timeout_seconds;
    demo->start_time = time(NULL);

    // Create window
    vgfx_window_params_t params = vgfx_window_params_default();
    params.width = 700;
    params.height = 500;
    params.title = "Color Widgets Demo";
    params.resizable = 1;
    params.fps = 60;

    demo->window = vgfx_create_window(&params);
    if (!demo->window) {
        fprintf(stderr, "Failed to create window: %s\n", vgfx_get_last_error());
        return false;
    }

    // Load font
    const char *font_paths[] = {"/System/Library/Fonts/SFNSMono.ttf",
                                "/System/Library/Fonts/Menlo.ttc",
                                "/System/Library/Fonts/Monaco.ttf",
                                "/System/Library/Fonts/Supplemental/Arial.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                "/usr/share/fonts/TTF/DejaVuSans.ttf",
                                "C:\\Windows\\Fonts\\arial.ttf",
                                NULL};

    for (int i = 0; font_paths[i] != NULL; i++) {
        demo->font = vg_font_load_file(font_paths[i]);
        if (demo->font) {
            printf("Loaded font: %s\n", font_paths[i]);
            break;
        }
    }

    if (!demo->font) {
        fprintf(stderr, "Warning: No font loaded. Text will not display.\n");
    }

    // Set dark theme
    vg_theme_set_current(vg_theme_dark());
    vg_theme_t *theme = vg_theme_get_current();

    // Create root container
    demo->root = vg_widget_create(VG_WIDGET_CONTAINER);
    if (!demo->root) {
        fprintf(stderr, "Failed to create root widget\n");
        return false;
    }

    demo->selected_color = 0xFFFF0000; // Red

    // Create section labels
    demo->swatch_label = vg_label_create(demo->root, "Color Swatches:");
    if (demo->swatch_label && demo->font) {
        vg_label_set_font(demo->swatch_label, demo->font, 14.0f);
        demo->swatch_label->base.x = 20;
        demo->swatch_label->base.y = 50;
        demo->swatch_label->base.width = 150;
        demo->swatch_label->base.height = 20;
    }

    // Create individual swatches
    demo->swatch1 = vg_colorswatch_create(demo->root, 0xFFFF0000); // Red
    if (demo->swatch1) {
        vg_colorswatch_set_size(demo->swatch1, 40.0f);
        demo->swatch1->base.x = 20;
        demo->swatch1->base.y = 75;
        demo->swatch1->base.width = 40;
        demo->swatch1->base.height = 40;
        vg_colorswatch_set_on_select(demo->swatch1, on_swatch_select, demo);
    }

    demo->swatch2 = vg_colorswatch_create(demo->root, 0xFF00FF00); // Green
    if (demo->swatch2) {
        vg_colorswatch_set_size(demo->swatch2, 40.0f);
        demo->swatch2->base.x = 70;
        demo->swatch2->base.y = 75;
        demo->swatch2->base.width = 40;
        demo->swatch2->base.height = 40;
        vg_colorswatch_set_on_select(demo->swatch2, on_swatch_select, demo);
    }

    demo->swatch3 = vg_colorswatch_create(demo->root, 0xFF0000FF); // Blue
    if (demo->swatch3) {
        vg_colorswatch_set_size(demo->swatch3, 40.0f);
        demo->swatch3->base.x = 120;
        demo->swatch3->base.y = 75;
        demo->swatch3->base.width = 40;
        demo->swatch3->base.height = 40;
        vg_colorswatch_set_on_select(demo->swatch3, on_swatch_select, demo);
    }

    // Preview swatch (larger)
    demo->preview_swatch = vg_colorswatch_create(demo->root, demo->selected_color);
    if (demo->preview_swatch) {
        vg_colorswatch_set_size(demo->preview_swatch, 50.0f);
        demo->preview_swatch->base.x = 290;
        demo->preview_swatch->base.y = 65;
        demo->preview_swatch->base.width = 50;
        demo->preview_swatch->base.height = 50;
    }

    // Palette section
    demo->palette_label = vg_label_create(demo->root, "Color Palette (16 colors):");
    if (demo->palette_label && demo->font) {
        vg_label_set_font(demo->palette_label, demo->font, 14.0f);
        demo->palette_label->base.x = 20;
        demo->palette_label->base.y = 130;
        demo->palette_label->base.width = 200;
        demo->palette_label->base.height = 20;
    }

    demo->palette = vg_colorpalette_create(demo->root);
    if (demo->palette) {
        vg_colorpalette_load_standard_16(demo->palette);
        vg_colorpalette_set_swatch_size(demo->palette, 24.0f);
        demo->palette->gap = 4.0f;
        demo->palette->selected_border = theme->colors.accent_primary;
        demo->palette->base.x = 20;
        demo->palette->base.y = 155;
        demo->palette->base.width = 8 * (24 + 4);
        demo->palette->base.height = 2 * (24 + 4);
        vg_colorpalette_set_on_select(demo->palette, on_palette_select, demo);
    }

    // Color picker section
    demo->picker_label = vg_label_create(demo->root, "Color Picker (RGB sliders + palette):");
    if (demo->picker_label && demo->font) {
        vg_label_set_font(demo->picker_label, demo->font, 14.0f);
        demo->picker_label->base.x = 20;
        demo->picker_label->base.y = 220;
        demo->picker_label->base.width = 300;
        demo->picker_label->base.height = 20;
    }

    demo->picker = vg_colorpicker_create(demo->root);
    if (demo->picker) {
        demo->picker->base.x = 20;
        demo->picker->base.y = 245;
        demo->picker->base.width = 350;
        demo->picker->base.height = 200;

        // Set initial color
        vg_colorpicker_set_color(demo->picker, 0xFF8844AA);

        // Position child widgets
        if (demo->picker->preview) {
            demo->picker->preview->base.x = demo->picker->base.x + 280;
            demo->picker->preview->base.y = demo->picker->base.y + 10;
            demo->picker->preview->base.width = 50;
            demo->picker->preview->base.height = 50;
        }

        // Position sliders
        float slider_x = demo->picker->base.x + 30;
        float slider_y = demo->picker->base.y + 15;
        float slider_w = 200;
        float slider_h = 20;
        float slider_gap = 30;

        if (demo->picker->slider_r) {
            demo->picker->slider_r->base.x = slider_x;
            demo->picker->slider_r->base.y = slider_y;
            demo->picker->slider_r->base.width = slider_w;
            demo->picker->slider_r->base.height = slider_h;
            demo->picker->slider_r->thumb_size = 12;
        }
        if (demo->picker->slider_g) {
            demo->picker->slider_g->base.x = slider_x;
            demo->picker->slider_g->base.y = slider_y + slider_gap;
            demo->picker->slider_g->base.width = slider_w;
            demo->picker->slider_g->base.height = slider_h;
            demo->picker->slider_g->thumb_size = 12;
        }
        if (demo->picker->slider_b) {
            demo->picker->slider_b->base.x = slider_x;
            demo->picker->slider_b->base.y = slider_y + slider_gap * 2;
            demo->picker->slider_b->base.width = slider_w;
            demo->picker->slider_b->base.height = slider_h;
            demo->picker->slider_b->thumb_size = 12;
        }

        // Position picker's palette
        if (demo->picker->palette) {
            demo->picker->palette->base.x = demo->picker->base.x + 10;
            demo->picker->palette->base.y = demo->picker->base.y + 120;
            demo->picker->palette->base.width = 8 * (20 + 2);
            demo->picker->palette->base.height = 2 * (20 + 2);
        }

        vg_colorpicker_set_on_change(demo->picker, on_picker_change, demo);
    }

    // Status label
    demo->status_label = vg_label_create(demo->root, "Click a color to select it");
    if (demo->status_label && demo->font) {
        vg_label_set_font(demo->status_label, demo->font, 14.0f);
        demo->status_label->base.x = 20;
        demo->status_label->base.y = 460;
        demo->status_label->base.width = 400;
        demo->status_label->base.height = 24;
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

/// @brief Run the timed interactive color-widget demonstration.
/// @param argc Process argument count; currently ignored.
/// @param argv Process argument vector; currently ignored.
/// @return Zero after normal completion, or one when initialization fails.
int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    printf("Color Widgets Demo\n");
    printf("==================\n");
    printf("Demonstrating: ColorSwatch, ColorPalette, ColorPicker\n");
    printf("Window will close automatically after 60 seconds\n");
    printf("Press ESC to exit early\n\n");
    fflush(stdout);

    memset(&g_demo, 0, sizeof(g_demo));

    if (!init_demo(&g_demo, 60)) { // 60 second timeout
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

    printf("Demo completed.\n");
    return 0;
}
