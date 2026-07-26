//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/localization/rt_collator_table.c
// Purpose: DUCET-lite weight classifier for the Collator class. Maps a
//          Unicode codepoint to a (primary, secondary, tertiary) weight
//          triple using a compact switch/range implementation rather than
//          a giant baked table — keeps binary size down while covering
//          basic Latin, Latin-1 Supplement, Latin Extended-A, and common
//          diacritic composite characters.
//
// Key invariants:
//   - Primary weights use a sparse 32-bit space ordered as whitespace/control,
//     punctuation, digits, base Latin letters, then codepoint fallback.
//   - Secondary weight 0 means "no diacritic"; 1..7 encode the common
//     Latin diacritic families (grave/acute/circumflex/tilde/diaeresis/
//     ring/cedilla) so strength-2 comparisons distinguish accented forms.
//   - Tertiary weight 0 = lowercase / neutral; 1 = uppercase. Strength-3
//     comparisons differentiate case.
//   - Locale patches are tiny data tables keyed by BCP-47 tag. They apply
//     position overrides — e.g. Swedish places å after z by overriding
//     its primary weight.
//
// Ownership/Lifetime:
//   - All tables are static immortal data.
//
// Links: src/runtime/localization/rt_collator.h (consumer),
//        src/runtime/localization/rt_collator.c (SortKey/Compare impl).
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements the compact invariant collation classifier and tailorings.
 * @details Maps supported Latin code points into stable primary, secondary,
 * and tertiary weights, assigns deterministic fallback weights to other
 * scripts, and supplies small immutable locale-specific override tables.
 */

#include "rt_collator.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

//===----------------------------------------------------------------------===//
// Primary weight bands (sparse, ordered)
//===----------------------------------------------------------------------===//
//
// Layout (big-endian for byte-wise comparison):
//   0x0001..0x00FF    control + whitespace + structural
//   0x0100..0x01FF    punctuation
//   0x0200..0x020F    digits 0-9
//   0x0300..0x031F    base Latin letters (a/A = 0x0300, b/B = 0x0301, …)
//   0x0400..           Latin Extended / other
//   0x8000..          codepoint-order fallback: primary = 0x8000 + cp
//
//===----------------------------------------------------------------------===//

/** First primary weight reserved for whitespace and structural characters. */
#define PRI_SPACE 0x0010u
/** First primary weight in the punctuation band. */
#define PRI_PUNCT 0x0100u
/** Primary weight assigned to ASCII digit zero. */
#define PRI_DIGIT0 0x0200u
/** Primary weight assigned to base Latin letter a. */
#define PRI_LETTER0 0x0300u

//===----------------------------------------------------------------------===//
// Codepoint classifier
//===----------------------------------------------------------------------===//

