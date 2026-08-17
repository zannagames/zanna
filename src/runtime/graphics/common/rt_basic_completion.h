//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file rt_basic_completion.h
/// @brief Declares the C ABI for BASIC diagnostics and editor-language services.
///
/// @details
/// These entry points mirror the Zia language-service bridge so IDE consumers
/// can request diagnostics, completions, document symbols, and hover data
/// without depending on frontend-specific C++ types. Strong implementations
/// live in the BASIC frontend; weak runtime stubs preserve linkability for
/// binaries that omit that frontend.
///
// File: src/runtime/graphics/common/rt_basic_completion.h
// Purpose: C ABI for the Zanna BASIC IDE language-service bridge (completion,
//          diagnostics, hover, symbols), mirroring the Zia bridge. The strong
//          implementations live in src/frontends/basic/rt_basic_completion.cpp
//          (part of fe_basic); weak stubs in src/runtime/core/
//          rt_basic_completion_stub.c keep zanna_runtime frontend-free. Symbols
//          resolve at final link when the binary links both fe_basic and
//          zanna_runtime.
// Key invariants:
//   - All functions are side-effect-free queries over a source string + path.
//   - Returned runtime objects (Seq/Map) and rt_string are owned by the caller.
//   - Result shapes match the Zia bridge so the IDE controllers consume both.
// Links: src/frontends/basic/rt_basic_completion.cpp,
//        src/runtime/graphics/common/rt_zia_completion.h,
//        docs/adr/0014-basic-language-service-runtime-bridge.md
//
//===----------------------------------------------------------------------===//
#ifndef ZANNA_RT_BASIC_COMPLETION_H
#define ZANNA_RT_BASIC_COMPLETION_H

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Collect diagnostics for BASIC source associated with a virtual file.
///
/// The returned sequence contains maps with the same schema as the Zia
/// toolchain, including file, range, severity, diagnostic code, message, and
/// compilation-stage fields.
///
/// @param source    BASIC source text to analyze.
/// @param file_path Virtual path used in diagnostics and source locations.
///
/// @return An owned runtime sequence of diagnostic maps; the caller owns the
///         returned runtime object.
void *rt_basic_toolchain_check_for_file(rt_string source, rt_string file_path);

/// @brief Collect BASIC completion items at a source position.
///
/// Completion maps include label, insertion text, kind, detail,
/// documentation, cursor offset, and replacement-range fields.
///
/// @param source    BASIC source text to analyze.
/// @param file_path Virtual path associated with the source.
/// @param line      One-based source line containing the cursor.
/// @param col       One-based source column containing the cursor.
///
/// @return An owned runtime sequence of completion-item maps; the caller owns
///         the returned runtime object.
void *rt_basic_completion_items_for_file(rt_string source,
                                         rt_string file_path,
                                         int64_t line,
                                         int64_t col);

/// @brief Serialize the document symbols declared by BASIC source.
///
/// @param source    BASIC source text to inspect.
/// @param file_path Virtual path associated with the source.
///
/// @return An owned tab-delimited runtime string with one
///         `name\tkind\ttype\tline` record per line.
rt_string rt_basic_completion_symbols_for_file(rt_string source, rt_string file_path);

/// @brief Collect hover information for the BASIC identifier at a source position.
///
/// @param source    BASIC source text to analyze.
/// @param file_path Virtual path associated with the source.
/// @param line      One-based source line containing the cursor.
/// @param col       Zero-based source column containing the cursor.
///
/// @return An owned runtime map containing availability, title, type, display,
///         and source fields; the caller owns the returned runtime object.
void *rt_basic_completion_hover_info_for_file(rt_string source,
                                              rt_string file_path,
                                              int64_t line,
                                              int64_t col);

#ifdef __cplusplus
}
#endif

#endif // ZANNA_RT_BASIC_COMPLETION_H
