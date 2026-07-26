//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/gui/rt_gui_linux_portal.h
// Purpose: Declare the dependency-free bridge for bounded synchronous reads from the Linux desktop
//          Settings portal.
// Key invariants:
//   - No GLib/GIO headers or link-time dependency are required.
//   - Loader availability is initialized once and remains immutable for process lifetime.
//   - Portal calls use a finite timeout and accept only Boolean or representable uint32 variants.
//   - Unavailable buses, portals, keys, and unexpected variants return not-found without changing
//     the caller's output.
// Ownership/Lifetime:
//   - Namespace and key strings are borrowed only for the duration of a query.
//   - Every temporary GIO object is released before a query returns; the successfully loaded shared
//     library remains open for process lifetime to keep its cached function table valid.
// Links: org.freedesktop.portal.Settings, docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

/// @brief Read one unsigned or Boolean Settings portal value as a signed integer.
/// @details Performs an optional late-bound session-bus query with a short finite timeout. The
///          function never requires callers to include GLib/GIO types and treats missing services,
///          malformed variants, unsupported types, and unsigned values above @c INT32_MAX as a
///          recoverable not-found result.
/// @param name_space Borrowed non-NULL NUL-terminated portal namespace.
/// @param key Borrowed non-NULL NUL-terminated setting key within @p name_space.
/// @param[out] out_value Receives the decoded integer or normalized Boolean only on success and is
///                       otherwise left unchanged.
/// @return One when a supported value was decoded into @p out_value, otherwise zero.
int rt_gui_linux_portal_read(const char *name_space, const char *key, int32_t *out_value);
