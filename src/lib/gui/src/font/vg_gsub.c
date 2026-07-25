//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: lib/gui/src/font/vg_gsub.c
// Purpose: Hand-written OpenType GSUB shaping for coding ligatures — parses
//          ScriptList/FeatureList/LookupList once per font, then applies the
//          liga/calt lookups (LookupTypes 1 single, 4 ligature, 6 chaining
//          context, 7 extension) over glyph runs.
// Key invariants:
//   - Every table read is bounds-checked against the GSUB slice; malformed
//     data degrades to "no substitution", never out-of-bounds access.
//   - Shaped output preserves per-source-character advance ownership: each
//     shaped glyph records the source span it covers so callers keep
//     per-character columns, selection, and caret positions (plan 06 caret
//     contract).
//   - Substitution never grows the glyph run (single and ligature lookups
//     only shrink or replace), so shaping fits in the caller's buffer.
// Ownership/Lifetime:
//   - gsub_feature_lookups is owned by the font and freed with it.
// Links: lib/gui/src/font/vg_ttf_internal.h, docs/adr/0137-premium-rendering-surface.md
//
//===----------------------------------------------------------------------===//

#include "../../include/vg_font.h"
#include "vg_ttf_internal.h"

#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements bounded OpenType GSUB parsing and coding-ligature shaping.
/// @details Load-time discovery records `liga` and `calt` lookups from the default or Latin
/// script. Run-time shaping supports single, ligature, chaining-context, and extension lookups
/// while preserving the source-character span represented by every output glyph.

#define GSUB_MAX_NESTED_DEPTH 4
#define GSUB_MAX_RULE_GLYPHS 16

//=============================================================================
// Bounds-checked readers over the GSUB slice
//=============================================================================

/// @brief Non-owning, bounds-delimited view of a font's GSUB table.
typedef struct gsub_slice {
    const uint8_t *data; ///< Start of the GSUB table.
    uint32_t length;     ///< Byte length of the GSUB table.
} gsub_slice_t;

/// @brief Read one big-endian 16-bit GSUB field.
/// @param gsub Bounded GSUB table view.
/// @param offset Byte offset from the beginning of @p gsub.
/// @return Decoded value, or zero when the field lies beyond the view.
static uint16_t gsub_u16(const gsub_slice_t *gsub, uint32_t offset) {
    if (offset + 2 > gsub->length)
        return 0;
    return ttf_read_u16(gsub->data + offset);
}

/// @brief Read one big-endian 32-bit GSUB field.
/// @param gsub Bounded GSUB table view.
/// @param offset Byte offset from the beginning of @p gsub.
/// @return Decoded value, or zero when the field lies beyond the view.
static uint32_t gsub_u32(const gsub_slice_t *gsub, uint32_t offset) {
    if (offset + 4 > gsub->length)
        return 0;
    return ttf_read_u32(gsub->data + offset);
}

//=============================================================================
// Coverage and class-definition tables
//=============================================================================

/// @brief Find a glyph's index in an OpenType Coverage table.
/// @details Supports format 1 glyph arrays and format 2 range records, using binary search for
/// both sorted representations.
/// @param gsub Bounded GSUB table view.
/// @param coverage Byte offset of the Coverage table within @p gsub.
/// @param glyph Glyph identifier to locate.
/// @return Zero-based coverage index, or `-1` when the glyph is absent or the format is unsupported.
static int32_t gsub_coverage_index(const gsub_slice_t *gsub, uint32_t coverage, uint16_t glyph) {
    uint16_t format = gsub_u16(gsub, coverage);
    if (format == 1) {
        uint16_t count = gsub_u16(gsub, coverage + 2);
        uint32_t lo = 0, hi = count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint16_t entry = gsub_u16(gsub, coverage + 4 + mid * 2);
            if (entry == glyph)
                return (int32_t)mid;
            if (entry < glyph)
                lo = mid + 1;
            else
                hi = mid;
        }
        return -1;
    }
    if (format == 2) {
        uint16_t range_count = gsub_u16(gsub, coverage + 2);
        uint32_t lo = 0, hi = range_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint32_t record = coverage + 4 + mid * 6;
            uint16_t start = gsub_u16(gsub, record);
            uint16_t end = gsub_u16(gsub, record + 2);
            if (glyph < start) {
                hi = mid;
            } else if (glyph > end) {
                lo = mid + 1;
            } else {
                uint16_t start_index = gsub_u16(gsub, record + 4);
                return (int32_t)(start_index + glyph - start);
            }
        }
        return -1;
    }
    return -1;
}

