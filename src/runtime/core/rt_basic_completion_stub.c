//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_basic_completion_stub.c
/// @file
/// @brief Supplies weak, protocol-shaped fallbacks for BASIC language services.
///
// Purpose: Weak-symbol stubs for the Zanna BASIC IDE language-service bridge.
//          The real implementations live in
//          src/frontends/basic/rt_basic_completion.cpp (part of fe_basic). When
//          fe_basic is linked the linker prefers those strong symbols; binaries
//          that omit the frontend fall back to these stubs, which return empty,
//          protocol-shaped payloads — never false editor warnings.
// Key invariants:
//   - Stubs use __attribute__((weak)) on Clang/GCC; on MSVC the macro is empty
//     (MSVC builds always link fe_basic, so the stubs are never emitted there).
//   - Diagnostics/completion return an empty owned Seq (a missing analyzer is
//     not a source diagnostic).
//   - If fe_basic is linked, none of these run; the strong symbols win.
// Ownership/Lifetime:
//   - Input strings are borrowed and ignored by the fallback implementation.
//   - Every returned Seq, string, or Map is newly created and transfers its
//     runtime reference to the caller.
// Links: src/frontends/basic/rt_basic_completion.cpp (strong overrides),
//        src/runtime/graphics/common/rt_basic_completion.h
//
//===----------------------------------------------------------------------===//

#include "rt_string.h"

#include "rt_map.h"
#include "rt_platform.h" // RT_WEAK (weak-symbol linkage, compiler-abstracted)
#include "rt_seq.h"

#include <stdint.h>
#include <string.h>

/// @brief Returns the fallback diagnostic result when the BASIC frontend is absent.
/// @param source Borrowed BASIC source text; ignored by the stub.
/// @param file_path Borrowed source identity/path; ignored by the stub.
/// @return New empty owning Seq, representing a successful check with no diagnostics.
RT_WEAK void *rt_basic_toolchain_check_for_file(rt_string source, rt_string file_path) {
    (void)source;
    (void)file_path;
    return rt_seq_new_owned();
}

/// @brief Returns the fallback completion list when the BASIC frontend is absent.
/// @param source Borrowed BASIC source text; ignored by the stub.
/// @param file_path Borrowed source identity/path; ignored by the stub.
/// @param line Zero-based or protocol-defined cursor line; ignored by the stub.
/// @param col Zero-based or protocol-defined cursor column; ignored by the stub.
/// @return New empty owning Seq of completion items.
RT_WEAK void *rt_basic_completion_items_for_file(rt_string source,
                                                 rt_string file_path,
                                                 int64_t line,
                                                 int64_t col) {
    (void)source;
    (void)file_path;
    (void)line;
    (void)col;
    return rt_seq_new_owned();
}

/// @brief Returns the fallback serialized symbol payload.
/// @param source Borrowed BASIC source text; ignored by the stub.
/// @param file_path Borrowed source identity/path; ignored by the stub.
/// @return New empty runtime string, representing no available symbols.
RT_WEAK rt_string rt_basic_completion_symbols_for_file(rt_string source, rt_string file_path) {
    (void)source;
    (void)file_path;
    return rt_string_from_bytes("", 0);
}

/// @brief Returns a protocol-shaped unavailable-hover response.
/// @param source Borrowed BASIC source text; ignored by the stub.
/// @param file_path Borrowed source identity/path; ignored by the stub.
/// @param line Cursor line; ignored by the stub.
/// @param col Cursor column; ignored by the stub.
/// @return New Map containing boolean field `available = false`.
RT_WEAK void *rt_basic_completion_hover_info_for_file(rt_string source,
                                                      rt_string file_path,
                                                      int64_t line,
                                                      int64_t col) {
    (void)source;
    (void)file_path;
    (void)line;
    (void)col;
    void *map = rt_map_new();
    rt_string key = rt_string_from_bytes("available", strlen("available"));
    rt_map_set_bool(map, key, 0);
    rt_string_unref(key);
    return map;
}
