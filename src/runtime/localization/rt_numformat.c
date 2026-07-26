//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_numformat.c
// Purpose: Implementation of Zanna.Localization.NumberFormat. Formats and
//          parses locale-aware numeric strings using the bound Locale's
//          rt_locale_data_t, including locale digit glyphs, primary/secondary
//          grouping, currency patterns, and strict or lenient parsing.
//
// Key invariants:
//   - Strict-mode parsing validates primary and secondary group widths.
//     Lenient mode permits noncanonical widths but still rejects leading,
//     repeated, or trailing group separators and unconsumed input.
//   - Every format path runs through a common sign-then-integer-then-fraction
//     builder so sign handling and separator insertion stay in one place.
//   - Currency patterns use the locale's `pattern_positive` / `pattern_negative`
//     templates with {n} and {s} substitution; inputs must not introduce any
//     other placeholders.
//
// Ownership/Lifetime:
//   - Instances are rt_obj_new_i64-allocated; GC-managed.
//   - Each instance retains both its Locale handle and the locale-data record
//     captured at construction. GetLocale returns a borrowed handle.
//   - Public formatting methods return owned runtime strings.
//
// Links: src/runtime/localization/rt_numformat.h (interface),
//        src/runtime/text/rt_numfmt.h (invariant ordinal fallback),
//        src/runtime/localization/rt_locale_manager.h (current-locale lookup),
//        docs/zannalib/localization/formatting.md (user documentation).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements locale-aware numeric formatting and parsing.
 * @details Provides deterministic C-locale conversion, explicit localized
 * digits and tokens, configurable rounding and grouping, decimal, integer,
 * percent, currency, scientific, and ordinal rendering, plus strict and
 * lenient parse paths with exact integer handling.
 */

#include "rt_numformat.h"

#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_locale.h"
#include "rt_locale_data.h"
#include "rt_locale_manager.h"
#include "rt_numfmt.h"
#include "rt_numfmt_internal.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <locale.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <pthread.h>
#endif

//===----------------------------------------------------------------------===//
// Rounding modes
//===----------------------------------------------------------------------===//

/// @brief Rounding policies supported by NumberFormat instances.
typedef enum {
    ROUND_HALF_EVEN = 0,
    ROUND_HALF_UP,
    ROUND_HALF_DOWN,
    ROUND_UP,
    ROUND_DOWN,
    ROUND_CEILING,
    ROUND_FLOOR,
} rounding_mode_t;

/// @brief Canonical CLDR-style name for a rounding mode (defaults "halfEven").
/// @param m Internal rounding-mode value to name.
/// @return Process-lifetime canonical name for @p m; unknown values map to
///         `"halfEven"`.
static const char *rounding_mode_name(rounding_mode_t m) {
    switch (m) {
        case ROUND_HALF_UP:
            return "halfUp";
        case ROUND_HALF_DOWN:
            return "halfDown";
        case ROUND_UP:
            return "up";
        case ROUND_DOWN:
            return "down";
        case ROUND_CEILING:
            return "ceiling";
        case ROUND_FLOOR:
            return "floor";
        case ROUND_HALF_EVEN:
        default:
            return "halfEven";
    }
}

/// @brief Parse a rounding-mode name; unknown/NULL falls back to halfEven.
/// @param s NUL-terminated canonical rounding-mode name, or NULL.
/// @return Matching internal mode, or @ref ROUND_HALF_EVEN when unrecognized.
static rounding_mode_t rounding_mode_parse(const char *s) {
    if (!s)
        return ROUND_HALF_EVEN;
    if (strcmp(s, "halfUp") == 0)
        return ROUND_HALF_UP;
    if (strcmp(s, "halfDown") == 0)
        return ROUND_HALF_DOWN;
    if (strcmp(s, "up") == 0)
        return ROUND_UP;
    if (strcmp(s, "down") == 0)
        return ROUND_DOWN;
    if (strcmp(s, "ceiling") == 0)
        return ROUND_CEILING;
    if (strcmp(s, "floor") == 0)
        return ROUND_FLOOR;
    return ROUND_HALF_EVEN;
}

#if defined(_WIN32)
/** Process-lifetime Windows handle for the immutable C numeric locale. */
static _locale_t loc_c_locale_cached_;

/// @brief Initialize the process-wide Windows C numeric-locale cache once.
/// @details Stores `_create_locale`'s result for process lifetime. Cache
///          creation failure is represented by a NULL cached locale.
/// @param once Windows once-control object supplied by `InitOnceExecuteOnce`.
/// @param param Unused caller parameter.
/// @param ctx Unused context-output slot.
/// @return Always TRUE so the once primitive does not retry initialization.
static BOOL CALLBACK loc_c_locale_init_(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    (void)once;
    (void)param;
    (void)ctx;
    loc_c_locale_cached_ = _create_locale(LC_NUMERIC, "C");
    return TRUE;
}
/// @brief Return the cached Windows C numeric-locale object.
/// @details Uses `InitOnceExecuteOnce` so concurrent first access performs
///          exactly one initialization (VDOC-080). The immutable locale is
///          intentionally retained for process lifetime.
/// @return Cached locale object, or NULL when `_create_locale` failed.
static _locale_t loc_cached_c_locale(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, loc_c_locale_init_, NULL, NULL);
    return loc_c_locale_cached_;
}
#else
/** Process-lifetime POSIX handle for the immutable C numeric locale. */
static locale_t loc_c_locale_cached_;

/// @brief Initialize the process-wide POSIX C numeric-locale cache once.
/// @details Stores `newlocale`'s result for process lifetime; allocation
///          failure leaves the cached handle equal to zero.
static void loc_c_locale_init_(void) {
    loc_c_locale_cached_ = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
}
/// @brief Return the cached POSIX C numeric-locale object.
/// @details Uses `pthread_once` so concurrent first access performs exactly
///          one initialization (VDOC-080). The immutable locale is
///          intentionally retained for process lifetime.
/// @return Cached locale object, or zero when `newlocale` failed.
static locale_t loc_cached_c_locale(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, loc_c_locale_init_);
    return loc_c_locale_cached_;
}
#endif

/// @brief vsnprintf forced through the "C" LC_NUMERIC locale so the decimal
///        separator is always '.' regardless of the process/thread locale.
/// @details This is the foundation of deterministic numeric formatting: digits
///          are produced locale-neutrally here, then localized glyphs/separators
///          are applied explicitly downstream. Per-platform: Windows uses
///          @c _vsnprintf_l with a "C" @c _locale_t; POSIX uses @c uselocale
///          with a "C" @c locale_t. Both degrade to plain @c vsnprintf if the
///          "C" locale object cannot be created.
/// @param out Destination buffer supplied to the underlying formatter.
/// @param cap Capacity of @p out in bytes.
/// @param fmt `printf`-style format string.
/// @param args Variadic argument cursor consumed by the formatter.
/// @return Underlying `vsnprintf`-family result: required byte count excluding
///         the terminator, or a negative value on encoding/format failure.
static int loc_vsnprintf_c(char *out, size_t cap, const char *fmt, va_list args) {
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
#if defined(_WIN32)
    _locale_t c_locale = loc_cached_c_locale();
    int n = c_locale ? _vsnprintf_l(out, cap, fmt, c_locale, args) : vsnprintf(out, cap, fmt, args);
#else
    locale_t c_locale = loc_cached_c_locale();
    int n;
    if (!c_locale) {
        n = vsnprintf(out, cap, fmt, args);
    } else {
        locale_t old = uselocale(c_locale);
        if (!old) {
            n = vsnprintf(out, cap, fmt, args);
        } else {
            n = vsnprintf(out, cap, fmt, args);
            uselocale(old);
        }
    }
#endif
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    return n;
}

/// @brief Variadic wrapper over @ref loc_vsnprintf_c (C-locale snprintf).
/// @param out Destination buffer supplied to the formatter.
/// @param cap Capacity of @p out in bytes.
/// @param fmt `printf`-style format string followed by its arguments.
/// @return Required byte count excluding the terminator, or a negative value
///         on encoding/format failure.
static int loc_snprintf_c(char *out, size_t cap, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = loc_vsnprintf_c(out, cap, fmt, args);
    va_end(args);
    return n;
}

/// @brief C-locale sprintf into a heap buffer that grows to fit (cap 4096).
/// @param out_len Receives the byte length on success (set to 0 otherwise).
/// @param fmt `printf`-style format string followed by its arguments.
/// @return malloc'd NUL-terminated string the caller frees, or NULL on OOM /
///         when the formatted result would exceed the 4096-byte safety cap.
static char *loc_sprintf_alloc_c(size_t *out_len, const char *fmt, ...) {
    if (out_len)
        *out_len = 0;
    size_t cap = 128;
    for (;;) {
        char *buf = (char *)malloc(cap);
        if (!buf)
            return NULL;
        va_list args;
        va_start(args, fmt);
        int n = loc_vsnprintf_c(buf, cap, fmt, args);
        va_end(args);
        if (n >= 0 && (size_t)n < cap) {
            if (out_len)
                *out_len = (size_t)n;
            return buf;
        }
        free(buf);
        if (n >= 0)
            cap = (size_t)n + 1;
        else
            cap *= 2;
        if (cap > 4096)
            return NULL;
    }
}

