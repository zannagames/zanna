//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_markdown.c
// Purpose: Implements Markdown parsing utilities for the Zanna.Text.Markdown
//          class. Provides ExtractLinks (URLs), ExtractHeadings (heading text),
//          ToHtml (basic Markdown to HTML conversion), and
//          ToText (remove formatting, return plain text).
//
// Key invariants:
//   - ExtractLinks returns a Seq<String> of URL targets, with unsafe schemes
//     rewritten to "#".
//   - ExtractHeadings returns a Seq<String> of heading text.
//   - ToHtml converts headings, bold, italic, links, code, and lists; does not
//     implement the full CommonMark spec.
//   - ToText removes matched emphasis/code delimiters and link destinations;
//     unmatched markers and intraword underscores remain literal.
//   - Empty input returns empty strings/sequences; whitespace-only input is
//     kept as content (e.g. ToHtml(" ") emits a paragraph containing a space).
//
// Ownership/Lifetime:
//   - All returned sequences and strings are fresh allocations owned by caller.
//   - Input strings are borrowed for the duration of the call.
//
// Links: src/runtime/text/rt_markdown.h (public API),
//        src/runtime/text/rt_html.h (HTML generation for ToHtml output)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_markdown.c
 * @brief Implements a safe line-oriented Markdown subset.
 * @details The renderer recognizes selected headings, emphasis, code, links,
 *          lists, fences, and horizontal rules, escapes user content, and
 *          rewrites unsafe URL schemes. Companion scanners produce plain text,
 *          copied link destinations, and ATX heading snapshots.
 */

#include "rt_markdown.h"

#include "rt_internal.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/// @brief Decode a small HTML entity into one character for URL scheme checks.
/// @details Handles numeric decimal/hex entities and `&colon;`, which are the
///          encodings relevant to obfuscated URL schemes. Returns zero when no
///          complete supported entity starts at @p s.
/// @param s Entity start, expected to point at `&`.
/// @param len Remaining input length.
/// @param consumed Receives bytes consumed when decoding succeeds.
/// @return Decoded ASCII character, or zero when unsupported/incomplete.
static char markdown_decode_entity_char(const char *s, int64_t len, int64_t *consumed) {
    if (consumed)
        *consumed = 0;
    if (!s || len < 4 || s[0] != '&')
        return 0;
    if (len >= 7 && strncmp(s, "&colon;", 7) == 0) {
        if (consumed)
            *consumed = 7;
        return ':';
    }
    if (s[1] != '#')
        return 0;
    int64_t i = 2;
    int base = 10;
    if (i < len && (s[i] == 'x' || s[i] == 'X')) {
        base = 16;
        i++;
    }
    int value = 0;
    int digits = 0;
    for (; i < len && s[i] != ';'; i++) {
        int d = -1;
        if (s[i] >= '0' && s[i] <= '9')
            d = s[i] - '0';
        else if (base == 16 && s[i] >= 'a' && s[i] <= 'f')
            d = s[i] - 'a' + 10;
        else if (base == 16 && s[i] >= 'A' && s[i] <= 'F')
            d = s[i] - 'A' + 10;
        else
            return 0;
        if (d >= base || value > 0x7F)
            return 0;
        value = value * base + d;
        digits++;
    }
    if (digits == 0 || i >= len || s[i] != ';' || value <= 0 || value > 0x7F)
        return 0;
    if (consumed)
        *consumed = i + 1;
    return (char)value;
}

