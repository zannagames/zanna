//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/gui/rt_gui_theme.c
// Purpose: Managed GUI theme palettes, custom palette validation, and dark/light/system/custom
//          mode control for the public Zanna.GUI.Theme runtime surface.
//
// Key invariants:
//   - ThemePalette values own one unscaled vg_theme_t clone and never expose its address.
//   - Palette mutation distinguishes an unknown token from a recognized but invalid value.
//   - Applying a palette clones it again into the active app; palette and app lifetimes are
//     independent, and per-app refresh performs DPI scaling exactly once.
//   - Theme revisions are monotonic and WasChanged consumes only its own observation cursor.
//
// Ownership/Lifetime:
//   - ThemePalette wrappers are GC-managed objects with a finalizer that destroys their theme.
//   - Managed fonts stored in a palette are retained per role. Legacy raw fonts remain borrowed;
//     applying either form makes the app retirement scan protect installed/base theme references.
//   - Returned Result and ThemePalette objects are caller-owned runtime values.
//
// Links: src/runtime/graphics/gui/rt_gui.h,
//        src/runtime/graphics/gui/rt_gui_internal.h,
//        src/lib/gui/include/vg_theme.h,
//        docs/adr/0107-gui-theme-accessibility-input-and-render-policy.md
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements managed GUI palettes and application theme-mode control.
 *
 * @details ThemePalette wrappers own unscaled toolkit clones, expose validated
 *          named color, metric, and font-role mutation, and apply independent
 *          clones to GUI applications. Theme mode, system appearance,
 *          accessibility adjustment, DPI scaling, font retirement, revisions,
 *          and one-shot change observation remain coordinated per application.
 */

#include "rt_gui_accessibility_platform.h"
#include "rt_gui_internal.h"
#include "rt_result.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Magic value authenticating live managed ThemePalette objects.
#define RT_GUI_THEME_PALETTE_MAGIC UINT64_C(0x5254475554484D31)

/// @brief Managed logical theme palette stored behind the Zanna-facing opaque object.
/// @details Invalid-value masks preserve the distinction between unknown token names (setter
///          returns false) and recognized names supplied with a value that cannot be represented
///          safely (setter returns true and Validate reports the token).
typedef struct rt_gui_theme_palette {
    uint64_t magic;               ///< Live-object discriminator.
    vg_theme_t *theme;            ///< Owned unscaled custom theme clone.
    uint64_t invalid_color_mask;  ///< Recognized color tokens with invalid attempted values.
    uint64_t invalid_metric_mask; ///< Recognized metric tokens with invalid attempted values.
    uint8_t invalid_font_mask;    ///< Invalid regular/bold/mono handle attempts.
    void *font_handles[3];        ///< Retained managed regular/bold/mono wrappers, or NULL.
} rt_gui_theme_palette_t;

/// @brief Description of one public 24-bit RGB theme token.
typedef struct rt_gui_color_token {
    const char *name; ///< Stable lower-camel public token name.
    size_t offset;    ///< Byte offset of the uint32_t field within vg_theme_t.
} rt_gui_color_token_t;

/// @brief Storage representation used by a public numeric theme token.
typedef enum rt_gui_metric_storage {
    RT_GUI_METRIC_FLOAT, ///< Single-precision floating-point field.
    RT_GUI_METRIC_INT,   ///< Signed integer field.
    RT_GUI_METRIC_U8,    ///< Unsigned eight-bit field.
    RT_GUI_METRIC_BOOL,  ///< Boolean field normalized from numeric input.
} rt_gui_metric_storage_t;

/// @brief Description and validation range for one public numeric theme token.
typedef struct rt_gui_metric_token {
    const char *name;                ///< Stable lower-camel public token name.
    size_t offset;                   ///< Byte offset of the field within vg_theme_t.
    rt_gui_metric_storage_t storage; ///< Concrete C representation at @ref offset.
    double minimum;                  ///< Smallest accepted numeric value.
    double maximum;                  ///< Largest accepted numeric value.
    bool minimum_is_exclusive;       ///< True when @ref minimum itself is invalid.
} rt_gui_metric_token_t;

/// @brief Alias mapping retained for intuitive and direct-C-field spellings.
typedef struct rt_gui_metric_alias {
    const char *alias;     ///< Alternate accepted lower-camel name.
    const char *canonical; ///< Name present in the primary metric table.
} rt_gui_metric_alias_t;

/// @brief Compute the byte offset of a color-scheme member in @c vg_theme_t.
/// @param member Member of @c vg_color_scheme_t.
#define THEME_COLOR_OFFSET(member)                                                                 \
    (offsetof(vg_theme_t, colors) + offsetof(vg_color_scheme_t, member))
/// @brief Compute the byte offset of a typography member.
/// @param member Member of @c vg_typography_t.
#define THEME_TYPOGRAPHY_OFFSET(member)                                                            \
    (offsetof(vg_theme_t, typography) + offsetof(vg_typography_t, member))
/// @brief Compute the byte offset of a spacing-scale member.
/// @param member Member of @c vg_spacing_t.
#define THEME_SPACING_OFFSET(member)                                                               \
    (offsetof(vg_theme_t, spacing) + offsetof(vg_spacing_t, member))
/// @brief Compute the byte offset of a button-theme member.
/// @param member Member of @c vg_button_theme_t.
#define THEME_BUTTON_OFFSET(member)                                                                \
    (offsetof(vg_theme_t, button) + offsetof(vg_button_theme_t, member))
/// @brief Compute the byte offset of an input-theme member.
/// @param member Member of @c vg_input_theme_t.
#define THEME_INPUT_OFFSET(member)                                                                 \
    (offsetof(vg_theme_t, input) + offsetof(vg_input_theme_t, member))
/// @brief Compute the byte offset of a scrollbar-theme member.
/// @param member Member of @c vg_scrollbar_theme_t.
#define THEME_SCROLLBAR_OFFSET(member)                                                             \
    (offsetof(vg_theme_t, scrollbar) + offsetof(vg_scrollbar_theme_t, member))
/// @brief Compute the byte offset of a radius-scale member.
/// @param member Member of @c vg_radius_scale_t.
#define THEME_RADIUS_OFFSET(member)                                                                \
    (offsetof(vg_theme_t, radius) + offsetof(vg_radius_scale_t, member))
/// @brief Compute the byte offset of one elevation-level member.
/// @param level Member of @c vg_elevation_scale_t selecting the level.
/// @param member Member of the selected @c vg_elevation_t.
#define THEME_ELEVATION_OFFSET(level, member)                                                      \
    (offsetof(vg_theme_t, elevation) + offsetof(vg_elevation_scale_t, level) +                     \
     offsetof(vg_elevation_t, member))
