//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_pluralize.c
// Purpose: Implements English noun pluralization and singularization for the
//          Zanna.Text.Pluralize class. Handles regular inflection rules
//          (e.g. -s, -es, -ies), common irregular forms (child/children,
//          mouse/mice), and uncountable nouns (sheep, fish).
//
// Key invariants:
//   - Irregular forms are checked before applying regular suffix rules.
//   - Uncountable nouns (mass nouns) return the input unchanged.
//   - Pluralize(1, "cat") returns "1 cat"; Pluralize(2, "cat") returns "2 cats".
//   - Rules are English-specific; other languages are not supported.
//   - Title-case irregulars retain an uppercase initial, and all-uppercase
//     inputs produce uppercase replacement text and suffixes.
//   - All lookups are case-insensitive for the irregular/uncountable tables.
//
// Ownership/Lifetime:
//   - Returned strings are fresh rt_string allocations owned by the caller.
//   - Input strings are borrowed for the duration of the call.
//
// Links: src/runtime/text/rt_pluralize.h (public API)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_pluralize.c
 * @brief Implements lightweight English noun inflection and count labels.
 * @details Case-insensitive irregular and uncountable tables precede ordered
 *          suffix heuristics for plural and singular forms. Replacement case
 *          is coarsely preserved, and count formatting combines a complete
 *          signed integer with the selected noun form.
 */

#include "rt_pluralize.h"

#include "rt_internal.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rt_trap.h"

/// Singular/plural spelling pair used before heuristic suffix rules.
typedef struct {
    const char *singular;
    const char *plural;
} irregular_t;

/// Null-terminated table of common irregular English inflections.
static const irregular_t irregulars[] = {{"child", "children"},
                                         {"foot", "feet"},
                                         {"goose", "geese"},
                                         {"man", "men"},
                                         {"mouse", "mice"},
                                         {"ox", "oxen"},
                                         {"person", "people"},
                                         {"tooth", "teeth"},
                                         {"woman", "women"},
                                         {"cactus", "cacti"},
                                         {"focus", "foci"},
                                         {"fungus", "fungi"},
                                         {"nucleus", "nuclei"},
                                         {"radius", "radii"},
                                         {"stimulus", "stimuli"},
                                         {"analysis", "analyses"},
                                         {"basis", "bases"},
                                         {"crisis", "crises"},
                                         {"diagnosis", "diagnoses"},
                                         {"thesis", "theses"},
                                         {"phenomenon", "phenomena"},
                                         {"criterion", "criteria"},
                                         {"datum", "data"},
                                         {"medium", "media"},
                                         {"appendix", "appendices"},
                                         {"index", "indices"},
                                         {"matrix", "matrices"},
                                         {"vertex", "vertices"},
                                         {"die", "dice"},
                                         {"leaf", "leaves"},
                                         {"life", "lives"},
                                         {"knife", "knives"},
                                         {"wife", "wives"},
                                         {"half", "halves"},
                                         {"wolf", "wolves"},
                                         {"shelf", "shelves"},
                                         {"self", "selves"},
                                         {NULL, NULL}};

/// Null-terminated table of nouns returned unchanged in either direction.
static const char *uncountables[] = {
    "sheep",   "fish",        "deer",      "series",   "species",  "money",
    "rice",    "information", "equipment", "news",     "advice",   "furniture",
    "luggage", "traffic",     "music",     "software", "hardware", "knowledge",
    "weather", "research",    "evidence",  "homework", NULL};

/// @brief Test whether a byte string ends with a case-insensitive suffix.
/// @details Case conversion uses the C library's byte-oriented `tolower`; this
///          is not Unicode case folding.
/// @param str Input byte buffer.
/// @param len Number of bytes in @p str.
/// @param suffix Null-terminated suffix to compare.
/// @return Nonzero when @p suffix matches the final bytes of @p str.
static int str_ends_with_ci(const char *str, size_t len, const char *suffix) {
    size_t slen = strlen(suffix);
    if (slen > len)
        return 0;
    const char *tail = str + len - slen;
    for (size_t i = 0; i < slen; i++) {
        if (tolower((unsigned char)tail[i]) != tolower((unsigned char)suffix[i]))
            return 0;
    }
    return 1;
}

