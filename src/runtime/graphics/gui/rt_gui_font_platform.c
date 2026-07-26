//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_font_platform.c
// Purpose: Resolve proportional regular and bold system UI fonts through deterministic,
//          zero-dependency platform candidate lists.
//
// Key invariants:
//   - Only rt_platform.h selects the host-specific adapter branch.
//   - Candidate failure is isolated: every path is tried until one complete font parses.
//   - Embedded fallback and public Result construction are owned by the runtime binding layer.
//
// Ownership/Lifetime:
//   - vg_font_load_file owns and closes each attempted file internally.
//   - The first successful font is returned with ownership transferred to the caller.
//
// Links: src/runtime/graphics/gui/rt_gui_font_platform.h,
//        src/lib/gui/src/font/vg_font.c,
//        src/runtime/rt_platform.h
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_font_platform.c
/// @brief Implements deterministic host UI-font discovery without external dependencies.
///
/// @details
/// Platform-selected candidate lists are tried in preference order through
/// Zanna's built-in font loader, returning the first complete parsed face and
/// leaving embedded fallback policy to the caller.

#include "rt_gui_font_platform.h"
#include "rt_platform.h"
#include "vg_font.h"

#include <stddef.h>

/// @brief Try every font path in a NULL-terminated candidate array.
/// @param paths Borrowed immutable array of UTF-8 paths terminated by NULL.
/// @return First successfully parsed owned font, or NULL if every candidate fails.
static vg_font_t *rt_gui_font_load_first(const char *const *paths) {
    if (!paths)
        return NULL;
    for (size_t index = 0; paths[index]; ++index) {
        vg_font_t *font = vg_font_load_file(paths[index]);
        if (font)
            return font;
    }
    return NULL;
}

/// @brief Load a proportional system UI font through Zanna's dependency-free TrueType parser.
/// @details Candidate paths are ordered deterministically per supported platform. The function
///          creates no window or platform font object and leaves embedded fallback selection to
///          the caller when every candidate is absent or unsupported.
/// @param bold False for the regular UI role or true for the bold UI role.
/// @return Newly allocated live font owned by the caller, or NULL when no candidate can be parsed.
vg_font_t *rt_gui_font_platform_load_system_ui(bool bold) {
#if RT_PLATFORM_MACOS
    static const char *const regular_paths[] = {
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        NULL,
    };
    static const char *const bold_paths[] = {
        "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/System/Library/Fonts/SFNS.ttf",
        NULL,
    };
#elif RT_PLATFORM_WINDOWS
    static const char *const regular_paths[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "C:\\Windows\\Fonts\\tahoma.ttf",
        NULL,
    };
    static const char *const bold_paths[] = {
        "C:\\Windows\\Fonts\\segoeuib.ttf",
        "C:\\Windows\\Fonts\\arialbd.ttf",
        "C:\\Windows\\Fonts\\tahomabd.ttf",
        NULL,
    };
#elif RT_PLATFORM_LINUX
    static const char *const regular_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        NULL,
    };
    static const char *const bold_paths[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        NULL,
    };
#else
    static const char *const regular_paths[] = {NULL};
    static const char *const bold_paths[] = {NULL};
#endif
    return rt_gui_font_load_first(bold ? bold_paths : regular_paths);
}