/// @brief Detect URL schemes that could execute script (javascript:, data:, vbscript:).
/// @details Used by the link/image handlers to block XSS via Markdown
///          links of the form `[click](javascript:alert(1))`. The
///          comparison normalizes case, leading/control whitespace, and
///          simple HTML entity obfuscation before matching. Matched schemes
///          get rewritten to `#` so the link is rendered but inert.
///          (Tracking ID: S-13.)
/// @param url Borrowed URL byte range.
/// @param len Number of bytes in @p url.
/// @return `true` for normalized `javascript:`, `data:`, or `vbscript:`.
static bool url_scheme_is_blocked(const char *url, int64_t len) {
    static const struct {
        const char *scheme;
        int scheme_len;
    } blocked[] = {{"javascript:", 11}, {"data:", 5}, {"vbscript:", 9}};

    while (len > 0 && (unsigned char)*url <= 0x20) {
        url++;
        len--;
    }

    char normalized[16];
    int64_t out_len = 0;
    for (int64_t i = 0; i < len && out_len < (int64_t)sizeof(normalized) - 1; i++) {
        unsigned char raw = (unsigned char)url[i];
        char c = (char)raw;
        if (raw <= 0x20)
            continue;
        if (c == '&') {
            int64_t consumed = 0;
            char decoded = markdown_decode_entity_char(url + i, len - i, &consumed);
            if (decoded) {
                c = decoded;
                i += consumed - 1;
            }
        }
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        normalized[out_len++] = c;
        if (c == ':')
            break;
    }
    normalized[out_len] = '\0';

    for (int s = 0; s < 3; s++) {
        int sl = blocked[s].scheme_len;
        if (out_len >= sl) {
            bool match = true;
            for (int i = 0; i < sl; i++) {
                char c = normalized[i];
                if (c != blocked[s].scheme[i]) {
                    match = false;
                    break;
                }
            }
            if (match)
                return true;
        }
    }
    return false;
}

// --- Helper: append escaped HTML ---

/// @brief Convert a string-builder error into the common Markdown trap.
/// @details A failed builder is freed before trapping. Callers rely on the
///          runtime trap being terminating and must not reuse it after failure.
/// @param sb Builder associated with @p status.
/// @param status Most recent builder-operation result.
static void markdown_check_sb(rt_string_builder *sb, rt_sb_status_t status) {
    if (status == RT_SB_OK)
        return;
    rt_sb_free(sb);
    rt_trap("Markdown: string builder allocation failed");
}

/// @brief Copy a byte range to a runtime string or raise a Markdown trap.
/// @param bytes Borrowed byte range accepted by `rt_string_from_bytes`.
/// @param len Number of bytes to copy.
/// @return Newly allocated runtime string, or null only if the trap hook returns.
static rt_string markdown_string_from_bytes_or_trap(const char *bytes, size_t len) {
    rt_string result = rt_string_from_bytes(bytes, len);
    if (!result)
        rt_trap("Markdown: string allocation failed");
    return result;
}

/// @brief Write one character to `sb`, escaping the four HTML metacharacters.
/// @details Maps `<`/`>`/`&`/`"` to their named entities; passes
///          everything else through verbatim. The single-quote `'`
///          is intentionally not escaped because it never causes
///          problems inside double-quoted attributes (the only
///          context we emit user content into here).
/// @param sb Initialized destination builder.
/// @param c Source byte to escape and append.
static void append_escaped(rt_string_builder *sb, char c) {
    switch (c) {
        case '<':
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "&lt;"));
            break;
        case '>':
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "&gt;"));
            break;
        case '&':
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "&amp;"));
            break;
        case '"':
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "&quot;"));
            break;
        default:
            markdown_check_sb(sb, rt_sb_append_bytes(sb, &c, 1));
            break;
    }
}

/// @brief Recognize an ATX heading marker at the exact start of a line.
/// @details Accepts one through six `#` bytes followed by one literal space.
///          Leading indentation and seven or more markers are not headings.
/// @param line First byte of the line.
/// @param eol One-past-the-end line pointer.
/// @return Heading level from 1 through 6, or zero when not recognized.
static int markdown_heading_level(const char *line, const char *eol) {
    int level = 0;
    const char *p = line;
    while (p < eol && *p == '#') {
        level++;
        p++;
    }
    if (level < 1 || level > 6)
        return 0;
    return (p < eol && *p == ' ') ? level : 0;
}

// --- Helper: process inline formatting ---