/// @brief Compute the byte offset of a gradient-theme member.
/// @param member Member of @c vg_gradient_theme_t.
#define THEME_GRADIENT_OFFSET(member)                                                              \
    (offsetof(vg_theme_t, gradient) + offsetof(vg_gradient_theme_t, member))
/// @brief Compute the byte offset of a focus-theme member.
/// @param member Member of @c vg_focus_theme_t.
#define THEME_FOCUS_OFFSET(member)                                                                 \
    (offsetof(vg_theme_t, focus) + offsetof(vg_focus_theme_t, member))
/// @brief Compute the byte offset of a motion-theme member.
/// @param member Member of @c vg_motion_theme_t.
#define THEME_MOTION_OFFSET(member)                                                                \
    (offsetof(vg_theme_t, motion) + offsetof(vg_motion_theme_t, member))

/// @brief Stable public color-token descriptors.
static const rt_gui_color_token_t k_color_tokens[] = {
    {"bgPrimary", THEME_COLOR_OFFSET(bg_primary)},
    {"bgSecondary", THEME_COLOR_OFFSET(bg_secondary)},
    {"bgTertiary", THEME_COLOR_OFFSET(bg_tertiary)},
    {"bgHover", THEME_COLOR_OFFSET(bg_hover)},
    {"bgActive", THEME_COLOR_OFFSET(bg_active)},
    {"bgSelected", THEME_COLOR_OFFSET(bg_selected)},
    {"bgDisabled", THEME_COLOR_OFFSET(bg_disabled)},
    {"fgPrimary", THEME_COLOR_OFFSET(fg_primary)},
    {"fgSecondary", THEME_COLOR_OFFSET(fg_secondary)},
    {"fgTertiary", THEME_COLOR_OFFSET(fg_tertiary)},
    {"fgDisabled", THEME_COLOR_OFFSET(fg_disabled)},
    {"fgPlaceholder", THEME_COLOR_OFFSET(fg_placeholder)},
    {"fgLink", THEME_COLOR_OFFSET(fg_link)},
    {"accentPrimary", THEME_COLOR_OFFSET(accent_primary)},
    {"accentSecondary", THEME_COLOR_OFFSET(accent_secondary)},
    {"accentDanger", THEME_COLOR_OFFSET(accent_danger)},
    {"accentWarning", THEME_COLOR_OFFSET(accent_warning)},
    {"accentSuccess", THEME_COLOR_OFFSET(accent_success)},
    {"accentInfo", THEME_COLOR_OFFSET(accent_info)},
    {"borderPrimary", THEME_COLOR_OFFSET(border_primary)},
    {"borderSecondary", THEME_COLOR_OFFSET(border_secondary)},
    {"borderFocus", THEME_COLOR_OFFSET(border_focus)},
    {"syntaxKeyword", THEME_COLOR_OFFSET(syntax_keyword)},
    {"syntaxType", THEME_COLOR_OFFSET(syntax_type)},
    {"syntaxFunction", THEME_COLOR_OFFSET(syntax_function)},
    {"syntaxVariable", THEME_COLOR_OFFSET(syntax_variable)},
    {"syntaxString", THEME_COLOR_OFFSET(syntax_string)},
    {"syntaxNumber", THEME_COLOR_OFFSET(syntax_number)},
    {"syntaxComment", THEME_COLOR_OFFSET(syntax_comment)},
    {"syntaxOperator", THEME_COLOR_OFFSET(syntax_operator)},
    {"syntaxError", THEME_COLOR_OFFSET(syntax_error)},
    {"elevationShadowColor",
     offsetof(vg_theme_t, elevation) + offsetof(vg_elevation_scale_t, shadow_rgb)},
    {"focusGlowColor", THEME_FOCUS_OFFSET(glow_color)},
};