/// @brief Resolve a glyph's class in an OpenType ClassDef table.
/// @details Supports format 1 consecutive class arrays and format 2 sorted range records. Class
/// zero is the implicit value for uncovered glyphs and malformed or unsupported tables.
/// @param gsub Bounded GSUB table view.
/// @param classdef Byte offset of the ClassDef table, or zero for the implicit default class.
/// @param glyph Glyph identifier to classify.
/// @return Class identifier, or zero when no explicit class applies.
static uint16_t gsub_glyph_class(const gsub_slice_t *gsub, uint32_t classdef, uint16_t glyph) {
    if (classdef == 0)
        return 0;
    uint16_t format = gsub_u16(gsub, classdef);
    if (format == 1) {
        uint16_t start = gsub_u16(gsub, classdef + 2);
        uint16_t count = gsub_u16(gsub, classdef + 4);
        if (glyph < start || glyph >= start + count)
            return 0;
        return gsub_u16(gsub, classdef + 6 + (uint32_t)(glyph - start) * 2);
    }
    if (format == 2) {
        uint16_t range_count = gsub_u16(gsub, classdef + 2);
        uint32_t lo = 0, hi = range_count;
        while (lo < hi) {
            uint32_t mid = (lo + hi) / 2;
            uint32_t record = classdef + 4 + mid * 6;
            uint16_t start = gsub_u16(gsub, record);
            uint16_t end = gsub_u16(gsub, record + 2);
            if (glyph < start)
                hi = mid;
            else if (glyph > end)
                lo = mid + 1;
            else
                return gsub_u16(gsub, record + 4);
        }
        return 0;
    }
    return 0;
}

//=============================================================================
// Shaping state
//=============================================================================

/// @brief Mutable glyph run and the GSUB data used to transform it.
typedef struct gsub_run {
    /// @brief Non-owning table view valid for the duration of shaping.
    const gsub_slice_t *gsub;
    vg_shaped_glyph_t *glyphs; ///< Mutable shaped run (glyph ids + source spans).
    int32_t count;             ///< Current glyph count.
} gsub_run_t;

/// @brief Apply a selected GSUB lookup at one run position.
/// @param run Mutable shaping run.
/// @param lookup_index Index in the GSUB LookupList.
/// @param position Glyph position at which matching begins.
/// @param depth Current contextual-lookup nesting depth.
/// @return Nonzero when a supported subtable applies, otherwise zero.
static int32_t
gsub_apply_lookup_at(gsub_run_t *run, uint32_t lookup_index, int32_t position, int32_t depth);

/// @brief Resolve a lookup's subtable, unwrapping extension (type 7) records.
/// @param gsub Bounded GSUB table view.
/// @param lookup_type Lookup type declared by the parent lookup.
/// @param subtable Byte offset of the selected lookup subtable.
/// @param subtable_out Receives the effective subtable offset.
/// @return Effective lookup type, or zero for an unsupported extension format.
static uint16_t gsub_resolve_subtable(const gsub_slice_t *gsub,
                                      uint16_t lookup_type,
                                      uint32_t subtable,
                                      uint32_t *subtable_out) {
    if (lookup_type == 7) {
        if (gsub_u16(gsub, subtable) != 1)
            return 0;
        uint16_t wrapped_type = gsub_u16(gsub, subtable + 2);
        uint32_t wrapped_offset = gsub_u32(gsub, subtable + 4);
        *subtable_out = subtable + wrapped_offset;
        return wrapped_type;
    }
    *subtable_out = subtable;
    return lookup_type;
}

