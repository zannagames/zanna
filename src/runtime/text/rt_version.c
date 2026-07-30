//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_version.c
// Purpose: Implements semantic version parsing, comparison, and constraint
//          checking for the Zanna.Text.Version class per SemVer 2.0.0
//          (https://semver.org/). Supports major.minor.patch with optional
//          pre-release identifiers and build metadata.
//
// Key invariants:
//   - Parses MAJOR.MINOR.PATCH[-prerelease][+build] per SemVer 2.0.0, with one
//     deliberate deviation: numeric core components are limited to the signed
//     64-bit range (the class exposes them as i64 properties), so components
//     above INT64_MAX are rejected even though SemVer sets no size limit.
//   - Comparison follows SemVer precedence: major > minor > patch > pre-release.
//   - Pre-release versions have lower precedence than the release they extend.
//   - Build metadata is ignored for comparison purposes.
//   - Constraint checking supports the operators: =, !=, <, <=, >, >=, ^, ~.
//   - Invalid version strings cause Parse to return NULL.
//
// Ownership/Lifetime:
//   - Parsed version objects are heap-allocated and managed by the runtime GC.
//   - Pre-release and build-metadata strings within the version are fresh copies.
//
// Links: src/runtime/text/rt_version.h (public API)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_version.c
 * @brief Implements SemVer parsing, precedence, constraints, and component bumps.
 * @details Managed Version objects store bounded signed core components plus
 *          owned prerelease and build text. Comparison follows SemVer
 *          identifier precedence while ignoring build metadata, and constraint
 *          evaluation supports relational, caret, and tilde operators.
 */

#include "rt_version.h"

#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_string_builder.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct rt_version_impl {
    void **vptr;
    int64_t major;
    int64_t minor;
    int64_t patch;
    char *prerelease; // NULL if none
    char *build;      // NULL if none
} rt_version_impl;

/// @brief GC finalizer — release the optional prerelease and build metadata strings.
/// @details The numeric major/minor/patch live inline in the struct,
///          but the optional `-pre` and `+build` portions are
///          runtime-allocated copies. `rt_free(NULL)` is a no-op so we
///          don't need to guard the optional fields explicitly.
/// @param obj Version object being finalized; null is ignored.
static void version_finalizer(void *obj) {
    rt_version_impl *v = (rt_version_impl *)obj;
    if (!v)
        return;
    rt_free(v->prerelease);
    rt_free(v->build);
}

/// @brief NUL-terminated heap copy of `s[0..len)`. Returns NULL on allocation failure.
/// @param s Source byte span.
/// @param len Number of bytes to copy.
/// @return Newly allocated C string, or null on allocation failure.
static char *dup_str(const char *s, size_t len) {
    if (!s || (uint64_t)len >= (uint64_t)INT64_MAX)
        return NULL;
    char *r = (char *)rt_alloc((int64_t)(len + 1));
    if (!r)
        return NULL;
    memcpy(r, s, len);
    r[len] = '\0';
    return r;
}

/// @brief Release a runtime-managed Version reference.
/// @param v Version object; null is ignored.
static void release_version(void *v) {
    if (v && rt_obj_release_check0(v))
        rt_obj_free(v);
}

/// @brief Trap when a version string-builder operation fails.
/// @param status Builder status to inspect.
/// @param op Null-terminated trap message.
static void check_sb(rt_sb_status_t status, const char *op) {
    if (status != RT_SB_OK)
        rt_trap(op);
}

/// @brief Copy a byte range after trimming C-library whitespace at both ends.
/// @param start Inclusive range start.
/// @param end Exclusive range end.
/// @return Newly allocated trimmed runtime string.
static rt_string trimmed_slice_string(const char *start, const char *end) {
    while (start < end && isspace((unsigned char)*start))
        start++;
    while (end > start && isspace((unsigned char)end[-1]))
        end--;
    return rt_string_from_bytes(start, (size_t)(end - start));
}

/// @brief Test whether a byte is legal in a SemVer identifier.
/// @param c Byte to classify.
/// @return Nonzero for an alphanumeric byte or hyphen.
static int semver_ident_char(char c) {
    return isalnum((unsigned char)c) || c == '-';
}

