//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_dateformat_patterns.c
// Purpose: CLDR-style date/time pattern interpreter. Consumes a pattern string
//          of letter runs (y/M/d/E/H/h/m/s/a) and quoted literals ('...'), and
//          emits the formatted representation of a Unix timestamp into a
//          string builder using the bound locale's month / day / AM-PM tables.
//
// Key invariants:
//   - Letter run widths select CLDR-style forms: 'y' emits a 2-digit year
//     only for count 2 (all other counts emit the full year); 'M' maps
//     1/2/3/4/5+ to numeric/zero-padded/abbreviated/wide/narrow; 'E' maps
//     <=3/4/5+ to abbreviated/wide/narrow; 'd', 'H', 'h', 'm', 's' zero-pad
//     to two digits for count >= 2. Widths are not otherwise clamped.
//   - Quoted literals use single apostrophes; a double apostrophe ('') emits
//     a literal apostrophe. An unterminated quoted literal traps.
//   - Pattern letters outside the supported set (y M d E H h m s a) are
//     rejected with a trap. Supported letters are explicitly enumerated
//     elsewhere in docs/zannalib/localization/formatting.md.
//
// Ownership/Lifetime:
//   - Caller owns the string builder; this function only appends bytes.
//
// Links: src/runtime/localization/rt_dateformat.h (class consumer),
//        src/runtime/core/rt_datetime.h (component accessors).
//
//===----------------------------------------------------------------------===//

#include "rt_dateformat.h"

#include "rt_datetime.h"
#include "rt_internal.h"
#include "rt_locale_data.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <stdio.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Small helpers
//===----------------------------------------------------------------------===//

typedef struct digit_spans {
    const char *ptr[10];
    size_t len[10];
    int valid;
} digit_spans_t;

/// @brief Byte length of the leading UTF-8 codepoint in @p s (0 if empty/NULL,
///        1 on a malformed lead byte so callers always make forward progress).
/// @details This is a bounded-glyph span helper for trusted locale data, not a
///          full UTF-8 validator; it classifies the lead byte and verifies only
///          that the expected following bytes exist before the NUL terminator.
/// @param s NUL-terminated text positioned at a glyph boundary; may be NULL.
/// @return Span length from one through four, zero at end/NULL, or one for an
///         unsupported/truncated lead.
static size_t utf8_cp_len(const char *s) {
    if (!s || !*s)
        return 0;
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80)
        return 1;
    if ((c & 0xE0) == 0xC0 && s[1])
        return 2;
    if ((c & 0xF0) == 0xE0 && s[1] && s[2])
        return 3;
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3])
        return 4;
    return 1;
}

/// @brief Slice the locale's 10 numbering-system digit glyphs into a span table.
/// @details Walks the (possibly multi-byte) @c numbers.digits string; falls back
///          to ASCII "0123456789". @c ds.valid is set only when exactly ten
///          codepoints were consumed, so emitters can safely index ds.ptr/len.
/// @param data Locale data whose numbering-system glyph string is inspected;
///             may be NULL for ASCII fallback.
/// @return Borrowed spans into the locale/ASCII digit string plus a validity flag.
static digit_spans_t digit_spans_from_locale(const rt_locale_data_t *data) {
    digit_spans_t ds;
    memset(&ds, 0, sizeof(ds));
    const char *digits = data && data->numbers.digits ? data->numbers.digits : "0123456789";
    const char *p = digits;
    for (int i = 0; i < 10; ++i) {
        size_t n = utf8_cp_len(p);
        if (n == 0) {
            ds.valid = 0;
            return ds;
        }
        ds.ptr[i] = p;
        ds.len[i] = n;
        p += n;
    }
    ds.valid = *p == '\0';
    return ds;
}

/// @brief Convert a string-builder append status into a DateFormat trap.
/// @details Date-pattern emission is a `void` side-effecting API. This helper
///          keeps the existing API stable while ensuring allocation, overflow,
///          and invalid-argument failures stop emission instead of silently
///          producing a truncated formatted date.
/// @param status Status returned by a string-builder append operation.
/// @return 1 when @p status is @ref RT_SB_OK, otherwise traps and returns 0.
static int dateformat_check_append(rt_sb_status_t status) {
    if (status == RT_SB_OK)
        return 1;
    rt_trap("Zanna.Localization.DateFormat: formatting failed");
    return 0;
}

