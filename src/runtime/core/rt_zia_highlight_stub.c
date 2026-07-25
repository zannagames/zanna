//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_zia_highlight_stub.c
// Purpose: Weak-symbol stub for the Zia syntax-highlight keyword bridge. The
//          real implementation lives in src/frontends/zia/rt_zia_highlight.cpp
//          (part of zia_editor_services) and consults the authoritative kKeywordTable.
//          When a test binary or other consumer links zanna_runtime without
//          editor services, the linker falls back to this stub which
//          conservatively reports "not a keyword" for every identifier.
//
// Key invariants:
//   - The stub uses __attribute__((weak)) on Clang/GCC (macOS, Linux); on
//     MSVC the macro expands to nothing because production Windows builds
//     always link zia_editor_services. (Mirrors the pattern in
//     rt_zia_completion_stub.c.)
//   - rt_zia_is_keyword stub returns 0 unconditionally. The highlighter
//     gracefully degrades to "no keyword coloring" rather than failing to
//     link or crashing.
//   - If zia_editor_services is linked, this stub is overridden; the strong
//     symbol in rt_zia_highlight.cpp takes precedence.
//
// Ownership/Lifetime:
//   - No allocation, no state. Pure function.
//
// Links: src/frontends/zia/rt_zia_highlight.cpp (strong-symbol override),
//        src/runtime/graphics/rt_gui_codeeditor.c (consumer)
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Weak no-keyword fallback for the Zia syntax-highlighting bridge.

#include <stdint.h>

#ifndef _MSC_VER
#define RT_WEAK __attribute__((weak))
#else
// MSVC: production Windows builds always link zia_editor_services, so no weak
// fallback is needed. A duplicate-symbol error here is louder than a silent stub.
#define RT_WEAK
#endif

/// @brief Weak stub: report no identifier as a keyword.
/// @details The fallback deliberately does not inspect or dereference the input,
///          so null pointers and any length are safe. A strong editor-service
///          definition overrides this symbol on platforms with weak linkage.
/// @param name Borrowed identifier bytes; ignored and may be NULL.
/// @param len Number of identifier bytes; ignored.
/// @return Always zero.
RT_WEAK int rt_zia_is_keyword(const char *name, int64_t len) {
    (void)name;
    (void)len;
    return 0;
}
