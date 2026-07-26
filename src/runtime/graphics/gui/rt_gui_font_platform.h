//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_font_platform.h
// Purpose: Declare the zero-dependency platform adapter used to resolve proportional system UI
//          font faces for the GUI runtime.
//
// Key invariants:
//   - The adapter performs no GUI/window initialization and is safe in headless test processes.
//   - It returns only fully parsed live fonts; fallback policy remains in the caller.
//   - Platform selection uses rt_platform.h rather than raw operating-system preprocessor checks.
//
// Ownership/Lifetime:
//   - Successful calls transfer one owned vg_font_t to the caller.
//   - Failed calls return NULL and retain no file handles or heap allocations.
//
// Links: src/runtime/graphics/gui/rt_gui_font_platform.c,
//        src/runtime/graphics/gui/rt_gui_widgets.c,
//        docs/adr/0107-gui-theme-accessibility-input-and-render-policy.md
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_font_platform.h
/// @brief Declares the dependency-free platform adapter for proportional system UI fonts.
///
/// @details
/// The narrow interface transfers ownership of successfully parsed fonts while
/// avoiding GUI initialization and backend-specific type dependencies.
#pragma once

#include <stdbool.h>

/// @brief Opaque lower-toolkit font type returned by the platform adapter.
/// @details The complete ZannaGUI font definition is deliberately excluded
///          from this narrow adapter contract so graphics-disabled runtime
///          sources do not acquire a backend include-path dependency.
typedef struct vg_font vg_font_t;

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Load the regular or bold proportional system UI face for the host platform.
/// @details Candidate files are tried in deterministic platform-preference order. The adapter
///          uses only Zanna's built-in TrueType parser and ordinary system font files; it never
///          links CoreText, DirectWrite, Fontconfig, or another external dependency. A missing or
///          unsupported host face is reported as NULL so the runtime can install its embedded
///          deterministic fallback.
/// @param bold False for the regular UI role; true for the bold UI role.
/// @return Newly allocated live font owned by the caller, or NULL when no candidate parses.
vg_font_t *rt_gui_font_platform_load_system_ui(bool bold);

#ifdef __cplusplus
}
#endif
