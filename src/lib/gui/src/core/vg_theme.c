//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/core/vg_theme.c
// Purpose: Theme system implementation — built-in dark/light themes,
//          current-theme state, custom theme lifecycle, and colour utilities.
// Key invariants:
//   - Built-in themes are statically allocated; never pass them to vg_theme_destroy.
//   - g_current_theme defaults to the dark theme on first access.
// Ownership/Lifetime:
//   - Custom themes created by vg_theme_create own their name string.
// Links: lib/gui/include/vg_theme.h,
//        lib/gui/include/vg_font.h
//
//===----------------------------------------------------------------------===//
#include "../../include/vg_theme.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements built-in and custom GUI themes plus packed-color transformations.
/// @details Dark and light themes have static lifetime. The current-theme pointer defaults
/// lazily to dark, custom themes shallow-copy borrowed font handles while owning a duplicated
/// name, and color helpers blend each ARGB channel after clamping invalid factors.

//=============================================================================
// Current Theme
//=============================================================================

static vg_theme_t *g_current_theme = NULL;

//=============================================================================
// Built-in Dark Theme
//=============================================================================

static vg_theme_t g_dark_theme = {
    .name = "Dark",
    .colors =
        {
            // Zanna brand palette: charcoal-green field, green primary accent,
            // teal informational accent, steel secondary text.
            // Background colors (0x00RRGGBB format for vgfx compatibility)
            .bg_primary = 0x0D1A1C,
            .bg_secondary = 0x132325,
            .bg_tertiary = 0x1A2E30,
            .bg_hover = 0x233B3C,
            .bg_active = 0x1E4A43,
            .bg_selected = 0x1C443F,
            .bg_disabled = 0x172629,

            // Foreground colors
            .fg_primary = 0xE9F1EF,
            .fg_secondary = 0xAFBEBC,
            .fg_tertiary = 0x87999B,
            .fg_disabled = 0x60716F,
            .fg_placeholder = 0x7E908D,
            .fg_link = 0x4FD4CE,

            // Accent colors
            .accent_primary = 0x8CC63F,
            .accent_secondary = 0x5FA832,
            .accent_danger = 0xFF6B6B,
            .accent_warning = 0xE8B04C,
            .accent_success = 0x71C784,
            .accent_info = 0x2BC8C4,

            // Border colors
            .border_primary = 0x2B4241,
            .border_secondary = 0x203434,
            .border_focus = 0x8CC63F,

            // Syntax highlighting
            .syntax_keyword = 0x9BD24A,  // Brand green
            .syntax_type = 0x45D0BF,     // Brand teal
            .syntax_function = 0xE5CA7E, // Warm gold
            .syntax_variable = 0xA8D8DC, // Light steel-teal
            .syntax_string = 0xD9A176,   // Amber-orange
            .syntax_number = 0xBDD3A5,   // Pale sage
            .syntax_comment = 0x7B9A88,  // Muted green-grey
            .syntax_operator = 0xD8E2DF, // Soft steel white
            .syntax_error = 0xF25C5C,    // Red
        },
    .typography =
        {
            .font_regular = NULL,
            .font_bold = NULL,
            .font_mono = NULL,
            .size_small = 11.0f,
            .size_normal = 13.5f,
            .size_large = 17.0f,
            .size_heading = 21.0f,
            .line_height = 1.4f,
        },
    .spacing =
        {
            .xs = 2.0f,
            .sm = 4.0f,
            .md = 8.0f,
            .lg = 16.0f,
            .xl = 24.0f,
        },
    .button =
        {
            .height = 28.0f,
            .padding_h = 16.0f,
            .border_radius = 4.0f,
            .border_width = 1.0f,
        },
    .input =
        {
            .height = 28.0f,
            .padding_h = 10.0f,
            .border_radius = 4.0f,
            .border_width = 1.0f,
        },
    .scrollbar =
        {
            .width = 12.0f,
            .min_thumb_size = 32.0f,
            .border_radius = 6.0f,
        },
    // Refined Depth visual tokens (dark): subtle gradients + real soft shadows,
    // tuned so depth reads clearly without lowering text/background contrast.
    .radius = {.none = 0.0f, .sm = 3.0f, .md = 6.0f, .lg = 10.0f, .xl = 16.0f, .pill = 9999.0f},
    .elevation =
        {
            .shadow_rgb = 0x000000,
            .level0 = {.blur = 0.0f, .dx = 0, .dy = 0, .alpha = 0},
            .level1 = {.blur = 4.0f, .dx = 0, .dy = 1, .alpha = 40},
            .level2 = {.blur = 10.0f, .dx = 0, .dy = 4, .alpha = 64},
            .level3 = {.blur = 20.0f, .dx = 0, .dy = 8, .alpha = 90},
        },
    .gradient = {.enabled = true, .strength = 0.06f},
    .focus = {.glow_color = 0x8CC63F, .glow_width = 2.0f, .glow_alpha = 120},
    .motion = {.enabled = true, .hover_ms = 90.0f, .press_ms = 60.0f, .focus_ms = 120.0f},
};