/// @brief Stable public numeric-token descriptors and accepted ranges.
static const rt_gui_metric_token_t k_metric_tokens[] = {
    {"fontSizeSmall", THEME_TYPOGRAPHY_OFFSET(size_small), RT_GUI_METRIC_FLOAT, 0.0, 1024.0, true},
    {"fontSizeNormal",
     THEME_TYPOGRAPHY_OFFSET(size_normal),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1024.0,
     true},
    {"fontSizeLarge", THEME_TYPOGRAPHY_OFFSET(size_large), RT_GUI_METRIC_FLOAT, 0.0, 1024.0, true},
    {"fontSizeHeading",
     THEME_TYPOGRAPHY_OFFSET(size_heading),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1024.0,
     true},
    {"lineHeight", THEME_TYPOGRAPHY_OFFSET(line_height), RT_GUI_METRIC_FLOAT, 0.5, 4.0, false},
    {"spacingExtraSmall", THEME_SPACING_OFFSET(xs), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"spacingSmall", THEME_SPACING_OFFSET(sm), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"spacingMedium", THEME_SPACING_OFFSET(md), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"spacingLarge", THEME_SPACING_OFFSET(lg), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"spacingExtraLarge", THEME_SPACING_OFFSET(xl), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"buttonHeight", THEME_BUTTON_OFFSET(height), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, true},
    {"buttonPaddingHorizontal",
     THEME_BUTTON_OFFSET(padding_h),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"buttonRadius",
     THEME_BUTTON_OFFSET(border_radius),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"buttonBorderWidth",
     THEME_BUTTON_OFFSET(border_width),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"inputHeight", THEME_INPUT_OFFSET(height), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, true},
    {"inputPaddingHorizontal",
     THEME_INPUT_OFFSET(padding_h),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"inputRadius", THEME_INPUT_OFFSET(border_radius), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"inputBorderWidth",
     THEME_INPUT_OFFSET(border_width),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"scrollbarWidth", THEME_SCROLLBAR_OFFSET(width), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, true},
    {"scrollbarMinThumbSize",
     THEME_SCROLLBAR_OFFSET(min_thumb_size),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"scrollbarRadius",
     THEME_SCROLLBAR_OFFSET(border_radius),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"radiusNone", THEME_RADIUS_OFFSET(none), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"radiusSmall", THEME_RADIUS_OFFSET(sm), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"radiusMedium", THEME_RADIUS_OFFSET(md), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"radiusLarge", THEME_RADIUS_OFFSET(lg), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"radiusExtraLarge", THEME_RADIUS_OFFSET(xl), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"radiusPill", THEME_RADIUS_OFFSET(pill), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"elevationLevel0Blur",
     THEME_ELEVATION_OFFSET(level0, blur),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"elevationLevel0X",
     THEME_ELEVATION_OFFSET(level0, dx),
     RT_GUI_METRIC_INT,
     -1000000.0,
     1000000.0,
     false},
    {"elevationLevel0Y",
     THEME_ELEVATION_OFFSET(level0, dy),
     RT_GUI_METRIC_INT,
     -1000000.0,
     1000000.0,
     false},
    {"elevationLevel0Alpha",
     THEME_ELEVATION_OFFSET(level0, alpha),
     RT_GUI_METRIC_U8,
     0.0,
     255.0,
     false},
    {"elevationLevel1Blur",
     THEME_ELEVATION_OFFSET(level1, blur),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"elevationLevel1X",
     THEME_ELEVATION_OFFSET(level1, dx),
     RT_GUI_METRIC_INT,
     -1000000.0,
     1000000.0,
     false},
    {"elevationLevel1Y",
     THEME_ELEVATION_OFFSET(level1, dy),
     RT_GUI_METRIC_INT,
     -1000000.0,
     1000000.0,
     false},
    {"elevationLevel1Alpha",
     THEME_ELEVATION_OFFSET(level1, alpha),
     RT_GUI_METRIC_U8,
     0.0,
     255.0,
     false},
    {"elevationLevel2Blur",
     THEME_ELEVATION_OFFSET(level2, blur),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"elevationLevel2X",
     THEME_ELEVATION_OFFSET(level2, dx),
     RT_GUI_METRIC_INT,
     -1000000.0,
     1000000.0,
     false},
    {"elevationLevel2Y",
     THEME_ELEVATION_OFFSET(level2, dy),
     RT_GUI_METRIC_INT,
     -1000000.0,
     1000000.0,
     false},
    {"elevationLevel2Alpha",
     THEME_ELEVATION_OFFSET(level2, alpha),
     RT_GUI_METRIC_U8,
     0.0,
     255.0,
     false},
    {"elevationLevel3Blur",
     THEME_ELEVATION_OFFSET(level3, blur),
     RT_GUI_METRIC_FLOAT,
     0.0,
     1000000.0,
     false},
    {"elevationLevel3X",
     THEME_ELEVATION_OFFSET(level3, dx),
     RT_GUI_METRIC_INT,
     -1000000.0,
     1000000.0,
     false},
    {"elevationLevel3Y",
     THEME_ELEVATION_OFFSET(level3, dy),
     RT_GUI_METRIC_INT,
     -1000000.0,
     1000000.0,
     false},
    {"elevationLevel3Alpha",
     THEME_ELEVATION_OFFSET(level3, alpha),
     RT_GUI_METRIC_U8,
     0.0,
     255.0,
     false},
    {"gradientEnabled", THEME_GRADIENT_OFFSET(enabled), RT_GUI_METRIC_BOOL, 0.0, 1.0, false},
    {"gradientStrength", THEME_GRADIENT_OFFSET(strength), RT_GUI_METRIC_FLOAT, 0.0, 1.0, false},
    {"focusGlowWidth", THEME_FOCUS_OFFSET(glow_width), RT_GUI_METRIC_FLOAT, 0.0, 1000000.0, false},
    {"focusGlowAlpha", THEME_FOCUS_OFFSET(glow_alpha), RT_GUI_METRIC_U8, 0.0, 255.0, false},
    {"motionEnabled", THEME_MOTION_OFFSET(enabled), RT_GUI_METRIC_BOOL, 0.0, 1.0, false},
    {"motionHoverMs", THEME_MOTION_OFFSET(hover_ms), RT_GUI_METRIC_FLOAT, 0.0, 60000.0, false},
    {"motionPressMs", THEME_MOTION_OFFSET(press_ms), RT_GUI_METRIC_FLOAT, 0.0, 60000.0, false},
    {"motionFocusMs", THEME_MOTION_OFFSET(focus_ms), RT_GUI_METRIC_FLOAT, 0.0, 60000.0, false},
};

/// @brief Alternate numeric-token spellings mapped to canonical descriptors.
static const rt_gui_metric_alias_t k_metric_aliases[] = {
    {"typographySizeSmall", "fontSizeSmall"},
    {"typographySizeNormal", "fontSizeNormal"},
    {"typographySizeLarge", "fontSizeLarge"},
    {"typographySizeHeading", "fontSizeHeading"},
    {"typographyLineHeight", "lineHeight"},
    {"spacingXs", "spacingExtraSmall"},
    {"spacingSm", "spacingSmall"},
    {"spacingMd", "spacingMedium"},
    {"spacingLg", "spacingLarge"},
    {"spacingXl", "spacingExtraLarge"},
    {"buttonPaddingH", "buttonPaddingHorizontal"},
    {"buttonBorderRadius", "buttonRadius"},
    {"inputPaddingH", "inputPaddingHorizontal"},
    {"inputBorderRadius", "inputRadius"},
    {"scrollbarBorderRadius", "scrollbarRadius"},
    {"radiusSm", "radiusSmall"},
    {"radiusMd", "radiusMedium"},
    {"radiusLg", "radiusLarge"},
    {"radiusXl", "radiusExtraLarge"},
    {"elevationLevel0Dx", "elevationLevel0X"},
    {"elevationLevel0Dy", "elevationLevel0Y"},
    {"elevationLevel1Dx", "elevationLevel1X"},
    {"elevationLevel1Dy", "elevationLevel1Y"},
    {"elevationLevel2Dx", "elevationLevel2X"},
    {"elevationLevel2Dy", "elevationLevel2Y"},
    {"elevationLevel3Dx", "elevationLevel3X"},
    {"elevationLevel3Dy", "elevationLevel3Y"},
};

_Static_assert(sizeof(k_color_tokens) / sizeof(k_color_tokens[0]) <= 64,
               "theme color invalid mask must fit in uint64_t");
_Static_assert(sizeof(k_metric_tokens) / sizeof(k_metric_tokens[0]) <= 64,
               "theme metric invalid mask must fit in uint64_t");

/// @brief Theme mode used when no GUI application context exists.
static int64_t s_fallback_theme_mode = RT_GUI_THEME_DARK;
/// @brief Last fallback system-appearance preference.
static int32_t s_fallback_theme_prefers_dark = 1;
/// @brief Fallback monotonic theme revision.
static uint64_t s_fallback_theme_revision = 0;
/// @brief Fallback revision consumed by Theme.WasChanged.
static uint64_t s_fallback_theme_reported_revision = 0;