/// @brief strtod forced through the "C" LC_NUMERIC locale (parses '.' as the
///        decimal point regardless of the ambient locale); the parse inverse
///        of @ref loc_vsnprintf_c. Degrades to plain @c strtod if the "C"
///        locale object cannot be created.
/// @param input NUL-terminated C-locale numeric text to parse.
/// @param endptr Optional output receiving the first unconsumed byte.
/// @return Parsed double value with the same error conventions as `strtod`.
static double loc_strtod_c(const char *input, char **endptr) {
#if defined(_WIN32)
    _locale_t c_locale = loc_cached_c_locale();
    if (!c_locale)
        return strtod(input, endptr);
    double v = _strtod_l(input, endptr, c_locale);
    return v;
#else
    locale_t c_locale = loc_cached_c_locale();
    if (!c_locale)
        return strtod(input, endptr);
    locale_t old = uselocale(c_locale);
    if (!old) {
        return strtod(input, endptr);
    }
    double v = strtod(input, endptr);
    uselocale(old);
    return v;
#endif
}

//===----------------------------------------------------------------------===//
// Instance struct
//===----------------------------------------------------------------------===//

/// @brief Mutable options and retained locale state for one NumberFormat.
typedef struct rt_numformat {
    void *locale;                 ///< strong Locale handle ref
    const rt_locale_data_t *data; ///< non-owning
    int64_t min_frac;             ///< Minimum emitted fractional digits.
    int64_t max_frac;             ///< Maximum emitted fractional digits.
    int8_t grouping;              ///< Whether integer grouping is enabled.
    int8_t strict;                ///< Whether parsing enforces canonical groups.
    rounding_mode_t rounding;     ///< Active rounding policy.
} rt_numformat_t;

/// @brief Unchecked cast of an opaque handle to the NumberFormat instance.
/// @param obj Opaque handle expected to reference an `rt_numformat_t`.
/// @return @p obj reinterpreted as a NumberFormat pointer, including NULL.
static rt_numformat_t *as_fmt(void *obj) {
    return (rt_numformat_t *)obj;
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Runtime handle to release, or NULL for a no-op.
static void fmt_release_handle(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief GC finalizer: release the format's locale data and locale handle.
/// @param obj NumberFormat allocation being finalized. NULL is accepted.
static void fmt_finalizer(void *obj) {
    rt_numformat_t *fmt = (rt_numformat_t *)obj;
    if (!fmt)
        return;
    rt_locale_manager_release_data(fmt->data);
    fmt_release_handle(fmt->locale);
    fmt->locale = NULL;
    fmt->data = NULL;
}

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// @brief Allocate and initialize a GC-managed NumberFormat for @p locale.
/// @details Retains the locale handle and its resolved data, then installs the
///          common defaults of zero-to-three fraction digits, grouping enabled,
///          lenient parsing, and half-even rounding. Traps on allocation
///          failure and installs @ref fmt_finalizer.
/// @param locale Locale handle to retain and bind to the formatter.
/// @return Newly allocated formatter, or NULL after allocation failure if the
///         runtime trap hook returns.
static rt_numformat_t *fmt_alloc(void *locale) {
    rt_numformat_t *fmt = (rt_numformat_t *)rt_obj_new_i64(0, (int64_t)sizeof(rt_numformat_t));
    if (!fmt) {
        rt_trap("Zanna.Localization.NumberFormat: allocation failed");
        return NULL;
    }
    memset(fmt, 0, sizeof(*fmt));
    fmt->locale = locale;
    if (fmt->locale)
        rt_heap_retain(fmt->locale);
    fmt->data = rt_locale_get_data(locale);
    rt_locale_manager_retain_data(fmt->data);
    fmt->min_frac = 0;
    fmt->max_frac = 3; // default matches common display conventions
    fmt->grouping = 1;
    fmt->strict = 0;
    fmt->rounding = ROUND_HALF_EVEN;
    rt_obj_set_finalizer(fmt, fmt_finalizer);
    return fmt;
}

/// @brief Create a NumberFormat bound to the locale manager's current locale.
/// @details Balances the temporary retained handle returned by the locale
///          manager after the formatter has retained its own reference.
/// @return Newly allocated NumberFormat, or NULL after an allocation failure
///         if the runtime trap hook returns.
void *rt_numformat_new(void) {
    void *current = rt_locale_manager_current();
    void *fmt = fmt_alloc(current);
    fmt_release_handle(current);
    return fmt;
}

/// @brief Create a NumberFormat bound to an explicit locale.
/// @param locale Locale handle retained by the new formatter.
/// @return Newly allocated NumberFormat, or NULL after an allocation failure
///         if the runtime trap hook returns.
void *rt_numformat_for_locale(void *locale) {
    return fmt_alloc(locale);
}

//===----------------------------------------------------------------------===//
// Property accessors
//===----------------------------------------------------------------------===//

/// @brief Return the locale bound to a NumberFormat.
/// @param self NumberFormat handle, or NULL.
/// @return Borrowed locale handle, or NULL when @p self is NULL.
void *rt_numformat_get_locale(void *self) {
    return self ? as_fmt(self)->locale : NULL;
}

/// @brief Read the minimum emitted fraction-digit count.
/// @param self NumberFormat handle, or NULL.
/// @return Configured minimum, or zero when @p self is NULL.
int64_t rt_numformat_get_min_frac(void *self) {
    return self ? as_fmt(self)->min_frac : 0;
}

/// @brief Read the maximum emitted fraction-digit count.
/// @param self NumberFormat handle, or NULL.
/// @return Configured maximum, or three when @p self is NULL.
int64_t rt_numformat_get_max_frac(void *self) {
    return self ? as_fmt(self)->max_frac : 3;
}

/// @brief Read whether integer grouping is enabled.
/// @param self NumberFormat handle, or NULL.
/// @return 0 or 1; defaults to 1 when @p self is NULL.
int8_t rt_numformat_get_grouping(void *self) {
    return self ? as_fmt(self)->grouping : 1;
}

/// @brief Read whether strict grouping validation is enabled during parsing.
/// @param self NumberFormat handle, or NULL.
/// @return 0 or 1; defaults to 0 when @p self is NULL.
int8_t rt_numformat_get_strict(void *self) {
    return self ? as_fmt(self)->strict : 0;
}

/// @brief Set the minimum emitted fraction-digit count.
/// @details Clamps @p value to 0 through 20 and raises the maximum to preserve
///          the invariant `min_frac <= max_frac`. NULL @p self is a no-op.
/// @param self NumberFormat handle to update, or NULL.
/// @param value Requested minimum fraction-digit count.
void rt_numformat_set_min_frac(void *self, int64_t value) {
    if (!self)
        return;
    if (value < 0)
        value = 0;
    if (value > 20)
        value = 20;
    as_fmt(self)->min_frac = value;
    if (as_fmt(self)->max_frac < value)
        as_fmt(self)->max_frac = value;
}

/// @brief Set the maximum emitted fraction-digit count.
/// @details Clamps @p value to 0 through 20 and lowers the minimum to preserve
///          the invariant `min_frac <= max_frac`. NULL @p self is a no-op.
/// @param self NumberFormat handle to update, or NULL.
/// @param value Requested maximum fraction-digit count.
void rt_numformat_set_max_frac(void *self, int64_t value) {
    if (!self)
        return;
    if (value < 0)
        value = 0;
    if (value > 20)
        value = 20;
    as_fmt(self)->max_frac = value;
    if (as_fmt(self)->min_frac > value)
        as_fmt(self)->min_frac = value;
}

/// @brief Enable or disable integer grouping.
/// @param self NumberFormat handle to update, or NULL.
/// @param value Zero disables grouping; any non-zero value enables it.
void rt_numformat_set_grouping(void *self, int8_t value) {
    if (self)
        as_fmt(self)->grouping = value ? 1 : 0;
}

/// @brief Enable or disable strict grouping validation during parsing.
/// @param self NumberFormat handle to update, or NULL.
/// @param value Zero selects lenient parsing; any non-zero value selects strict parsing.
void rt_numformat_set_strict(void *self, int8_t value) {
    if (self)
        as_fmt(self)->strict = value ? 1 : 0;
}

/// @brief Return the formatter's canonical rounding-mode name.
/// @param self NumberFormat handle, or NULL for the half-even default.
/// @return Newly allocated runtime string naming the mode.
rt_string rt_numformat_get_rounding(void *self) {
    rounding_mode_t m = self ? as_fmt(self)->rounding : ROUND_HALF_EVEN;
    const char *name = rounding_mode_name(m);
    return rt_string_from_bytes(name, strlen(name));
}

/// @brief Select a rounding mode by canonical name.
/// @details Unknown names fall back to half-even. NULL arguments are ignored.
/// @param self NumberFormat handle to update, or NULL.
/// @param mode Runtime string such as `"halfEven"`, `"up"`, or `"floor"`.
void rt_numformat_set_rounding(void *self, rt_string mode) {
    if (!self || !mode)
        return;
    const char *cs = rt_string_cstr(mode);
    as_fmt(self)->rounding = rounding_mode_parse(cs);
}

//===----------------------------------------------------------------------===//
// Rounding
//===----------------------------------------------------------------------===//

/// @brief Round @p value to @p digits fractional places under @p mode.
/// @details Scales by 10^digits, applies the mode, then unscales. Non-finite
///          values and inputs that would overflow the scale are returned
///          unchanged. HALF_EVEN uses @c rint (banker's rounding on common
///          libcs); the signed-magnitude forms keep ±0 and sign correct.
/// @param value Number to round.
/// @param digits Fractional decimal places used to construct the scale.
/// @param mode Rounding policy to apply to the scaled value.
/// @return Rounded value, or @p value unchanged when it is non-finite or
///         cannot be scaled safely.
static double apply_rounding(double value, int digits, rounding_mode_t mode) {
    if (!isfinite(value))
        return value;
    double scale = pow(10.0, (double)digits);
    if (!isfinite(scale) || fabs(value) > DBL_MAX / scale)
        return value;
    double scaled = value * scale;
    double rounded;
    switch (mode) {
        case ROUND_HALF_UP: {
            double s = value < 0 ? -1.0 : 1.0;
            rounded = s * floor(fabs(scaled) + 0.5);
            break;
        }
        case ROUND_HALF_DOWN: {
            double s = value < 0 ? -1.0 : 1.0;
            double f = fabs(scaled);
            double fr = f - floor(f);
            if (fr > 0.5)
                rounded = s * (floor(f) + 1.0);
            else
                rounded = s * floor(f);
            break;
        }
        case ROUND_UP: {
            double s = value < 0 ? -1.0 : 1.0;
            rounded = s * ceil(fabs(scaled));
            break;
        }
        case ROUND_DOWN: {
            double s = value < 0 ? -1.0 : 1.0;
            rounded = s * floor(fabs(scaled));
            break;
        }
        case ROUND_CEILING:
            rounded = ceil(scaled);
            break;
        case ROUND_FLOOR:
            rounded = floor(scaled);
            break;
        case ROUND_HALF_EVEN:
        default:
            // rint with fegetround default gives banker's rounding on most
            // libcs. snprintf("%.*f") already does half-to-even on glibc.
            // Use the simple portable approximation here.
            rounded = rint(scaled);
            break;
    }
    return rounded / scale;
}

//===----------------------------------------------------------------------===//
// Core format helper: render a double with locale decimal + group separators
//===----------------------------------------------------------------------===//

/// @brief Byte slices for the ten code points in a locale digit set.
typedef struct digit_spans {
    const char *ptr[10];
    size_t len[10];
    int valid;
} digit_spans_t;

/// @brief Byte length of the leading UTF-8 codepoint in @p s (0 if empty/NULL,
///        1 on a malformed lead byte so callers always make forward progress).
/// @param s NUL-terminated byte sequence to inspect, or NULL.
/// @return Apparent leading code-point width from zero through four bytes.
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

/// @brief Slice the numbering system's 10 digit glyphs into a span table.
/// @details Walks the (possibly multi-byte) @c nums->digits string; falls back
///          to ASCII. @c ds.valid is set only when exactly ten codepoints were
///          consumed, so callers can safely index ds.ptr/len by digit value.
/// @param nums Borrowed locale numbering data, or NULL for ASCII digits.
/// @return Span table whose `valid` member reports whether exactly ten glyphs
///         consumed the complete digit string.
static digit_spans_t digit_spans_from_locale(const rt_locdata_numbers_t *nums) {
    digit_spans_t ds;
    memset(&ds, 0, sizeof(ds));
    const char *digits = nums && nums->digits ? nums->digits : "0123456789";
    const char *p = digits;
    for (int i = 0; i < 10; ++i) {
        size_t l = utf8_cp_len(p);
        if (l == 0) {
            ds.valid = 0;
            return ds;
        }
        ds.ptr[i] = p;
        ds.len[i] = l;
        p += l;
    }
    ds.valid = *p == '\0';
    return ds;
}

/// @brief Append bytes to a localized number-format builder.
/// @details Number formatting often builds intermediate grouped and localized
///          representations. This helper makes allocation and overflow failures
///          explicit so public formatters can return an empty string instead of
///          a partially formatted number.
/// @param sb Destination builder.
/// @param bytes Bytes to append.
/// @param len Number of bytes in @p bytes.
/// @return 1 on success, 0 when the append failed.
static int nf_append_bytes_checked(rt_string_builder *sb, const char *bytes, size_t len) {
    return rt_sb_append_bytes(sb, bytes, len) == RT_SB_OK;
}

/// @brief Append a C string to a localized number-format builder.
/// @param sb Destination builder.
/// @param text NUL-terminated bytes to append.
/// @return 1 on success, 0 when the append failed.
static int nf_append_cstr_checked(rt_string_builder *sb, const char *text) {
    return rt_sb_append_cstr(sb, text) == RT_SB_OK;
}

/// @brief Append @p len bytes, transliterating ASCII '0'-'9' to the numbering
///        system's native digit glyphs; non-digit bytes pass through verbatim.
/// @param sb Initialized destination builder.
/// @param nums Borrowed numbering data defining locale digit glyphs.
/// @param bytes Source bytes to append.
/// @param len Number of source bytes.
/// @return 1 on success, 0 when a builder append failed.
static int append_localized_bytes(rt_string_builder *sb,
                                  const rt_locdata_numbers_t *nums,
                                  const char *bytes,
                                  size_t len) {
    digit_spans_t ds = digit_spans_from_locale(nums);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)bytes[i];
        if (ds.valid && c >= '0' && c <= '9') {
            int d = (int)(c - '0');
            if (!nf_append_bytes_checked(sb, ds.ptr[d], ds.len[d]))
                return 0;
        } else {
            if (!nf_append_bytes_checked(sb, bytes + i, 1))
                return 0;
        }
    }
    return 1;
}