/// @brief Resolve the base Latin letter (lowercase, A-Z primary column) and
///        the diacritic family for a Latin-1 Supplement / Extended-A
///        composite codepoint.
/// @param cp Unicode code point to decompose.
/// @param base_out Optional destination for the lowercase ASCII base letter.
/// @param sec_out Optional destination for the stable secondary variant weight.
/// @return 1 when the compact Latin-1 table covers @p cp, otherwise 0.
static int decompose_latin(uint32_t cp, char *base_out, uint16_t *sec_out) {
    char base = 0;
    uint16_t sec = 0;
    switch (cp) {
        // Latin-1 Supplement — lowercase with diacritics
        case 0x00E0:
            base = 'a';
            sec = 1;
            break; // à grave
        case 0x00E1:
            base = 'a';
            sec = 2;
            break; // á acute
        case 0x00E2:
            base = 'a';
            sec = 3;
            break; // â circumflex
        case 0x00E3:
            base = 'a';
            sec = 4;
            break; // ã tilde
        case 0x00E4:
            base = 'a';
            sec = 5;
            break; // ä diaeresis
        case 0x00E5:
            base = 'a';
            sec = 6;
            break; // å ring
        case 0x00E7:
            base = 'c';
            sec = 7;
            break; // ç cedilla
        case 0x00E8:
            base = 'e';
            sec = 1;
            break;
        case 0x00E9:
            base = 'e';
            sec = 2;
            break;
        case 0x00EA:
            base = 'e';
            sec = 3;
            break;
        case 0x00EB:
            base = 'e';
            sec = 5;
            break;
        case 0x00EC:
            base = 'i';
            sec = 1;
            break;
        case 0x00ED:
            base = 'i';
            sec = 2;
            break;
        case 0x00EE:
            base = 'i';
            sec = 3;
            break;
        case 0x00EF:
            base = 'i';
            sec = 5;
            break;
        case 0x00F1:
            base = 'n';
            sec = 4;
            break;
        case 0x00F2:
            base = 'o';
            sec = 1;
            break;
        case 0x00F3:
            base = 'o';
            sec = 2;
            break;
        case 0x00F4:
            base = 'o';
            sec = 3;
            break;
        case 0x00F5:
            base = 'o';
            sec = 4;
            break;
        case 0x00F6:
            base = 'o';
            sec = 5;
            break;
        case 0x00F9:
            base = 'u';
            sec = 1;
            break;
        case 0x00FA:
            base = 'u';
            sec = 2;
            break;
        case 0x00FB:
            base = 'u';
            sec = 3;
            break;
        case 0x00FC:
            base = 'u';
            sec = 5;
            break;
        case 0x00FD:
            base = 'y';
            sec = 2;
            break;
        case 0x00FF:
            base = 'y';
            sec = 5;
            break;
        // Latin-1 Supplement — uppercase with diacritics (tertiary handled outside)
        case 0x00C0:
            base = 'a';
            sec = 1;
            break;
        case 0x00C1:
            base = 'a';
            sec = 2;
            break;
        case 0x00C2:
            base = 'a';
            sec = 3;
            break;
        case 0x00C3:
            base = 'a';
            sec = 4;
            break;
        case 0x00C4:
            base = 'a';
            sec = 5;
            break;
        case 0x00C5:
            base = 'a';
            sec = 6;
            break;
        case 0x00C7:
            base = 'c';
            sec = 7;
            break;
        case 0x00C8:
            base = 'e';
            sec = 1;
            break;
        case 0x00C9:
            base = 'e';
            sec = 2;
            break;
        case 0x00CA:
            base = 'e';
            sec = 3;
            break;
        case 0x00CB:
            base = 'e';
            sec = 5;
            break;
        case 0x00CC:
            base = 'i';
            sec = 1;
            break;
        case 0x00CD:
            base = 'i';
            sec = 2;
            break;
        case 0x00CE:
            base = 'i';
            sec = 3;
            break;
        case 0x00CF:
            base = 'i';
            sec = 5;
            break;
        case 0x00D1:
            base = 'n';
            sec = 4;
            break;
        case 0x00D2:
            base = 'o';
            sec = 1;
            break;
        case 0x00D3:
            base = 'o';
            sec = 2;
            break;
        case 0x00D4:
            base = 'o';
            sec = 3;
            break;
        case 0x00D5:
            base = 'o';
            sec = 4;
            break;
        case 0x00D6:
            base = 'o';
            sec = 5;
            break;
        case 0x00D9:
            base = 'u';
            sec = 1;
            break;
        case 0x00DA:
            base = 'u';
            sec = 2;
            break;
        case 0x00DB:
            base = 'u';
            sec = 3;
            break;
        case 0x00DC:
            base = 'u';
            sec = 5;
            break;
        case 0x00DD:
            base = 'y';
            sec = 2;
            break;
        case 0x00E6:
            base = 'a';
            sec = 8;
            break; // æ -> treat as a-with-special
        case 0x00C6:
            base = 'a';
            sec = 8;
            break; // Æ
        case 0x00F8:
            base = 'o';
            sec = 8;
            break; // ø
        case 0x00D8:
            base = 'o';
            sec = 8;
            break; // Ø
        case 0x00DF:
            base = 's';
            sec = 8;
            break; // ß -> treat as s-special for now
        default:
            return 0;
    }
    if (base_out)
        *base_out = base;
    if (sec_out)
        *sec_out = sec;
    return 1;
}

/** Compact decomposition record for one Latin Extended-A scalar. */
typedef struct {
    uint32_t cp;      ///< Unicode scalar being decomposed.
    char base;        ///< Lowercase ASCII base letter defining primary weight.
    uint16_t secondary; ///< Stable diacritic or variant weight.
} latin_ext_a_decomp_t;