/// @brief Return a checked palette wrapper without accepting arbitrary GUI handles.
/// @param handle Opaque runtime value supplied to a ThemePalette method.
/// @return Live palette pointer, or NULL for NULL/wrong/stale values.
static rt_gui_theme_palette_t *rt_gui_theme_palette_checked(void *handle) {
    if (!handle)
        return NULL;
    rt_gui_theme_palette_t *palette = (rt_gui_theme_palette_t *)handle;
    return palette->magic == RT_GUI_THEME_PALETTE_MAGIC && palette->theme ? palette : NULL;
}

/// @brief Release one retained managed font-role wrapper slot.
/// @details Legacy raw handles are never placed in these slots. The helper clears before release
///          so a Font finalizer cannot observe a stale palette-owned wrapper reference.
/// @param slot Address of a retained managed wrapper slot; may be NULL.
static void rt_gui_theme_release_font_handle(void **slot) {
    if (!slot || !*slot)
        return;
    void *handle = *slot;
    *slot = NULL;
    if (rt_obj_release_check0(handle))
        rt_obj_free(handle);
}

/// @brief Finalize one managed palette and release its owned logical theme.
/// @param object Palette payload supplied by the runtime object manager; may be NULL.
static void rt_gui_theme_palette_finalize(void *object) {
    rt_gui_theme_palette_t *palette = (rt_gui_theme_palette_t *)object;
    if (!palette || palette->magic != RT_GUI_THEME_PALETTE_MAGIC)
        return;
    palette->magic = 0;
    for (size_t role = 0; role < 3; ++role)
        rt_gui_theme_release_font_handle(&palette->font_handles[role]);
    vg_theme_destroy(palette->theme);
    palette->theme = NULL;
    palette->invalid_color_mask = 0;
    palette->invalid_metric_mask = 0;
    palette->invalid_font_mask = 0;
}

/// @brief Allocate a managed palette cloned from one unscaled base theme.
/// @details Allocation failure in the lower clone returns NULL without publishing a partial
///          object. The runtime object allocator follows the normal runtime trapping contract.
/// @param base Borrowed base theme; NULL selects the built-in dark theme.
/// @return Caller-owned managed palette, or NULL when the theme clone cannot be allocated.
static void *rt_gui_theme_palette_create(const vg_theme_t *base) {
    vg_theme_t *theme = vg_theme_create("Custom", base ? base : vg_theme_dark());
    if (!theme)
        return NULL;
    rt_gui_theme_palette_t *palette =
        (rt_gui_theme_palette_t *)rt_obj_new_i64(0, (int64_t)sizeof(*palette));
    if (!palette) {
        vg_theme_destroy(theme);
        return NULL;
    }
    memset(palette, 0, sizeof(*palette));
    palette->magic = RT_GUI_THEME_PALETTE_MAGIC;
    palette->theme = theme;
    rt_obj_set_finalizer(palette, rt_gui_theme_palette_finalize);
    return palette;
}

/// @brief Find one color token using exact, case-sensitive public spelling.
/// @param name Borrowed NUL-terminated token name.
/// @param out_index Optional destination for the stable table index.
/// @return Token descriptor, or NULL when unknown.
static const rt_gui_color_token_t *rt_gui_find_color_token(const char *name, size_t *out_index) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(k_color_tokens) / sizeof(k_color_tokens[0]); ++i) {
        if (strcmp(name, k_color_tokens[i].name) == 0) {
            if (out_index)
                *out_index = i;
            return &k_color_tokens[i];
        }
    }
    return NULL;
}

/// @brief Resolve a metric alias to its canonical spelling.
/// @param name Borrowed NUL-terminated token name.
/// @return Borrowed canonical name or @p name itself when it is not an alias.
static const char *rt_gui_metric_canonical_name(const char *name) {
    if (!name)
        return NULL;
    for (size_t i = 0; i < sizeof(k_metric_aliases) / sizeof(k_metric_aliases[0]); ++i) {
        if (strcmp(name, k_metric_aliases[i].alias) == 0)
            return k_metric_aliases[i].canonical;
    }
    return name;
}

/// @brief Find one numeric token, accepting documented direct-field aliases.
/// @param name Borrowed NUL-terminated token name.
/// @param out_index Optional destination for the canonical table index.
/// @return Canonical descriptor, or NULL when unknown.
static const rt_gui_metric_token_t *rt_gui_find_metric_token(const char *name, size_t *out_index) {
    const char *canonical = rt_gui_metric_canonical_name(name);
    if (!canonical)
        return NULL;
    for (size_t i = 0; i < sizeof(k_metric_tokens) / sizeof(k_metric_tokens[0]); ++i) {
        if (strcmp(canonical, k_metric_tokens[i].name) == 0) {
            if (out_index)
                *out_index = i;
            return &k_metric_tokens[i];
        }
    }
    return NULL;
}

/// @brief Read a numeric metric from its concrete theme storage as double.
/// @param theme Borrowed theme containing the token field.
/// @param token Metric descriptor with a valid in-bounds field offset.
/// @return Exact numeric value promoted to double, or zero for invalid arguments.
static double rt_gui_theme_metric_read(const vg_theme_t *theme,
                                       const rt_gui_metric_token_t *token) {
    if (!theme || !token)
        return 0.0;
    const unsigned char *field = (const unsigned char *)theme + token->offset;
    switch (token->storage) {
        case RT_GUI_METRIC_FLOAT: {
            float stored = 0.0f;
            memcpy(&stored, field, sizeof(stored));
            return (double)stored;
        }
        case RT_GUI_METRIC_INT: {
            int stored = 0;
            memcpy(&stored, field, sizeof(stored));
            return (double)stored;
        }
        case RT_GUI_METRIC_U8: {
            uint8_t stored = 0;
            memcpy(&stored, field, sizeof(stored));
            return (double)stored;
        }
        case RT_GUI_METRIC_BOOL: {
            bool stored = false;
            memcpy(&stored, field, sizeof(stored));
            return stored ? 1.0 : 0.0;
        }
        default:
            return 0.0;
    }
}

/// @brief Check whether one metric value is finite, in range, and representable by its storage.
/// @param token Metric descriptor defining range and concrete representation.
/// @param value Candidate public double value.
/// @return True only when writing @p value is lossless enough for the declared storage contract.
static bool rt_gui_theme_metric_is_valid(const rt_gui_metric_token_t *token, double value) {
    if (!token || !isfinite(value))
        return false;
    if (token->minimum_is_exclusive ? value <= token->minimum : value < token->minimum)
        return false;
    if (value > token->maximum)
        return false;
    if ((token->storage == RT_GUI_METRIC_INT || token->storage == RT_GUI_METRIC_U8 ||
         token->storage == RT_GUI_METRIC_BOOL) &&
        trunc(value) != value)
        return false;
    return true;
}

