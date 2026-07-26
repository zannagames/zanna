//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_graphics_stubs.c
/// @brief Anchors the split graphics-disabled runtime implementation.
///
/// @details
/// Public fallback entry points live in focused Canvas, Canvas3D, asset,
/// physics, scene, world, media, and helper stub translation units. This
/// source intentionally exports no functions but preserves the historical
/// build target and common internal-header dependency.
///
// File: src/runtime/graphics/common/rt_graphics_stubs.c
// Purpose: Compatibility anchor for the graphics-disabled runtime stubs.
//
// Key invariants:
//   - This file intentionally contains no public runtime entry points.
//   - Disabled graphics APIs are implemented in the focused split files next
//     to this anchor and wired through src/runtime/CMakeLists.txt.
//
// Ownership/Lifetime:
//   - No resources are allocated here.
//
// Links: src/runtime/graphics/common/rt_graphics_stubs_internal.h,
//        src/runtime/graphics/common/rt_canvas_stubs.c,
//        src/runtime/graphics/common/rt_canvas3d_stubs.c
//
//===----------------------------------------------------------------------===//

#include "rt_graphics_stubs_internal.h"