/// @brief Insert locale group separators into an ASCII digit run, honoring
///        distinct primary vs. secondary group sizes (e.g. Indian 12,34,567).
/// @details When grouping is disabled or no separator is defined the digits
///          are appended unchanged. Equal primary and secondary widths use a
///          uniform split; distinct widths reserve the rightmost primary group
///          and divide the remaining prefix by the secondary width.
/// @param sb Initialized destination builder.
/// @param fmt Formatter providing grouping options and locale number data.
/// @param digits ASCII decimal digit run without a sign or separator.
/// @param digit_len Number of bytes in @p digits.
/// @return 1 on success, 0 when a builder append failed.
static int append_grouped_ascii_number(rt_string_builder *sb,
                                       const rt_numformat_t *fmt,
                                       const char *digits,
                                       int digit_len) {
    const rt_locdata_numbers_t *nums = &fmt->data->numbers;
    rt_string_builder tmp;
    rt_sb_init(&tmp);
    int ok = 1;
    if (fmt->grouping && nums->group_sep && nums->group_sep[0] != '\0') {
        int primary = nums->group_size > 0 ? nums->group_size : 3;
        int secondary = nums->secondary_group_size > 0 ? nums->secondary_group_size : primary;
        size_t sep_len = strlen(nums->group_sep);
        if (primary == secondary) {
            if (digit_len <= 0) {
                ok = 1;
            } else if (primary <= 0 || primary >= digit_len) {
                ok = nf_append_bytes_checked(&tmp, digits, (size_t)digit_len);
            } else {
                int first = digit_len % primary;
                if (first == 0)
                    first = primary;
                ok = nf_append_bytes_checked(&tmp, digits, (size_t)first);
                int pos = first;
                while (ok && pos < digit_len) {
                    ok = nf_append_bytes_checked(&tmp, nums->group_sep, sep_len) &&
                         nf_append_bytes_checked(&tmp, digits + pos, (size_t)primary);
                    pos += primary;
                }
            }
        } else if (digit_len <= primary || primary <= 0 || secondary <= 0) {
            ok = nf_append_bytes_checked(&tmp, digits, (size_t)digit_len);
        } else {
            int prefix_len = digit_len - primary;
            int first = prefix_len % secondary;
            if (first == 0)
                first = secondary;
            ok = nf_append_bytes_checked(&tmp, digits, (size_t)first);
            int pos = first;
            while (ok && pos < prefix_len) {
                ok = nf_append_bytes_checked(&tmp, nums->group_sep, sep_len) &&
                     nf_append_bytes_checked(&tmp, digits + pos, (size_t)secondary);
                pos += secondary;
            }
            if (ok)
                ok = nf_append_bytes_checked(&tmp, nums->group_sep, sep_len) &&
                     nf_append_bytes_checked(&tmp, digits + prefix_len, (size_t)primary);
        }
    } else {
        ok = nf_append_bytes_checked(&tmp, digits, (size_t)digit_len);
    }
    if (ok)
        ok = append_localized_bytes(sb, nums, tmp.data, tmp.len);
    rt_sb_free(&tmp);
    return ok;
}