/** Sorted decomposition records covering the Latin Extended-A block. */
static const latin_ext_a_decomp_t k_latin_ext_a_decomp[] = {
    {0x0100, 'a', 9},  {0x0101, 'a', 9},  {0x0102, 'a', 10}, {0x0103, 'a', 10}, {0x0104, 'a', 11},
    {0x0105, 'a', 11}, {0x0106, 'c', 2},  {0x0107, 'c', 2},  {0x0108, 'c', 3},  {0x0109, 'c', 3},
    {0x010A, 'c', 14}, {0x010B, 'c', 14}, {0x010C, 'c', 12}, {0x010D, 'c', 12}, {0x010E, 'd', 12},
    {0x010F, 'd', 12}, {0x0110, 'd', 13}, {0x0111, 'd', 13}, {0x0112, 'e', 9},  {0x0113, 'e', 9},
    {0x0114, 'e', 10}, {0x0115, 'e', 10}, {0x0116, 'e', 14}, {0x0117, 'e', 14}, {0x0118, 'e', 11},
    {0x0119, 'e', 11}, {0x011A, 'e', 12}, {0x011B, 'e', 12}, {0x011C, 'g', 3},  {0x011D, 'g', 3},
    {0x011E, 'g', 10}, {0x011F, 'g', 10}, {0x0120, 'g', 14}, {0x0121, 'g', 14}, {0x0122, 'g', 7},
    {0x0123, 'g', 7},  {0x0124, 'h', 3},  {0x0125, 'h', 3},  {0x0126, 'h', 13}, {0x0127, 'h', 13},
    {0x0128, 'i', 4},  {0x0129, 'i', 4},  {0x012A, 'i', 9},  {0x012B, 'i', 9},  {0x012C, 'i', 10},
    {0x012D, 'i', 10}, {0x012E, 'i', 11}, {0x012F, 'i', 11}, {0x0130, 'i', 14}, {0x0131, 'i', 0},
    {0x0132, 'i', 16}, {0x0133, 'i', 16}, {0x0134, 'j', 3},  {0x0135, 'j', 3},  {0x0136, 'k', 7},
    {0x0137, 'k', 7},  {0x0138, 'k', 16}, {0x0139, 'l', 2},  {0x013A, 'l', 2},  {0x013B, 'l', 7},
    {0x013C, 'l', 7},  {0x013D, 'l', 12}, {0x013E, 'l', 12}, {0x013F, 'l', 14}, {0x0140, 'l', 14},
    {0x0141, 'l', 13}, {0x0142, 'l', 13}, {0x0143, 'n', 2},  {0x0144, 'n', 2},  {0x0145, 'n', 7},
    {0x0146, 'n', 7},  {0x0147, 'n', 12}, {0x0148, 'n', 12}, {0x0149, 'n', 16}, {0x014A, 'n', 16},
    {0x014B, 'n', 16}, {0x014C, 'o', 9},  {0x014D, 'o', 9},  {0x014E, 'o', 10}, {0x014F, 'o', 10},
    {0x0150, 'o', 15}, {0x0151, 'o', 15}, {0x0152, 'o', 16}, {0x0153, 'o', 16}, {0x0154, 'r', 2},
    {0x0155, 'r', 2},  {0x0156, 'r', 7},  {0x0157, 'r', 7},  {0x0158, 'r', 12}, {0x0159, 'r', 12},
    {0x015A, 's', 2},  {0x015B, 's', 2},  {0x015C, 's', 3},  {0x015D, 's', 3},  {0x015E, 's', 7},
    {0x015F, 's', 7},  {0x0160, 's', 12}, {0x0161, 's', 12}, {0x0162, 't', 7},  {0x0163, 't', 7},
    {0x0164, 't', 12}, {0x0165, 't', 12}, {0x0166, 't', 13}, {0x0167, 't', 13}, {0x0168, 'u', 4},
    {0x0169, 'u', 4},  {0x016A, 'u', 9},  {0x016B, 'u', 9},  {0x016C, 'u', 10}, {0x016D, 'u', 10},
    {0x016E, 'u', 6},  {0x016F, 'u', 6},  {0x0170, 'u', 15}, {0x0171, 'u', 15}, {0x0172, 'u', 11},
    {0x0173, 'u', 11}, {0x0174, 'w', 3},  {0x0175, 'w', 3},  {0x0176, 'y', 3},  {0x0177, 'y', 3},
    {0x0178, 'y', 5},  {0x0179, 'z', 2},  {0x017A, 'z', 2},  {0x017B, 'z', 14}, {0x017C, 'z', 14},
    {0x017D, 'z', 12}, {0x017E, 'z', 12}, {0x017F, 's', 0},
};

