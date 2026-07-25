//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/gui/src/widgets/vg_codeeditor.c
// Purpose: Orchestrates the CodeEditor widget implementation fragments.
//
// Key invariants:
//   - Fragment include order preserves the original monolithic declaration
//     order, static helper visibility, and vtable wiring.
//   - Text storage, undo/redo, rendering, folding, selection, and input APIs
//     keep the existing vg_codeeditor_t ABI and public behavior.
//   - Lines hidden by folds remain excluded from visual row calculations and
//     painting through the layout and paint fragments.
//
// Ownership/Lifetime:
//   - Each vg_code_line_t owns its text and colors buffers; the lifecycle
//     fragment releases lines, history, fold regions, inlay hints, gutter icons,
//     layout caches, and extra cursors.
//
// Links: src/lib/gui/include/vg_ide_widgets_editor.h,
//        src/lib/gui/src/widgets/vg_codeeditor_core.inc,
//        src/lib/gui/src/widgets/vg_codeeditor_history.inc,
//        src/lib/gui/src/widgets/vg_codeeditor_editing.inc,
//        src/lib/gui/src/widgets/vg_codeeditor_lifecycle.inc,
//        src/lib/gui/src/widgets/vg_codeeditor_paint.inc,
//        src/lib/gui/src/widgets/vg_codeeditor_input.inc,
//        src/lib/gui/src/widgets/vg_codeeditor_api.inc
//
//===----------------------------------------------------------------------===//
#include "../../../graphics/include/vgfx.h"
#include "../../include/vg_draw.h"
#include "../../include/vg_event.h"
#include "../../include/vg_ide_widgets.h"
#include "../../include/vg_theme.h"
#include "zanna_text_buffer.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Assembles the code-editor implementation from ordered private fragments.
/// @details The fragments intentionally share one translation unit so their static helpers,
/// forward declarations, vtable, and public definitions retain monolithic visibility and ordering.
/// The include sequence is therefore part of the implementation contract.

// clang-format off
#include "vg_codeeditor_core.inc"
#include "vg_codeeditor_history.inc"
#include "vg_codeeditor_editing.inc"
#include "vg_codeeditor_lifecycle.inc"
#include "vg_codeeditor_paint.inc"
#include "vg_codeeditor_input.inc"
#include "vg_codeeditor_api.inc"
#include "vg_codeeditor_buffer.inc"
// clang-format on
