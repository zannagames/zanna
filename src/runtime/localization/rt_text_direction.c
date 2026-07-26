//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_text_direction.c
// Purpose: Implementation of Zanna.Localization.TextDirection. Walks UTF-8
//          strings, decodes each codepoint, and classifies it as strong-LTR,
//          strong-RTL, or neutral based on a small range table. Callers
//          typically use the results to mirror layouts or pick the right
//          BiDi override marks for mixed-content strings.
//
// Key invariants:
//   - The RTL range table covers Hebrew, Arabic-family scripts and
//     presentation forms, Syriac, Thaana, N'Ko, Samaritan, Mandaic, and
//     selected historic RTL blocks. Other non-neutral code points are LTR.
//   - "Mixed" result implies at least one strong codepoint of each
//     direction somewhere in the string. Leading neutrals are ignored
//     for FirstStrong.
//
// Ownership/Lifetime:
//   - Returned rt_strings are fresh allocations owned by the caller.
//
// Links: src/runtime/localization/rt_text_direction.h (interface),
//        src/runtime/localization/rt_locale.h (OfLocale delegate).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements UTF-8 text-direction detection and bidi isolation.
 * @details Strictly advances through malformed UTF-8, classifies strong and
 * neutral code points with fixed script ranges, computes aggregate and
 * first-strong direction, and inserts bounded RTL isolate markers into mixed
 * text without implementing the full Unicode bidi algorithm.
 */

#include "rt_text_direction.h"

#include "rt_internal.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include <stdint.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// UTF-8 codepoint decode
//===----------------------------------------------------------------------===//