/// @brief Compare a byte span with a null-terminated word case-insensitively.
/// @details Used to match user input against the irregular and
///          uncountable word tables, which are stored lowercase.
///          Folding is byte-oriented through the C library and does not
///          implement Unicode case equivalence.
/// @param a Input byte span.
/// @param a_len Number of bytes in @p a.
/// @param b Null-terminated table word.
/// @return Nonzero when lengths and case-folded bytes match.
static int str_eq_nocase_len(const char *a, size_t a_len, const char *b) {
    size_t b_len = strlen(b);
    if (a_len != b_len)
        return 0;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

/// @brief Determine whether a byte span contains letters but no lowercase letters.
/// @details Letter classification uses the C library's byte-oriented ctype
///          functions. Nonletters are ignored.
/// @param src Input byte span.
/// @param len Number of bytes in @p src.
/// @return Nonzero when at least one byte is alphabetic and every alphabetic
///         byte is uppercase.
static int is_ascii_all_caps_word(const char *src, size_t len) {
    int saw_alpha = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalpha(c)) {
            saw_alpha = 1;
            if (islower(c))
                return 0;
        }
    }
    return saw_alpha;
}

/// @brief Copy a table inflection while applying the input word's coarse casing.
/// @details All-uppercase input uppercases the entire replacement; otherwise an
///          uppercase first input byte uppercases only the replacement initial.
///          Allocation failure traps and falls back to a runtime copy of the
///          lowercase replacement.
/// @param src Original input word bytes.
/// @param src_len Number of bytes in @p src.
/// @param replacement Lowercase null-terminated irregular form.
/// @return Newly allocated runtime string containing the case-adjusted form.
static rt_string inflection_with_input_case(const char *src,
                                            size_t src_len,
                                            const char *replacement) {
    size_t len = strlen(replacement);
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        rt_trap("Pluralize: memory allocation failed");
        return rt_string_from_bytes(replacement, len);
    }
    memcpy(buf, replacement, len + 1);

    if (is_ascii_all_caps_word(src, src_len)) {
        for (size_t i = 0; i < len; i++)
            buf[i] = (char)toupper((unsigned char)buf[i]);
    } else if (src_len > 0 && isupper((unsigned char)src[0]) && len > 0) {
        buf[0] = (char)toupper((unsigned char)buf[0]);
    }

    rt_string result = rt_string_from_bytes(buf, len);
    free(buf);
    return result;
}

/// @brief Copy a suffix and uppercase it for an all-uppercase source word.
/// @param dst Writable destination with room for @p suffix_len bytes.
/// @param src Original source word bytes.
/// @param src_len Number of bytes in @p src.
/// @param suffix Suffix bytes to copy.
/// @param suffix_len Number of bytes in @p suffix.
static void copy_suffix_with_input_case(
    char *dst, const char *src, size_t src_len, const char *suffix, size_t suffix_len) {
    memcpy(dst, suffix, suffix_len);
    if (is_ascii_all_caps_word(src, src_len)) {
        for (size_t i = 0; i < suffix_len; i++)
            dst[i] = (char)toupper((unsigned char)dst[i]);
    }
}

/// @brief Append a case-adjusted suffix to a word.
/// @details Preserves the source bytes verbatim and uppercases the suffix when
///          the source is all uppercase. Size overflow or temporary allocation
///          failure traps and returns a runtime copy of the source.
/// @param src Source word bytes.
/// @param len Number of bytes in @p src.
/// @param suffix Lowercase null-terminated suffix.
/// @return Newly allocated runtime string containing the inflected word.
static rt_string append_suffix(const char *src, size_t len, const char *suffix) {
    size_t suffix_len = strlen(suffix);
    if (len > SIZE_MAX - suffix_len || len + suffix_len == SIZE_MAX) {
        rt_trap("Pluralize: output length overflow");
        return rt_string_from_bytes(src ? src : "", src ? len : 0);
    }
    size_t blen = len + suffix_len;
    char *buf = (char *)malloc(blen + 1);
    if (!buf) {
        rt_trap("Pluralize: memory allocation failed");
        return rt_string_from_bytes(src ? src : "", src ? len : 0);
    }
    memcpy(buf, src, len);
    copy_suffix_with_input_case(buf + len, src, len, suffix, suffix_len);
    buf[blen] = '\0';
    rt_string result = rt_string_from_bytes(buf, blen);
    free(buf);
    return result;
}

/// @brief Replace trailing source bytes with a case-adjusted suffix.
/// @details If @p remove_len exceeds @p len, the entire source is removed.
///          Size overflow or temporary allocation failure traps and returns a
///          runtime copy of the source.
/// @param src Source word bytes.
/// @param len Number of bytes in @p src.
/// @param remove_len Number of trailing source bytes to discard.
/// @param replacement Lowercase null-terminated replacement suffix.
/// @return Newly allocated runtime string containing the inflected word.
static rt_string replace_suffix(const char *src,
                                size_t len,
                                size_t remove_len,
    const char *replacement) {
    size_t replacement_len = strlen(replacement);
    size_t stem_len = remove_len > len ? 0 : len - remove_len;
    if (stem_len > SIZE_MAX - replacement_len || stem_len + replacement_len == SIZE_MAX) {
        rt_trap("Pluralize: output length overflow");
        return rt_string_from_bytes(src ? src : "", src ? len : 0);
    }
    size_t blen = stem_len + replacement_len;
    char *buf = (char *)malloc(blen + 1);
    if (!buf) {
        rt_trap("Pluralize: memory allocation failed");
        return rt_string_from_bytes(src ? src : "", src ? len : 0);
    }
    memcpy(buf, src, stem_len);
    copy_suffix_with_input_case(buf + stem_len, src, len, replacement, replacement_len);
    buf[blen] = '\0';
    rt_string result = rt_string_from_bytes(buf, blen);
    free(buf);
    return result;
}

