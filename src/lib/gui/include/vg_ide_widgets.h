//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/include/vg_ide_widgets.h
// Purpose: Umbrella header for the IDE-specific widget library — includes all
//          sub-headers to preserve backward compatibility. Individual sub-headers
//          may be included directly for faster compilation.
// Key invariants:
//   - All create functions return NULL on allocation failure.
//   - String parameters are copied internally unless documented otherwise.
//   - Callbacks receive a user_data pointer that the widget never dereferences.
// Ownership/Lifetime:
//   - Widgets are owned by their parent in the widget tree; destroying the
//     parent recursively destroys children.
//   - Some widgets (Dialog, ContextMenu, CommandPalette) may be created
//     without a parent and must be explicitly destroyed.
// Links: lib/gui/include/vg_ide_widgets_common.h,
//        lib/gui/include/vg_ide_widgets_ui.h,
//        lib/gui/include/vg_ide_widgets_dialog.h,
//        lib/gui/include/vg_ide_widgets_editor.h,
//        lib/gui/include/vg_ide_widgets_tree.h,
//        lib/gui/include/vg_ide_widgets_panels.h
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Provides one compatibility include for the complete IDE widget API.
/// @details Including this header exposes shared IDE types plus dialog, editor, panel, tree,
/// and general UI widgets. Applications with narrower needs may include the corresponding
/// component header directly.

#ifndef VG_IDE_WIDGETS_H
#define VG_IDE_WIDGETS_H

#include "vg_ide_widgets_common.h"
#include "vg_ide_widgets_dialog.h"
#include "vg_ide_widgets_editor.h"
#include "vg_ide_widgets_panels.h"
#include "vg_ide_widgets_tree.h"
#include "vg_ide_widgets_ui.h"

#endif /* VG_IDE_WIDGETS_H */
