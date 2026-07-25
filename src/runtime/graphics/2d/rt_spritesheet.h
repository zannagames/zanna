//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_spritesheet.h
// Purpose: Public C ABI for defining, generating, querying, enumerating,
// removing, and independently extracting named rectangular regions from one
// retained Pixels atlas.
//
// Key invariants:
//   - Regions are stored by name; duplicate names overwrite the previous region.
//   - Name lookup is case-sensitive and enumeration preserves insertion order.
//   - rt_spritesheet_get_region extracts a region into a new Pixels buffer (a copy),
//     returning NULL when the named region does not exist.
//   - All regions must reference valid coordinates within the atlas bounds.
//     Invalid or empty definitions are silently ignored.
//   - The atlas Pixels object is retained while the spritesheet is in use.
//
// Ownership/Lifetime:
//   - Constructors return caller-owned runtime reference-counted objects.
//   - The SpriteSheet owns copied C region names and retains the atlas Pixels.
//   - Region extraction and name enumeration return newly owned runtime objects.
//
// Links: src/runtime/graphics/2d/rt_spritesheet.c (implementation),
//        src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @file
/// @brief Declares the named-region SpriteSheet runtime interface.

/// Runtime class tag used to validate opaque SpriteSheet handles.
#define RT_SPRITESHEET_CLASS_ID INT64_C(-0x520102)

/// @brief Create a new sprite sheet from an atlas Pixels buffer.
/// @details The sheet starts with no regions and retains the atlas.
/// @param atlas_pixels Valid Pixels object containing the full atlas.
/// @return A caller-owned SpriteSheet, or `NULL` for invalid input or
///         allocation failure.
void *rt_spritesheet_new(void *atlas_pixels);

/// @brief Create a sprite sheet with uniform grid layout.
/// @details Atlas dimensions must be exactly divisible by the positive cell
///          dimensions. Regions are named `"0"`, `"1"`, and so on in row-major
///          order. A later metadata-allocation failure can return a partial sheet.
/// @param atlas_pixels Valid Pixels atlas retained by the returned sheet.
/// @param frame_w Positive width of each cell.
/// @param frame_h Positive height of each cell.
/// @return A caller-owned SpriteSheet, or `NULL` for invalid geometry,
///         arithmetic overflow, or initial allocation failure.
void *rt_spritesheet_from_grid(void *atlas_pixels, int64_t frame_w, int64_t frame_h);

/// @brief Define a named region within the atlas.
/// @details An existing case-sensitive name is updated in place. A new name is
///          copied and appended. Invalid sheets/strings, empty names, and
///          nonpositive or out-of-bounds rectangles are silent no-ops.
/// @param sheet Candidate SpriteSheet handle.
/// @param name Borrowed runtime region name, such as `"walk_0"`.
/// @param x Nonnegative atlas-space left edge.
/// @param y Nonnegative atlas-space top edge.
/// @param w Positive region width.
/// @param h Positive region height.
void rt_spritesheet_set_region(
    void *sheet, rt_string name, int64_t x, int64_t y, int64_t w, int64_t h);

/// @brief Extract a named region as a new Pixels buffer.
/// @details The result is an independent copy; mutating it does not affect the
///          atlas or later extractions.
/// @param sheet Candidate SpriteSheet handle.
/// @param name Borrowed runtime string containing the case-sensitive name.
/// @return A caller-owned Pixels object, or `NULL` for invalid input, a missing
///         region, or allocation failure.
void *rt_spritesheet_get_region(void *sheet, rt_string name);

/// @brief Check if a region name exists.
/// @param sheet Candidate SpriteSheet handle.
/// @param name Borrowed runtime string containing the case-sensitive name.
/// @return `1` if the name exists, otherwise `0`.
int8_t rt_spritesheet_has_region(void *sheet, rt_string name);

/// @brief Get the number of defined regions.
/// @param sheet Candidate SpriteSheet handle.
/// @return Number of regions, or `0` for an invalid sheet.
int64_t rt_spritesheet_region_count(void *sheet);

/// @brief Get the width of the underlying atlas.
/// @param sheet Candidate SpriteSheet handle.
/// @return Atlas width in pixels, or `0` for an invalid sheet.
int64_t rt_spritesheet_width(void *sheet);

/// @brief Get the height of the underlying atlas.
/// @param sheet Candidate SpriteSheet handle.
/// @return Atlas height in pixels, or `0` for an invalid sheet.
int64_t rt_spritesheet_height(void *sheet);

/// @brief Get all region names as a Seq.
/// @details Names appear in insertion order. Replacing an existing region does
///          not move it; removing one shifts later names left.
/// @param sheet Candidate SpriteSheet handle.
/// @return A caller-owned Seq of runtime strings. Invalid sheets yield a newly
///         allocated empty Seq.
void *rt_spritesheet_region_names(void *sheet);

/// @brief Remove a named region.
/// @param sheet Candidate SpriteSheet handle.
/// @param name Borrowed runtime string containing the case-sensitive name.
/// @return `1` if removed, or `0` for invalid input or a missing name.
int8_t rt_spritesheet_remove_region(void *sheet, rt_string name);

#ifdef __cplusplus
}
#endif