//=============================================================================
// Built-in Light Theme
//=============================================================================

static vg_theme_t g_light_theme = {
    .name = "Light",
    .colors =
        {
            // Zanna brand palette on a light field: green-tinted neutrals,
            // deep-green primary accent, deep-teal informational accent.
            // Background colors (0x00RRGGBB format for vgfx compatibility)
            .bg_primary = 0xFAFCFA,
            .bg_secondary = 0xF0F5F1,
            .bg_tertiary = 0xE4EDE7,
            .bg_hover = 0xDCE8E0,
            .bg_active = 0xC8E8D0,
            .bg_selected = 0xCDE9D4,
            .bg_disabled = 0xF0F3F1,

            // Foreground colors
            .fg_primary = 0x16211D,
            .fg_secondary = 0x4F615A,
            .fg_tertiary = 0x6C7E77,
            .fg_disabled = 0x97A5A0,
            .fg_placeholder = 0x66776F,
            .fg_link = 0x0C7B77,

            // Accent colors
            .accent_primary = 0x4E8A22,
            .accent_secondary = 0x3D701B,
            .accent_danger = 0xD64545,
            .accent_warning = 0xA8731A,
            .accent_success = 0x2C8F5A,
            .accent_info = 0x0C7B77,

            // Border colors
            .border_primary = 0xC3D3CA,
            .border_secondary = 0xDCE6DF,
            .border_focus = 0x4E8A22,

            // Syntax highlighting
            .syntax_keyword = 0x2E7D0F,  // Deep green
            .syntax_type = 0x0F766E,     // Deep teal
            .syntax_function = 0x7A5D14, // Dark gold
            .syntax_variable = 0x175E73, // Steel-teal
            .syntax_string = 0xA34A2A,   // Rust
            .syntax_number = 0x0E7A52,   // Teal-green
            .syntax_comment = 0x5C7568,  // Grey-green
            .syntax_operator = 0x24312C, // Near-black green
            .syntax_error = 0xD5121E,    // Red
        },
    .typography =
        {
            .font_regular = NULL,
            .font_bold = NULL,
            .font_mono = NULL,
            .size_small = 11.0f,
            .size_normal = 13.5f,
            .size_large = 17.0f,
            .size_heading = 21.0f,
            .line_height = 1.4f,
        },
    .spacing =
        {
            .xs = 2.0f,
            .sm = 4.0f,
            .md = 8.0f,
            .lg = 16.0f,
            .xl = 24.0f,
        },
    .button =
        {
            .height = 28.0f,
            .padding_h = 16.0f,
            .border_radius = 4.0f,
            .border_width = 1.0f,
        },
    .input =
        {
            .height = 28.0f,
            .padding_h = 10.0f,
            .border_radius = 4.0f,
            .border_width = 1.0f,
        },
    .scrollbar =
        {
            .width = 12.0f,
            .min_thumb_size = 32.0f,
            .border_radius = 6.0f,
        },
    // Refined Depth visual tokens (light): softer, cooler shadows on bright bg.
    .radius = {.none = 0.0f, .sm = 3.0f, .md = 6.0f, .lg = 10.0f, .xl = 16.0f, .pill = 9999.0f},
    .elevation =
        {
            .shadow_rgb = 0x0A1A2F,
            .level0 = {.blur = 0.0f, .dx = 0, .dy = 0, .alpha = 0},
            .level1 = {.blur = 6.0f, .dx = 0, .dy = 1, .alpha = 28},
            .level2 = {.blur = 14.0f, .dx = 0, .dy = 4, .alpha = 40},
            .level3 = {.blur = 26.0f, .dx = 0, .dy = 10, .alpha = 56},
        },
    .gradient = {.enabled = true, .strength = 0.04f},
    .focus = {.glow_color = 0x4E8A22, .glow_width = 2.0f, .glow_alpha = 110},
    .motion = {.enabled = true, .hover_ms = 90.0f, .press_ms = 60.0f, .focus_ms = 120.0f},
};