/// @brief Emit @p value into @p sb using locale numeric conventions and
///        effective fraction-digit limits.
/// @param sb         destination
/// @param fmt        formatter (for locale data + options)
/// @param value      real-valued input
/// @param override_digits    >= 0 to force exactly this many fraction digits
///                           (used by DecimalN); -1 to use min/max
/// @return 1 when the complete localized representation was appended; 0 on a
///         builder failure.
static int fmt_render_number(rt_string_builder *sb,
                             const rt_numformat_t *fmt,
                             double value,
                             int override_digits) {
    const rt_locdata_numbers_t *nums = &fmt->data->numbers;

    if (!isfinite(value)) {
        if (isnan(value)) {
            const char *s = nums->nan ? nums->nan : "NaN";
            return nf_append_cstr_checked(sb, s);
        }
        // Infinity: handle sign via the locale's minus/plus (rarely used).
        const char *inf = nums->infinity ? nums->infinity : "\xE2\x88\x9E";
        if (value < 0) {
            const char *minus = nums->minus ? nums->minus : "-";
            if (!nf_append_cstr_checked(sb, minus))
                return 0;
        }
        return nf_append_cstr_checked(sb, inf);
    }

    int digits_used;
    if (override_digits >= 0) {
        digits_used = override_digits;
        if (digits_used > 20)
            digits_used = 20;
    } else {
        int64_t mx = fmt->max_frac;
        if (mx < 0)
            mx = 0;
        if (mx > 20)
            mx = 20;
        digits_used = (int)mx;
    }

    // Round to the target precision.
    double rounded = apply_rounding(value, digits_used, fmt->rounding);
    int negative = rounded < 0;
    double abs_val = negative ? -rounded : rounded;

    // Render integer + fraction with a growable buffer. DBL_MAX with fixed
    // decimals needs hundreds of bytes; fixed stack buffers truncated this
    // path and let later grouping code read past the terminator.
    size_t full_size = 0;
    char *full = loc_sprintf_alloc_c(&full_size, "%.*f", digits_used, abs_val);
    if (!full) {
        return nf_append_cstr_checked(sb, nums->nan ? nums->nan : "NaN");
    }
    int full_len = full_size > (size_t)INT_MAX ? INT_MAX : (int)full_size;

    // Locate the decimal point in the "full" string.
    const char *frac_ptr = NULL;
    int frac_len = 0;
    for (int i = 0; i < full_len; ++i) {
        if (full[i] == '.') {
            frac_ptr = full + i + 1;
            frac_len = full_len - (i + 1);
            break;
        }
    }

    // Emit sign.
    if (negative) {
        const char *minus = nums->minus ? nums->minus : "-";
        if (!nf_append_cstr_checked(sb, minus))
            goto render_error;
    }

    // Emit integer part with optional grouping.
    int whole_len = frac_ptr ? (int)((frac_ptr - full) - 1) : full_len;
    if (!append_grouped_ascii_number(sb, fmt, full, whole_len))
        goto render_error;

    // Determine how many fraction digits to emit: clamp to [min_frac, digits_used]
    // and strip trailing zeros past min_frac.
    int emit_digits = digits_used;
    int min_f = override_digits >= 0 ? digits_used : (int)fmt->min_frac;
    if (min_f < 0)
        min_f = 0;
    if (min_f > digits_used)
        min_f = digits_used;
    if (frac_ptr && frac_len > 0 && override_digits < 0) {
        while (emit_digits > min_f && frac_ptr[emit_digits - 1] == '0')
            --emit_digits;
    }
    if (emit_digits > 0 && frac_ptr) {
        const char *dec_sep = nums->decimal_sep ? nums->decimal_sep : ".";
        if (!nf_append_cstr_checked(sb, dec_sep) ||
            !append_localized_bytes(sb, nums, frac_ptr, (size_t)emit_digits))
            goto render_error;
    }
    free(full);
    return 1;

render_error:
    free(full);
    return 0;
}

/// @brief Render a 64-bit integer with locale sign + grouping, no float path.
/// @details Avoids @c double so every magnitude (incl. INT64_MIN) is exact;
///          builds digits right-to-left into a stack buffer, negates via the
///          @c -(v+1)+1 trick so INT64_MIN doesn't overflow, then groups.
/// @param sb Initialized destination builder.
/// @param fmt Formatter providing sign, digit, and grouping conventions.
/// @param value Signed integer to render.
/// @return 1 when the complete representation was appended; 0 on failure.
static int fmt_render_integer_exact(rt_string_builder *sb,
                                    const rt_numformat_t *fmt,
                                    int64_t value) {
    const rt_locdata_numbers_t *nums = &fmt->data->numbers;
    uint64_t mag;
    if (value < 0) {
        const char *minus = nums->minus ? nums->minus : "-";
        if (!nf_append_cstr_checked(sb, minus))
            return 0;
        mag = (uint64_t)(-(value + 1)) + 1u;
    } else {
        mag = (uint64_t)value;
    }

    char digits[32];
    size_t pos = sizeof(digits);
    digits[--pos] = '\0';
    if (mag == 0) {
        digits[--pos] = '0';
    } else {
        while (mag > 0) {
            digits[--pos] = (char)('0' + (mag % 10u));
            mag /= 10u;
        }
    }
    return append_grouped_ascii_number(sb, fmt, digits + pos, (int)(sizeof(digits) - pos - 1));
}

//===----------------------------------------------------------------------===//
// Format surface
//===----------------------------------------------------------------------===//

/// @brief Format a double using the formatter's fraction and grouping options.
/// @details Localizes sign, separators, digit glyphs, NaN, and infinity.
/// @param self NumberFormat handle, or NULL.
/// @param value Floating-point value to format.
/// @return Newly allocated formatted string, or an owned empty string for NULL
///         @p self or a builder failure.
rt_string rt_numformat_decimal(void *self, double value) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_numformat_t *fmt = as_fmt(self);
    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!fmt_render_number(&sb, fmt, value, /*override_digits=*/-1)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

/// @brief Format a double with exactly a requested number of fraction digits.
/// @details Clamps @p digits to 0 through 20 and bypasses trailing-zero trimming.
/// @param self NumberFormat handle, or NULL.
/// @param value Floating-point value to format.
/// @param digits Requested exact fraction-digit count.
/// @return Newly allocated formatted string, or an owned empty string for NULL
///         @p self or a builder failure.
rt_string rt_numformat_decimal_n(void *self, double value, int64_t digits) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_numformat_t *fmt = as_fmt(self);
    if (digits < 0)
        digits = 0;
    if (digits > 20)
        digits = 20;
    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!fmt_render_number(&sb, fmt, value, (int)digits)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

/// @brief Format an exact signed 64-bit integer without a floating conversion.
/// @param self NumberFormat handle, or NULL.
/// @param value Integer to format, including the full `int64_t` range.
/// @return Newly allocated localized string, or an owned empty string for NULL
///         @p self or a builder failure.
rt_string rt_numformat_integer(void *self, int64_t value) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!fmt_render_integer_exact(&sb, as_fmt(self), value)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

