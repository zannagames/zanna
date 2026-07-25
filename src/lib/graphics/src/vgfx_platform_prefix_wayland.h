//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_platform_prefix_wayland.h
// Purpose: Namespace the Wayland adapter when linked into Linux AUTO builds.
// Key invariants: Every externally visible platform symbol is prefixed exactly once.
// Ownership/Lifetime: Preprocessor-only build adapter; owns no runtime state.
// Links: src/lib/graphics/src/vgfx_platform_linux_auto.c
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Configure Linux adapter ABI namespacing for the Wayland backend.
/// @details Defines the qualification transform consumed by
///          `vgfx_platform_prefix_symbols.h`, causing every external adapter
///          symbol in the Wayland translation unit to receive the
///          `vgfx_wayland_` prefix used by the runtime AUTO dispatcher.

#pragma once

/// @brief Qualify one external graphics platform symbol for the Wayland adapter.
/// @param name Unqualified adapter ABI symbol.
#define VGFX_PREFIXED(name) vgfx_wayland_##name
#include "vgfx_platform_prefix_symbols.h"