/// @brief Append @p len bytes, transliterating ASCII '0'-'9' to the locale's
///        native digit glyphs; non-digit bytes are passed through verbatim.
/// @param sb Destination string builder.
/// @param data Locale supplying numbering-system digit glyphs.
/// @param bytes Bounded ASCII-formatted bytes to append.
/// @param len Number of readable bytes at @p bytes.
/// @return 1 on success, 0 when a builder append failed.
static int emit_ascii_digits(rt_string_builder *sb,
                             const rt_locale_data_t *data,
                             const char *bytes,
                             size_t len) {
    digit_spans_t ds = digit_spans_from_locale(data);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)bytes[i];
        if (ds.valid && c >= '0' && c <= '9') {
            int d = (int)(c - '0');
            if (!dateformat_check_append(rt_sb_append_bytes(sb, ds.ptr[d], ds.len[d])))
                return 0;
        } else {
            if (!dateformat_check_append(rt_sb_append_bytes(sb, bytes + i, 1)))
                return 0;
        }
    }
    return 1;
}

/// @brief Append @p value as a zero-padded decimal of @p width digits.
/// @details A negative sign is emitted before padding and is not counted in
///          @p width. Decimal digits are translated through the Locale's
///          numbering system when it supplies exactly ten glyphs.
/// @param sb Destination string builder.
/// @param data Locale supplying digit glyphs.
/// @param value Signed integer to format.
/// @param width Minimum number of magnitude digits.
/// @return 1 on success, 0 when formatting or append work failed.
static int emit_padded_int(rt_string_builder *sb,
                           const rt_locale_data_t *data,
                           int64_t value,
                           int width) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)value);
    if (n < 0)
        return 0;
    if ((size_t)n >= sizeof(buf))
        return 0;
    if (buf[0] == '-') {
        if (!dateformat_check_append(rt_sb_append_bytes(sb, "-", 1)))
            return 0;
        memmove(buf, buf + 1, (size_t)n);
        --n;
    }
    if (width > n) {
        digit_spans_t ds = digit_spans_from_locale(data);
        for (int i = n; i < width; ++i) {
            if (ds.valid) {
                if (!dateformat_check_append(rt_sb_append_bytes(sb, ds.ptr[0], ds.len[0])))
                    return 0;
            } else if (!dateformat_check_append(rt_sb_append_bytes(sb, "0", 1))) {
                return 0;
            }
        }
    }
    return emit_ascii_digits(sb, data, buf, (size_t)n);
}

/// @brief Append @p value as an unpadded decimal.
/// @param sb Destination string builder.
/// @param data Locale supplying digit glyphs.
/// @param value Signed integer to format.
/// @return 1 on success, 0 when formatting or append work failed.
static int emit_int(rt_string_builder *sb, const rt_locale_data_t *data, int64_t value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)value);
    if (n < 0)
        return 0;
    if ((size_t)n >= sizeof(buf))
        return 0;
    return emit_ascii_digits(sb, data, buf, (size_t)n);
}

/// @brief Append a C string (NULL-tolerant).
/// @param sb Destination string builder.
/// @param s NUL-terminated bytes to append; NULL and empty are successful no-ops.
/// @return 1 on success, 0 when a builder append failed.
static int emit_cstr(rt_string_builder *sb, const char *s) {
    if (!s || !*s)
        return 1;
    return dateformat_check_append(rt_sb_append_cstr(sb, s));
}

/// @brief Append the single leading UTF-8 codepoint of @p s (narrow form).
/// @details Locale name data does not store dedicated narrow forms, so the
///          first glyph of the wide name is used. NULL/empty input is a no-op.
/// @param sb Destination string builder.
/// @param s NUL-terminated wide name.
/// @return 1 on success, 0 when a builder append failed.
static int emit_narrow(rt_string_builder *sb, const char *s) {
    if (!s || !*s)
        return 1;
    unsigned char c = (unsigned char)s[0];
    size_t len = 1;
    if ((c & 0xE0) == 0xC0 && s[1])
        len = 2;
    else if ((c & 0xF0) == 0xE0 && s[1] && s[2])
        len = 3;
    else if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3])
        len = 4;
    return dateformat_check_append(rt_sb_append_bytes(sb, s, len));
}

//===----------------------------------------------------------------------===//
// Letter run emitters
//===----------------------------------------------------------------------===//

typedef struct {
    int64_t year;
    int64_t month;  // 1-12
    int64_t day;    // 1-31
    int64_t hour;   // 0-23
    int64_t minute; // 0-59
    int64_t second; // 0-59
    int64_t dow;    // 0=Sunday..6=Saturday
} dt_components_t;