/// @brief Test whether `word` is in the uncountable-noun list (case-insensitive).
/// @details Linear scan — the table is small (~22 entries) so a hash
///          lookup wouldn't pay off. Words like "sheep", "rice",
///          "information" pluralize to themselves, so the caller
///          short-circuits with "return the word as-is" on a hit.
/// @param word Input noun bytes.
/// @param len Number of bytes in @p word.
/// @return Nonzero when @p word matches a table entry case-insensitively.
static int is_uncountable(const char *word, size_t len) {
    for (int i = 0; uncountables[i]; ++i) {
        if (str_eq_nocase_len(word, len, uncountables[i]))
            return 1;
    }
    return 0;
}

/// @brief Convert an English noun from singular to plural form.
/// @details Three-tier rule application:
///          1. **Uncountable** ("sheep", "rice", ...) → return as-is.
///          2. **Irregular** ("child" → "children", "person" → "people")
///             → lookup in the irregular table.
///          3. **Regular suffix rules** in priority order:
///             - `-s/-x/-z/-ch/-sh` → `+es` ("box" → "boxes")
///             - consonant + `y` → `-y +ies` ("city" → "cities")
///             - `-f/-fe` → `-f/-fe +ves` ("life" → "lives")
///             - default → `+s` ("cat" → "cats")
///          A consonant-plus-`o` heuristic also adds `es`. Source bytes are
///          preserved, while generated text follows title-case or all-uppercase
///          input. Null, unbacked, and empty strings yield an empty string.
/// @param word Borrowed singular noun to inflect.
/// @return Newly allocated plural-form runtime string.
rt_string rt_pluralize(rt_string word) {
    if (!word)
        return rt_string_from_bytes("", 0);
    const char *src = rt_string_cstr(word);
    if (!src)
        return rt_string_from_bytes("", 0);
    size_t len = (size_t)rt_str_len(word);
    if (len == 0)
        return rt_string_from_bytes("", 0);

    // Check uncountable
    if (is_uncountable(src, len))
        return rt_string_from_bytes(src, len);

    // Check irregulars
    for (int i = 0; irregulars[i].singular; ++i) {
        if (str_eq_nocase_len(src, len, irregulars[i].singular))
            return inflection_with_input_case(src, len, irregulars[i].plural);
    }

    // Rules applied in order:
    // -s, -x, -z, -ch, -sh -> +es
    if (str_ends_with_ci(src, len, "s") || str_ends_with_ci(src, len, "x") ||
        str_ends_with_ci(src, len, "z") || str_ends_with_ci(src, len, "ch") ||
        str_ends_with_ci(src, len, "sh")) {
        return append_suffix(src, len, "es");
    }

    // consonant + y -> ies
    if (len >= 2 && tolower((unsigned char)src[len - 1]) == 'y') {
        char prev = (char)tolower((unsigned char)src[len - 2]);
        if (prev != 'a' && prev != 'e' && prev != 'i' && prev != 'o' && prev != 'u') {
            return replace_suffix(src, len, 1, "ies");
        }
    }

    // -f -> -ves (but not already covered by irregulars)
    if (len >= 2 && tolower((unsigned char)src[len - 1]) == 'f' &&
        tolower((unsigned char)src[len - 2]) != 'f') {
        return replace_suffix(src, len, 1, "ves");
    }

    // -fe -> -ves
    if (str_ends_with_ci(src, len, "fe")) {
        return replace_suffix(src, len, 2, "ves");
    }

    // -o -> -oes for certain words (simplified: consonant + o -> oes)
    if (len >= 2 && tolower((unsigned char)src[len - 1]) == 'o') {
        char prev = (char)tolower((unsigned char)src[len - 2]);
        if (prev != 'a' && prev != 'e' && prev != 'i' && prev != 'o' && prev != 'u') {
            return append_suffix(src, len, "es");
        }
    }

    // Default: add -s
    return append_suffix(src, len, "s");
}