/// @brief Decompose a Latin Extended-A codepoint into base letter and secondary weight.
/// @details The table covers U+0100..U+017F letters with common macron, breve,
///          ogonek, caron, stroke, dot, double-acute, and ligature-style weights.
/// @param cp Unicode codepoint in Latin Extended-A.
/// @param base_out Receives lowercase ASCII base letter.
/// @param sec_out Receives accent/variant secondary weight.
/// @return 1 when @p cp is covered; otherwise 0.
static int decompose_latin_extended_a(uint32_t cp, char *base_out, uint16_t *sec_out) {
    for (size_t i = 0; i < sizeof(k_latin_ext_a_decomp) / sizeof(k_latin_ext_a_decomp[0]); i++) {
        if (k_latin_ext_a_decomp[i].cp == cp) {
            if (base_out)
                *base_out = k_latin_ext_a_decomp[i].base;
            if (sec_out)
                *sec_out = k_latin_ext_a_decomp[i].secondary;
            return 1;
        }
    }
    return 0;
}

/// @brief Return whether a Latin Extended-A codepoint is uppercase for tertiary ordering.
/// @details Most Extended-A letters are encoded as uppercase/lowercase pairs with the uppercase
///          value first. This helper handles singleton and ligature exceptions explicitly.
/// @param cp Latin Extended-A code point covered by the decomposition table.
/// @return 1 for an uppercase form, otherwise 0.
static int latin_extended_a_is_upper(uint32_t cp) {
    if (cp == 0x0130 || cp == 0x0132 || cp == 0x014A || cp == 0x0178)
        return 1;
    if (cp == 0x0131 || cp == 0x0133 || cp == 0x0138 || cp == 0x0149 || cp == 0x014B ||
        cp == 0x017F)
        return 0;
    return (cp & 1u) == 0;
}

/// @brief Classify one code point into deterministic collation weights.
/// @details ASCII structure, digits, and Latin letters use ordered sparse
///          bands. Latin-1 and Extended-A composites share base-letter
///          primaries with secondary variant and tertiary case weights.
///          Everything else receives @c 0x8000+cp as a codepoint-order
///          fallback. Any output pointer may be NULL.
/// @param cp Unicode code point to classify.
/// @param primary Optional destination for the 32-bit primary weight.
/// @param secondary Optional destination for the accent/variant weight.
/// @param tertiary Optional destination for the case weight.
/// @return 0 for explicitly classified input, or 1 when fallback ordering was used.
int rt_collator_codepoint_weights(uint32_t cp,
                                  uint32_t *primary,
                                  uint16_t *secondary,
                                  uint16_t *tertiary) {
    uint32_t p = 0;
    uint16_t s = 0;
    uint16_t t = 0;

    if (cp == 0) {
        if (primary)
            *primary = 0;
        if (secondary)
            *secondary = 0;
        if (tertiary)
            *tertiary = 0;
        return 0;
    }

    // Whitespace / control — sort before printables.
    if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') {
        p = PRI_SPACE;
    }
    // ASCII punctuation.
    else if (cp < 0x30 || (cp >= 0x3A && cp <= 0x40) || (cp >= 0x5B && cp <= 0x60) ||
             (cp >= 0x7B && cp <= 0x7E)) {
        p = PRI_PUNCT + (cp & 0x7F);
    }
    // ASCII digits.
    else if (cp >= '0' && cp <= '9') {
        p = PRI_DIGIT0 + (cp - '0');
    }
    // ASCII letters.
    else if (cp >= 'a' && cp <= 'z') {
        p = PRI_LETTER0 + (cp - 'a');
    } else if (cp >= 'A' && cp <= 'Z') {
        p = PRI_LETTER0 + (cp - 'A');
        t = 1; // uppercase marker
    }
    // Latin-1 composite letters.
    else if (cp >= 0x00C0 && cp <= 0x00FF) {
        char base = 0;
        uint16_t sec = 0;
        if (decompose_latin(cp, &base, &sec)) {
            p = PRI_LETTER0 + (uint32_t)(base - 'a');
            s = sec;
            // Uppercase Latin-1 letters occupy 0xC0-0xDE; the odd ×/÷ are
            // handled by the decompose table not returning 1. Detect case:
            if (cp <= 0x00DE && cp != 0x00D7)
                t = 1;
        } else {
            // Fallback (e.g. × multiplication sign).
            p = 0x8000u + cp;
        }
    }
    // Latin Extended-A composites and special Latin letters.
    else if (cp >= 0x0100 && cp <= 0x017F) {
        char base = 0;
        uint16_t sec = 0;
        if (decompose_latin_extended_a(cp, &base, &sec)) {
            p = PRI_LETTER0 + (uint32_t)(base - 'a');
            s = sec;
            t = latin_extended_a_is_upper(cp) ? 1 : 0;
        } else {
            p = 0x8000u + cp;
        }
    }
    // Everything else: codepoint-order fallback.
    else {
        p = 0x8000u + cp;
        if (primary)
            *primary = p;
        if (secondary)
            *secondary = 0;
        if (tertiary)
            *tertiary = 0;
        return 1;
    }

    if (primary)
        *primary = p;
    if (secondary)
        *secondary = s;
    if (tertiary)
        *tertiary = t;
    return 0;
}