/// @brief Convert one Markdown line's inline span syntax to HTML, appending into `sb`.
/// @details Handles four span constructs in priority order:
///          1. `**bold**`     → `<strong>...</strong>`
///          2. `*italic*`     → `<em>...</em>` (skipped if followed by `*`
///             so `**` always wins as bold).
///          3. `` `code` ``   → `<code>...</code>`
///          4. `[text](url)`  → `<a href="url">text</a>`, with the URL
///             scheme passed through `url_scheme_is_blocked`.
///          All literal characters route through `append_escaped` so
///          attacker-controlled `<`/`>`/`&` can't break out into raw
///          HTML. Unmatched closing markers (e.g. `*foo` with no
///          closing `*`) just emit the leading marker as a literal.
/// @param sb Initialized destination builder.
/// @param line Borrowed inline-content byte range.
/// @param len Number of bytes at @p line.
static void process_inline(rt_string_builder *sb, const char *line, int64_t len) {
    int64_t i = 0;
    while (i < len) {
        // Bold **text**
        if (i + 1 < len && line[i] == '*' && line[i + 1] == '*') {
            int64_t close = i + 2;
            while (close + 1 < len && !(line[close] == '*' && line[close + 1] == '*'))
                close++;
            if (close + 1 >= len) {
                append_escaped(sb, line[i++]);
                append_escaped(sb, line[i++]);
                continue;
            }
            i += 2;
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "<strong>"));
            while (i < close) {
                append_escaped(sb, line[i]);
                i++;
            }
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "</strong>"));
            i += 2;
            continue;
        }

        // Italic *text*
        if (line[i] == '*' && (i + 1 < len) && line[i + 1] != '*') {
            int64_t close = i + 1;
            while (close < len && line[close] != '*')
                close++;
            if (close >= len) {
                append_escaped(sb, line[i++]);
                continue;
            }
            i++;
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "<em>"));
            while (i < close) {
                append_escaped(sb, line[i]);
                i++;
            }
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "</em>"));
            i++;
            continue;
        }

        // Inline code `code`
        if (line[i] == '`') {
            int64_t close = i + 1;
            while (close < len && line[close] != '`')
                close++;
            if (close >= len) {
                append_escaped(sb, line[i++]);
                continue;
            }
            i++;
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "<code>"));
            while (i < close) {
                append_escaped(sb, line[i]);
                i++;
            }
            markdown_check_sb(sb, rt_sb_append_cstr(sb, "</code>"));
            i++;
            continue;
        }

        // Link [text](url)
        if (line[i] == '[') {
            int64_t text_start = i + 1;
            int64_t j = text_start;
            while (j < len && line[j] != ']')
                j++;
            if (j + 1 < len && line[j] == ']' && line[j + 1] == '(') {
                int64_t url_start = j + 2;
                int64_t k = url_start;
                while (k < len && line[k] != ')')
                    k++;
                if (k < len) {
                    markdown_check_sb(sb, rt_sb_append_cstr(sb, "<a href=\""));
                    /* S-13: Block unsafe URL schemes to prevent XSS */
                    int64_t url_len = k - url_start;
                    if (url_scheme_is_blocked(line + url_start, url_len))
                        markdown_check_sb(sb, rt_sb_append_cstr(sb, "#"));
                    else {
                        for (int64_t m = url_start; m < k; m++)
                            append_escaped(sb, line[m]);
                    }
                    markdown_check_sb(sb, rt_sb_append_cstr(sb, "\">"));
                    for (int64_t m = text_start; m < j; m++)
                        append_escaped(sb, line[m]);
                    markdown_check_sb(sb, rt_sb_append_cstr(sb, "</a>"));
                    i = k + 1;
                    continue;
                }
            }
        }

        append_escaped(sb, line[i]);
        i++;
    }
}

// --- Public API ---