/// @brief Apply a single substitution (LookupType 1) at @p position.
/// @param run Mutable shaping run.
/// @param subtable Offset of a type 1 subtable.
/// @param position Glyph position to test and potentially replace.
/// @return One when a substitution occurs, otherwise zero.
static int32_t
gsub_apply_single(gsub_run_t *run, uint32_t subtable, int32_t position) {
    uint16_t glyph = run->glyphs[position].glyph_id;
    uint16_t format = gsub_u16(run->gsub, subtable);
    uint32_t coverage = subtable + gsub_u16(run->gsub, subtable + 2);
    int32_t coverage_index = gsub_coverage_index(run->gsub, coverage, glyph);
    if (coverage_index < 0)
        return 0;
    if (format == 1) {
        int16_t delta = (int16_t)gsub_u16(run->gsub, subtable + 4);
        run->glyphs[position].glyph_id = (uint16_t)(glyph + delta);
        return 1;
    }
    if (format == 2) {
        uint16_t count = gsub_u16(run->gsub, subtable + 4);
        if ((uint32_t)coverage_index >= count)
            return 0;
        run->glyphs[position].glyph_id =
            gsub_u16(run->gsub, subtable + 6 + (uint32_t)coverage_index * 2);
        return 1;
    }
    return 0;
}

/// @brief Apply a ligature substitution (LookupType 4) at @p position.
/// @details On match, the matched glyphs collapse into one shaped glyph whose
///          source span covers every merged character.
/// @param run Mutable shaping run.
/// @param subtable Offset of a type 4 ligature-substitution subtable.
/// @param position Position of the first candidate component.
/// @return Number of glyphs consumed (>=2) when a ligature formed, else 0.
static int32_t
gsub_apply_ligature(gsub_run_t *run, uint32_t subtable, int32_t position) {
    if (gsub_u16(run->gsub, subtable) != 1)
        return 0;
    uint16_t first = run->glyphs[position].glyph_id;
    uint32_t coverage = subtable + gsub_u16(run->gsub, subtable + 2);
    int32_t coverage_index = gsub_coverage_index(run->gsub, coverage, first);
    if (coverage_index < 0)
        return 0;
    uint16_t set_count = gsub_u16(run->gsub, subtable + 4);
    if ((uint32_t)coverage_index >= set_count)
        return 0;
    uint32_t set = subtable + gsub_u16(run->gsub, subtable + 6 + (uint32_t)coverage_index * 2);
    uint16_t ligature_count = gsub_u16(run->gsub, set);
    for (uint16_t i = 0; i < ligature_count; ++i) {
        uint32_t ligature = set + gsub_u16(run->gsub, set + 2 + (uint32_t)i * 2);
        uint16_t lig_glyph = gsub_u16(run->gsub, ligature);
        uint16_t component_count = gsub_u16(run->gsub, ligature + 2);
        if (component_count < 2 || component_count > GSUB_MAX_RULE_GLYPHS)
            continue;
        if (position + component_count > run->count)
            continue;
        int32_t matched = 1;
        for (uint16_t component = 1; component < component_count; ++component) {
            uint16_t expected = gsub_u16(run->gsub, ligature + 4 + (uint32_t)(component - 1) * 2);
            if (run->glyphs[position + component].glyph_id != expected) {
                matched = 0;
                break;
            }
        }
        if (!matched)
            continue;
        // Collapse: one glyph owning the union of the source spans.
        uint16_t span_start = run->glyphs[position].source_start;
        uint16_t span_len = 0;
        for (uint16_t component = 0; component < component_count; ++component)
            span_len = (uint16_t)(span_len + run->glyphs[position + component].source_len);
        run->glyphs[position].glyph_id = lig_glyph;
        run->glyphs[position].source_start = span_start;
        run->glyphs[position].source_len = span_len;
        int32_t tail = run->count - (position + component_count);
        memmove(&run->glyphs[position + 1],
                &run->glyphs[position + component_count],
                (size_t)tail * sizeof(vg_shaped_glyph_t));
        run->count -= component_count - 1;
        return component_count;
    }
    return 0;
}

/// @brief Apply nested substitution records for a matched contextual rule.
/// @param run Mutable shaping run.
/// @param records Offset of the first `SubstLookupRecord`.
/// @param record_count Number of consecutive records to inspect.
/// @param position Run position corresponding to sequence index zero.
/// @param depth Current contextual-lookup nesting depth.
static void gsub_apply_records(gsub_run_t *run,
                               uint32_t records,
                               uint16_t record_count,
                               int32_t position,
                               int32_t depth) {
    for (uint16_t i = 0; i < record_count; ++i) {
        uint16_t sequence_index = gsub_u16(run->gsub, records + (uint32_t)i * 4);
        uint16_t nested_lookup = gsub_u16(run->gsub, records + (uint32_t)i * 4 + 2);
        if (position + sequence_index < run->count)
            (void)gsub_apply_lookup_at(run, nested_lookup, position + sequence_index, depth + 1);
    }
}

