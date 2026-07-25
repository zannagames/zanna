//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_platform_prefix_x11.h
// Purpose: Namespace the X11 adapter when linked into Linux AUTO builds.
// Key invariants: Every externally visible platform symbol is prefixed exactly once.
// Ownership/Lifetime: Preprocessor-only build adapter; owns no runtime state.
// Links: src/lib/graphics/src/vgfx_platform_linux_auto.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Configure Linux adapter ABI namespacing for the X11 backend.
/// @details Defines the qualification transform consumed by
///          `vgfx_platform_prefix_symbols.h`, causing every external adapter
///          symbol in the X11 translation unit to receive the `vgfx_x11_`
///          prefix used by the runtime AUTO dispatcher.

#pragma once

/// @brief Qualify one external graphics platform symbol for the X11 adapter.
/// @param name Unqualified adapter ABI symbol.
#define VGFX_PREFIXED(name) vgfx_x11_##name
#include "vgfx_platform_prefix_symbols.h"