/// @brief Format a ratio as a localized percentage.
/// @details Multiplies @p value by 100, uses the formatter's ordinary fraction
///          settings, then appends the locale percent token without added spacing.
/// @param self NumberFormat handle, or NULL.
/// @param value Ratio to scale and format.
/// @return Newly allocated percentage string, or an owned empty string for
///         NULL @p self or a builder failure.
rt_string rt_numformat_percent(void *self, double value) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_numformat_t *fmt = as_fmt(self);
    rt_string_builder sb;
    rt_sb_init(&sb);
    if (!fmt_render_number(&sb, fmt, value * 100.0, /*override_digits=*/-1)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    const char *pct = fmt->data->numbers.percent ? fmt->data->numbers.percent : "%";
    if (!nf_append_cstr_checked(&sb, pct)) {
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

/// @brief Expand a currency pattern ("{s}{n}" or similar) into @p sb.
/// @details Replaces `{s}` with @p symbol and `{n}` with @p number; all other
///          bytes, including unknown brace sequences, are copied literally.
/// @param sb Initialized destination builder.
/// @param pattern NUL-terminated locale pattern, or NULL for `"{s}{n}"`.
/// @param symbol Currency-symbol text, or NULL for `"$"`.
/// @param number Owned or borrowed runtime string containing the formatted magnitude.
/// @return 1 on success, 0 when a builder append failed or @p number is invalid.
static int expand_currency(rt_string_builder *sb,
                           const char *pattern,
                           const char *symbol,
                           rt_string number) {
    if (!pattern)
        pattern = "{s}{n}";
    const char *p = pattern;
    while (*p) {
        if (p[0] == '{' && p[1] == 's' && p[2] == '}') {
            if (!nf_append_cstr_checked(sb, symbol ? symbol : "$"))
                return 0;
            p += 3;
        } else if (p[0] == '{' && p[1] == 'n' && p[2] == '}') {
            if (!number)
                return 0;
            const char *cs = rt_string_cstr(number);
            int64_t len = rt_str_len(number);
            if (len < 0)
                return 0;
            if (cs && len > 0 && !nf_append_bytes_checked(sb, cs, (size_t)len))
                return 0;
            p += 3;
        } else {
            char c = *p++;
            if (!nf_append_bytes_checked(sb, &c, 1))
                return 0;
        }
    }
    return 1;
}

/// @brief Format a value using the locale's default currency metadata.
/// @details Renders the absolute magnitude at the currency fraction precision,
///          then applies the positive or negative locale pattern with the
///          default symbol.
/// @param self NumberFormat handle, or NULL.
/// @param value Monetary value to format.
/// @return Newly allocated currency string, or an owned empty string for NULL
///         @p self or an allocation/builder failure.
rt_string rt_numformat_currency(void *self, double value) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_numformat_t *fmt = as_fmt(self);
    const rt_locdata_currency_t *cur = &fmt->data->currency;

    // Render absolute number at the currency's fraction-digit precision.
    int digits = cur->fraction_digits >= 0 ? cur->fraction_digits : 2;
    rt_string_builder num_sb;
    rt_sb_init(&num_sb);
    if (!fmt_render_number(&num_sb, fmt, value < 0 ? -value : value, digits)) {
        rt_sb_free(&num_sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string num = rt_string_from_bytes(num_sb.data, num_sb.len);
    rt_sb_free(&num_sb);
    if (!num)
        return rt_string_from_bytes("", 0);

    rt_string_builder sb;
    rt_sb_init(&sb);
    const char *pattern = value < 0 ? cur->pattern_negative : cur->pattern_positive;
    if (!expand_currency(&sb, pattern, cur->symbol, num)) {
        rt_string_unref(num);
        rt_sb_free(&sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string_unref(num);

    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;
}

/// @brief Format a value for a specified ISO 4217 currency code.
/// @details Requires exactly three uppercase ASCII letters. The locale symbol
///          is used for its default code; other codes are emitted literally in
///          the pattern's symbol position because no global symbol table is
///          maintained here.
/// @param self NumberFormat handle, or NULL.
/// @param value Monetary value to format.
/// @param code Three-uppercase-letter runtime string.
/// @return Newly allocated currency string, or an owned empty string after
///         invalid input, NULL @p self, or an allocation/builder failure.
rt_string rt_numformat_currency_of(void *self, double value, rt_string code) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_numformat_t *fmt = as_fmt(self);
    const rt_locdata_currency_t *cur = &fmt->data->currency;

    // Currency code must be 3 chars (ISO-4217) — use it as the symbol if
    // no locale-specific override is available. A future phase can maintain
    // a baked ISO-code -> symbol table.
    const char *sym = cur->symbol ? cur->symbol : "$";
    const char *code_cs = code ? rt_string_cstr(code) : NULL;
    int64_t code_len = code ? rt_str_len(code) : 0;
    if (!code_cs || code_len != 3 || code_cs[0] < 'A' || code_cs[0] > 'Z' || code_cs[1] < 'A' ||
        code_cs[1] > 'Z' || code_cs[2] < 'A' || code_cs[2] > 'Z') {
        rt_trap(
            "Zanna.Localization.NumberFormat: CurrencyOf requires a 3-letter uppercase ISO code");
        return rt_string_from_bytes("", 0);
    }
    if (fmt->data->currency.default_code &&
        strncmp(code_cs, fmt->data->currency.default_code, 3) != 0) {
        // Non-default code — use the code itself as the symbol placeholder.
        // Phase 2 ships without the full ISO symbol table; treating it as
        // the literal code is honest and safe.
        sym = code_cs;
    }

    int digits = cur->fraction_digits >= 0 ? cur->fraction_digits : 2;
    rt_string_builder num_sb;
    rt_sb_init(&num_sb);
    if (!fmt_render_number(&num_sb, fmt, value < 0 ? -value : value, digits)) {
        rt_sb_free(&num_sb);
        return rt_string_from_bytes("", 0);
    }
    rt_string num = rt_string_from_bytes(num_sb.data, num_sb.len);
    rt_sb_free(&num_sb);
    if (!num)
        return rt_string_from_bytes("", 0);

    rt_string_builder sb;
    rt_sb_init(&sb);
    const char *pattern = value < 0 ? cur->pattern_negative : cur->pattern_positive;
    // Emulate expand_currency with the overridden symbol.
    if (!pattern)
        pattern = "{s}{n}";
    const char *p = pattern;
    while (*p) {
        if (p[0] == '{' && p[1] == 's' && p[2] == '}') {
            if (!nf_append_cstr_checked(&sb, sym))
                goto currency_of_error;
            p += 3;
        } else if (p[0] == '{' && p[1] == 'n' && p[2] == '}') {
            const char *cs = rt_string_cstr(num);
            int64_t len = rt_str_len(num);
            if (len < 0)
                goto currency_of_error;
            if (cs && len > 0 && !nf_append_bytes_checked(&sb, cs, (size_t)len))
                goto currency_of_error;
            p += 3;
        } else {
            char c = *p++;
            if (!nf_append_bytes_checked(&sb, &c, 1))
                goto currency_of_error;
        }
    }
    rt_string_unref(num);

    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;

currency_of_error:
    rt_string_unref(num);
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}

/// @brief Format a value in localized scientific notation.
/// @details Clamps mantissa fraction digits to 0 through 20, renders the
///          intermediate in the C locale, and substitutes locale decimal,
///          exponent, sign, and digit tokens. Non-finite values use the same
///          tokens as decimal formatting.
/// @param self NumberFormat handle, or NULL.
/// @param value Floating-point value to format.
/// @param digits Requested mantissa fraction-digit count.
/// @return Newly allocated scientific string, or an owned empty string for
///         NULL @p self or a formatting/builder failure.
rt_string rt_numformat_scientific(void *self, double value, int64_t digits) {
    if (!self)
        return rt_string_from_bytes("", 0);
    rt_numformat_t *fmt = as_fmt(self);
    const rt_locdata_numbers_t *nums = &fmt->data->numbers;

    // Special values use the same locale tokens as Decimal (VDOC-072).
    if (!isfinite(value)) {
        rt_string_builder ssb;
        rt_sb_init(&ssb);
        if (!fmt_render_number(&ssb, fmt, value, 0)) {
            rt_sb_free(&ssb);
            return rt_string_from_bytes("", 0);
        }
        rt_string sr = rt_string_from_bytes(ssb.data, ssb.len);
        rt_sb_free(&ssb);
        return sr;
    }

    int d = (int)digits;
    if (d < 0)
        d = 0;
    if (d > 20)
        d = 20;

    char buf[64];
    int len = loc_snprintf_c(buf, sizeof(buf), "%.*e", d, value);
    if (len < 0)
        len = 0;

    // Substitute the locale's decimal separator, exponent marker, digit
    // glyphs, and mantissa/exponent sign tokens (VDOC-072).
    const char *dec_sep = nums->decimal_sep;
    const char *exp_char = nums->exponent;
    const char *minus = nums->minus ? nums->minus : "-";
    const char *plus = nums->plus ? nums->plus : "+";
    rt_string_builder sb;
    rt_sb_init(&sb);
    for (int i = 0; i < len; ++i) {
        if (buf[i] == '.' && dec_sep && strcmp(dec_sep, ".") != 0) {
            if (!nf_append_cstr_checked(&sb, dec_sep))
                goto scientific_error;
        } else if ((buf[i] == 'e' || buf[i] == 'E') && exp_char) {
            if (!nf_append_cstr_checked(&sb, exp_char))
                goto scientific_error;
        } else if (buf[i] == '-') {
            if (!nf_append_cstr_checked(&sb, minus))
                goto scientific_error;
        } else if (buf[i] == '+') {
            if (!nf_append_cstr_checked(&sb, plus))
                goto scientific_error;
        } else {
            if (!append_localized_bytes(&sb, nums, buf + i, 1))
                goto scientific_error;
        }
    }
    rt_string r = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return r;

scientific_error:
    rt_sb_free(&sb);
    return rt_string_from_bytes("", 0);
}

/// @brief Format an integer with the invariant English ordinal suffix rules.
/// @details This initial implementation delegates to
///          `Zanna.Text.InvariantNumberFormat`; it does not yet consult the
///          bound locale or locale-specific ordinal category data.
/// @param self NumberFormat handle, currently unused and permitted to be NULL.
/// @param value Integer to format as an ordinal.
/// @return Newly allocated invariant ordinal string.
rt_string rt_numformat_ordinal(void *self, int64_t value) {
    (void)self;
    // v1: delegate to the existing Zanna.Text.InvariantNumberFormat.Ordinal which
    // implements English suffixes (st/nd/rd/th). Locale-specific ordinal
    // rendering uses plural category lookup and is deferred to a later
    // phase alongside JSON-loaded ordinal suffix tables.
    return rt_numfmt_ordinal(value);
}

//===----------------------------------------------------------------------===//
// Parse helpers
//===----------------------------------------------------------------------===//

/// @brief Try to match @p prefix at the start of @p input; return length
///        consumed on success, 0 on no match. NULL/empty prefix matches 0.
/// @param input Input byte slice to inspect.
/// @param input_len Available bytes in @p input.
/// @param prefix NUL-terminated prefix to match, or NULL.
/// @return Byte length of @p prefix on an exact match; otherwise zero.
static size_t match_prefix(const char *input, size_t input_len, const char *prefix) {
    if (!prefix || !*prefix)
        return 0;
    size_t plen = strlen(prefix);
    if (input_len < plen)
        return 0;
    if (memcmp(input, prefix, plen) == 0)
        return plen;
    return 0;
}

/// @brief Value and sign metadata returned by internal numeric parsers.
typedef struct {
    int success;  ///< 1 on full parse, 0 on failure
    double value; ///< parsed value (double precision)
    int had_sign; ///< 1 if input had an explicit sign
    int negative;
    int64_t int_value; ///< exact integer value when allow_fraction == 0
} parse_result_t;

/// @brief Match one digit at @p input, ASCII or the locale's native glyphs.
/// @param input Input byte slice to inspect.
/// @param input_len Available bytes in @p input.
/// @param nums Borrowed locale numbering data.
/// @param consumed Receives the byte length of the matched glyph (0 if none).
/// @return The digit value 0-9, or -1 if @p input does not start with a digit.
static int match_locale_digit(const char *input,
                              size_t input_len,
                              const rt_locdata_numbers_t *nums,
                              size_t *consumed) {
    if (consumed)
        *consumed = 0;
    if (input_len == 0)
        return -1;
    if (input[0] >= '0' && input[0] <= '9') {
        if (consumed)
            *consumed = 1;
        return input[0] - '0';
    }
    digit_spans_t ds = digit_spans_from_locale(nums);
    if (!ds.valid)
        return -1;
    for (int d = 0; d < 10; ++d) {
        if (ds.len[d] > 0 && input_len >= ds.len[d] && memcmp(input, ds.ptr[d], ds.len[d]) == 0) {
            if (consumed)
                *consumed = ds.len[d];
            return d;
        }
    }
    return -1;
}

/// @brief Builders and flags produced by locale-aware numeric tokenization.
typedef struct {
    int success;
    int negative;
    int had_sign;
    int had_frac;
    rt_string_builder digits;
    rt_string_builder frac;
} scan_number_t;

/// @brief Free the two string builders owned by a scan_number_t.
/// @param sn Initialized scan state whose builders will be released.
static void scan_number_free(scan_number_t *sn) {
    rt_sb_free(&sn->digits);
    rt_sb_free(&sn->frac);
}

/// @brief Tokenize @p input into sign / integer-digit / fraction-digit parts.
/// @details Locale-aware: skips leading space, recognizes the locale minus
///          sign and (when @p allow_fraction) decimal separator, accepts ASCII
///          or native digit glyphs, and ignores group separators. Leaves the
///          collected ASCII digit runs in @c sn->digits / @c sn->frac for a
///          downstream exact/double parse. @c sn->success reports a clean scan.
/// @param sn Output scan state; initialized by this function even on failure.
/// @param input Input byte slice to tokenize.
/// @param input_len Number of available input bytes.
/// @param fmt Formatter supplying locale tokens and strictness.
/// @param allow_fraction Non-zero to accept a locale decimal separator and
///                       fractional digits; zero rejects fractional content.
static void scan_number_parts(scan_number_t *sn,
                              const char *input,
                              size_t input_len,
                              const rt_numformat_t *fmt,
                              int allow_fraction) {
    memset(sn, 0, sizeof(*sn));
    rt_sb_init(&sn->digits);
    rt_sb_init(&sn->frac);

    size_t i = 0;
    while (i < input_len && isspace((unsigned char)input[i]))
        ++i;
    if (i >= input_len)
        return;

    const rt_locdata_numbers_t *nums = &fmt->data->numbers;
    const char *minus = nums->minus ? nums->minus : "-";
    const char *plus = nums->plus ? nums->plus : "+";
    const char *dec_sep = nums->decimal_sep ? nums->decimal_sep : ".";
    const char *grp_sep = nums->group_sep ? nums->group_sep : "";
    size_t dec_len = strlen(dec_sep);
    size_t grp_len = strlen(grp_sep);

    // Sign.
    size_t adv = match_prefix(input + i, input_len - i, minus);
    if (adv) {
        sn->had_sign = 1;
        sn->negative = 1;
        i += adv;
    } else {
        adv = match_prefix(input + i, input_len - i, plus);
        if (adv) {
            sn->had_sign = 1;
            i += adv;
        }
    }

    size_t digit_count = 0;
    int group_run = 0; // digits since last separator
    int had_group_sep = 0;
    int first_group_len = -1; // length of leading group when we hit first sep
    int primary_group = nums->group_size > 0 ? nums->group_size : 3;
    int secondary_group =
        nums->secondary_group_size > 0 ? nums->secondary_group_size : primary_group;

    while (i < input_len) {
        size_t consumed = 0;
        int digit = match_locale_digit(input + i, input_len - i, nums, &consumed);
        if (digit >= 0) {
            char c = (char)('0' + digit);
            if (!nf_append_bytes_checked(&sn->digits, &c, 1))
                return;
            digit_count++;
            i += consumed;
            ++group_run;
        } else if (grp_len && i + grp_len <= input_len &&
                   memcmp(input + i, grp_sep, grp_len) == 0) {
            if (digit_count == 0)
                return; // leading group sep
            if (group_run == 0)
                return; // empty group run, e.g. "1,,2" (VDOC-071)
            if (!had_group_sep) {
                first_group_len = group_run;
                had_group_sep = 1;
            } else if (fmt->strict && group_run != secondary_group) {
                return; // strict: inner groups must be exactly secondary_group
            }
            group_run = 0;
            i += grp_len;
        } else {
            break;
        }
    }

    // A separator must always be followed by at least one digit, even in
    // lenient mode ("1," and "1,.5" are malformed, not noncanonical).
    if (had_group_sep && group_run == 0)
        return;

    if (fmt->strict && had_group_sep) {
        if (group_run != primary_group)
            return;
        if (first_group_len <= 0 || first_group_len > secondary_group)
            return;
    }

    // Fractional part.
    size_t frac_count = 0;
    if (allow_fraction && i + dec_len <= input_len && memcmp(input + i, dec_sep, dec_len) == 0) {
        i += dec_len;
        sn->had_frac = 1;
        while (i < input_len) {
            size_t consumed = 0;
            int digit = match_locale_digit(input + i, input_len - i, nums, &consumed);
            if (digit >= 0) {
                char c = (char)('0' + digit);
                if (!nf_append_bytes_checked(&sn->frac, &c, 1))
                    return;
                frac_count++;
                i += consumed;
            } else {
                break;
            }
        }
        if (frac_count == 0)
            return; // bare separator with no digits
    }

    // Trailing whitespace tolerated; anything else is a failure.
    while (i < input_len && isspace((unsigned char)input[i]))
        ++i;
    if (i != input_len)
        return;

    if (digit_count == 0 && frac_count == 0)
        return;

    sn->success = 1;
}

/// @brief Recognize the locale's non-finite tokens: [sign] (nan | infinity).
/// @details Formatting deliberately emits `numbers.nan` / `numbers.infinity`,
///          so the paired parsers accept those exact tokens back (VDOC-085).
/// @param input Input byte slice to inspect.
/// @param input_len Number of available input bytes.
/// @param fmt Formatter supplying sign and non-finite tokens.
/// @param pr Parse-result output filled only after a complete token match.
/// @return 1 with @p pr filled when the whole input is a special token.
static int parse_special_value(const char *input,
                               size_t input_len,
                               const rt_numformat_t *fmt,
                               parse_result_t *pr) {
    const rt_locdata_numbers_t *nums = &fmt->data->numbers;
    const char *p = input;
    size_t len = input_len;
    while (len > 0 && isspace((unsigned char)p[0])) {
        ++p;
        --len;
    }
    while (len > 0 && isspace((unsigned char)p[len - 1]))
        --len;

    int negative = 0;
    int had_sign = 0;
    const char *minus = nums->minus ? nums->minus : "-";
    const char *plus = nums->plus ? nums->plus : "+";
    size_t adv = match_prefix(p, len, minus);
    if (adv) {
        negative = 1;
        had_sign = 1;
        p += adv;
        len -= adv;
    } else {
        adv = match_prefix(p, len, plus);
        if (adv) {
            had_sign = 1;
            p += adv;
            len -= adv;
        }
    }

    const char *nan_tok = nums->nan ? nums->nan : "NaN";
    const char *inf_tok = nums->infinity ? nums->infinity : "\xE2\x88\x9E";
    size_t nan_len = strlen(nan_tok);
    size_t inf_len = strlen(inf_tok);
    if (nan_len && len == nan_len && memcmp(p, nan_tok, nan_len) == 0) {
        pr->value = (double)NAN;
        pr->had_sign = had_sign;
        pr->negative = negative;
        pr->success = 1;
        return 1;
    }
    if (inf_len && len == inf_len && memcmp(p, inf_tok, inf_len) == 0) {
        pr->value = negative ? -(double)INFINITY : (double)INFINITY;
        pr->had_sign = had_sign;
        pr->negative = negative;
        pr->success = 1;
        return 1;
    }
    return 0;
}

/// @brief Parse a locale-formatted decimal number from @p input.
/// @details Accepts locale or ASCII digits, locale signs and separators,
///          surrounding whitespace, and the locale's NaN/infinity tokens.
///          Finite input is normalized to a C-locale decimal before conversion.
/// @param input Input byte slice to parse.
/// @param input_len Number of available input bytes.
/// @param fmt Formatter supplying locale data and strictness.
/// @param allow_fraction Non-zero to allow a fractional component.
/// @return Parse result with `success` set only after a complete, in-range
///         conversion.
static parse_result_t parse_decimal(const char *input,
                                    size_t input_len,
                                    const rt_numformat_t *fmt,
                                    int allow_fraction) {
    parse_result_t pr = {0};
    if (parse_special_value(input, input_len, fmt, &pr))
        return pr;
    scan_number_t sn;
    scan_number_parts(&sn, input, input_len, fmt, allow_fraction);
    if (!sn.success) {
        scan_number_free(&sn);
        return pr;
    }

    // Build a canonical "." decimal string and strtod it.
    rt_string_builder canonical;
    rt_sb_init(&canonical);
    if (sn.negative && !nf_append_bytes_checked(&canonical, "-", 1))
        goto parse_fail;
    if (sn.digits.len == 0) {
        if (!nf_append_bytes_checked(&canonical, "0", 1))
            goto parse_fail;
    } else if (!nf_append_bytes_checked(&canonical, sn.digits.data, sn.digits.len)) {
        goto parse_fail;
    }
    if (sn.had_frac) {
        if (!nf_append_bytes_checked(&canonical, ".", 1) ||
            !nf_append_bytes_checked(&canonical, sn.frac.data, sn.frac.len))
            goto parse_fail;
    }
    if (!nf_append_bytes_checked(&canonical, "\0", 1))
        goto parse_fail;

    char *end = NULL;
    errno = 0;
    double v = loc_strtod_c(canonical.data, &end);
    if (end == canonical.data || errno == ERANGE || !isfinite(v)) {
        rt_sb_free(&canonical);
        scan_number_free(&sn);
        return pr;
    }

    pr.value = v;
    pr.had_sign = sn.had_sign;
    pr.negative = sn.negative;
    pr.success = 1;
    rt_sb_free(&canonical);
    scan_number_free(&sn);
    return pr;

parse_fail:
    rt_sb_free(&canonical);
    scan_number_free(&sn);
    return pr;
}

/// @brief Parse @p input to an exact int64 (no float round-trip).
/// @details Scans digits via @ref scan_number_parts (fraction disallowed) then
///          accumulates in @c uint64_t with a per-digit overflow guard against
///          a sign-aware limit, so INT64_MIN parses and out-of-range fails
///          cleanly. @c pr.success is 0 on empty/overflow/garbage input.
/// @param input Input byte slice to parse.
/// @param input_len Number of available input bytes.
/// @param fmt Formatter supplying locale data and strictness.
/// @return Parse result with `int_value` populated only on a complete,
///         in-range integer conversion.
static parse_result_t parse_integer_exact(const char *input,
                                          size_t input_len,
                                          const rt_numformat_t *fmt) {
    parse_result_t pr = {0};
    scan_number_t sn;
    scan_number_parts(&sn, input, input_len, fmt, /*allow_fraction=*/0);
    if (!sn.success || sn.digits.len == 0) {
        scan_number_free(&sn);
        return pr;
    }
    uint64_t limit = sn.negative ? ((uint64_t)INT64_MAX + 1u) : (uint64_t)INT64_MAX;
    uint64_t acc = 0;
    for (size_t i = 0; i < sn.digits.len; ++i) {
        unsigned digit = (unsigned)(sn.digits.data[i] - '0');
        if (acc > (limit - digit) / 10u) {
            scan_number_free(&sn);
            return pr;
        }
        acc = acc * 10u + digit;
    }
    if (sn.negative) {
        if (acc == ((uint64_t)INT64_MAX + 1u))
            pr.int_value = INT64_MIN;
        else
            pr.int_value = -(int64_t)acc;
    } else {
        pr.int_value = (int64_t)acc;
    }
    pr.value = (double)pr.int_value;
    pr.had_sign = sn.had_sign;
    pr.negative = sn.negative;
    pr.success = 1;
    scan_number_free(&sn);
    return pr;
}

//===----------------------------------------------------------------------===//
// Parse API
//===----------------------------------------------------------------------===//

/// @brief Parse a required locale-formatted decimal.
/// @details Accepts locale non-finite tokens as well as finite decimal input.
///          Invalid input raises a runtime trap and yields zero if the trap
///          hook returns.
/// @param self NumberFormat handle.
/// @param input Runtime string to parse.
/// @return Parsed double, or zero after a trapped error.
double rt_numformat_parse_decimal(void *self, rt_string input) {
    if (!self || !input) {
        rt_trap("Zanna.Localization.NumberFormat: ParseDecimal received null input");
        return 0;
    }
    const char *cs = rt_string_cstr(input);
    int64_t len = rt_str_len(input);
    parse_result_t pr = parse_decimal(cs,
                                      (size_t)len,
                                      as_fmt(self),
                                      /*allow_fraction=*/1);
    if (!pr.success) {
        rt_trap("Zanna.Localization.NumberFormat: cannot parse input as a decimal");
        return 0;
    }
    return pr.value;
}

/// @brief Try to parse a locale-formatted decimal without trapping.
/// @param self NumberFormat handle, or NULL.
/// @param input Runtime string to parse, or NULL.
/// @return Newly allocated `Some<f64>` on success; otherwise `None`.
void *rt_numformat_try_parse_decimal(void *self, rt_string input) {
    if (!self || !input)
        return rt_option_none();
    const char *cs = rt_string_cstr(input);
    int64_t len = rt_str_len(input);
    parse_result_t pr = parse_decimal(cs,
                                      (size_t)len,
                                      as_fmt(self),
                                      /*allow_fraction=*/1);
    if (!pr.success)
        return rt_option_none();
    return rt_option_some_f64(pr.value);
}

/// @brief Parse a required locale-formatted exact 64-bit integer.
/// @details Fractional, non-finite, malformed, or out-of-range input raises a
///          runtime trap and yields zero if the trap hook returns.
/// @param self NumberFormat handle.
/// @param input Runtime string to parse.
/// @return Exact parsed integer, or zero after a trapped error.
int64_t rt_numformat_parse_integer(void *self, rt_string input) {
    if (!self || !input) {
        rt_trap("Zanna.Localization.NumberFormat: ParseInteger received null input");
        return 0;
    }
    const char *cs = rt_string_cstr(input);
    int64_t len = rt_str_len(input);
    parse_result_t pr = parse_integer_exact(cs, (size_t)len, as_fmt(self));
    if (!pr.success) {
        rt_trap("Zanna.Localization.NumberFormat: cannot parse input as an integer");
        return 0;
    }
    return pr.int_value;
}

/// @brief Try to parse a locale-formatted exact integer without trapping.
/// @param self NumberFormat handle, or NULL.
/// @param input Runtime string to parse, or NULL.
/// @return Newly allocated `Some<i64>` on success; otherwise `None`.
void *rt_numformat_try_parse_integer(void *self, rt_string input) {
    if (!self || !input)
        return rt_option_none();
    const char *cs = rt_string_cstr(input);
    int64_t len = rt_str_len(input);
    parse_result_t pr = parse_integer_exact(cs, (size_t)len, as_fmt(self));
    if (!pr.success)
        return rt_option_none();
    return rt_option_some_i64(pr.int_value);
}

/// @brief Trim the locale's currency symbol (and the default ISO code) from
///        either end of @p input, returning the trimmed slice via @p out_start
///        and @p out_len.
/// @details Also accepts one standalone three-uppercase-letter code at either
///          end and trims whitespace around the remaining numeric slice.
/// @param fmt Formatter supplying the default symbol and ISO code.
/// @param input Currency input byte slice.
/// @param input_len Number of available input bytes.
/// @param out_start Receives a borrowed pointer into @p input.
/// @param out_len Receives the length of the trimmed numeric slice.
static void strip_currency_affixes(const rt_numformat_t *fmt,
                                   const char *input,
                                   size_t input_len,
                                   const char **out_start,
                                   size_t *out_len) {
    const char *symbol = fmt->data->currency.symbol;
    const char *code = fmt->data->currency.default_code;
    const char *p = input;
    size_t len = input_len;

    // Leading whitespace.
    while (len > 0 && isspace((unsigned char)p[0])) {
        ++p;
        --len;
    }

    // Leading symbol / code. Any 3-uppercase-letter ISO code is accepted so
    // CurrencyOf's non-default-code output round-trips (VDOC-084); the code
    // must not be immediately followed by another letter.
    for (int attempt = 0; attempt < 2; ++attempt) {
        size_t adv = match_prefix(p, len, symbol);
        if (adv) {
            p += adv;
            len -= adv;
            continue;
        }
        adv = match_prefix(p, len, code);
        if (adv) {
            p += adv;
            len -= adv;
            continue;
        }
        if (len >= 3 && p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'A' && p[1] <= 'Z' &&
            p[2] >= 'A' && p[2] <= 'Z' &&
            !(len > 3 && ((p[3] >= 'A' && p[3] <= 'Z') || (p[3] >= 'a' && p[3] <= 'z')))) {
            p += 3;
            len -= 3;
            continue;
        }
        break;
    }

    // Leading whitespace again.
    while (len > 0 && isspace((unsigned char)p[0])) {
        ++p;
        --len;
    }

    // Trailing whitespace.
    while (len > 0 && isspace((unsigned char)p[len - 1])) {
        --len;
    }

    // Trailing symbol / code (any 3-uppercase-letter ISO code accepted,
    // provided it is not preceded by another letter — see VDOC-084).
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (symbol) {
            size_t slen = strlen(symbol);
            if (slen && len >= slen && memcmp(p + len - slen, symbol, slen) == 0) {
                len -= slen;
                continue;
            }
        }
        if (code) {
            size_t clen = strlen(code);
            if (clen && len >= clen && memcmp(p + len - clen, code, clen) == 0) {
                len -= clen;
                continue;
            }
        }
        if (len >= 3 && p[len - 1] >= 'A' && p[len - 1] <= 'Z' && p[len - 2] >= 'A' &&
            p[len - 2] <= 'Z' && p[len - 3] >= 'A' && p[len - 3] <= 'Z' &&
            !(len > 3 && ((p[len - 4] >= 'A' && p[len - 4] <= 'Z') ||
                          (p[len - 4] >= 'a' && p[len - 4] <= 'z')))) {
            len -= 3;
            continue;
        }
        break;
    }

    // Re-trim whitespace around inner content.
    while (len > 0 && isspace((unsigned char)p[0])) {
        ++p;
        --len;
    }
    while (len > 0 && isspace((unsigned char)p[len - 1])) {
        --len;
    }

    *out_start = p;
    *out_len = len;
}

/// @brief Expand a currency-pattern affix slice into literal text: "{s}" →
///        @p symbol, other bytes copied as-is.
/// @param sb Initialized destination builder.
/// @param p Pattern-affix byte slice to expand.
/// @param len Number of bytes in @p p.
/// @param symbol NUL-terminated replacement for `{s}`, or NULL to emit nothing.
/// @return 1 on success; 0 if the "{n}" number placeholder is encountered
///         or a builder append fails (the caller splits the pattern on "{n}",
///         so it must not appear here).
static int expand_currency_pattern_affix(rt_string_builder *sb,
                                         const char *p,
                                         size_t len,
                                         const char *symbol) {
    for (size_t i = 0; i < len;) {
        if (i + 3 <= len && p[i] == '{' && p[i + 1] == 's' && p[i + 2] == '}') {
            if (symbol && !nf_append_cstr_checked(sb, symbol))
                return 0;
            i += 3;
        } else if (i + 3 <= len && p[i] == '{' && p[i + 1] == 'n' && p[i + 2] == '}') {
            return 0;
        } else {
            if (!nf_append_bytes_checked(sb, p + i, 1))
                return 0;
            ++i;
        }
    }
    return 1;
}

/// @brief Match @p input against a currency @p pattern split on "{n}".
/// @details Expands the prefix/suffix affixes (substituting @p symbol), then
///          checks @p input starts/ends with them; on success returns the
///          interior number slice via @p out_start / @p out_len.
/// @param pattern NUL-terminated locale currency pattern.
/// @param symbol NUL-terminated replacement for the symbol placeholder.
/// @param input Currency input byte slice to match.
/// @param input_len Number of available input bytes.
/// @param out_start Receives a borrowed pointer to the interior number.
/// @param out_len Receives the interior number's byte length.
/// @return 1 on match, 0 if the pattern lacks "{n}" or the affixes don't match.
static int match_currency_pattern(const char *pattern,
                                  const char *symbol,
                                  const char *input,
                                  size_t input_len,
                                  const char **out_start,
                                  size_t *out_len) {
    if (!pattern)
        return 0;
    const char *nph = strstr(pattern, "{n}");
    if (!nph)
        return 0;
    rt_string_builder pre;
    rt_string_builder suf;
    rt_sb_init(&pre);
    rt_sb_init(&suf);
    if (!expand_currency_pattern_affix(&pre, pattern, (size_t)(nph - pattern), symbol) ||
        !expand_currency_pattern_affix(&suf, nph + 3, strlen(nph + 3), symbol)) {
        rt_sb_free(&pre);
        rt_sb_free(&suf);
        return 0;
    }
    if (input_len < pre.len + suf.len || (pre.len && memcmp(input, pre.data, pre.len) != 0) ||
        (suf.len && memcmp(input + input_len - suf.len, suf.data, suf.len) != 0)) {
        rt_sb_free(&pre);
        rt_sb_free(&suf);
        return 0;
    }
    *out_start = input + pre.len;
    *out_len = input_len - pre.len - suf.len;
    rt_sb_free(&pre);
    rt_sb_free(&suf);
    return 1;
}

/// @brief Strip currency framing to expose the bare numeric slice.
/// @details Trims whitespace, detects accounting-style negatives "(123)" and
///          reports them via @p out_negative, then removes the locale's
///          currency symbol/ISO-code affixes. Yields the number text via
///          @p out_start / @p out_len for a downstream numeric parse.
/// @param fmt Formatter supplying currency patterns, symbol, and default code.
/// @param input Currency input byte slice.
/// @param input_len Number of available input bytes.
/// @param out_start Receives a borrowed pointer to the bare number slice.
/// @param out_len Receives the bare number slice's byte length.
/// @param out_negative Receives non-zero when accounting or negative-pattern
///                     framing supplies a negative sign.
/// @return Always 1 after producing a slice; unrecognized framing falls back
///         to permissive affix stripping.
static int extract_currency_number(const rt_numformat_t *fmt,
                                   const char *input,
                                   size_t input_len,
                                   const char **out_start,
                                   size_t *out_len,
                                   int *out_negative) {
    const char *p = input;
    size_t len = input_len;
    while (len > 0 && isspace((unsigned char)p[0])) {
        ++p;
        --len;
    }
    while (len > 0 && isspace((unsigned char)p[len - 1])) {
        --len;
    }

    int accounting = 0;
    if (len >= 2 && p[0] == '(' && p[len - 1] == ')') {
        accounting = 1;
        ++p;
        len -= 2;
        while (len > 0 && isspace((unsigned char)p[0])) {
            ++p;
            --len;
        }
        while (len > 0 && isspace((unsigned char)p[len - 1])) {
            --len;
        }
    }

    const char *inner = NULL;
    size_t inner_len = 0;
    const char *symbol = fmt->data->currency.symbol;
    const char *code = fmt->data->currency.default_code;
    if (match_currency_pattern(
            fmt->data->currency.pattern_negative, symbol, p, len, &inner, &inner_len) ||
        match_currency_pattern(
            fmt->data->currency.pattern_negative, code, p, len, &inner, &inner_len)) {
        *out_start = inner;
        *out_len = inner_len;
        *out_negative = 1;
        return 1;
    }
    if (match_currency_pattern(
            fmt->data->currency.pattern_positive, symbol, p, len, &inner, &inner_len) ||
        match_currency_pattern(
            fmt->data->currency.pattern_positive, code, p, len, &inner, &inner_len)) {
        *out_start = inner;
        *out_len = inner_len;
        *out_negative = accounting;
        return 1;
    }

    // Non-default ISO code (VDOC-084): CurrencyOf substitutes any 3-letter
    // code as the symbol, so detect a standalone 3-uppercase run in the
    // input and retry the patterns with it.
    char cand[4] = {0};
    for (size_t k = 0; k + 3 <= len; ++k) {
        int upper3 = p[k] >= 'A' && p[k] <= 'Z' && p[k + 1] >= 'A' && p[k + 1] <= 'Z' &&
                     p[k + 2] >= 'A' && p[k + 2] <= 'Z';
        int lone = (k == 0 || !isalpha((unsigned char)p[k - 1])) &&
                   (k + 3 == len || !isalpha((unsigned char)p[k + 3]));
        if (upper3 && lone) {
            memcpy(cand, p + k, 3);
            break;
        }
    }
    if (cand[0]) {
        if (match_currency_pattern(
                fmt->data->currency.pattern_negative, cand, p, len, &inner, &inner_len)) {
            *out_start = inner;
            *out_len = inner_len;
            *out_negative = 1;
            return 1;
        }
        if (match_currency_pattern(
                fmt->data->currency.pattern_positive, cand, p, len, &inner, &inner_len)) {
            *out_start = inner;
            *out_len = inner_len;
            *out_negative = accounting;
            return 1;
        }
    }
    strip_currency_affixes(fmt, p, len, out_start, out_len);
    *out_negative = accounting;
    return 1;
}

/// @brief Parse a required locale-formatted currency value.
/// @details Recognizes locale positive/negative currency patterns, default and
///          arbitrary uppercase ISO-code affixes, and accounting parentheses
///          before parsing the interior as a decimal. Invalid input traps.
/// @param self NumberFormat handle.
/// @param input Runtime string containing the currency text.
/// @return Parsed monetary value, or zero after a trapped error.
double rt_numformat_parse_currency(void *self, rt_string input) {
    if (!self || !input) {
        rt_trap("Zanna.Localization.NumberFormat: ParseCurrency received null input");
        return 0;
    }
    rt_numformat_t *fmt = as_fmt(self);
    const char *cs = rt_string_cstr(input);
    int64_t len = rt_str_len(input);
    const char *inner = NULL;
    size_t inner_len = 0;
    int negative_pattern = 0;
    extract_currency_number(fmt, cs, (size_t)len, &inner, &inner_len, &negative_pattern);
    parse_result_t pr = parse_decimal(inner,
                                      inner_len,
                                      fmt,
                                      /*allow_fraction=*/1);
    if (!pr.success) {
        rt_trap("Zanna.Localization.NumberFormat: cannot parse input as currency");
        return 0;
    }
    return negative_pattern && pr.value > 0 ? -pr.value : pr.value;
}

/// @brief Try to parse a locale-formatted currency value without trapping.
/// @param self NumberFormat handle, or NULL.
/// @param input Runtime string containing the currency text, or NULL.
/// @return Newly allocated `Some<f64>` on success; otherwise `None`.
void *rt_numformat_try_parse_currency(void *self, rt_string input) {
    if (!self || !input)
        return rt_option_none();
    rt_numformat_t *fmt = as_fmt(self);
    const char *cs = rt_string_cstr(input);
    int64_t len = rt_str_len(input);
    const char *inner = NULL;
    size_t inner_len = 0;
    int negative_pattern = 0;
    extract_currency_number(fmt, cs, (size_t)len, &inner, &inner_len, &negative_pattern);
    parse_result_t pr = parse_decimal(inner,
                                      inner_len,
                                      fmt,
                                      /*allow_fraction=*/1);
    if (!pr.success)
        return rt_option_none();
    return rt_option_some_f64(negative_pattern && pr.value > 0 ? -pr.value : pr.value);
}