/// @brief Apply a chaining contextual substitution (LookupType 6) at @p position.
/// @details Handles glyph-, class-, and coverage-based chaining formats. Matching checks input,
/// backtrack, and lookahead sequences before dispatching nested substitution records.
/// @param run Mutable shaping run.
/// @param subtable Offset of a type 6 chaining-context subtable.
/// @param position Input position at which matching begins.
/// @param depth Current contextual-lookup nesting depth.
/// @return One when a contextual rule matches and its records are processed, otherwise zero.
static int32_t
gsub_apply_chain(gsub_run_t *run, uint32_t subtable, int32_t position, int32_t depth) {
    uint16_t format = gsub_u16(run->gsub, subtable);
    uint16_t glyph = run->glyphs[position].glyph_id;

    if (format == 3) {
        uint32_t cursor = subtable + 2;
        uint16_t backtrack_count = gsub_u16(run->gsub, cursor);
        cursor += 2;
        uint32_t backtrack = cursor;
        cursor += (uint32_t)backtrack_count * 2;
        uint16_t input_count = gsub_u16(run->gsub, cursor);
        cursor += 2;
        uint32_t input = cursor;
        cursor += (uint32_t)input_count * 2;
        uint16_t lookahead_count = gsub_u16(run->gsub, cursor);
        cursor += 2;
        uint32_t lookahead = cursor;
        cursor += (uint32_t)lookahead_count * 2;
        uint16_t record_count = gsub_u16(run->gsub, cursor);
        uint32_t records = cursor + 2;

        if (input_count == 0 || input_count > GSUB_MAX_RULE_GLYPHS)
            return 0;
        if (position < backtrack_count || position + input_count > run->count)
            return 0;
        if (position + input_count + lookahead_count > run->count)
            return 0;
        for (uint16_t i = 0; i < input_count; ++i) {
            uint32_t coverage = subtable + gsub_u16(run->gsub, input + (uint32_t)i * 2);
            if (gsub_coverage_index(run->gsub, coverage, run->glyphs[position + i].glyph_id) < 0)
                return 0;
        }
        // Backtrack coverages run from nearest-previous outward.
        for (uint16_t i = 0; i < backtrack_count; ++i) {
            uint32_t coverage = subtable + gsub_u16(run->gsub, backtrack + (uint32_t)i * 2);
            if (gsub_coverage_index(
                    run->gsub, coverage, run->glyphs[position - 1 - i].glyph_id) < 0)
                return 0;
        }
        for (uint16_t i = 0; i < lookahead_count; ++i) {
            uint32_t coverage = subtable + gsub_u16(run->gsub, lookahead + (uint32_t)i * 2);
            if (gsub_coverage_index(run->gsub,
                                    coverage,
                                    run->glyphs[position + input_count + i].glyph_id) < 0)
                return 0;
        }
        gsub_apply_records(run, records, record_count, position, depth);
        return 1;
    }

    if (format == 2) {
        uint32_t coverage = subtable + gsub_u16(run->gsub, subtable + 2);
        if (gsub_coverage_index(run->gsub, coverage, glyph) < 0)
            return 0;
        uint32_t backtrack_classdef = gsub_u16(run->gsub, subtable + 4);
        uint32_t input_classdef = gsub_u16(run->gsub, subtable + 6);
        uint32_t lookahead_classdef = gsub_u16(run->gsub, subtable + 8);
        uint16_t set_count = gsub_u16(run->gsub, subtable + 10);
        uint16_t input_class =
            gsub_glyph_class(run->gsub,
                             input_classdef ? subtable + input_classdef : 0,
                             glyph);
        if (input_class >= set_count)
            return 0;
        uint16_t set_offset = gsub_u16(run->gsub, subtable + 12 + (uint32_t)input_class * 2);
        if (set_offset == 0)
            return 0;
        uint32_t set = subtable + set_offset;
        uint16_t rule_count = gsub_u16(run->gsub, set);
        for (uint16_t rule_index = 0; rule_index < rule_count; ++rule_index) {
            uint32_t rule = set + gsub_u16(run->gsub, set + 2 + (uint32_t)rule_index * 2);
            uint32_t cursor = rule;
            uint16_t backtrack_count = gsub_u16(run->gsub, cursor);
            cursor += 2;
            uint32_t backtrack = cursor;
            cursor += (uint32_t)backtrack_count * 2;
            uint16_t input_count = gsub_u16(run->gsub, cursor);
            cursor += 2;
            uint32_t input = cursor; // input_count-1 classes (first is implicit).
            cursor += (uint32_t)(input_count > 0 ? input_count - 1 : 0) * 2;
            uint16_t lookahead_count = gsub_u16(run->gsub, cursor);
            cursor += 2;
            uint32_t lookahead = cursor;
            cursor += (uint32_t)lookahead_count * 2;
            uint16_t record_count = gsub_u16(run->gsub, cursor);
            uint32_t records = cursor + 2;

            if (input_count == 0 || input_count > GSUB_MAX_RULE_GLYPHS)
                continue;
            if (position < backtrack_count || position + input_count > run->count ||
                position + input_count + lookahead_count > run->count)
                continue;
            int32_t matched = 1;
            for (uint16_t i = 1; i < input_count && matched; ++i) {
                uint16_t expected = gsub_u16(run->gsub, input + (uint32_t)(i - 1) * 2);
                uint16_t actual =
                    gsub_glyph_class(run->gsub,
                                     input_classdef ? subtable + input_classdef : 0,
                                     run->glyphs[position + i].glyph_id);
                if (actual != expected)
                    matched = 0;
            }
            for (uint16_t i = 0; i < backtrack_count && matched; ++i) {
                uint16_t expected = gsub_u16(run->gsub, backtrack + (uint32_t)i * 2);
                uint16_t actual =
                    gsub_glyph_class(run->gsub,
                                     backtrack_classdef ? subtable + backtrack_classdef : 0,
                                     run->glyphs[position - 1 - i].glyph_id);
                if (actual != expected)
                    matched = 0;
            }
            for (uint16_t i = 0; i < lookahead_count && matched; ++i) {
                uint16_t expected = gsub_u16(run->gsub, lookahead + (uint32_t)i * 2);
                uint16_t actual =
                    gsub_glyph_class(run->gsub,
                                     lookahead_classdef ? subtable + lookahead_classdef : 0,
                                     run->glyphs[position + input_count + i].glyph_id);
                if (actual != expected)
                    matched = 0;
            }
            if (!matched)
                continue;
            gsub_apply_records(run, records, record_count, position, depth);
            return 1;
        }
        return 0;
    }

    if (format == 1) {
        uint32_t coverage = subtable + gsub_u16(run->gsub, subtable + 2);
        int32_t coverage_index = gsub_coverage_index(run->gsub, coverage, glyph);
        if (coverage_index < 0)
            return 0;
        uint16_t set_count = gsub_u16(run->gsub, subtable + 4);
        if ((uint32_t)coverage_index >= set_count)
            return 0;
        uint32_t set = subtable + gsub_u16(run->gsub, subtable + 6 + (uint32_t)coverage_index * 2);
        uint16_t rule_count = gsub_u16(run->gsub, set);
        for (uint16_t rule_index = 0; rule_index < rule_count; ++rule_index) {
            uint32_t rule = set + gsub_u16(run->gsub, set + 2 + (uint32_t)rule_index * 2);
            uint32_t cursor = rule;
            uint16_t backtrack_count = gsub_u16(run->gsub, cursor);
            cursor += 2;
            uint32_t backtrack = cursor;
            cursor += (uint32_t)backtrack_count * 2;
            uint16_t input_count = gsub_u16(run->gsub, cursor);
            cursor += 2;
            uint32_t input = cursor; // input_count-1 glyph ids.
            cursor += (uint32_t)(input_count > 0 ? input_count - 1 : 0) * 2;
            uint16_t lookahead_count = gsub_u16(run->gsub, cursor);
            cursor += 2;
            uint32_t lookahead = cursor;
            cursor += (uint32_t)lookahead_count * 2;
            uint16_t record_count = gsub_u16(run->gsub, cursor);
            uint32_t records = cursor + 2;

            if (input_count == 0 || input_count > GSUB_MAX_RULE_GLYPHS)
                continue;
            if (position < backtrack_count || position + input_count > run->count ||
                position + input_count + lookahead_count > run->count)
                continue;
            int32_t matched = 1;
            for (uint16_t i = 1; i < input_count && matched; ++i) {
                if (run->glyphs[position + i].glyph_id !=
                    gsub_u16(run->gsub, input + (uint32_t)(i - 1) * 2))
                    matched = 0;
            }
            for (uint16_t i = 0; i < backtrack_count && matched; ++i) {
                if (run->glyphs[position - 1 - i].glyph_id !=
                    gsub_u16(run->gsub, backtrack + (uint32_t)i * 2))
                    matched = 0;
            }
            for (uint16_t i = 0; i < lookahead_count && matched; ++i) {
                if (run->glyphs[position + input_count + i].glyph_id !=
                    gsub_u16(run->gsub, lookahead + (uint32_t)i * 2))
                    matched = 0;
            }
            if (!matched)
                continue;
            gsub_apply_records(run, records, record_count, position, depth);
            return 1;
        }
        return 0;
    }
    return 0;
}