//===----------------------------------------------------------------------===//
// Locale tailorings
//===----------------------------------------------------------------------===//
//
// A tailoring is a small array of (codepoint, primary, secondary, tertiary)
// records that override the default weights for specific characters. This
// lets Swedish push å/ä/ö to the end of the alphabet without disturbing
// en-US comparisons.
//
//===----------------------------------------------------------------------===//

// Swedish: å (U+00E5/U+00C5), ä (U+00E4/U+00C4), ö (U+00F6/U+00D6) sort after z.
// We give them primary weights starting at PRI_LETTER0 + 26 (beyond 'z' which
// is PRI_LETTER0 + 25) so they collate at the end of the alphabet.
/** Swedish case-paired overrides that place å, ä, and ö after z. */
static const rt_collator_locale_patch_t g_patches_sv[] = {
    {0x00E5u, PRI_LETTER0 + 26u, 0, 0},
    {0x00C5u, PRI_LETTER0 + 26u, 0, 1},
    {0x00E4u, PRI_LETTER0 + 27u, 0, 0},
    {0x00C4u, PRI_LETTER0 + 27u, 0, 1},
    {0x00F6u, PRI_LETTER0 + 28u, 0, 0},
    {0x00D6u, PRI_LETTER0 + 28u, 0, 1},
};

// German currently uses the invariant Latin decomposition: umlauts share
// their base-letter primary with an elevated secondary, and ß uses the
// special s primary. No locale-specific override is required, so this
// sentinel table intentionally reports zero entries.
/** Sentinel storage representing the currently empty German override set. */
static const rt_collator_locale_patch_t g_patches_de[] = {
    {0, 0, 0, 0}, // Sentinel; count returns 0 below.
};

/// @brief Select immutable locale-specific collation overrides by language tag.
/// @details Swedish tags (`sv` or `sv-*`) receive six å/ä/ö case patches.
///          German and all other tags currently use invariant classifier
///          weights and return no patch table.
/// @param tag Non-empty canonical NUL-terminated locale tag.
/// @param out_count Optional destination reset to zero and set to the returned table size.
/// @return Borrowed static patch array, or NULL when no override applies.
const rt_collator_locale_patch_t *rt_collator_locale_patches(const char *tag, size_t *out_count) {
    if (out_count)
        *out_count = 0;
    if (!tag)
        return NULL;

    // Swedish: "sv", "sv-SE", "sv-FI"
    if (tag[0] == 's' && tag[1] == 'v' && (tag[2] == '\0' || tag[2] == '-')) {
        if (out_count)
            *out_count = sizeof(g_patches_sv) / sizeof(g_patches_sv[0]);
        return g_patches_sv;
    }

    // German phonebook uses defaults.
    if (tag[0] == 'd' && tag[1] == 'e' && (tag[2] == '\0' || tag[2] == '-')) {
        (void)g_patches_de;
        return NULL;
    }

    return NULL;
}