/// @brief Emit the year for a CLDR 'y' run: count==2 → 2-digit (year % 100),
///        count>=4 → zero-padded to @p count, else the full year unclamped.
/// @param sb Destination string builder.
/// @param c Extracted local date/time components.
/// @param count Number of consecutive `y` pattern letters.
/// @param data Locale supplying digit glyphs.
/// @return 1 on success, otherwise 0.
static int emit_year(rt_string_builder *sb,
                     const dt_components_t *c,
                     int count,
                     const rt_locale_data_t *data) {
    if (count == 2) {
        return emit_padded_int(sb, data, c->year % 100, 2);
    } else {
        // 1, 3, 4, 5+ all emit the full year (no width clamping).
        int width = count >= 4 ? count : 1;
        return emit_padded_int(sb, data, c->year, width);
    }
}

/// @brief Emit the month for a CLDR 'M' run: 1=numeric, 2=zero-padded,
///        3=abbreviated name, 4=wide name, 5+=narrow (first glyph of wide).
/// @details Traps if the component month is outside 1-12.
/// @param sb Destination string builder.
/// @param c Extracted local date/time components.
/// @param count Number of consecutive `M` pattern letters.
/// @param data Locale supplying names and digit glyphs.
/// @return 1 on success, otherwise 0.
static int emit_month(rt_string_builder *sb,
                      const dt_components_t *c,
                      int count,
                      const rt_locale_data_t *data) {
    int idx = (int)(c->month - 1);
    if (idx < 0 || idx > 11) {
        rt_trap("Zanna.Localization.DateFormat: month component out of range");
        return 0;
    }
    if (count == 1) {
        return emit_int(sb, data, c->month);
    } else if (count == 2) {
        return emit_padded_int(sb, data, c->month, 2);
    } else if (count == 3) {
        return emit_cstr(sb, data->dates.months_abbr ? data->dates.months_abbr[idx] : NULL);
    } else if (count == 4) {
        return emit_cstr(sb, data->dates.months_wide ? data->dates.months_wide[idx] : NULL);
    } else {
        // Narrow: one-glyph form; fall back to the wide name's first codepoint.
        const char *wide = data->dates.months_wide ? data->dates.months_wide[idx] : NULL;
        return emit_narrow(sb, wide);
    }
}

/// @brief Emit the day-of-month for a CLDR 'd' run: count>=2 → zero-padded to
///        two digits, otherwise the bare number.
/// @param sb Destination string builder.
/// @param c Extracted local date/time components.
/// @param count Number of consecutive `d` pattern letters.
/// @param data Locale supplying digit glyphs.
/// @return 1 on success, otherwise 0.
static int emit_day(rt_string_builder *sb,
                    const dt_components_t *c,
                    int count,
                    const rt_locale_data_t *data) {
    if (count >= 2)
        return emit_padded_int(sb, data, c->day, 2);
    return emit_int(sb, data, c->day);
}

/// @brief Emit the weekday name for a CLDR 'E' run: count<=3 → abbreviated,
///        4 → wide, 5+ → narrow (first glyph of wide).
/// @details Traps if the component day-of-week is outside 0-6 (Sun..Sat).
/// @param sb Destination string builder.
/// @param c Extracted local date/time components.
/// @param count Number of consecutive `E` pattern letters.
/// @param data Locale supplying weekday names.
/// @return 1 on success, otherwise 0.
static int emit_dow(rt_string_builder *sb,
                    const dt_components_t *c,
                    int count,
                    const rt_locale_data_t *data) {
    int idx = (int)c->dow;
    if (idx < 0 || idx > 6) {
        rt_trap("Zanna.Localization.DateFormat: weekday component out of range");
        return 0;
    }
    if (count <= 3) {
        return emit_cstr(sb, data->dates.days_abbr ? data->dates.days_abbr[idx] : NULL);
    } else if (count == 4) {
        return emit_cstr(sb, data->dates.days_wide ? data->dates.days_wide[idx] : NULL);
    } else {
        const char *wide = data->dates.days_wide ? data->dates.days_wide[idx] : NULL;
        return emit_narrow(sb, wide);
    }
}