/// @brief Validate dot-separated prerelease or build identifiers.
/// @details Empty identifiers are rejected. Numeric prerelease identifiers
///          additionally reject leading zeros; build identifiers do not.
/// @param s Identifier-list bytes.
/// @param len Number of bytes in @p s.
/// @param prerelease Nonzero to apply prerelease numeric rules.
/// @return Nonzero when the complete list is valid.
static int semver_validate_identifiers(const char *s, size_t len, int prerelease) {
    if (len == 0)
        return 0;

    size_t start = 0;
    while (start < len) {
        size_t end = start;
        int numeric = 1;
        while (end < len && s[end] != '.') {
            if (!semver_ident_char(s[end]))
                return 0;
            if (!isdigit((unsigned char)s[end]))
                numeric = 0;
            end++;
        }
        if (end == start)
            return 0;
        if (prerelease && numeric && end - start > 1 && s[start] == '0')
            return 0;
        if (end == len)
            return 1;
        start = end + 1;
    }

    return 0;
}

/// @brief Parse a non-negative decimal integer from `str` at `*pos`, advancing the cursor.
/// @details Enforces the SemVer "no leading zeros" rule (so `01` is
///          rejected, but plain `0` is fine). Returns -1 on
///          malformed input, leaving `*pos` unchanged for the
///          caller to react. Leaves `*pos` pointing at the first
///          non-digit byte after a successful parse.
/// @param str Complete version byte buffer.
/// @param len Number of bytes in @p str.
/// @param pos In/out parse offset.
/// @return Parsed component in `[0, INT64_MAX]`, or `-1` on failure.
static int64_t parse_num(const char *str, size_t len, size_t *pos) {
    if (*pos >= len || !isdigit((unsigned char)str[*pos]))
        return -1;

    // No leading zeros (except "0" itself)
    if (str[*pos] == '0' && *pos + 1 < len && isdigit((unsigned char)str[*pos + 1]))
        return -1;

    int64_t val = 0;
    while (*pos < len && isdigit((unsigned char)str[*pos])) {
        int digit = str[*pos] - '0';
        if (val > (INT64_MAX - digit) / 10)
            return -1;
        val = val * 10 + digit;
        (*pos)++;
    }
    return val;
}

/// @brief Parse a SemVer 2.0 string ("1.2.3-rc.1+build.5") into a Version object. Returns NULL
/// for malformed input. Optional prerelease (after `-`) and build metadata (after `+`) are kept
/// separate; build metadata is ignored by `_cmp` per SemVer spec.
/// @details Accepts an optional leading `v` or `V`. Core components must fit
///          signed 64-bit integers and follow the no-leading-zero rule.
/// @param str Borrowed version text.
/// @return Caller-owned runtime-managed Version, or null for invalid input.
void *rt_version_parse(rt_string str) {
    if (!str)
        return NULL;
    const char *src = rt_string_cstr(str);
    if (!src)
        return NULL;
    size_t len = (size_t)rt_str_len(str);
    if (len == 0)
        return NULL;

    // Skip optional leading 'v' or 'V'
    size_t pos = 0;
    if (src[0] == 'v' || src[0] == 'V')
        pos = 1;

    // Parse MAJOR
    int64_t major = parse_num(src, len, &pos);
    if (major < 0)
        return NULL;

    // Expect '.'
    if (pos >= len || src[pos] != '.')
        return NULL;
    pos++;

    // Parse MINOR
    int64_t minor = parse_num(src, len, &pos);
    if (minor < 0)
        return NULL;

    // Expect '.'
    if (pos >= len || src[pos] != '.')
        return NULL;
    pos++;

    // Parse PATCH
    int64_t patch = parse_num(src, len, &pos);
    if (patch < 0)
        return NULL;

    // Pre-release: -alpha.1.beta
    char *prerelease = NULL;
    if (pos < len && src[pos] == '-') {
        pos++;
        size_t start = pos;
        while (pos < len && src[pos] != '+')
            pos++;
        if (!semver_validate_identifiers(src + start, pos - start, 1))
            return NULL;
        prerelease = dup_str(src + start, pos - start);
        if (!prerelease) {
            rt_trap("Version.Parse: memory allocation failed");
            return NULL;
        }
    }

    // Build metadata: +build.42
    char *build = NULL;
    if (pos < len && src[pos] == '+') {
        pos++;
        size_t start = pos;
        while (pos < len)
            pos++;
        if (!semver_validate_identifiers(src + start, pos - start, 0)) {
            rt_free(prerelease);
            return NULL;
        }
        build = dup_str(src + start, pos - start);
        if (!build) {
            rt_free(prerelease);
            rt_trap("Version.Parse: memory allocation failed");
            return NULL;
        }
    }

    // Should have consumed everything
    if (pos != len) {
        rt_free(prerelease);
        rt_free(build);
        return NULL;
    }

    rt_version_impl *v = (rt_version_impl *)rt_obj_new_i64(0, sizeof(rt_version_impl));
    if (!v) {
        rt_free(prerelease);
        rt_free(build);
        return NULL;
    }
    v->major = major;
    v->minor = minor;
    v->patch = patch;
    v->prerelease = prerelease;
    v->build = build;

    rt_obj_set_finalizer(v, version_finalizer);
    return v;
}