/// @brief Store a previously validated metric in its concrete theme field.
/// @param theme Mutable palette theme.
/// @param token Metric descriptor with an in-bounds field offset.
/// @param value Finite, validated value accepted by @ref rt_gui_theme_metric_is_valid.
static void rt_gui_theme_metric_write(vg_theme_t *theme,
                                      const rt_gui_metric_token_t *token,
                                      double value) {
    unsigned char *field = (unsigned char *)theme + token->offset;
    switch (token->storage) {
        case RT_GUI_METRIC_FLOAT: {
            float stored = (float)value;
            memcpy(field, &stored, sizeof(stored));
            break;
        }
        case RT_GUI_METRIC_INT: {
            int stored = (int)value;
            memcpy(field, &stored, sizeof(stored));
            break;
        }
        case RT_GUI_METRIC_U8: {
            uint8_t stored = (uint8_t)value;
            memcpy(field, &stored, sizeof(stored));
            break;
        }
        case RT_GUI_METRIC_BOOL: {
            bool stored = value != 0.0;
            memcpy(field, &stored, sizeof(stored));
            break;
        }
    }
}

/// @brief Convert an sRGB channel to WCAG linear-light intensity.
/// @param channel Eight-bit sRGB component normalized internally to [0,1].
/// @return Linear channel intensity in [0,1].
static double rt_gui_theme_linear_channel(uint8_t channel) {
    double value = (double)channel / 255.0;
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

/// @brief Compute WCAG 2 contrast for two packed 24-bit RGB values.
/// @param foreground First 0xRRGGBB color.
/// @param background Second 0xRRGGBB color.
/// @return Symmetric contrast ratio in [1,21].
static double rt_gui_theme_contrast_ratio(uint32_t foreground, uint32_t background) {
    double foreground_luminance =
        0.2126 * rt_gui_theme_linear_channel((uint8_t)(foreground >> 16u)) +
        0.7152 * rt_gui_theme_linear_channel((uint8_t)(foreground >> 8u)) +
        0.0722 * rt_gui_theme_linear_channel((uint8_t)foreground);
    double background_luminance =
        0.2126 * rt_gui_theme_linear_channel((uint8_t)(background >> 16u)) +
        0.7152 * rt_gui_theme_linear_channel((uint8_t)(background >> 8u)) +
        0.0722 * rt_gui_theme_linear_channel((uint8_t)background);
    double lighter = fmax(foreground_luminance, background_luminance);
    double darker = fmin(foreground_luminance, background_luminance);
    return (lighter + 0.05) / (darker + 0.05);
}

/// @brief Construct a Result.Err using the stable invalid-token diagnostic format.
/// @param token Borrowed token name included in the copied message.
/// @return Caller-owned Zanna.Result error object.
static void *rt_gui_theme_invalid_result(const char *token) {
    char message[192];
    snprintf(message,
             sizeof(message),
             "GUI theme token %s has an invalid value",
             token ? token : "<unknown>");
    rt_string runtime_message = rt_const_cstr(message);
    void *result = rt_result_err_str(runtime_message);
    rt_str_release_maybe(runtime_message);
    return result;
}

/// @brief Return the first invalid palette token, if any.
/// @details Validation order is stable: colors, metrics, font roles, then required primary and
///          secondary normal-text contrast pairs. This produces deterministic diagnostics.
/// @param palette Checked live palette.
/// @return Borrowed stable token name, or NULL when the palette is valid.
static const char *rt_gui_theme_palette_first_invalid(const rt_gui_theme_palette_t *palette) {
    if (!palette || !palette->theme)
        return "palette";
    for (size_t i = 0; i < sizeof(k_color_tokens) / sizeof(k_color_tokens[0]); ++i) {
        if ((palette->invalid_color_mask & (UINT64_C(1) << i)) != 0)
            return k_color_tokens[i].name;
    }
    for (size_t i = 0; i < sizeof(k_metric_tokens) / sizeof(k_metric_tokens[0]); ++i) {
        if ((palette->invalid_metric_mask & (UINT64_C(1) << i)) != 0 ||
            !rt_gui_theme_metric_is_valid(
                &k_metric_tokens[i], rt_gui_theme_metric_read(palette->theme, &k_metric_tokens[i])))
            return k_metric_tokens[i].name;
    }
    static const char *font_tokens[] = {"fontRegular", "fontBold", "fontMono"};
    vg_font_t *fonts[] = {palette->theme->typography.font_regular,
                          palette->theme->typography.font_bold,
                          palette->theme->typography.font_mono};
    for (size_t i = 0; i < sizeof(fonts) / sizeof(fonts[0]); ++i) {
        if ((palette->invalid_font_mask & (uint8_t)(1u << i)) != 0 ||
            (fonts[i] && !vg_font_is_live(fonts[i])))
            return font_tokens[i];
    }
    if (rt_gui_theme_contrast_ratio(palette->theme->colors.fg_primary,
                                    palette->theme->colors.bg_primary) < 4.5)
        return "fgPrimary";
    if (rt_gui_theme_contrast_ratio(palette->theme->colors.fg_secondary,
                                    palette->theme->colors.bg_secondary) < 4.5)
        return "fgSecondary";
    return NULL;
}

/// @brief Create a palette initialized from the built-in dark logical theme.
/// @return Caller-owned ThemePalette, or NULL on lower allocation failure.
void *rt_theme_palette_new(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_gui_theme_palette_create(vg_theme_dark());
}

/// @brief Create a palette initialized from the built-in dark logical theme.
/// @return Caller-owned ThemePalette, or NULL on lower allocation failure.
void *rt_theme_palette_from_dark(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_gui_theme_palette_create(vg_theme_dark());
}

/// @brief Create a palette initialized from the built-in light logical theme.
/// @return Caller-owned ThemePalette, or NULL on lower allocation failure.
void *rt_theme_palette_from_light(void) {
    RT_ASSERT_MAIN_THREAD();
    return rt_gui_theme_palette_create(vg_theme_light());
}

/// @brief Deep-clone a managed logical palette, including pending validation state.
/// @param palette_handle Live ThemePalette handle.
/// @return Independent caller-owned palette, or NULL for invalid handles/allocation failure.
void *rt_theme_palette_clone(void *palette_handle) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_theme_palette_t *source = rt_gui_theme_palette_checked(palette_handle);
    if (!source)
        return NULL;
    rt_gui_theme_palette_t *clone =
        (rt_gui_theme_palette_t *)rt_gui_theme_palette_create(source->theme);
    if (!clone)
        return NULL;
    clone->invalid_color_mask = source->invalid_color_mask;
    clone->invalid_metric_mask = source->invalid_metric_mask;
    clone->invalid_font_mask = source->invalid_font_mask;
    for (size_t role = 0; role < 3; ++role) {
        if (source->font_handles[role]) {
            rt_obj_retain_maybe(source->font_handles[role]);
            clone->font_handles[role] = source->font_handles[role];
        }
    }
    return clone;
}