/// @brief Apply one lookup (all its subtables) at a single run position.
/// @details Extension subtables are unwrapped before dispatch. Supported effective lookup types
/// are single substitution, ligature substitution, and chaining contextual substitution.
/// @param run Mutable shaping run.
/// @param lookup_index Index in the GSUB LookupList.
/// @param position Glyph position at which matching begins.
/// @param depth Current contextual-lookup nesting depth.
/// @return Non-zero when any subtable substituted at this position.
static int32_t
gsub_apply_lookup_at(gsub_run_t *run, uint32_t lookup_index, int32_t position, int32_t depth) {
    if (depth > GSUB_MAX_NESTED_DEPTH || position < 0 || position >= run->count)
        return 0;
    const gsub_slice_t *gsub = run->gsub;
    uint32_t lookup_list = gsub_u16(gsub, 8);
    uint16_t lookup_count = gsub_u16(gsub, lookup_list);
    if (lookup_index >= lookup_count)
        return 0;
    uint32_t lookup = lookup_list + gsub_u16(gsub, lookup_list + 2 + lookup_index * 2);
    uint16_t lookup_type = gsub_u16(gsub, lookup);
    uint16_t subtable_count = gsub_u16(gsub, lookup + 4);
    for (uint16_t i = 0; i < subtable_count; ++i) {
        uint32_t subtable = lookup + gsub_u16(gsub, lookup + 6 + (uint32_t)i * 2);
        uint32_t resolved = 0;
        uint16_t effective_type = gsub_resolve_subtable(gsub, lookup_type, subtable, &resolved);
        int32_t applied = 0;
        switch (effective_type) {
            case 1:
                applied = gsub_apply_single(run, resolved, position);
                break;
            case 4:
                applied = gsub_apply_ligature(run, resolved, position);
                break;
            case 6:
                applied = gsub_apply_chain(run, resolved, position, depth);
                break;
            default:
                break;
        }
        if (applied)
            return applied;
    }
    return 0;
}