/// @brief Check whether a string is a valid semantic version (major.minor.patch).
/// @param str Borrowed version text.
/// @return `1` when `rt_version_parse` succeeds, otherwise `0`.
int8_t rt_version_is_valid(rt_string str) {
    void *v = rt_version_parse(str);
    if (!v)
        return 0;
    release_version(v);
    return 1;
}

/// @brief Return the MAJOR component of the parsed version.
/// @param ver Borrowed Version object; null reports zero.
/// @return Major component.
int64_t rt_version_major(void *ver) {
    if (!ver)
        return 0;
    return ((rt_version_impl *)ver)->major;
}

/// @brief Return the MINOR component of the parsed version.
/// @param ver Borrowed Version object; null reports zero.
/// @return Minor component.
int64_t rt_version_minor(void *ver) {
    if (!ver)
        return 0;
    return ((rt_version_impl *)ver)->minor;
}

/// @brief Return the PATCH component of the parsed version.
/// @param ver Borrowed Version object; null reports zero.
/// @return Patch component.
int64_t rt_version_patch(void *ver) {
    if (!ver)
        return 0;
    return ((rt_version_impl *)ver)->patch;
}

/// @brief Return the prerelease label (text after `-`), or empty string if absent.
/// @param ver Borrowed Version object.
/// @return Newly allocated prerelease text or empty string.
rt_string rt_version_prerelease(void *ver) {
    if (!ver || !((rt_version_impl *)ver)->prerelease)
        return rt_string_from_bytes("", 0);
    const char *pr = ((rt_version_impl *)ver)->prerelease;
    return rt_string_from_bytes(pr, strlen(pr));
}

/// @brief Return the build metadata (text after `+`), or empty string if absent.
/// @details Per SemVer 2.0, build metadata does not affect comparison
///          ordering — it's metadata for the consumer's bookkeeping
///          (CI build numbers, commit hashes, etc.).
/// @param ver Borrowed Version object.
/// @return Newly allocated build metadata or empty string.
rt_string rt_version_build(void *ver) {
    if (!ver || !((rt_version_impl *)ver)->build)
        return rt_string_from_bytes("", 0);
    const char *b = ((rt_version_impl *)ver)->build;
    return rt_string_from_bytes(b, strlen(b));
}