/// @brief Emit the hour on a 24-hour clock (CLDR 'H'); count>=2 → zero-padded.
/// @param sb Destination string builder.
/// @param c Extracted local date/time components.
/// @param count Number of consecutive `H` pattern letters.
/// @param data Locale supplying digit glyphs.
/// @return 1 on success, otherwise 0.
static int emit_hour24(rt_string_builder *sb,
                       const dt_components_t *c,
                       int count,
                       const rt_locale_data_t *data) {
    if (count >= 2)
        return emit_padded_int(sb, data, c->hour, 2);
    return emit_int(sb, data, c->hour);
}

/// @brief Emit the hour on a 12-hour clock (CLDR 'h'); 0 maps to 12,
///        count>=2 → zero-padded.
/// @param sb Destination string builder.
/// @param c Extracted local date/time components.
/// @param count Number of consecutive `h` pattern letters.
/// @param data Locale supplying digit glyphs.
/// @return 1 on success, otherwise 0.
static int emit_hour12(rt_string_builder *sb,
                       const dt_components_t *c,
                       int count,
                       const rt_locale_data_t *data) {
    int64_t h = c->hour % 12;
    if (h == 0)
        h = 12;
    if (count >= 2)
        return emit_padded_int(sb, data, h, 2);
    return emit_int(sb, data, h);
}

/// @brief Emit the minute (CLDR 'm'); count>=2 → zero-padded to two digits.
/// @param sb Destination string builder.
/// @param c Extracted local date/time components.
/// @param count Number of consecutive `m` pattern letters.
/// @param data Locale supplying digit glyphs.
/// @return 1 on success, otherwise 0.
static int emit_minute(rt_string_builder *sb,
                       const dt_components_t *c,
                       int count,
                       const rt_locale_data_t *data) {
    if (count >= 2)
        return emit_padded_int(sb, data, c->minute, 2);
    return emit_int(sb, data, c->minute);
}

/// @brief Emit the second (CLDR 's'); count>=2 → zero-padded to two digits.
/// @param sb Destination string builder.
/// @param c Extracted local date/time components.
/// @param count Number of consecutive `s` pattern letters.
/// @param data Locale supplying digit glyphs.
/// @return 1 on success, otherwise 0.
static int emit_second(rt_string_builder *sb,
                       const dt_components_t *c,
                       int count,
                       const rt_locale_data_t *data) {
    if (count >= 2)
        return emit_padded_int(sb, data, c->second, 2);
    return emit_int(sb, data, c->second);
}

/// @brief Emit the AM/PM marker (CLDR 'a') using the locale's tokens,
///        falling back to literal "AM"/"PM" when the locale omits them.
/// @param sb Destination string builder.
/// @param c Extracted local date/time components used to select the period.
/// @param data Locale supplying AM/PM tokens.
/// @return 1 on success, otherwise 0.
static int emit_ampm(rt_string_builder *sb,
                     const dt_components_t *c,
                     const rt_locale_data_t *data) {
    const char *token = c->hour < 12 ? data->dates.am : data->dates.pm;
    return emit_cstr(sb, token ? token : (c->hour < 12 ? "AM" : "PM"));
}

//===----------------------------------------------------------------------===//
// Public entry point
//===----------------------------------------------------------------------===//

int rt_dateformat_emit_pattern_checked(rt_string_builder *sb,
                                       int64_t timestamp,
                                       const char *pattern,
                                       size_t pattern_len,
                                       const rt_locale_data_t *data);

void rt_dateformat_emit_pattern(rt_string_builder *sb,
                                int64_t timestamp,
                                const char *pattern,
                                size_t pattern_len,
                                const rt_locale_data_t *data);

/// @brief Compatibility wrapper for callers that ignore pattern-emission status.
/// @details New code should prefer @ref rt_dateformat_emit_pattern_checked so
///          allocation and formatting failures do not silently materialize a
///          partial date string. This wrapper leaves any partial builder
///          contents in place when checked emission fails.
/// @param sb Caller-owned destination builder.
/// @param timestamp Unix seconds whose local civil-time components are emitted.
/// @param pattern Bounded pattern bytes; no terminating NUL is required.
/// @param pattern_len Number of readable pattern bytes, at most 256.
/// @param data Captured Locale data supplying names, tokens, and digits.
void rt_dateformat_emit_pattern(rt_string_builder *sb,
                                int64_t timestamp,
                                const char *pattern,
                                size_t pattern_len,
                                const rt_locale_data_t *data) {
    (void)rt_dateformat_emit_pattern_checked(sb, timestamp, pattern, pattern_len, data);
}

