//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_string.c
// Purpose: Translation-unit placeholder for the `rt_string` runtime API.
//          The implementation is split across:
//            - rt_string_ops.c         — core constructors, refcount, comparison
//            - rt_string_advanced.c    — find/replace/split/format helpers
//            - rt_string_builder.c     — `StringBuilder` runtime class
//            - rt_string_encode.c      — UTF-8 validation and codec entry points
//            - rt_string_format.c      — printf-style formatting for runtime types
//            - rt_string_intern.c      — string interning table and lookup
//            - rt_string_specialized.c — opcode-specialised fast paths
//          The public C ABI is declared in rt_string.h.
//
// Key invariants:
//   - This file intentionally contains no symbols. CMake includes it in the
//     runtime library target so the file path remains valid as a build input;
//     adding a definition here would create a duplicate symbol with one of
//     the *_ops / *_advanced / *_builder etc. translation units.
//
// Ownership/Lifetime:
//   - N/A — no state, no functions.
//
// Links: src/runtime/core/rt_string.h,
//        src/runtime/core/rt_string_ops.c (primary impl),
//        src/runtime/core/rt_string_advanced.c,
//        src/runtime/core/rt_string_builder.c,
//        src/runtime/core/rt_string_encode.c,
//        src/runtime/core/rt_string_format.c,
//        src/runtime/core/rt_string_intern.c,
//        src/runtime/core/rt_string_specialized.c
//
//===----------------------------------------------------------------------===//
/// @file
/// @brief Symbol-free build placeholder and implementation map for rt_string.h.
