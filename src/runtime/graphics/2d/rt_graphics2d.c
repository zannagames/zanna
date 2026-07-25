//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_graphics2d.c
// Purpose: Orchestrates the CPU-backed Zanna.Graphics 2D runtime fragments.
//
// Key invariants:
//   - Fragment include order preserves the original monolithic declaration
//     order so private static helpers remain visible to the same code.
//   - All drawing still lowers to rt_pixels_* primitives; no GPU resources are
//     owned by this CPU-backed layer.
//   - Public entry points and class ids remain ABI-compatible with the original
//     single-file implementation.
//
// Ownership/Lifetime:
//   - Runtime-allocated impl structs are freed by GC finalizers in the
//     fragments that define each class family.
//   - Renderer2D / DebugDraw2D command buffers own their command arrays; source
//     and target pixels are borrowed or retained according to existing APIs.
//
// Links: rt_graphics2d_core.inc, rt_graphics2d_surface.inc,
//        rt_graphics2d_texture_renderer.inc,
//        rt_graphics2d_material_fx_viewport.inc, rt_graphics2d_path.inc,
//        rt_graphics2d_extended.inc
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Assembles the CPU-backed implementation of the 2D graphics runtime.
///
/// The implementation is divided into ordered `.inc` fragments so that the
/// private types and static helpers from the former monolithic translation
/// unit remain visible to later subsystems without exporting them through an
/// internal header.  Consequently, these fragments are implementation details
/// and must be compiled only through this file.
///
/// The assembled translation unit provides render targets, textures, retained
/// drawing commands, materials and post-processing, paths and shapes, tiled
/// content, text, particles, lighting, collision, and debug drawing.  Public
/// ABI declarations live in @ref rt_graphics2d.h; shared private declarations
/// live in @ref rt_graphics2d_internal.h.

#include "rt_graphics2d.h"
#include "rt_graphics2d_internal.h"

#include "rt_bitmapfont.h"
#include "rt_camera.h"
#include "rt_graphics.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"
#include "rt_pixels_internal.h"
#include "rt_sprite.h"
#include "rt_texatlas.h"
#include "rt_tilemap.h"

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @name Ordered implementation fragments
/// @{
/// @brief Includes every 2D subsystem in dependency order.
///
/// Earlier fragments deliberately define private helpers used by later
/// fragments, so reordering or compiling an included fragment independently
/// can change visibility and initialization behavior.
// clang-format off
#include "rt_graphics2d_core.inc"
#include "rt_graphics2d_surface.inc"
#include "rt_graphics2d_texture_renderer.inc"
#include "rt_graphics2d_material_fx_viewport.inc"
#include "rt_graphics2d_path.inc"
#include "rt_graphics2d_extended.inc"
// clang-format on
/// @}
