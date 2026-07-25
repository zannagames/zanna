//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_theme.h
// Purpose: Theming system for consistent widget appearance — colour schemes,
//          typography presets, spacing, and per-widget-class style overrides.
// Key invariants:
//   - There is always a valid current theme; defaults to the dark theme.
//   - Built-in themes (dark/light) are statically allocated and must NOT be freed.
//   - Custom themes created with vg_theme_create must be freed with vg_theme_destroy.
// Ownership/Lifetime:
//   - Font pointers in vg_typography_t are not owned by the theme.
//   - The name string is owned by the theme for custom themes.
// Links: lib/gui/src/core/vg_theme.c,
//        lib/gui/include/vg_font.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vg_font.h"
#include "vg_string.h"
#include <stdbool.h>
#include <stdint.h>

/// @file
/// @brief Declares global GUI themes, visual design tokens, and packed-color helpers.
/// @details Themes combine semantic colors, borrowed fonts, typography, spacing, widget
/// dimensions, radii, elevation, gradients, focus treatment, and motion timing. Built-in themes
/// have static lifetime, while custom themes are copied and caller-owned.

#ifdef __cplusplus
extern "C" {
#endif

//=============================================================================
// Color Scheme
//=============================================================================

/// @brief Complete colour palette for the GUI, covering backgrounds, text,
///        accents, borders, and syntax-highlighting tokens.
///
/// @details All colour values are 24-bit RGB (0x00RRGGBB). The scheme is
///          divided into logical groups so that widget code can pick the
///          semantically correct colour without hard-coding values.
typedef struct vg_color_scheme {
    // Background colors
    uint32_t bg_primary;   ///< Primary background (e.g. main editor area).
    uint32_t bg_secondary; ///< Secondary background (e.g. sidebars).
    uint32_t bg_tertiary;  ///< Tertiary background (e.g. nested panels).
    uint32_t bg_hover;     ///< Background used when the mouse hovers an item.
    uint32_t bg_active;    ///< Background used when an item is actively pressed.
    uint32_t bg_selected;  ///< Background for selected items (list rows, tabs).
    uint32_t bg_disabled;  ///< Background for disabled controls.

    // Foreground (text) colors
    uint32_t fg_primary;     ///< Primary text colour.
    uint32_t fg_secondary;   ///< Secondary/muted text colour.
    uint32_t fg_tertiary;    ///< Tertiary/hint text colour.
    uint32_t fg_disabled;    ///< Text colour for disabled controls.
    uint32_t fg_placeholder; ///< Placeholder text colour in input fields.
    uint32_t fg_link;        ///< Hyperlink text colour.

    // Accent colors
    uint32_t accent_primary;   ///< Primary accent (e.g. focused border, active tab indicator).
    uint32_t accent_secondary; ///< Secondary accent colour.
    uint32_t accent_danger;    ///< Danger / destructive-action accent (e.g. delete buttons).
    uint32_t accent_warning;   ///< Warning accent colour.
    uint32_t accent_success;   ///< Success / confirmation accent colour.
    uint32_t accent_info;      ///< Informational accent colour.

    // Border colors
    uint32_t border_primary;   ///< Default border colour for widgets.
    uint32_t border_secondary; ///< Lighter/secondary border colour.
    uint32_t border_focus;     ///< Border colour when a widget has keyboard focus.

    // Syntax highlighting (for code editor)
    uint32_t syntax_keyword;  ///< Language keyword colour (e.g. if, while, return).
    uint32_t syntax_type;     ///< Type name colour (e.g. int, float, struct).
    uint32_t syntax_function; ///< Function/method name colour.
    uint32_t syntax_variable; ///< Variable name colour.
    uint32_t syntax_string;   ///< String literal colour.
    uint32_t syntax_number;   ///< Numeric literal colour.
    uint32_t syntax_comment;  ///< Comment colour.
    uint32_t syntax_operator; ///< Operator symbol colour.
    uint32_t syntax_error;    ///< Error/diagnostic underline or text colour.
} vg_color_scheme_t;

//=============================================================================
// Typography
//=============================================================================

/// @brief Font handles and size presets used throughout the UI.
///
/// @details The theme references three font slots (regular, bold, monospace)
///          and four size presets. Widget code picks the appropriate font and
///          size from the current theme rather than hard-coding values.
typedef struct vg_typography {
    vg_font_t *font_regular; ///< Regular-weight proportional font.
    vg_font_t *font_bold;    ///< Bold-weight proportional font.
    vg_font_t *font_mono;    ///< Monospace font (used by code editor, output pane).

    float size_small;   ///< Small text size in pixels (e.g. 11).
    float size_normal;  ///< Normal body-text size in pixels (e.g. 13).
    float size_large;   ///< Large text size in pixels (e.g. 16).
    float size_heading; ///< Heading text size in pixels (e.g. 20).

    float line_height; ///< Line-height multiplier (e.g. 1.4).
} vg_typography_t;

//=============================================================================
// Spacing
//=============================================================================

/// @brief Named spacing presets for consistent padding, margins, and gaps.
typedef struct vg_spacing {
    float xs; ///< Extra-small spacing (e.g. 2 px).
    float sm; ///< Small spacing (e.g. 4 px).
    float md; ///< Medium spacing (e.g. 8 px).
    float lg; ///< Large spacing (e.g. 16 px).
    float xl; ///< Extra-large spacing (e.g. 24 px).
} vg_spacing_t;

//=============================================================================
// Button Style
//=============================================================================

/// @brief Theme-level style overrides specific to button widgets.
typedef struct vg_button_theme {
    float height;        ///< Default button height in pixels.
    float padding_h;     ///< Horizontal padding inside the button.
    float border_radius; ///< Corner radius for rounded button borders.
    float border_width;  ///< Width of the button border stroke.
} vg_button_theme_t;

//=============================================================================
// Input Style
//=============================================================================

/// @brief Theme-level style overrides specific to text-input widgets.
typedef struct vg_input_theme {
    float height;        ///< Default text-input height in pixels.
    float padding_h;     ///< Horizontal padding inside the input field.
    float border_radius; ///< Corner radius for the input border.
    float border_width;  ///< Width of the input border stroke.
} vg_input_theme_t;

//=============================================================================
// Scrollbar Style
//=============================================================================

/// @brief Theme-level style overrides for scrollbar rendering.
typedef struct vg_scrollbar_theme {
    float width;          ///< Scrollbar track width in pixels.
    float min_thumb_size; ///< Minimum thumb (grip) length in pixels.
    float border_radius;  ///< Corner radius of the scrollbar thumb.
} vg_scrollbar_theme_t;

//=============================================================================
// Refined Depth visual tokens
//=============================================================================

/// @brief Named corner-radius presets (px) giving the UI one rounding language.
typedef struct vg_radius_scale {
    float none; ///< Square corners (0).
    float sm;   ///< Small radius (chips, tags, inline controls).
    float md;   ///< Medium radius (buttons, inputs, tabs).
    float lg;   ///< Large radius (cards, dialogs, popovers).
    float xl;   ///< Extra-large radius (hero panels).
    float pill; ///< Fully rounded; the renderer clamps to half the short side.
} vg_radius_scale_t;

/// @brief One elevation level: a soft drop shadow described by blur, offset and
///        peak opacity over the shared shadow colour.
typedef struct vg_elevation {
    float blur;    ///< Blur radius in pixels (0 disables the shadow).
    int dx;        ///< Horizontal shadow offset in pixels.
    int dy;        ///< Vertical shadow offset in pixels.
    uint8_t alpha; ///< Peak shadow opacity (0-255).
} vg_elevation_t;

/// @brief The elevation scale: shared shadow colour plus four depth levels.
typedef struct vg_elevation_scale {
    uint32_t shadow_rgb;   ///< Shadow colour (0x00RRGGBB), usually near-black.
    vg_elevation_t level0; ///< Flush with the surface (no shadow).
    vg_elevation_t level1; ///< Subtle lift (buttons, inputs).
    vg_elevation_t level2; ///< Raised (dropdowns, menus, tooltips).
    vg_elevation_t level3; ///< Floating (dialogs, floating panels).
} vg_elevation_scale_t;

/// @brief Subtle top-to-bottom gradient applied to raised surfaces.
typedef struct vg_gradient_theme {
    bool enabled;   ///< Master switch; false => flat fills everywhere.
    float strength; ///< Top/bottom lightness delta in [0,1] (e.g. 0.06 subtle).
} vg_gradient_theme_t;

/// @brief Appearance of the keyboard-focus glow drawn around focused widgets.
typedef struct vg_focus_theme {
    uint32_t glow_color; ///< Glow colour (0x00RRGGBB), usually accent_primary.
    float glow_width;    ///< Glow ring stroke width in pixels.
    uint8_t glow_alpha;  ///< Peak glow opacity (0-255).
} vg_focus_theme_t;

/// @brief Durations for state-change animations (hover/press/focus). When
///        disabled, widgets snap instantly — a built-in "reduce motion" path.
typedef struct vg_motion_theme {
    bool enabled;   ///< When false, no tweening (instant state changes).
    float hover_ms; ///< Hover fade duration in milliseconds.
    float press_ms; ///< Press feedback duration in milliseconds.
    float focus_ms; ///< Focus-ring fade duration in milliseconds.
} vg_motion_theme_t;

//=============================================================================
// Complete Theme
//=============================================================================

/// @brief Aggregate theme structure holding all visual parameters for the GUI.
///
/// @details Widget paint code reads from the current theme to decide colours,
///          sizes, fonts, and spacing without hard-coding any values.
typedef struct vg_theme {
    char *name;                     ///< Human-readable theme name; owned for custom themes.
    vg_color_scheme_t colors;       ///< Full colour palette.
    vg_typography_t typography;     ///< Font and size presets.
    vg_spacing_t spacing;           ///< Named spacing values.
    vg_button_theme_t button;       ///< Button-specific style overrides.
    vg_input_theme_t input;         ///< Text-input-specific style overrides.
    vg_scrollbar_theme_t scrollbar; ///< Scrollbar style overrides.
    float ui_scale;                 ///< HiDPI pixel scale factor (1.0 = standard, 2.0 = Retina).
                    ///< Set by the app after vg_theme_set_current; treat 0.0 as 1.0.
    // Refined Depth visual tokens (appended; zero-safe for legacy initializers).
    vg_radius_scale_t radius;       ///< Corner-radius scale.
    vg_elevation_scale_t elevation; ///< Soft-shadow elevation levels.
    vg_gradient_theme_t gradient;   ///< Subtle surface gradient.
    vg_focus_theme_t focus;         ///< Focus-ring glow.
    vg_motion_theme_t motion;       ///< State-change animation timings.
} vg_theme_t;

//=============================================================================
// Theme API
//=============================================================================

/// @brief Retrieve the currently active global theme.
///
/// @return Pointer to the current theme (never NULL).
vg_theme_t *vg_theme_get_current(void);

/// @brief Set the global current theme used by all widget paint code.
///
/// @details Switching themes takes effect on the next paint pass. The caller
///          must ensure the theme remains valid for as long as it is current.
///
/// @param theme The theme to make current (must not be NULL).
void vg_theme_set_current(vg_theme_t *theme);

/// @brief Obtain a pointer to the built-in dark theme.
///
/// @details The dark theme is statically allocated; do not free it.
///
/// @return Pointer to the built-in dark theme.
vg_theme_t *vg_theme_dark(void);

/// @brief Obtain a pointer to the built-in light theme.
///
/// @details The light theme is statically allocated; do not free it.
///
/// @return Pointer to the built-in light theme.
vg_theme_t *vg_theme_light(void);

/// @brief Create a new custom theme by copying from a base theme.
///
/// @details Allocates a new vg_theme_t and deep-copies all fields from
///          @p base. The caller can then modify individual fields. The
///          resulting theme must be freed with vg_theme_destroy.
///
/// @param name A name for the new theme (copied internally).
/// @param base The base theme to copy from (may be a built-in or custom theme).
/// @return Newly allocated theme, or NULL on failure.
vg_theme_t *vg_theme_create(const char *name, const vg_theme_t *base);

/// @brief Free a custom theme created with vg_theme_create.
///
/// @details Must not be called on built-in themes returned by vg_theme_dark
///          or vg_theme_light. If the theme is currently active it should be
///          replaced first.
///
/// @param theme The custom theme to destroy (may be NULL).
void vg_theme_destroy(vg_theme_t *theme);

//=============================================================================
// Color Helpers
//=============================================================================

/// @brief Construct a fully opaque colour from 8-bit RGB components.
///
/// @param r Red component (0-255).
/// @param g Green component (0-255).
/// @param b Blue component (0-255).
/// @return Packed ARGB colour with alpha = 0xFF.
static inline uint32_t vg_rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/// @brief Construct a colour from 8-bit RGBA components.
///
/// @param r Red component (0-255).
/// @param g Green component (0-255).
/// @param b Blue component (0-255).
/// @param a Alpha component (0 = fully transparent, 255 = fully opaque).
/// @return Packed ARGB colour.
static inline uint32_t vg_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/// @brief Extract the red channel from a packed ARGB colour.
///
/// @param color Packed ARGB colour value.
/// @return Red component (0-255).
static inline uint8_t vg_color_r(uint32_t color) {
    return (color >> 16) & 0xFF;
}

/// @brief Extract the green channel from a packed ARGB colour.
///
/// @param color Packed ARGB colour value.
/// @return Green component (0-255).
static inline uint8_t vg_color_g(uint32_t color) {
    return (color >> 8) & 0xFF;
}

/// @brief Extract the blue channel from a packed ARGB colour.
///
/// @param color Packed ARGB colour value.
/// @return Blue component (0-255).
static inline uint8_t vg_color_b(uint32_t color) {
    return color & 0xFF;
}

/// @brief Extract the alpha channel from a packed ARGB colour.
///
/// @param color Packed ARGB colour value.
/// @return Alpha component (0-255).
static inline uint8_t vg_color_a(uint32_t color) {
    return (color >> 24) & 0xFF;
}

/// @brief Linearly interpolate between two colours.
///
/// @details Each ARGB channel is blended independently. When @p t is 0.0 the
///          result is @p c1; when @p t is 1.0 the result is @p c2.
///
/// @param c1 First colour.
/// @param c2 Second colour.
/// @param t  Blend factor in the range [0.0, 1.0].
/// @return The blended colour.
uint32_t vg_color_blend(uint32_t c1, uint32_t c2, float t);

/// @brief Lighten a colour by blending it towards white.
///
/// @param color  The colour to lighten.
/// @param amount Lightening factor in the range [0.0, 1.0] (0 = no change,
///               1 = fully white).
/// @return The lightened colour.
uint32_t vg_color_lighten(uint32_t color, float amount);

/// @brief Darken a colour by blending it towards black.
///
/// @param color  The colour to darken.
/// @param amount Darkening factor in the range [0.0, 1.0] (0 = no change,
///               1 = fully black).
/// @return The darkened colour.
uint32_t vg_color_darken(uint32_t color, float amount);

#ifdef __cplusplus
}
#endif