/// @brief Return 1 when a line is a horizontal rule: three or more of the same
///        marker (-, *, _) separated only by spaces. Checked before list-item
///        recognition so `* * *` and `- - -` render as <hr>, not list items
///        (VDOC-049).
/// @param p First byte of the line.
/// @param line_len Number of bytes in the line.
/// @return Non-zero for a recognized horizontal-rule line.
static int markdown_line_is_hr(const char *p, int64_t line_len) {
    if (line_len < 3)
        return 0;
    char hr_char = *p;
    if (hr_char != '-' && hr_char != '*' && hr_char != '_')
        return 0;
    int count = 0;
    for (int64_t i = 0; i < line_len; i++) {
        if (p[i] == hr_char)
            count++;
        else if (p[i] != ' ')
            return 0;
    }
    return count >= 3;
}

/// @brief Convert Markdown text to HTML.
/// @details Implements a deliberately small line-oriented subset: ATX
///          headings, horizontal rules, `-`/`*` unordered lists, fenced code,
///          one paragraph per other non-empty line, plus inline bold, italic,
///          code, and links. Literal content is HTML-escaped and unsafe URL
///          schemes are rewritten to `#`. Blank lines close lists but emit no
///          markup. This is not a CommonMark parser.
/// @param md Borrowed Markdown source; null is treated as empty.
/// @return Newly allocated HTML string owned by the caller.
rt_string rt_markdown_to_html(rt_string md) {
    if (!md)
        return rt_string_from_bytes("", 0);

    const char *src = rt_string_cstr(md);
    int64_t src_len = rt_str_len(md);
    if (!src || src_len < 0)
        return rt_string_from_bytes("", 0);
    rt_string_builder sb;
    rt_sb_init(&sb);

    const char *p = src;
    int in_list = 0;

    const char *end = src + src_len;

    while (p < end) {
        // Find end of line
        const char *eol = p;
        while (eol < end && *eol != '\n')
            eol++;
        const char *line_break = eol;
        // CRLF input: exclude a trailing CR from line content (VDOC-050).
        if (eol > p && eol[-1] == '\r')
            eol--;
        int64_t line_len = (int64_t)(eol - p);

        // Empty line
        if (line_len == 0) {
            if (in_list) {
                markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "</ul>\n"));
                in_list = 0;
            }
            p = line_break < end ? line_break + 1 : end;
            continue;
        }

        // Classify the line once so an open list closes before ANY non-list
        // block (heading, code fence, rule, paragraph) — otherwise those
        // elements land inside the <ul> (VDOC-049).
        const int is_hr = markdown_line_is_hr(p, line_len);
        const int is_list_item =
            !is_hr && (*p == '-' || *p == '*') && p + 1 < eol && p[1] == ' ';
        if (in_list && !is_list_item) {
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "</ul>\n"));
            in_list = 0;
        }

        // Horizontal rule (before list items: `* * *` is a rule, not a list)
        if (is_hr) {
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "<hr>\n"));
            p = line_break < end ? line_break + 1 : end;
            continue;
        }

        // Heading
        int heading_level = markdown_heading_level(p, eol);
        if (heading_level > 0) {
            int level = heading_level;
            const char *h = p + level + 1;
            char tag[8];
            snprintf(tag, sizeof(tag), "<h%d>", level);
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, tag));
            process_inline(&sb, h, (int64_t)(eol - h));
            snprintf(tag, sizeof(tag), "</h%d>\n", level);
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, tag));
            p = line_break < end ? line_break + 1 : end;
            continue;
        }

        // Unordered list item (- or *)
        if (is_list_item) {
            if (!in_list) {
                markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "<ul>\n"));
                in_list = 1;
            }
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "<li>"));
            process_inline(&sb, p + 2, (int64_t)(eol - p - 2));
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "</li>\n"));
            p = line_break < end ? line_break + 1 : end;
            continue;
        }

        // Code block ```
        if (line_len >= 3 && p[0] == '`' && p[1] == '`' && p[2] == '`') {
            p = line_break < end ? line_break + 1 : end;
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "<pre><code>"));
            while (p < end) {
                eol = p;
                while (eol < end && *eol != '\n')
                    eol++;
                const char *fence_break = eol;
                if (eol > p && eol[-1] == '\r')
                    eol--;
                line_len = (int64_t)(eol - p);
                if (line_len >= 3 && p[0] == '`' && p[1] == '`' && p[2] == '`') {
                    p = fence_break < end ? fence_break + 1 : end;
                    break;
                }
                for (int64_t i = 0; i < line_len; i++)
                    append_escaped(&sb, p[i]);
                markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "\n"));
                p = fence_break < end ? fence_break + 1 : end;
            }
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "</code></pre>\n"));
            continue;
        }

        // Regular paragraph
        markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "<p>"));
        process_inline(&sb, p, line_len);
        markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "</p>\n"));
        p = line_break < end ? line_break + 1 : end;
    }

    if (in_list)
        markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "</ul>\n"));

    rt_string result = markdown_string_from_bytes_or_trap(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

/// @brief Convert Markdown text to plain text (strip all formatting).
/// @details Removes recognized heading prefixes, matched `*`, `_`, and
///          backtick spans, and Markdown link destinations while preserving
///          link labels. Unmatched markers and intraword underscores remain
///          literal. Line boundaries are preserved; list markers and code-fence
///          markers are not treated specially.
/// @param md Borrowed Markdown source; null is treated as empty.
/// @return Newly allocated plain-text string owned by the caller.
rt_string rt_markdown_to_text(rt_string md) {
    if (!md)
        return rt_string_from_bytes("", 0);

    const char *src = rt_string_cstr(md);
    int64_t src_len = rt_str_len(md);
    if (!src || src_len < 0)
        return rt_string_from_bytes("", 0);
    rt_string_builder sb;
    rt_sb_init(&sb);

    const char *p = src;
    const char *end = src + src_len;
    int first_line = 1;

    while (p < end) {
        const char *eol = p;
        while (eol < end && *eol != '\n')
            eol++;
        const char *line_break = eol;
        // CRLF input: exclude a trailing CR from line content (VDOC-050).
        if (eol > p && eol[-1] == '\r')
            eol--;

        if (!first_line)
            markdown_check_sb(&sb, rt_sb_append_cstr(&sb, "\n"));
        first_line = 0;

        // Skip heading markers
        const char *start = p;
        int heading_level = markdown_heading_level(p, eol);
        if (heading_level > 0)
            start = p + heading_level + 1;

        // Strip inline formatting. Marker bytes are dropped only when they
        // form a matched pair on this line (mirrors process_inline); an
        // unmatched marker stays literal. Underscores additionally follow the
        // intraword rule: `_` flanked by alphanumerics (snake_case) never
        // opens or closes emphasis, so identifiers survive (VDOC-051).
        for (const char *c = start; c < eol; c++) {
            if (*c == '*' || *c == '`') {
                const char *close = c + 1;
                while (close < eol && *close != *c)
                    close++;
                if (close >= eol) {
                    markdown_check_sb(&sb, rt_sb_append_bytes(&sb, c, 1));
                    continue;
                }
                if (close > c + 1)
                    markdown_check_sb(&sb, rt_sb_append_bytes(&sb, c + 1, close - (c + 1)));
                c = close;
                continue;
            }
            if (*c == '_') {
                int open_intraword = (c > start && isalnum((unsigned char)c[-1])) &&
                                     (c + 1 < eol && isalnum((unsigned char)c[1]));
                const char *close = eol;
                if (!open_intraword) {
                    close = c + 1;
                    while (close < eol) {
                        int close_intraword =
                            isalnum((unsigned char)close[-1]) &&
                            (close + 1 < eol && isalnum((unsigned char)close[1]));
                        if (*close == '_' && !close_intraword)
                            break;
                        close++;
                    }
                }
                if (close >= eol) {
                    markdown_check_sb(&sb, rt_sb_append_bytes(&sb, c, 1));
                    continue;
                }
                if (close > c + 1)
                    markdown_check_sb(&sb, rt_sb_append_bytes(&sb, c + 1, close - (c + 1)));
                c = close;
                continue;
            }
            if (*c == '[') {
                // Extract link text only
                const char *link_text_end = c + 1;
                while (link_text_end < eol && *link_text_end != ']')
                    link_text_end++;
                if (link_text_end >= eol || *link_text_end != ']') {
                    markdown_check_sb(&sb, rt_sb_append_bytes(&sb, c, 1));
                    continue;
                }
                for (const char *t = c + 1; t < link_text_end; t++)
                    markdown_check_sb(&sb, rt_sb_append_bytes(&sb, t, 1));
                // Skip URL part
                if ((eol - link_text_end) > 1 && link_text_end[1] == '(') {
                    const char *close = link_text_end + 2;
                    while (close < eol && *close != ')')
                        close++;
                    c = close;
                } else {
                    c = link_text_end;
                }
                continue;
            }
            markdown_check_sb(&sb, rt_sb_append_bytes(&sb, c, 1));
        }
        p = line_break < end ? line_break + 1 : end;
    }

    rt_string result = markdown_string_from_bytes_or_trap(sb.data, sb.len);
    rt_sb_free(&sb);
    return result;
}

/// @brief Extract link destinations from simple inline Markdown links.
/// @details Recognizes non-nested `[label](url)` byte patterns, copies only the
///          URL, and rewrites blocked script-capable schemes to `"#"`. Images,
///          reference links, escaped delimiters, and nested parentheses are not
///          implemented.
/// @param md Borrowed Markdown source; null is treated as empty.
/// @return Newly allocated element-owning Seq of copied URL strings.
void *rt_markdown_extract_links(rt_string md) {
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    if (!md)
        return seq;

    const char *src = rt_string_cstr(md);
    int64_t src_len = rt_str_len(md);
    if (!src || src_len < 0)
        return seq;
    const char *p = src;
    const char *end_src = src + src_len;

    while (p < end_src) {
        // Find [text](url) pattern
        if (*p == '[') {
            const char *end = p + 1;
            while (end < end_src && *end != ']')
                end++;
            if (end < end_src && *end == ']' && end + 1 < end_src && end[1] == '(') {
                const char *url_start = end + 2;
                const char *url_end = url_start;
                while (url_end < end_src && *url_end != ')')
                    url_end++;
                if (url_end < end_src && *url_end == ')') {
                    int64_t url_len = (int64_t)(url_end - url_start);
                    rt_string url =
                        url_scheme_is_blocked(url_start, url_len)
                            ? markdown_string_from_bytes_or_trap("#", 1)
                            : markdown_string_from_bytes_or_trap(url_start, (size_t)url_len);
                    rt_seq_push(seq, url);
                    rt_string_unref(url);
                    p = url_end + 1;
                    continue;
                }
            }
        }
        p++;
    }
    return seq;
}

/// @brief Extract raw text following recognized ATX heading markers.
/// @details A heading must start at column zero with one through six `#` bytes
///          followed by a literal space. Returned content retains inline
///          Markdown markers and trims only a CR from CRLF line endings.
/// @param md Borrowed Markdown source; null is treated as empty.
/// @return Newly allocated element-owning Seq of copied heading strings.
void *rt_markdown_extract_headings(rt_string md) {
    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    if (!md)
        return seq;

    const char *src = rt_string_cstr(md);
    int64_t src_len = rt_str_len(md);
    if (!src || src_len < 0)
        return seq;
    const char *p = src;
    const char *end_src = src + src_len;

    while (p < end_src) {
        // Beginning of line (or start of string)
        if (p == src || p[-1] == '\n') {
            const char *line_end = p;
            while (line_end < end_src && *line_end != '\n')
                line_end++;
            int level = markdown_heading_level(p, line_end);
            if (level > 0) {
                const char *h = p + level + 1;
                const char *eol = h;
                while (eol < end_src && *eol != '\n')
                    eol++;
                const char *content_end = eol;
                if (content_end > h && content_end[-1] == '\r')
                    content_end--;
                rt_string heading =
                    markdown_string_from_bytes_or_trap(h, (size_t)(content_end - h));
                rt_seq_push(seq, heading);
                rt_string_unref(heading);
                p = eol;
                if (p < end_src)
                    p++;
                continue;
            }
        }
        p++;
    }
    return seq;
}