/// @brief Set one named 24-bit RGB token in a managed palette.
/// @details Unknown or embedded-NUL names return false. Recognized names return true; an RGB value
///          outside [0,0xFFFFFF] is recorded as invalid without truncating the previous field, so
///          Validate can report it and a later valid write can repair it atomically.
/// @param palette_handle Live ThemePalette handle.
/// @param token Runtime string naming a canonical lower-camel color field.
/// @param rgb Packed 0xRRGGBB value.
/// @return One when @p token is recognized (even if its value is invalid), otherwise zero.
int64_t rt_theme_palette_set_color(void *palette_handle, rt_string token, int64_t rgb) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_theme_palette_t *palette = rt_gui_theme_palette_checked(palette_handle);
    char *name = rt_string_to_cstr_no_nul(token);
    if (!palette || !name) {
        free(name);
        return 0;
    }
    size_t index = 0;
    const rt_gui_color_token_t *descriptor = rt_gui_find_color_token(name, &index);
    free(name);
    if (!descriptor)
        return 0;
    uint64_t bit = UINT64_C(1) << index;
    if (rgb < 0 || rgb > INT64_C(0xFFFFFF)) {
        palette->invalid_color_mask |= bit;
        return 1;
    }
    *(uint32_t *)((unsigned char *)palette->theme + descriptor->offset) = (uint32_t)rgb;
    palette->invalid_color_mask &= ~bit;
    return 1;
}

/// @brief Read one named 24-bit RGB token from a managed palette.
/// @param palette_handle Live ThemePalette handle.
/// @param token Runtime string naming a canonical lower-camel color field.
/// @return Packed 0xRRGGBB value, or zero for invalid handles/unknown/embedded-NUL names.
int64_t rt_theme_palette_get_color(void *palette_handle, rt_string token) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_theme_palette_t *palette = rt_gui_theme_palette_checked(palette_handle);
    char *name = rt_string_to_cstr_no_nul(token);
    if (!palette || !name) {
        free(name);
        return 0;
    }
    const rt_gui_color_token_t *descriptor = rt_gui_find_color_token(name, NULL);
    free(name);
    if (!descriptor)
        return 0;
    return (int64_t)*(const uint32_t *)((const unsigned char *)palette->theme + descriptor->offset);
}

/// @brief Set one named logical numeric theme token.
/// @details Unknown names return false. A recognized non-finite, out-of-range, or fractional
///          integer value is recorded as invalid without changing the previous field; Validate
///          reports the canonical token and a later valid setter clears that state.
/// @param palette_handle Live ThemePalette handle.
/// @param token Canonical lower-camel name or documented direct-field alias.
/// @param value Logical distance, multiplier, opacity, flag, or millisecond duration.
/// @return One when the name is recognized (even if value is invalid), otherwise zero.
int64_t rt_theme_palette_set_metric(void *palette_handle, rt_string token, double value) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_theme_palette_t *palette = rt_gui_theme_palette_checked(palette_handle);
    char *name = rt_string_to_cstr_no_nul(token);
    if (!palette || !name) {
        free(name);
        return 0;
    }
    size_t index = 0;
    const rt_gui_metric_token_t *descriptor = rt_gui_find_metric_token(name, &index);
    free(name);
    if (!descriptor)
        return 0;
    uint64_t bit = UINT64_C(1) << index;
    if (!rt_gui_theme_metric_is_valid(descriptor, value)) {
        palette->invalid_metric_mask |= bit;
        return 1;
    }
    rt_gui_theme_metric_write(palette->theme, descriptor, value);
    palette->invalid_metric_mask &= ~bit;
    return 1;
}

/// @brief Read one named logical numeric theme token.
/// @param palette_handle Live ThemePalette handle.
/// @param token Canonical lower-camel name or documented direct-field alias.
/// @return Numeric value as f64, or zero for invalid handles/unknown/embedded-NUL names.
double rt_theme_palette_get_metric(void *palette_handle, rt_string token) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_theme_palette_t *palette = rt_gui_theme_palette_checked(palette_handle);
    char *name = rt_string_to_cstr_no_nul(token);
    if (!palette || !name) {
        free(name);
        return 0.0;
    }
    const rt_gui_metric_token_t *descriptor = rt_gui_find_metric_token(name, NULL);
    free(name);
    return descriptor ? rt_gui_theme_metric_read(palette->theme, descriptor) : 0.0;
}

/// @brief Enable or disable palette-defined state-transition motion.
/// @details This is the strongly typed convenience for the `motionEnabled` metric. Invalid
///          handles are no-ops; any prior invalid attempt for that token is repaired.
/// @param palette_handle Live ThemePalette handle.
/// @param enabled Non-zero enables motion; zero disables it.
void rt_theme_palette_set_motion_enabled(void *palette_handle, int64_t enabled) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_theme_palette_t *palette = rt_gui_theme_palette_checked(palette_handle);
    if (!palette)
        return;
    size_t index = 0;
    const rt_gui_metric_token_t *descriptor = rt_gui_find_metric_token("motionEnabled", &index);
    palette->theme->motion.enabled = enabled != 0;
    if (descriptor)
        palette->invalid_metric_mask &= ~(UINT64_C(1) << index);
}

