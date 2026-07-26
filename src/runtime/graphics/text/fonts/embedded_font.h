//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/text/fonts/embedded_font.h
// Purpose: Declares the generated JetBrains Mono Regular TTF payload used as
// the built-in GUI font and fallback font.
//
// Key invariants:
//   - Font data is the complete TTF file embedded as a byte array.
//   - JetBrains Mono is licensed under the SIL Open Font License 1.1.
//   - The array length is provided by vg_embedded_font_size.
//   - The data pointer is read-only; callers must not modify it.
//
// Ownership/Lifetime:
//   - The embedded font data is a global constant; no allocation or lifetime management.
//   - Callers borrow the pointer; they must not free it.
//
// Links: src/runtime/graphics/text/fonts/embedded_font.c,
//        scripts/embed_font.py,
//        src/runtime/graphics/gui/rt_gui_app.c,
//        src/runtime/graphics/gui/rt_gui_widgets.c
//
//===----------------------------------------------------------------------===//
#pragma once

/// @brief Raw bytes of the complete embedded JetBrains Mono Regular TTF file.
///
/// The array has static storage duration, is read-only, and contains exactly
/// @ref vg_embedded_font_size bytes. Pass it as borrowed input to a font
/// loader; do not modify or free it.
extern const unsigned char vg_embedded_font_data[];

/// @brief Exact number of valid bytes in @ref vg_embedded_font_data.
///
/// The value is generated alongside the byte array and is suitable for
/// conversion to the `size_t` length accepted by `vg_font_load()`.
extern const unsigned int vg_embedded_font_size;