/// @brief Emit a CLDR-like date/time pattern into @p sb and report success.
/// @details Interprets supported date pattern letters, quoted literals, and
///          locale-specific digits. Any unsupported pattern, invalid component,
///          or builder append failure traps through the existing DateFormat
///          error policy and returns 0 after recovery. NULL arguments return
///          zero directly. On any failure the caller must discard or otherwise
///          account for bytes appended before the error.
/// @param sb Caller-owned destination builder.
/// @param timestamp Unix seconds interpreted by local-time component accessors.
/// @param pattern Bounded pattern bytes; no terminating NUL is required.
/// @param pattern_len Number of readable pattern bytes, at most 256.
/// @param data Captured Locale data supplying names, tokens, and digits.
/// @return 1 when the full pattern was emitted, 0 when emission stopped early.
int rt_dateformat_emit_pattern_checked(rt_string_builder *sb,
                                       int64_t timestamp,
                                       const char *pattern,
                                       size_t pattern_len,
                                       const rt_locale_data_t *data) {
    if (!sb || !pattern || !data)
        return 0;

    dt_components_t c;
    c.year = rt_datetime_year(timestamp);
    c.month = rt_datetime_month(timestamp);
    c.day = rt_datetime_day(timestamp);
    c.hour = rt_datetime_hour(timestamp);
    c.minute = rt_datetime_minute(timestamp);
    c.second = rt_datetime_second(timestamp);
    c.dow = rt_datetime_day_of_week(timestamp);

    // Cap pattern length defensively (plan calls for 256 chars at load time;
    // Custom() pattern inputs from users are capped here as well).
    if (pattern_len > 256) {
        rt_trap("Zanna.Localization.DateFormat: pattern too long (max 256 chars)");
        return 0;
    }

    size_t i = 0;
    while (i < pattern_len) {
        char ch = pattern[i];

        // Quoted literal: '...' (with '' meaning a literal apostrophe).
        if (ch == '\'') {
            ++i;
            // Case: '' at the start => literal '
            if (i < pattern_len && pattern[i] == '\'') {
                if (!dateformat_check_append(rt_sb_append_bytes(sb, "'", 1)))
                    return 0;
                ++i;
                continue;
            }
            int closed = 0;
            while (i < pattern_len) {
                if (pattern[i] == '\'') {
                    if (i + 1 < pattern_len && pattern[i + 1] == '\'') {
                        // Escaped apostrophe inside the literal.
                        if (!dateformat_check_append(rt_sb_append_bytes(sb, "'", 1)))
                            return 0;
                        i += 2;
                        continue;
                    }
                    ++i; // closing quote
                    closed = 1;
                    break;
                }
                if (!dateformat_check_append(rt_sb_append_bytes(sb, pattern + i, 1)))
                    return 0;
                ++i;
            }
            if (!closed) {
                rt_trap("Zanna.Localization.DateFormat: unterminated quoted literal");
                return 0;
            }
            continue;
        }

        // Pattern letter run.
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
            size_t j = i + 1;
            while (j < pattern_len && pattern[j] == ch)
                ++j;
            int count = (int)(j - i);
            i = j;
            switch (ch) {
                case 'y':
                    if (!emit_year(sb, &c, count, data))
                        return 0;
                    break;
                case 'M':
                    if (!emit_month(sb, &c, count, data))
                        return 0;
                    break;
                case 'd':
                    if (!emit_day(sb, &c, count, data))
                        return 0;
                    break;
                case 'E':
                    if (!emit_dow(sb, &c, count, data))
                        return 0;
                    break;
                case 'H':
                    if (!emit_hour24(sb, &c, count, data))
                        return 0;
                    break;
                case 'h':
                    if (!emit_hour12(sb, &c, count, data))
                        return 0;
                    break;
                case 'm':
                    if (!emit_minute(sb, &c, count, data))
                        return 0;
                    break;
                case 's':
                    if (!emit_second(sb, &c, count, data))
                        return 0;
                    break;
                case 'a':
                    if (!emit_ampm(sb, &c, data))
                        return 0;
                    break;
                default:
                    rt_trap("Zanna.Localization.DateFormat: unsupported pattern letter");
                    return 0;
            }
            continue;
        }

        // Any other character is emitted verbatim (separators, punctuation).
        if (!dateformat_check_append(rt_sb_append_bytes(sb, &ch, 1)))
            return 0;
        ++i;
    }
    return 1;
}