//=============================================================================
// Load-time feature resolution
//=============================================================================

/// @brief Collect supported ligature lookup indices from a parsed font.
/// @details Selects the `DFLT` script or falls back to `latn`, reads its default language system,
/// gathers unique `liga` and `calt` lookup indices, sorts them into LookupList order, and stores an
/// owned copy on the font. Missing or malformed data simply leaves shaping disabled.
/// @param font Parsed font whose GSUB metadata and owned lookup list are updated.
void vg_gsub_init(struct vg_font *font) {
    if (!font || !font->gsub_offset || font->gsub_len < 10)
        return;
    gsub_slice_t gsub = {font->data + font->gsub_offset, font->gsub_len};
    uint32_t script_list = gsub_u16(&gsub, 4);
    uint32_t feature_list = gsub_u16(&gsub, 6);
    if (!script_list || !feature_list)
        return;

    // Pick 'DFLT', falling back to 'latn'.
    uint16_t script_count = gsub_u16(&gsub, script_list);
    uint32_t script = 0;
    for (int pass = 0; pass < 2 && !script; ++pass) {
        uint32_t want = pass == 0 ? TTF_TAG('D', 'F', 'L', 'T') : TTF_TAG('l', 'a', 't', 'n');
        for (uint16_t i = 0; i < script_count; ++i) {
            uint32_t record = script_list + 2 + (uint32_t)i * 6;
            if (gsub_u32(&gsub, record) == want) {
                script = script_list + gsub_u16(&gsub, record + 4);
                break;
            }
        }
    }
    if (!script)
        return;
    uint16_t default_langsys_offset = gsub_u16(&gsub, script);
    if (!default_langsys_offset)
        return;
    uint32_t langsys = script + default_langsys_offset;
    uint16_t feature_index_count = gsub_u16(&gsub, langsys + 4);

    uint16_t collected[128];
    uint16_t collected_count = 0;
    for (uint16_t i = 0; i < feature_index_count; ++i) {
        uint16_t feature_index = gsub_u16(&gsub, langsys + 6 + (uint32_t)i * 2);
        uint32_t record = feature_list + 2 + (uint32_t)feature_index * 6;
        uint32_t tag = gsub_u32(&gsub, record);
        if (tag != TTF_TAG('l', 'i', 'g', 'a') && tag != TTF_TAG('c', 'a', 'l', 't'))
            continue;
        uint32_t feature = feature_list + gsub_u16(&gsub, record + 4);
        uint16_t lookup_index_count = gsub_u16(&gsub, feature + 2);
        for (uint16_t j = 0; j < lookup_index_count && collected_count < 128; ++j) {
            uint16_t lookup_index = gsub_u16(&gsub, feature + 4 + (uint32_t)j * 2);
            int already = 0;
            for (uint16_t k = 0; k < collected_count; ++k) {
                if (collected[k] == lookup_index) {
                    already = 1;
                    break;
                }
            }
            if (!already)
                collected[collected_count++] = lookup_index;
        }
    }
    if (!collected_count)
        return;
    // Lookups apply in LookupList order per the OpenType processing model.
    for (uint16_t i = 1; i < collected_count; ++i) {
        uint16_t value = collected[i];
        int32_t j = i - 1;
        while (j >= 0 && collected[j] > value) {
            collected[j + 1] = collected[j];
            --j;
        }
        collected[j + 1] = value;
    }
    font->gsub_feature_lookups = (uint16_t *)malloc(collected_count * sizeof(uint16_t));
    if (!font->gsub_feature_lookups)
        return;
    memcpy(font->gsub_feature_lookups, collected, collected_count * sizeof(uint16_t));
    font->gsub_feature_lookup_count = collected_count;
}