/// @brief Assign regular, bold, and monospace font roles atomically.
/// @details NULL clears a role. Every non-NULL argument must be a live GUI Font handle; otherwise
///          no role changes and Validate reports the first invalid role. Managed wrappers are
///          retained independently for each role; legacy raw handles remain borrowed. New managed
///          references are acquired before old ones are released, making same-handle replacement
///          safe even when the palette owns the last external reference.
/// @param palette_handle Live ThemePalette handle.
/// @param regular Nullable live proportional regular font.
/// @param bold Nullable live proportional bold font.
/// @param mono Nullable live monospace font.
void rt_theme_palette_set_font_roles(void *palette_handle, void *regular, void *bold, void *mono) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_theme_palette_t *palette = rt_gui_theme_palette_checked(palette_handle);
    if (!palette)
        return;
    vg_font_t *checked_regular = regular ? rt_gui_font_handle_checked(regular) : NULL;
    vg_font_t *checked_bold = bold ? rt_gui_font_handle_checked(bold) : NULL;
    vg_font_t *checked_mono = mono ? rt_gui_font_handle_checked(mono) : NULL;
    uint8_t invalid = (regular && !checked_regular ? 1u : 0u) | (bold && !checked_bold ? 2u : 0u) |
                      (mono && !checked_mono ? 4u : 0u);
    if (invalid) {
        palette->invalid_font_mask |= invalid;
        return;
    }
    void *new_handles[3] = {
        rt_gui_font_handle_is_managed(regular) ? regular : NULL,
        rt_gui_font_handle_is_managed(bold) ? bold : NULL,
        rt_gui_font_handle_is_managed(mono) ? mono : NULL,
    };
    for (size_t role = 0; role < 3; ++role) {
        if (new_handles[role])
            rt_obj_retain_maybe(new_handles[role]);
    }
    for (size_t role = 0; role < 3; ++role) {
        rt_gui_theme_release_font_handle(&palette->font_handles[role]);
        palette->font_handles[role] = new_handles[role];
    }
    palette->theme->typography.font_regular = checked_regular;
    palette->theme->typography.font_bold = checked_bold;
    palette->theme->typography.font_mono = checked_mono;
    palette->invalid_font_mask = 0;
}

/// @brief Validate the complete structural and normal-text accessibility contract of a palette.
/// @details The first invalid token is reported deterministically. Validation covers pending
///          invalid setter attempts, all numeric ranges/representations, font liveness, and WCAG
///          4.5:1 primary/secondary normal-text pairs. It does not mutate the palette.
/// @param palette_handle Live ThemePalette handle.
/// @return Caller-owned Result.Ok(1) when valid, or Result.ErrStr with the stable token diagnostic.
void *rt_theme_palette_validate(void *palette_handle) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_theme_palette_t *palette = rt_gui_theme_palette_checked(palette_handle);
    if (!palette)
        return rt_gui_theme_invalid_result("palette");
    const char *invalid = rt_gui_theme_palette_first_invalid(palette);
    return invalid ? rt_gui_theme_invalid_result(invalid) : rt_result_ok_i64(1);
}

/// @brief Set the active app's theme mode or the no-app fallback mode.
/// @details Valid values are Dark=0, Light=1, System=2, and Custom=3. Custom requires an active
///          app with an installed palette. No-app System mode samples the platform once and sets
///          the global toolkit theme; app System mode remains live and is synchronized per frame.
/// @param mode Requested stable ThemeMode integer; invalid values are ignored.
void rt_theme_set_mode(int64_t mode) {
    RT_ASSERT_MAIN_THREAD();
    if (mode < RT_GUI_THEME_DARK || mode > RT_GUI_THEME_CUSTOM)
        return;
    rt_gui_app_t *app = rt_gui_get_active_app();
    if (app) {
        rt_gui_set_theme_kind(app, (rt_gui_theme_kind_t)mode);
        return;
    }
    if (mode == RT_GUI_THEME_CUSTOM)
        return;
    int32_t prefers_dark = mode == RT_GUI_THEME_SYSTEM
                               ? (rt_gui_accessibility_platform_prefers_dark(NULL) ? 1 : 0)
                               : (mode == RT_GUI_THEME_DARK ? 1 : 0);
    bool changed = s_fallback_theme_mode != mode ||
                   (mode == RT_GUI_THEME_SYSTEM && s_fallback_theme_prefers_dark != prefers_dark);
    s_fallback_theme_mode = mode;
    s_fallback_theme_prefers_dark = prefers_dark;
    if (mode == RT_GUI_THEME_LIGHT)
        vg_theme_set_current(vg_theme_light());
    else if (mode == RT_GUI_THEME_SYSTEM && !prefers_dark)
        vg_theme_set_current(vg_theme_light());
    else
        vg_theme_set_current(vg_theme_dark());
    if (changed && s_fallback_theme_revision < UINT64_MAX)
        ++s_fallback_theme_revision;
}

/// @brief Return the active app's stable ThemeMode integer.
/// @return Dark=0, Light=1, System=2, Custom=3, or the no-app fallback mode.
int64_t rt_theme_get_mode(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    return app ? (int64_t)app->theme_kind : s_fallback_theme_mode;
}

/// @brief Select live platform-following theme behavior.
/// @details Equivalent to `Theme.SetMode(System)` and retained as a discoverable convenience.
void rt_theme_follow_system(void) {
    rt_theme_set_mode(RT_GUI_THEME_SYSTEM);
}

/// @brief Validate, clone, and atomically select a custom palette for the active app.
/// @details The managed palette remains independently mutable after return. A missing app, invalid
///          handle/value/font/contrast contract, or allocation failure returns false without
///          changing the app's selected custom palette. Successful installation rebuilds the
///          scaled/accessibility-adjusted theme and increments its revision.
/// @param palette_handle Live ThemePalette to copy.
/// @return One on successful installation, otherwise zero.
int64_t rt_theme_set_palette(void *palette_handle) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    rt_gui_theme_palette_t *palette = rt_gui_theme_palette_checked(palette_handle);
    if (!app || !palette || rt_gui_theme_palette_first_invalid(palette))
        return 0;
    vg_theme_t *candidate = vg_theme_create("Custom", palette->theme);
    if (!candidate)
        return 0;
    if (!rt_gui_install_custom_theme(app, candidate)) {
        vg_theme_destroy(candidate);
        return 0;
    }
    return 1;
}

/// @brief Snapshot the active app's logical palette before DPI/accessibility transformation.
/// @details The returned palette is independent and caller-owned. Without an active app, the
///          currently selected built-in fallback palette is cloned. Invalid global state falls
///          back to dark.
/// @return Caller-owned ThemePalette, or NULL on allocation failure.
void *rt_theme_get_palette(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    const vg_theme_t *base = NULL;
    if (app) {
        rt_gui_sync_system_theme(app);
        base = rt_gui_app_theme_base(app);
    } else {
        if (s_fallback_theme_mode == RT_GUI_THEME_LIGHT)
            base = vg_theme_light();
        else if (s_fallback_theme_mode == RT_GUI_THEME_SYSTEM) {
            int32_t prefers_dark = rt_gui_accessibility_platform_prefers_dark(NULL) ? 1 : 0;
            if (prefers_dark != s_fallback_theme_prefers_dark) {
                s_fallback_theme_prefers_dark = prefers_dark;
                if (s_fallback_theme_revision < UINT64_MAX)
                    ++s_fallback_theme_revision;
            }
            base = prefers_dark ? vg_theme_dark() : vg_theme_light();
        } else {
            base = vg_theme_dark();
        }
    }
    return rt_gui_theme_palette_create(base);
}