/// @brief Convert an English plural noun back to singular form.
/// @details Rule order mirrors `rt_pluralize` in reverse:
///          1. Uncountable → return as-is.
///          2. Irregular table → reverse lookup ("children" → "child").
///          3. Suffix peeling:
///             - `-ves` → `-f` ("wolves" → "wolf").
///             - `-ies` → `-y` ("cities" → "city").
///             - `-shes/-ches/-ses/-xes/-zes/-oes` → strip `-es`.
///             - bare `-s` (but not `-ss`) → strip.
///          For ambiguous forms ("foxes" could come from "fox" or
///          "foxe"), the rules pick the more common reverse — never
///          perfect but matches typical English.
///          Null, unbacked, and empty strings yield an empty string.
/// @param word Borrowed plural noun to inflect.
/// @return Newly allocated singular-form runtime string.
rt_string rt_singularize(rt_string word) {
    if (!word)
        return rt_string_from_bytes("", 0);
    const char *src = rt_string_cstr(word);
    if (!src)
        return rt_string_from_bytes("", 0);
    size_t len = (size_t)rt_str_len(word);
    if (len == 0)
        return rt_string_from_bytes("", 0);

    // Check uncountable
    if (is_uncountable(src, len))
        return rt_string_from_bytes(src, len);

    // Check irregulars (reverse lookup)
    for (int i = 0; irregulars[i].singular; ++i) {
        if (str_eq_nocase_len(src, len, irregulars[i].plural))
            return inflection_with_input_case(src, len, irregulars[i].singular);
    }

    // -ves -> -f or -fe
    if (str_ends_with_ci(src, len, "ves") && len > 3) {
        return replace_suffix(src, len, 3, "f");
    }

    // -ies -> -y
    if (str_ends_with_ci(src, len, "ies") && len > 3) {
        return replace_suffix(src, len, 3, "y");
    }

    // -ses, -xes, -zes, -ches, -shes -> remove -es
    if (str_ends_with_ci(src, len, "shes") || str_ends_with_ci(src, len, "ches")) {
        return rt_string_from_bytes(src, len - 2);
    }
    if (str_ends_with_ci(src, len, "ses") || str_ends_with_ci(src, len, "xes") ||
        str_ends_with_ci(src, len, "zes")) {
        return rt_string_from_bytes(src, len - 2);
    }

    // -oes -> -o
    if (str_ends_with_ci(src, len, "oes") && len > 3) {
        return rt_string_from_bytes(src, len - 2);
    }

    // -s (but not -ss) -> remove -s
    if (len > 1 && tolower((unsigned char)src[len - 1]) == 's' &&
        tolower((unsigned char)src[len - 2]) != 's') {
        return rt_string_from_bytes(src, len - 1);
    }

    // Already singular
    return rt_string_from_bytes(src, len);
}

/// @brief Format a count + noun pair, pluralizing the noun based on the count.
/// @details Returns "1 item" for ±1, "5 items" / "0 items" otherwise.
///          Matches the convention of natural English where "0 items"
///          and "5 items" both use the plural form (unlike Russian or
///          Polish which have separate "few" / "many" forms — out of
///          scope here). Both `1` and `-1` retain the supplied singular noun.
///          A null noun produces an empty string rather than a count alone.
/// @param count Signed item count to render.
/// @param word Borrowed singular noun.
/// @return Newly allocated `"count noun"` runtime string.
rt_string rt_pluralize_count(int64_t count, rt_string word) {
    if (!word)
        return rt_string_from_bytes("", 0);

    rt_string noun = (count == 1 || count == -1) ? rt_string_ref(word) : rt_pluralize(word);
    const char *nstr = rt_string_cstr(noun);
    if (!nstr)
        nstr = "";

    size_t noun_len = (size_t)rt_str_len(noun);
    char count_buf[32];
    int count_len = snprintf(count_buf, sizeof(count_buf), "%lld", (long long)count);
    if (count_len < 0) {
        rt_string_unref(noun);
        rt_trap("Pluralize: count formatting failed");
        return rt_string_from_bytes("", 0);
    }
    if ((size_t)count_len > SIZE_MAX - noun_len - 1) {
        rt_string_unref(noun);
        rt_trap("Pluralize: output length overflow");
        return rt_string_from_bytes("", 0);
    }

    size_t blen = (size_t)count_len + 1 + noun_len;
    char *buf = (char *)malloc(blen + 1);
    if (!buf) {
        rt_string_unref(noun);
        rt_trap("Pluralize: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }
    memcpy(buf, count_buf, (size_t)count_len);
    buf[count_len] = ' ';
    memcpy(buf + count_len + 1, nstr, noun_len);
    buf[blen] = '\0';
    rt_string_unref(noun);
    rt_string result = rt_string_from_bytes(buf, blen);
    free(buf);
    return result;
}