//=============================================================================
// Public shaping API
//=============================================================================

/// @copydoc vg_font_has_ligatures
bool vg_font_has_ligatures(vg_font_t *font) {
    return font && font->gsub_feature_lookup_count > 0;
}

/// @copydoc vg_font_shape
int32_t vg_font_shape(vg_font_t *font,
                      const uint32_t *codepoints,
                      int32_t count,
                      vg_shaped_glyph_t *out,
                      int32_t out_capacity) {
    if (!font || !codepoints || !out || count <= 0 || out_capacity < count)
        return 0;
    for (int32_t i = 0; i < count; ++i) {
        out[i].glyph_id = ttf_get_glyph_index(font, codepoints[i]);
        out[i].source_start = (uint16_t)i;
        out[i].source_len = 1;
    }
    if (!font->gsub_feature_lookup_count || !font->gsub_offset)
        return count;

    gsub_slice_t gsub = {font->data + font->gsub_offset, font->gsub_len};
    gsub_run_t run = {&gsub, out, count};
    for (uint16_t lookup = 0; lookup < font->gsub_feature_lookup_count; ++lookup) {
        uint32_t lookup_index = font->gsub_feature_lookups[lookup];
        for (int32_t position = 0; position < run.count; ++position)
            (void)gsub_apply_lookup_at(&run, lookup_index, position, 0);
    }
    return run.count;
}