/// @brief Remove the active app's stored custom palette without affecting built-in themes.
/// @details If Custom mode is active, the app switches to Dark before the old palette is
///          destroyed, ensuring no installed/cache pointer observes reclaimed storage. Calls
///          without an app or without a stored palette are no-ops.
void rt_theme_reset_custom(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    if (!app || !app->custom_theme_base)
        return;
    vg_theme_t *old_custom = app->custom_theme_base;
    app->custom_theme_base = NULL;
    if (app->theme_kind == RT_GUI_THEME_CUSTOM) {
        app->theme_kind = RT_GUI_THEME_DARK;
        app->theme_base = NULL;
        app->theme_scale = 0.0f;
        rt_gui_refresh_theme(app);
    }
    vg_theme_destroy(old_custom);
}

/// @brief Consume the active theme's changed edge independently of revision observers.
/// @details A theme install caused by mode, palette, scale, accessibility, or System appearance
///          returns one once. Revision remains observable through GetRevision. No-app fallback
///          changes use an independent process cursor.
/// @return One exactly once per newly observed revision, otherwise zero.
int64_t rt_theme_was_changed(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    if (app) {
        if (app->theme_reported_revision == app->theme_revision)
            return 0;
        app->theme_reported_revision = app->theme_revision;
        return 1;
    }
    if (s_fallback_theme_reported_revision == s_fallback_theme_revision)
        return 0;
    s_fallback_theme_reported_revision = s_fallback_theme_revision;
    return 1;
}

/// @brief Read the non-consuming active theme revision.
/// @return Monotonic revision clamped to INT64_MAX for the public signed integer ABI.
int64_t rt_theme_get_revision(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_gui_app_t *app = rt_gui_get_active_app();
    uint64_t revision = app ? app->theme_revision : s_fallback_theme_revision;
    return rt_gui_saturating_u64_to_i64(revision);
}

#else

/// @brief Graphics-disabled ThemePalette constructor stub.
/// @return NULL because the GUI theme backend is unavailable.
void *rt_theme_palette_new(void) {
    return NULL;
}

/// @brief Graphics-disabled dark-palette constructor stub.
/// @return NULL because the GUI theme backend is unavailable.
void *rt_theme_palette_from_dark(void) {
    return NULL;
}

/// @brief Graphics-disabled light-palette constructor stub.
/// @return NULL because the GUI theme backend is unavailable.
void *rt_theme_palette_from_light(void) {
    return NULL;
}

/// @brief Graphics-disabled palette clone stub.
/// @param palette_handle Ignored unavailable palette.
/// @return NULL.
void *rt_theme_palette_clone(void *palette_handle) {
    (void)palette_handle;
    return NULL;
}

/// @brief Graphics-disabled color setter stub.
/// @param palette_handle Ignored unavailable palette.
/// @param token Ignored token.
/// @param rgb Ignored color.
/// @return Always zero.
int64_t rt_theme_palette_set_color(void *palette_handle, rt_string token, int64_t rgb) {
    (void)palette_handle;
    (void)token;
    (void)rgb;
    return 0;
}

/// @brief Graphics-disabled color getter stub.
/// @param palette_handle Ignored unavailable palette.
/// @param token Ignored token.
/// @return Always zero.
int64_t rt_theme_palette_get_color(void *palette_handle, rt_string token) {
    (void)palette_handle;
    (void)token;
    return 0;
}

/// @brief Graphics-disabled metric setter stub.
/// @param palette_handle Ignored unavailable palette.
/// @param token Ignored token.
/// @param value Ignored value.
/// @return Always zero.
int64_t rt_theme_palette_set_metric(void *palette_handle, rt_string token, double value) {
    (void)palette_handle;
    (void)token;
    (void)value;
    return 0;
}

/// @brief Graphics-disabled metric getter stub.
/// @param palette_handle Ignored unavailable palette.
/// @param token Ignored token.
/// @return Always zero.
double rt_theme_palette_get_metric(void *palette_handle, rt_string token) {
    (void)palette_handle;
    (void)token;
    return 0.0;
}

/// @brief Graphics-disabled motion setter stub.
/// @param palette_handle Ignored unavailable palette.
/// @param enabled Ignored flag.
void rt_theme_palette_set_motion_enabled(void *palette_handle, int64_t enabled) {
    (void)palette_handle;
    (void)enabled;
}

/// @brief Graphics-disabled font-role setter stub.
/// @param palette_handle Ignored unavailable palette.
/// @param regular Ignored font.
/// @param bold Ignored font.
/// @param mono Ignored font.
void rt_theme_palette_set_font_roles(void *palette_handle, void *regular, void *bold, void *mono) {
    (void)palette_handle;
    (void)regular;
    (void)bold;
    (void)mono;
}

/// @brief Graphics-disabled validation result.
/// @param palette_handle Ignored unavailable palette.
/// @return Caller-owned Result.ErrStr describing unavailable GUI support.
void *rt_theme_palette_validate(void *palette_handle) {
    (void)palette_handle;
    return rt_result_err_str(rt_const_cstr("GUI support is not available in this build"));
}

/// @brief Graphics-disabled theme mode setter stub.
/// @param mode Ignored mode.
void rt_theme_set_mode(int64_t mode) {
    (void)mode;
}

/// @brief Graphics-disabled theme mode query.
/// @return Dark mode's stable integer zero.
int64_t rt_theme_get_mode(void) {
    return RT_GUI_THEME_DARK;
}

/// @brief Graphics-disabled System-mode convenience stub.
void rt_theme_follow_system(void) {}

/// @brief Graphics-disabled custom palette installer stub.
/// @param palette_handle Ignored unavailable palette.
/// @return Always zero.
int64_t rt_theme_set_palette(void *palette_handle) {
    (void)palette_handle;
    return 0;
}

/// @brief Graphics-disabled logical palette snapshot stub.
/// @return NULL.
void *rt_theme_get_palette(void) {
    return NULL;
}

/// @brief Graphics-disabled custom palette reset stub.
void rt_theme_reset_custom(void) {}

/// @brief Graphics-disabled theme change edge query.
/// @return Always zero.
int64_t rt_theme_was_changed(void) {
    return 0;
}

/// @brief Graphics-disabled theme revision query.
/// @return Always zero.
int64_t rt_theme_get_revision(void) {
    return 0;
}

#endif