/// @brief Convert the version to a human-readable string representation.
/// @param ver Borrowed Version object.
/// @return Newly allocated canonical version text, or empty string for null.
rt_string rt_version_to_string(void *ver) {
    if (!ver)
        return rt_string_from_bytes("", 0);
    rt_version_impl *v = (rt_version_impl *)ver;

    rt_string_builder sb;
    rt_sb_init(&sb);
    check_sb(rt_sb_append_int(&sb, v->major), "Version.ToString: formatting failed");
    check_sb(rt_sb_append_cstr(&sb, "."), "Version.ToString: formatting failed");
    check_sb(rt_sb_append_int(&sb, v->minor), "Version.ToString: formatting failed");
    check_sb(rt_sb_append_cstr(&sb, "."), "Version.ToString: formatting failed");
    check_sb(rt_sb_append_int(&sb, v->patch), "Version.ToString: formatting failed");
    if (v->prerelease) {
        check_sb(rt_sb_append_cstr(&sb, "-"), "Version.ToString: formatting failed");
        check_sb(rt_sb_append_cstr(&sb, v->prerelease), "Version.ToString: formatting failed");
    }
    if (v->build) {
        check_sb(rt_sb_append_cstr(&sb, "+"), "Version.ToString: formatting failed");
        check_sb(rt_sb_append_cstr(&sb, v->build), "Version.ToString: formatting failed");
    }

    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

/// @brief Compare prerelease identifier lists by SemVer precedence.
/// @details Absence sorts after any prerelease. Numeric identifiers sort by
///          magnitude and before alphanumeric identifiers; remaining text uses
///          bytewise lexical order.
/// @param a Null-terminated prerelease text, or null for a release.
/// @param b Null-terminated prerelease text, or null for a release.
/// @return `-1`, `0`, or `1` according to precedence.
static int cmp_prerelease(const char *a, const char *b) {
    // Both NULL -> equal
    if (!a && !b)
        return 0;
    // No pre-release has higher precedence
    if (!a)
        return 1;
    if (!b)
        return -1;

    // Split by '.' and compare each identifier
    const char *pa = a, *pb = b;
    while (*pa || *pb) {
        if (!*pa)
            return -1; // Fewer identifiers -> lower precedence
        if (!*pb)
            return 1;

        // Extract next identifier
        const char *ea = strchr(pa, '.');
        const char *eb = strchr(pb, '.');
        size_t la = ea ? (size_t)(ea - pa) : strlen(pa);
        size_t lb = eb ? (size_t)(eb - pb) : strlen(pb);

        // Check if numeric
        int a_num = 1, b_num = 1;
        for (size_t i = 0; i < la; ++i)
            if (!isdigit((unsigned char)pa[i]))
                a_num = 0;
        for (size_t i = 0; i < lb; ++i)
            if (!isdigit((unsigned char)pb[i]))
                b_num = 0;

        if (a_num && b_num) {
            // Both numeric: compare by length and bytes to avoid integer overflow.
            if (la < lb)
                return -1;
            if (la > lb)
                return 1;
            int cmp = memcmp(pa, pb, la);
            if (cmp != 0)
                return cmp < 0 ? -1 : 1;
        } else if (a_num != b_num) {
            // Numeric < alphanumeric
            return a_num ? -1 : 1;
        } else {
            // Both alphanumeric: lexicographic
            size_t minl = la < lb ? la : lb;
            int cmp = memcmp(pa, pb, minl);
            if (cmp != 0)
                return cmp < 0 ? -1 : 1;
            if (la < lb)
                return -1;
            if (la > lb)
                return 1;
        }

        pa += la + (ea ? 1 : 0);
        pb += lb + (eb ? 1 : 0);
    }
    return 0;
}

/// @brief Compare two parsed versions by SemVer precedence.
/// @details Build metadata is ignored. Null sorts before a non-null Version;
///          two null pointers compare equal.
/// @param a Borrowed first Version, or null.
/// @param b Borrowed second Version, or null.
/// @return `-1`, `0`, or `1`.
int64_t rt_version_cmp(void *a, void *b) {
    if (!a && !b)
        return 0;
    if (!a)
        return -1;
    if (!b)
        return 1;

    rt_version_impl *va = (rt_version_impl *)a;
    rt_version_impl *vb = (rt_version_impl *)b;

    if (va->major != vb->major)
        return va->major < vb->major ? -1 : 1;
    if (va->minor != vb->minor)
        return va->minor < vb->minor ? -1 : 1;
    if (va->patch != vb->patch)
        return va->patch < vb->patch ? -1 : 1;

    return cmp_prerelease(va->prerelease, vb->prerelease);
}

/// @brief Parse and compare two semantic version strings.
/// @details Invalid input parses as null and therefore follows
///          `rt_version_cmp`'s null ordering.
/// @param a Borrowed first version text.
/// @param b Borrowed second version text.
/// @return `-1`, `0`, or `1`.
int64_t rt_version_compare(rt_string a, rt_string b) {
    void *av = rt_version_parse(a);
    void *bv = rt_version_parse(b);
    int64_t result = rt_version_cmp(av, bv);
    release_version(av);
    release_version(bv);
    return result;
}

/// @brief Parse a semantic version string and return its major component.
/// @param str Borrowed version text.
/// @return Parsed major component, or zero when parsing fails.
int64_t rt_version_major_str(rt_string str) {
    void *ver = rt_version_parse(str);
    int64_t result = rt_version_major(ver);
    release_version(ver);
    return result;
}

/// @brief Parse a semantic version string and return its minor component.
/// @param str Borrowed version text.
/// @return Parsed minor component, or zero when parsing fails.
int64_t rt_version_minor_str(rt_string str) {
    void *ver = rt_version_parse(str);
    int64_t result = rt_version_minor(ver);
    release_version(ver);
    return result;
}

/// @brief Parse a semantic version string and return its patch component.
/// @param str Borrowed version text.
/// @return Parsed patch component, or zero when parsing fails.
int64_t rt_version_patch_str(rt_string str) {
    void *ver = rt_version_parse(str);
    int64_t result = rt_version_patch(ver);
    release_version(ver);
    return result;
}

/// @brief Test a parsed Version against one simple SemVer constraint.
/// @details Supports exact/no operator, `=`, `!=`, `<`, `<=`, `>`, `>=`,
///          caret, and tilde. Constraint whitespace is trimmed. Empty
///          constraints match every non-null Version. Caret compatibility
///          follows major/minor/patch zero rules; tilde requires equal
///          major/minor and no lower precedence.
/// @param ver Borrowed parsed Version.
/// @param constraint Borrowed constraint containing a complete three-part version.
/// @return `1` when satisfied, otherwise `0` for mismatch or invalid input.
int8_t rt_version_satisfies(void *ver, rt_string constraint) {
    if (!ver || !constraint)
        return 0;

    const char *cstr = rt_string_cstr(constraint);
    if (!cstr)
        return 0;
    size_t len = (size_t)rt_str_len(constraint);
    const char *constraint_end = cstr + len;
    while (cstr < constraint_end && isspace((unsigned char)*cstr))
        cstr++;
    while (constraint_end > cstr && isspace((unsigned char)constraint_end[-1]))
        constraint_end--;
    len = (size_t)(constraint_end - cstr);
    if (len == 0)
        return 1; // Empty constraint matches all

    rt_version_impl *v = (rt_version_impl *)ver;

    const char *p = cstr;

    // Parse operator and version
    char op[3] = {0};

    if (p < constraint_end && p[0] == '^') {
        // Caret: compatible with (same major, if major > 0)
        p++;
        rt_string vs = trimmed_slice_string(p, constraint_end);
        void *cv = rt_version_parse(vs);
        rt_string_unref(vs);
        if (!cv)
            return 0;

        rt_version_impl *c = (rt_version_impl *)cv;
        int8_t result = 0;
        if (c->major > 0) {
            result = (v->major == c->major && rt_version_cmp(ver, cv) >= 0) ? 1 : 0;
        } else if (c->minor > 0) {
            result =
                (v->major == 0 && v->minor == c->minor && rt_version_cmp(ver, cv) >= 0) ? 1 : 0;
        } else {
            result = (rt_version_cmp(ver, cv) == 0) ? 1 : 0;
        }
        release_version(cv);
        return result;
    } else if (p < constraint_end && p[0] == '~') {
        // Tilde: same major.minor
        p++;
        rt_string vs = trimmed_slice_string(p, constraint_end);
        void *cv = rt_version_parse(vs);
        rt_string_unref(vs);
        if (!cv)
            return 0;

        rt_version_impl *c = (rt_version_impl *)cv;
        int8_t result =
            (v->major == c->major && v->minor == c->minor && rt_version_cmp(ver, cv) >= 0) ? 1 : 0;
        release_version(cv);
        return result;
    }

    // Comparison operators: >=, <=, !=, >, <, =
    if (p + 1 < constraint_end && p[0] == '>' && p[1] == '=') {
        op[0] = '>';
        op[1] = '=';
        p += 2;
    } else if (p + 1 < constraint_end && p[0] == '<' && p[1] == '=') {
        op[0] = '<';
        op[1] = '=';
        p += 2;
    } else if (p + 1 < constraint_end && p[0] == '!' && p[1] == '=') {
        op[0] = '!';
        op[1] = '=';
        p += 2;
    } else if (p < constraint_end && p[0] == '>') {
        op[0] = '>';
        p++;
    } else if (p < constraint_end && p[0] == '<') {
        op[0] = '<';
        p++;
    } else if (p < constraint_end && p[0] == '=') {
        op[0] = '=';
        p++;
    } else {
        // No operator -> exact match
        op[0] = '=';
    }

    // Skip whitespace after operator
    while (p < constraint_end && isspace((unsigned char)*p))
        p++;

    rt_string vs = trimmed_slice_string(p, constraint_end);
    void *cv = rt_version_parse(vs);
    rt_string_unref(vs);
    if (!cv)
        return 0;

    int64_t cmp = rt_version_cmp(ver, cv);
    release_version(cv);

    if (op[0] == '>' && op[1] == '=')
        return cmp >= 0 ? 1 : 0;
    if (op[0] == '<' && op[1] == '=')
        return cmp <= 0 ? 1 : 0;
    if (op[0] == '!' && op[1] == '=')
        return cmp != 0 ? 1 : 0;
    if (op[0] == '>')
        return cmp > 0 ? 1 : 0;
    if (op[0] == '<')
        return cmp < 0 ? 1 : 0;
    // '='
    return cmp == 0 ? 1 : 0;
}

/// @brief Return a new version string with the major component incremented and minor/patch reset to
/// 0.
/// @details Prerelease and build metadata are discarded. Component overflow traps.
/// @param ver Borrowed Version object.
/// @return Newly allocated bumped version text, or empty string for null/overflow.
rt_string rt_version_bump_major(void *ver) {
    if (!ver)
        return rt_string_from_bytes("", 0);
    rt_version_impl *v = (rt_version_impl *)ver;
    if (v->major == INT64_MAX) {
        rt_trap("Version.BumpMajor: component overflow");
        return rt_string_from_bytes("", 0);
    }
    rt_string_builder sb;
    rt_sb_init(&sb);
    check_sb(rt_sb_append_int(&sb, v->major + 1), "Version.BumpMajor: formatting failed");
    check_sb(rt_sb_append_cstr(&sb, ".0.0"), "Version.BumpMajor: formatting failed");
    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

/// @brief Return a new version string with the minor component incremented and patch reset to 0.
/// @details Prerelease and build metadata are discarded. Component overflow traps.
/// @param ver Borrowed Version object.
/// @return Newly allocated bumped version text, or empty string for null/overflow.
rt_string rt_version_bump_minor(void *ver) {
    if (!ver)
        return rt_string_from_bytes("", 0);
    rt_version_impl *v = (rt_version_impl *)ver;
    if (v->minor == INT64_MAX) {
        rt_trap("Version.BumpMinor: component overflow");
        return rt_string_from_bytes("", 0);
    }
    rt_string_builder sb;
    rt_sb_init(&sb);
    check_sb(rt_sb_append_int(&sb, v->major), "Version.BumpMinor: formatting failed");
    check_sb(rt_sb_append_cstr(&sb, "."), "Version.BumpMinor: formatting failed");
    check_sb(rt_sb_append_int(&sb, v->minor + 1), "Version.BumpMinor: formatting failed");
    check_sb(rt_sb_append_cstr(&sb, ".0"), "Version.BumpMinor: formatting failed");
    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

/// @brief Return a new version string with the patch component incremented.
/// @details Prerelease and build metadata are discarded. Component overflow traps.
/// @param ver Borrowed Version object.
/// @return Newly allocated bumped version text, or empty string for null/overflow.
rt_string rt_version_bump_patch(void *ver) {
    if (!ver)
        return rt_string_from_bytes("", 0);
    rt_version_impl *v = (rt_version_impl *)ver;
    if (v->patch == INT64_MAX) {
        rt_trap("Version.BumpPatch: component overflow");
        return rt_string_from_bytes("", 0);
    }
    rt_string_builder sb;
    rt_sb_init(&sb);
    check_sb(rt_sb_append_int(&sb, v->major), "Version.BumpPatch: formatting failed");
    check_sb(rt_sb_append_cstr(&sb, "."), "Version.BumpPatch: formatting failed");
    check_sb(rt_sb_append_int(&sb, v->minor), "Version.BumpPatch: formatting failed");
    check_sb(rt_sb_append_cstr(&sb, "."), "Version.BumpPatch: formatting failed");
    check_sb(rt_sb_append_int(&sb, v->patch + 1), "Version.BumpPatch: formatting failed");
    rt_string result = rt_string_from_bytes(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}