/// @brief Decode a single UTF-8 codepoint from @p s at offset @p *pos.
/// @details Advances @p *pos by the codepoint's byte length on success. On
///          malformed input, advances by 1 and returns 0xFFFD (replacement).
/// @param s Input UTF-8 byte sequence.
/// @param len Number of available bytes in @p s.
/// @param pos In/out byte offset. Advanced to the next decoding position.
/// @return Decoded scalar value, U+FFFD for malformed input, or zero at end.
static uint32_t decode_codepoint(const char *s, size_t len, size_t *pos) {
    size_t i = *pos;
    if (i >= len)
        return 0;
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    size_t need;
    uint32_t min_cp = 0;
    if (c < 0x80) {
        cp = c;
        need = 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        cp = c & 0x1F;
        need = 2;
        min_cp = 0x80;
    } else if (c >= 0xE0 && c <= 0xEF) {
        cp = c & 0x0F;
        need = 3;
        min_cp = 0x800;
    } else if (c >= 0xF0 && c <= 0xF4) {
        cp = c & 0x07;
        need = 4;
        min_cp = 0x10000;
    } else {
        *pos = i + 1;
        return 0xFFFD;
    }
    if (i + need > len) {
        *pos = len;
        return 0xFFFD;
    }
    for (size_t k = 1; k < need; ++k) {
        uint8_t nc = (uint8_t)s[i + k];
        if ((nc & 0xC0) != 0x80) {
            *pos = i + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (nc & 0x3F);
    }
    if ((need > 1 && cp < min_cp) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        *pos = i + 1;
        return 0xFFFD;
    }
    *pos = i + need;
    return cp;
}

//===----------------------------------------------------------------------===//
// Directional classification
//===----------------------------------------------------------------------===//

/// @brief Reduced strong-direction classification used by the scanner.
typedef enum {
    DIR_NEUTRAL = 0,
    DIR_LTR,
    DIR_RTL,
} cp_dir_t;

/// @brief Classify a codepoint's bidi direction.
/// @details Checks known weak/neutral marks before the fixed RTL script and
///          presentation-form ranges. Common punctuation, controls, digits,
///          and combining-mark blocks are neutral; remaining values are LTR.
/// @param cp Unicode scalar value to classify.
/// @return Strong LTR, strong RTL, or neutral according to the fixed table.
static cp_dir_t classify(uint32_t cp) {
    // Weak and neutral characters inside the RTL blocks (VDOC-074): digits
    // with bidi class AN and combining marks (NSM) are not strong, so they
    // must not decide FirstStrong or count as directional content.
    if ((cp >= 0x0660 && cp <= 0x0669) || (cp >= 0x06F0 && cp <= 0x06F9))
        return DIR_NEUTRAL; // Arabic-Indic digits (bidi class AN)
    if ((cp >= 0x0591 && cp <= 0x05BD) || cp == 0x05BF || cp == 0x05C1 || cp == 0x05C2 ||
        cp == 0x05C4 || cp == 0x05C5 || cp == 0x05C7)
        return DIR_NEUTRAL; // Hebrew points (NSM)
    if ((cp >= 0x0610 && cp <= 0x061A) || (cp >= 0x064B && cp <= 0x065F) || cp == 0x0670 ||
        (cp >= 0x06D6 && cp <= 0x06DC) || (cp >= 0x06DF && cp <= 0x06E4) || cp == 0x06E7 ||
        cp == 0x06E8 || (cp >= 0x06EA && cp <= 0x06ED))
        return DIR_NEUTRAL; // Arabic combining marks (NSM)

    // Generic combining-mark blocks (NSM) outside the RTL ranges.
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x0483 && cp <= 0x0489) ||
        (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
        (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F))
        return DIR_NEUTRAL;

    // RTL scripts.
    if (cp >= 0x0590 && cp <= 0x05FF)
        return DIR_RTL; // Hebrew
    if (cp >= 0x0600 && cp <= 0x06FF)
        return DIR_RTL; // Arabic
    if (cp >= 0x0700 && cp <= 0x074F)
        return DIR_RTL; // Syriac
    if (cp >= 0x0750 && cp <= 0x077F)
        return DIR_RTL; // Arabic Supplement
    if (cp >= 0x0780 && cp <= 0x07BF)
        return DIR_RTL; // Thaana
    if (cp >= 0x07C0 && cp <= 0x07FF)
        return DIR_RTL; // N'Ko
    if (cp >= 0x0800 && cp <= 0x083F)
        return DIR_RTL; // Samaritan
    if (cp >= 0x0840 && cp <= 0x085F)
        return DIR_RTL; // Mandaic
    if (cp >= 0x0860 && cp <= 0x086F)
        return DIR_RTL; // Syriac Supplement
    if (cp >= 0x0870 && cp <= 0x089F)
        return DIR_RTL; // Arabic Extended-B
    if (cp >= 0x08A0 && cp <= 0x08FF)
        return DIR_RTL; // Arabic Extended-A
    if (cp >= 0xFB1D && cp <= 0xFB4F)
        return DIR_RTL; // Hebrew presentation
    if (cp >= 0xFB50 && cp <= 0xFDFF)
        return DIR_RTL; // Arabic presentation
    if (cp >= 0xFE70 && cp <= 0xFEFF)
        return DIR_RTL; // Arabic presentation-B
    if (cp >= 0x10800 && cp <= 0x1091F)
        return DIR_RTL; // Cypriot/Kharoshthi
    if (cp >= 0x10A00 && cp <= 0x10A5F)
        return DIR_RTL; // Kharoshthi
    if (cp >= 0x10A60 && cp <= 0x10A7F)
        return DIR_RTL; // Old South Arabian
    if (cp >= 0x10AC0 && cp <= 0x10AFF)
        return DIR_RTL; // Manichaean
    if (cp >= 0x10B00 && cp <= 0x10B7F)
        return DIR_RTL; // Avestan/Inscriptional
    if (cp >= 0x10D00 && cp <= 0x10D3F)
        return DIR_RTL; // Hanifi Rohingya
    if (cp >= 0x10E60 && cp <= 0x10E7F)
        return DIR_RTL; // Rumi numerals
    if (cp >= 0x10EC0 && cp <= 0x10EFF)
        return DIR_RTL; // Arabic Extended-C
    if (cp >= 0x1E800 && cp <= 0x1E95F)
        return DIR_RTL; // Mende/Adlam

    // Common neutrals: ASCII space, punctuation, digits, control.
    if (cp < 0x30)
        return DIR_NEUTRAL; // controls + space + punct
    if (cp <= 0x39)
        return DIR_NEUTRAL; // digits
    if (cp <= 0x40)
        return DIR_NEUTRAL; // more punct
    if (cp >= 0x5B && cp <= 0x60)
        return DIR_NEUTRAL; // [\]^_`
    if (cp >= 0x7B && cp <= 0xBF)
        return DIR_NEUTRAL; // punct + Latin-1 supp controls
    if (cp == 0x00A0)
        return DIR_NEUTRAL; // NBSP
    if (cp >= 0x2000 && cp <= 0x206F)
        return DIR_NEUTRAL; // general punctuation

    // Everything else (Latin, Greek, Cyrillic, CJK, etc.) is LTR.
    return DIR_LTR;
}

