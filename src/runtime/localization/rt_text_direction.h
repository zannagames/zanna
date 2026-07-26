//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_text_direction.h
// Purpose: Public C API for Zanna.Localization.TextDirection — a static
//          utility class that classifies text as left-to-right (LTR),
//          right-to-left (RTL), or mixed, based on UTF-8 codepoint scanning
//          against a fixed RTL-script range table.
//
// Key invariants:
//   - Codepoint classification: characters in recognized RTL script ranges
//     (Hebrew, Arabic, Syriac, Thaana, N'Ko, Samaritan, Mandaic, and related
//     extensions/presentation forms) are strong-RTL; Latin / Greek / Cyrillic
//     / CJK / etc. are strong-LTR; digits / punctuation / whitespace are neutral.
//   - IsRTL is true only for a strict RTL majority. IsLTR wins ties and treats
//     all-neutral, empty, and NULL input as LTR.
//   - Bidi(str) does NOT implement the full Unicode BiDi algorithm; it only
//     wraps RTL runs with U+2067 (RLI) / U+2069 (PDI) isolates so mixed-script
//     strings render with bounded direction. Full UBA is out of scope.
//
// Ownership/Lifetime:
//   - Static class; no instance state. All functions take explicit input
//     parameters and return owned strings.
//   - Bidi may retain and return its input handle unchanged when no isolation
//     is required; callers still own the returned reference.
//
// Links: src/runtime/localization/rt_text_direction.c (implementation),
//        docs/zannalib/localization/collation.md (user documentation).
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt.hpp"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Return the text direction declared by @p locale's data ("ltr"/"rtl").
/// @param locale Locale handle, or NULL for invariant locale data.
/// @return Newly allocated direction string; missing data falls back to `"ltr"`.
rt_string rt_text_direction_of_locale(void *locale);

/// @brief Detect text direction of @p s by scanning codepoints.
/// @param s Runtime UTF-8 string to scan, or NULL.
/// @return Newly allocated `"ltr"`, `"rtl"`, or `"mixed"`; NULL and empty
///         input return an owned empty string, while non-empty neutral text is LTR.
rt_string rt_text_direction_detect(rt_string s);

/// @brief True when the majority of strong codepoints in @p s are RTL.
/// @param s Runtime UTF-8 string to scan, or NULL.
/// @return 1 for a strict RTL majority; otherwise 0.
int8_t rt_text_direction_is_rtl(rt_string s);

/// @brief True when LTR strong codepoints equal or outnumber RTL ones.
/// @param s Runtime UTF-8 string to scan, or NULL.
/// @return 1 for an LTR majority, a tie, or neutral/empty/NULL input; otherwise 0.
int8_t rt_text_direction_is_ltr(rt_string s);

/// @brief Return "ltr"/"rtl"/"neutral" based on the first strong codepoint.
/// @param s Runtime UTF-8 string to scan, or NULL.
/// @return Newly allocated canonical direction string.
rt_string rt_text_direction_first_strong(rt_string s);

/// @brief Wrap mixed-script @p s with BiDi isolates when it contains
///        both LTR and RTL content. Pure-LTR and pure-RTL inputs pass
///        through unchanged.
/// @details Uses U+2067 RLI and U+2069 PDI around RTL runs. Builder failure
///          traps and yields an owned empty string if the trap hook returns.
/// @param s Runtime UTF-8 string to process, or NULL.
/// @return Owned bidi-safe string; may be the same retained handle as @p s
///         when no isolation is needed.
rt_string rt_text_direction_bidi(rt_string s);

#ifdef __cplusplus
}
#endif