//=============================================================================
// Theme API
//=============================================================================

/// @brief Returns the currently active theme, defaulting to the built-in dark theme on first call.
/// @copydoc vg_theme_get_current
vg_theme_t *vg_theme_get_current(void) {
    if (!g_current_theme) {
        g_current_theme = &g_dark_theme;
    }
    return g_current_theme;
}

/// @brief Sets @p theme as the active theme; falls back to the dark theme if @p theme is NULL.
/// @copydoc vg_theme_set_current
void vg_theme_set_current(vg_theme_t *theme) {
    g_current_theme = theme ? theme : &g_dark_theme;
}

/// @brief Returns a pointer to the statically-allocated built-in dark theme.
/// @copydoc vg_theme_dark
vg_theme_t *vg_theme_dark(void) {
    return &g_dark_theme;
}

/// @brief Returns a pointer to the statically-allocated built-in light theme.
/// @copydoc vg_theme_light
vg_theme_t *vg_theme_light(void) {
    return &g_light_theme;
}

/// @brief Heap-allocates a new theme copied from @p base (or the dark theme if NULL), with @p name
/// strdup'd as its name.
/// @copydoc vg_theme_create
vg_theme_t *vg_theme_create(const char *name, const vg_theme_t *base) {
    vg_theme_t *theme = malloc(sizeof(vg_theme_t));
    if (!theme)
        return NULL;

    if (base) {
        *theme = *base;
    } else {
        *theme = g_dark_theme;
    }

    theme->name = vg_strdup(name ? name : "Custom");
    if (!theme->name) {
        free(theme);
        return NULL;
    }

    return theme;
}

/// @brief Frees a custom theme's name string and the theme struct; no-ops on NULL or built-in
/// themes.
/// @copydoc vg_theme_destroy
void vg_theme_destroy(vg_theme_t *theme) {
    if (!theme)
        return;

    // Don't destroy built-in themes
    if (theme == &g_dark_theme || theme == &g_light_theme)
        return;

    if (g_current_theme == theme)
        g_current_theme = &g_dark_theme;

    free(theme->name);

    free(theme);
}

//=============================================================================
// Color Helpers
//=============================================================================

/// @brief Linearly interpolates between colors @p c1 and @p c2 by factor @p t in [0,1], blending
/// all four channels.
/// @copydoc vg_color_blend
uint32_t vg_color_blend(uint32_t c1, uint32_t c2, float t) {
    if (!isfinite(t) || t <= 0.0f)
        return c1;
    if (t >= 1.0f)
        return c2;

    uint8_t r1 = vg_color_r(c1), g1 = vg_color_g(c1), b1 = vg_color_b(c1), a1 = vg_color_a(c1);
    uint8_t r2 = vg_color_r(c2), g2 = vg_color_g(c2), b2 = vg_color_b(c2), a2 = vg_color_a(c2);

    float inv_t = 1.0f - t;
    uint8_t r = (uint8_t)(r1 * inv_t + r2 * t);
    uint8_t g = (uint8_t)(g1 * inv_t + g2 * t);
    uint8_t b = (uint8_t)(b1 * inv_t + b2 * t);
    uint8_t a = (uint8_t)(a1 * inv_t + a2 * t);

    return vg_rgba(r, g, b, a);
}

/// @brief Blends @p color toward white by @p amount (0 = no change, 1 = fully white).
/// @copydoc vg_color_lighten
uint32_t vg_color_lighten(uint32_t color, float amount) {
    return vg_color_blend(color, 0xFFFFFFFF, amount);
}

/// @brief Blends @p color toward black by @p amount (0 = no change, 1 = fully black).
/// @copydoc vg_color_darken
uint32_t vg_color_darken(uint32_t color, float amount) {
    return vg_color_blend(color, 0xFF000000, amount);
}