/// @brief Append bytes while preserving text-direction formatting failures.
/// @details Direction isolation inserts Unicode isolate markers while copying
///          the original UTF-8 bytes. If the builder cannot grow, returning a
///          partial bidi-wrapped string is misleading, so this helper traps and
///          lets the caller return an empty fallback after cleanup.
/// @param sb Destination builder.
/// @param bytes Byte span to append.
/// @param len Number of bytes in @p bytes.
/// @return Non-zero on success; zero after reporting an append failure.
static int text_direction_append_checked(rt_string_builder *sb, const char *bytes, size_t len) {
    rt_sb_status_t status = rt_sb_append_bytes(sb, bytes, len);
    if (status == RT_SB_OK)
        return 1;
    rt_trap(status == RT_SB_ERROR_OVERFLOW ? "Zanna.Localization.TextDirection: output overflow"
                                           : "Zanna.Localization.TextDirection: allocation failed");
    return 0;
}

//===----------------------------------------------------------------------===//
// Scanning helpers
//===----------------------------------------------------------------------===//

/// @brief Strong-direction counts and first-strong result for one scan.
typedef struct {
    int64_t ltr_count;
    int64_t rtl_count;
    cp_dir_t first_strong; ///< DIR_NEUTRAL if the string has no strong chars
} scan_result_t;

/// @brief Single pass over a UTF-8 string tallying strong LTR/RTL codepoints
///        and recording the first strong direction (the UAX#9 P2/P3 input).
/// @param s UTF-8 byte sequence to scan, or NULL.
/// @param len Number of available bytes in @p s.
/// @return Counts plus @c first_strong (DIR_NEUTRAL if no strong char appears).
static scan_result_t scan(const char *s, size_t len) {
    scan_result_t r = {0, 0, DIR_NEUTRAL};
    if (!s || len == 0)
        return r;
    size_t pos = 0;
    while (pos < len) {
        uint32_t cp = decode_codepoint(s, len, &pos);
        cp_dir_t d = classify(cp);
        if (d == DIR_LTR) {
            r.ltr_count++;
            if (r.first_strong == DIR_NEUTRAL)
                r.first_strong = DIR_LTR;
        } else if (d == DIR_RTL) {
            r.rtl_count++;
            if (r.first_strong == DIR_NEUTRAL)
                r.first_strong = DIR_RTL;
        }
    }
    return r;
}

//===----------------------------------------------------------------------===//
// Public API
//===----------------------------------------------------------------------===//

/// @brief Return the direction declared by a locale-data record.
/// @param locale Locale handle, or NULL for invariant locale data.
/// @return Newly allocated `"ltr"` or locale-provided direction string;
///         missing/empty data falls back to `"ltr"`.
rt_string rt_text_direction_of_locale(void *locale) {
    const rt_locale_data_t *d = rt_locale_get_data(locale);
    const char *td = d->text_direction;
    if (!td || !*td)
        td = "ltr";
    return rt_string_from_bytes(td, strlen(td));
}

/// @brief Classify text from its aggregate strong directional content.
/// @param s Runtime UTF-8 string to scan, or NULL.
/// @return Newly allocated `"mixed"`, `"rtl"`, or `"ltr"`; NULL and empty
///         inputs return an owned empty string, while all-neutral non-empty
///         input defaults to `"ltr"`.
rt_string rt_text_direction_detect(rt_string s) {
    if (!s)
        return rt_string_from_bytes("", 0);
    const char *cs = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!cs || len <= 0)
        return rt_string_from_bytes("", 0);
    scan_result_t r = scan(cs, (size_t)len);
    if (r.ltr_count > 0 && r.rtl_count > 0)
        return rt_string_from_bytes("mixed", 5);
    if (r.rtl_count > 0)
        return rt_string_from_bytes("rtl", 3);
    if (r.ltr_count > 0)
        return rt_string_from_bytes("ltr", 3);
    // All neutral (digits / punctuation / whitespace) -> default LTR.
    return rt_string_from_bytes("ltr", 3);
}

