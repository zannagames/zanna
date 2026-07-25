//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_ide_widgets_common.h
// Purpose: Shared types and forward declarations for IDE widget sub-headers.
//          Extracted from vg_ide_widgets.h to avoid circular dependencies
//          between the split headers.
// Key invariants:
//   - vg_icon_t is the canonical icon type used across all IDE widgets.
//   - String parameters are copied internally unless documented otherwise.
// Ownership/Lifetime:
//   - vg_icon_destroy frees any resources owned by an icon.
// Links: lib/gui/include/vg_ide_widgets.h,
//        lib/gui/include/vg_widget.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "vg_font.h"
#include "vg_layout.h"
#include "vg_widget.h"

/// @file
/// @brief Declares IDE-widget forward types and the shared icon value.
/// @details This dependency-light header breaks cycles among the split IDE widget APIs.
/// The icon abstraction can represent no icon, a Unicode glyph, copied pixel data, a copied
/// file path, or a registry-owned vector icon id.

#ifdef __cplusplus
extern "C" {
#endif

// Forward declarations used across IDE widget headers
/// @brief Opaque item belonging to an IDE menu.
typedef struct vg_menu_item vg_menu_item_t;

/// @brief Opaque IDE menu widget.
typedef struct vg_menu vg_menu_t;

/// @brief Opaque contextual menu widget.
typedef struct vg_contextmenu vg_contextmenu_t;

/// @brief Opaque tree-view widget.
struct vg_treeview;

/// @brief Opaque code-editor widget.
struct vg_codeeditor;

/// @brief Opaque text-input widget.
struct vg_textinput;

/// @brief Opaque button widget.
struct vg_button;

/// @brief Opaque checkbox widget.
struct vg_checkbox;

//=========================================================================
// Icon — shared icon type used by Toolbar, TreeView, Dialog, Breadcrumb,
//        CommandPalette, and others.
//=========================================================================

/// @brief Copyable tagged specification for an IDE widget icon.
/// @details Pixel and path variants may own heap data and must eventually be released with
/// @ref vg_icon_destroy. Glyph and vector variants contain scalar values only.
typedef struct vg_icon {
    /// @brief Discriminator selecting the active member of @ref data.
    enum {
        VG_ICON_NONE,  ///< No icon
        VG_ICON_GLYPH, ///< Unicode character
        VG_ICON_IMAGE, ///< Pixel data
        VG_ICON_PATH,  ///< File path
        VG_ICON_VECTOR ///< Named scalable vector icon (vg_icon_vector library)
    } type;

    /// @brief Payload selected by @ref type.
    union {
        uint32_t glyph; ///< Unicode codepoint

        /// @brief Owned RGBA image and its pixel dimensions.
        struct {
            uint8_t *pixels; ///< RGBA pixel data

            /// @brief Image width in pixels.
            uint32_t width;

            /// @brief Image height in pixels.
            uint32_t height;
        } image;

        char *path; ///< File path

        int32_t vector_id; ///< Vector icon id from vg_icon_vector_find
    } data;
} vg_icon_t;

/// @brief Create a non-owning icon specification from a Unicode glyph.
/// @param codepoint Unicode scalar value to render through the widget font.
/// @return Glyph icon specification requiring no destruction resources.
vg_icon_t vg_icon_from_glyph(uint32_t codepoint);

/// @brief Create an image icon by copying caller-supplied pixel data.
/// @param rgba Tightly packed RGBA8 pixels to copy.
/// @param w Image width in pixels.
/// @param h Image height in pixels.
/// @return Owned image icon, or @ref VG_ICON_NONE when input or allocation fails.
vg_icon_t vg_icon_from_pixels(uint8_t *rgba, uint32_t w, uint32_t h);

/// @brief Create an icon referencing a named scalable vector icon.
/// @param vector_id Icon id from vg_icon_vector_find; a negative id yields @ref VG_ICON_NONE.
/// @return Vector icon specification requiring no heap allocation.
vg_icon_t vg_icon_from_vector(int32_t vector_id);

/// @brief Create a file-backed icon specification by copying a path.
/// @param path NUL-terminated file path to copy.
/// @return Owned path icon, or @ref VG_ICON_NONE when input or allocation fails.
vg_icon_t vg_icon_from_file(const char *path);

/// @brief Deep-copy an icon and any owned payload.
/// @param icon Icon to copy.
/// @return Independent icon value, or @ref VG_ICON_NONE for null input or allocation failure.
vg_icon_t vg_icon_clone(const vg_icon_t *icon);

/// @brief Release an icon's owned resources and reset it to @ref VG_ICON_NONE.
/// @param icon Icon to destroy; NULL is ignored.
void vg_icon_destroy(vg_icon_t *icon);

#ifdef __cplusplus
}
#endif