/// @brief Test whether RTL strong code points strictly outnumber LTR ones.
/// @param s Runtime UTF-8 string to scan, or NULL.
/// @return 1 only for a strict RTL majority; ties, neutral, empty, and NULL
///         inputs return 0.
int8_t rt_text_direction_is_rtl(rt_string s) {
    if (!s)
        return 0;
    const char *cs = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!cs || len <= 0)
        return 0;
    scan_result_t r = scan(cs, (size_t)len);
    return (int8_t)(r.rtl_count > r.ltr_count ? 1 : 0);
}

/// @brief Test whether LTR strong code points equal or outnumber RTL ones.
/// @param s Runtime UTF-8 string to scan, or NULL.
/// @return 1 for an LTR majority, a tie, or neutral/empty/NULL input; otherwise 0.
int8_t rt_text_direction_is_ltr(rt_string s) {
    if (!s)
        return 1; // empty -> LTR (matches Detect)
    const char *cs = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!cs || len <= 0)
        return 1;
    scan_result_t r = scan(cs, (size_t)len);
    return (int8_t)(r.ltr_count >= r.rtl_count ? 1 : 0);
}

/// @brief Report the direction of the first strong code point.
/// @param s Runtime UTF-8 string to scan, or NULL.
/// @return Newly allocated `"ltr"`, `"rtl"`, or `"neutral"`.
rt_string rt_text_direction_first_strong(rt_string s) {
    if (!s)
        return rt_string_from_bytes("neutral", 7);
    const char *cs = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!cs || len <= 0)
        return rt_string_from_bytes("neutral", 7);
    scan_result_t r = scan(cs, (size_t)len);
    switch (r.first_strong) {
        case DIR_LTR:
            return rt_string_from_bytes("ltr", 3);
        case DIR_RTL:
            return rt_string_from_bytes("rtl", 3);
        default:
            return rt_string_from_bytes("neutral", 7);
    }
}

/// @brief Isolate RTL runs when a string contains both LTR and RTL content.
/// @details Mixed input is copied with U+2067 RLI and U+2069 PDI around RTL
///          runs. Pure or all-neutral input is retained and returned unchanged.
///          Builder failure traps and returns an owned empty string if resumed.
/// @param s Runtime UTF-8 string to process, or NULL.
/// @return Owned bidi-safe string. This may be the same retained handle as
///         @p s when isolation is unnecessary.
rt_string rt_text_direction_bidi(rt_string s) {
    if (!s)
        return rt_string_from_bytes("", 0);
    const char *cs = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!cs || len <= 0)
        return rt_string_from_bytes("", 0);

    scan_result_t r = scan(cs, (size_t)len);
    // Pure-LTR or pure-RTL: return as-is.
    if (!(r.ltr_count > 0 && r.rtl_count > 0)) {
        rt_string_ref(s);
        return s;
    }

    // Mixed: walk codepoints, isolate RTL runs instead of overriding their
    // embedding direction. Isolates do not leak into surrounding text.
    // U+2067 RIGHT-TO-LEFT ISOLATE = 0xE2 0x81 0xA7
    // U+2069 POP DIRECTIONAL ISOLATE = 0xE2 0x81 0xA9
    static const char RLI[] = "\xE2\x81\xA7";
    static const char PDI[] = "\xE2\x81\xA9";

    rt_string_builder sb;
    rt_sb_init(&sb);
    size_t pos = 0;
    int in_rtl_run = 0;
    while (pos < (size_t)len) {
        size_t cp_start = pos;
        uint32_t cp = decode_codepoint(cs, (size_t)len, &pos);
        cp_dir_t d = classify(cp);
        if (d == DIR_RTL && !in_rtl_run) {
            if (!text_direction_append_checked(&sb, RLI, 3))
                goto wrap_error;
            in_rtl_run = 1;
        } else if (d == DIR_LTR && in_rtl_run) {
            if (!text_direction_append_checked(&sb, PDI, 3))
                goto wrap_error;
            in_rtl_run = 0;
        }
        if (!text_direction_append_checked(&sb, cs + cp_start, pos - cp_start))
            goto wrap_error;
    }
    if (in_rtl_run)
        if (!text_direction_append_checked(&sb, PDI, 3))
            goto wrap_error;
    rt_string out = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return out;

wrap_error:
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}
