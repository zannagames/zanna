//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_csv.c
// Purpose: Implements CSV parsing and formatting for the Zanna.Data.Csv class,
//          following RFC 4180 quoting rules. Handles quoted fields, doubled-quote
//          escaping, and CRLF/LF/CR line endings. Supports a configurable
//          single-byte field delimiter. Deviations from strict RFC 4180:
//          multi-row Format emits LF (not CRLF) row endings, unequal per-row
//          field counts are accepted, and LF/CR line endings are parsed as
//          extensions.
//
// Key invariants:
//   - Fields containing the delimiter, a double-quote, or a newline must be
//     quoted; double-quotes within are escaped by doubling ("").
//   - ParseLine returns a Seq<String> for one row; Parse returns Seq<Seq<String>>.
//   - FormatLine quotes fields that require quoting; FormatAll formats all rows.
//   - Empty lines produce a single-element row containing an empty string.
//   - The default delimiter is comma; alternative delimiters (e.g. tab) are
//     accepted at construction time.
//   - All functions are thread-safe with no global mutable state.
//
// Ownership/Lifetime:
//   - All returned Seq and String objects are fresh allocations owned by caller.
//   - Input strings are borrowed read-only for the duration of the call.
//
// Links: src/runtime/text/rt_csv.h (public API),
//        src/runtime/rt_seq.h (Seq container used for rows and fields)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_csv.c
 * @brief Implements RFC 4180-style CSV parsing and formatting.
 * @details The parser handles quoted fields, doubled quotes, embedded line
 *          endings, CRLF/LF/CR record separators, and validated one-byte
 *          delimiters. Formatting quotes fields only when required and builds
 *          caller-owned element-owning sequences and Strings transactionally.
 */

#include "rt_csv.h"

#include "rt_box.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_seq_internal.h"
#include "rt_string.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// Default CSV delimiter.
#define DEFAULT_DELIMITER ','

/// @brief Release a local runtime object reference if it drops to zero.
/// @details This helper is used on parser failure paths after a value has been
///          created locally but should not be returned to the caller.
/// @param obj Runtime object pointer or NULL.
static void release_local_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Convert a CSV field value to an owned string safely.
/// @details Raw runtime strings are retained, boxed strings are unboxed, and other boxed/runtime
///          values use the shared object stringification path. This avoids interpreting arbitrary
///          object payloads as rt_string handles.
/// @param val Runtime field value; NULL produces NULL.
/// @param owned Optional flag cleared initially and set when the returned
///        non-NULL string reference must be released by the caller.
/// @return Retained or newly stringified runtime string, or NULL for @p val NULL.
static rt_string csv_value_to_string(void *val, bool *owned) {
    if (owned)
        *owned = false;
    if (!val)
        return NULL;
    if (rt_string_is_handle(val)) {
        if (owned)
            *owned = true;
        return rt_string_ref((rt_string)val);
    }
    if (owned)
        *owned = true;
    return rt_obj_to_string(val);
}

/// @brief Get delimiter character from string.
/// @details NULL and empty strings select comma. Other values must be exactly
///          one non-NUL byte and cannot be quote, CR, or LF; invalid values trap
///          and return comma as the recovery fallback.
/// @param delim Optional runtime delimiter string.
/// @return Validated delimiter byte or DEFAULT_DELIMITER.
static char get_delim(rt_string delim) {
    int64_t len = delim ? rt_str_len(delim) : 0;
    if (!delim || len <= 0)
        return DEFAULT_DELIMITER;
    const char *s = rt_string_cstr(delim);
    if (!s || len != 1 || memchr(s, '\0', (size_t)len)) {
        rt_trap("Csv: delimiter must be exactly one byte");
        return DEFAULT_DELIMITER;
    }
    char d = s[0];
    if (d == '\0' || d == '"' || d == '\r' || d == '\n') {
        rt_trap("Csv: invalid delimiter");
        return DEFAULT_DELIMITER;
    }
    return d;
}

/// @brief Test whether a runtime object is a Seq with the expected payload size.
/// @param obj Candidate runtime object.
/// @return True for a valid Seq instance, otherwise false.
static bool csv_is_seq(void *obj) {
    return rt_obj_is_instance(obj, RT_SEQ_CLASS_ID, sizeof(rt_seq_impl)) != 0;
}

/// @brief Add to an output-size accumulator with overflow trapping.
/// @param total Mutable accumulated size.
/// @param add Increment to add.
/// @param op Trap diagnostic on overflow.
/// @return True after updating @p total, or false after an overflow trap.
static bool csv_checked_add(size_t *total, size_t add, const char *op) {
    if (*total > SIZE_MAX - add) {
        rt_trap(op);
        return false;
    }
    *total += add;
    return true;
}

/// @brief Check if field needs quoting for CSV output.
/// @param field Borrowed field bytes.
/// @param len Field byte count.
/// @param delim Active delimiter byte.
/// @return True when the field contains delimiter, quote, CR, or LF.
static bool needs_quoting(const char *field, size_t len, char delim) {
    for (size_t i = 0; i < len; i++) {
        char c = field[i];
        if (c == delim || c == '"' || c == '\n' || c == '\r')
            return true;
    }
    return false;
}

//=============================================================================
// Parsing Implementation
//=============================================================================

/// @brief Parse state for RFC 4180 CSV parsing.
typedef struct {
    const char *input;   ///< Input string.
    size_t len;          ///< Total length.
    size_t pos;          ///< Current position.
    char delim;          ///< Delimiter character.
    bool has_error;      ///< True after a malformed record or allocation failure.
    const char *message; ///< Static diagnostic describing the first parse error.
} csv_parser;

/// @brief Initialize parser state.
/// @param p Parser structure to initialize.
/// @param input Borrowed CSV input bytes.
/// @param len Input byte count.
/// @param delim Validated single-byte delimiter.
static void parser_init(csv_parser *p, const char *input, size_t len, char delim) {
    p->input = input;
    p->len = len;
    p->pos = 0;
    p->delim = delim;
    p->has_error = false;
    p->message = NULL;
}

/// @brief Record a CSV parse failure before invoking the runtime trap hook.
/// @details Runtime trap hooks can return in recovery-oriented tests and
///          embedders. Recording the failure first lets callers stop parsing
///          instead of interpreting the fallback return value as a real field.
/// @param p Parser state to mark as failed.
/// @param message Static diagnostic to expose to the caller and trap hook.
static void csv_parse_error(csv_parser *p, const char *message) {
    if (p) {
        p->has_error = true;
        p->message = message;
        p->pos = p->len;
    }
    rt_trap(message);
}

/// @brief Check if parser is at end of input.
/// @param p Parser state.
/// @return True when the cursor is at or beyond the input length.
static bool parser_eof(csv_parser *p) {
    return p->pos >= p->len;
}

/// @brief Peek current character without advancing.
/// @param p Parser state.
/// @return Current byte, or NUL at end-of-input.
static char parser_peek(csv_parser *p) {
    if (p->pos >= p->len)
        return '\0';
    return p->input[p->pos];
}

/// @brief Consume current character and advance.
/// @param p Parser state.
/// @return Consumed byte, or NUL without advancing at end-of-input.
static char parser_consume(csv_parser *p) {
    if (p->pos >= p->len)
        return '\0';
    return p->input[p->pos++];
}

/// @brief Parse a single CSV field, handling RFC 4180 quoting and line endings.
/// @details Two distinct paths based on the leading character:
///          1. **Quoted field** (starts with `"`): consume content
///             until the closing quote, treating `""` as an escaped
///             literal `"`. Newlines inside a quoted field are
///             preserved (multi-line records are RFC-legal).
///          2. **Unquoted field**: read raw bytes until the next
///             delimiter or line terminator — no escape handling.
///          After the field content is captured, the parser steps over
///          one trailing terminator: a delimiter (still in row), `\r`,
///          `\n`, or `\r\n` (row done — sets `*at_line_end = true`).
///          EOF mid-field always counts as line-end.
///          Quoted-field buffer grows geometrically starting at 64;
///          unquoted-field returns a slice of the input directly with
///          no extra alloc.
/// @param p Parser state.
/// @param at_line_end Output: set to true if field ends at line boundary.
/// @return Newly allocated field string.
static rt_string parse_field(csv_parser *p, bool *at_line_end) {
    *at_line_end = false;

    // EOF case - return empty field and signal end of line
    if (parser_eof(p)) {
        *at_line_end = true;
        return rt_string_from_bytes("", 0);
    }

    // Check for quoted field
    if (parser_peek(p) == '"') {
        parser_consume(p); // consume opening quote

        // Build field content with escaped quotes handled
        size_t cap = 64;
        size_t len = 0;
        char *buf = (char *)malloc(cap);
        if (!buf) {
            csv_parse_error(p, "Csv.Parse: memory allocation failed");
            return NULL;
        }

        bool closed = false;
        while (!parser_eof(p)) {
            char c = parser_consume(p);

            if (c == '"') {
                // Check for escaped quote
                if (parser_peek(p) == '"') {
                    // Escaped quote - consume and add single quote
                    parser_consume(p);
                    if (len + 1 >= cap) {
                        if (cap > SIZE_MAX / 2) {
                            free(buf);
                            csv_parse_error(p, "Csv.Parse: field length overflow");
                            return NULL;
                        }
                        cap *= 2;
                        char *tmp = (char *)realloc(buf, cap);
                        if (!tmp) {
                            free(buf);
                            csv_parse_error(p, "Csv.Parse: memory allocation failed");
                            return NULL;
                        }
                        buf = tmp;
                    }
                    buf[len++] = '"';
                } else {
                    // End of quoted field
                    closed = true;
                    break;
                }
            } else {
                // Regular character (including newlines in quoted fields)
                if (len + 1 >= cap) {
                    if (cap > SIZE_MAX / 2) {
                        free(buf);
                        csv_parse_error(p, "Csv.Parse: field length overflow");
                        return NULL;
                    }
                    cap *= 2;
                    char *tmp = (char *)realloc(buf, cap);
                    if (!tmp) {
                        free(buf);
                        csv_parse_error(p, "Csv.Parse: memory allocation failed");
                        return NULL;
                    }
                    buf = tmp;
                }
                buf[len++] = c;
            }
        }

        if (!closed) {
            free(buf);
            csv_parse_error(p, "Csv.Parse: unterminated quoted field");
            return NULL;
        }

        buf[len] = '\0';
        rt_string result = rt_string_from_bytes(buf, len);
        free(buf);

        // Skip to delimiter or line end
        if (!parser_eof(p)) {
            char c = parser_peek(p);
            if (c == p->delim) {
                parser_consume(p);
            } else if (c == '\r') {
                parser_consume(p);
                if (parser_peek(p) == '\n')
                    parser_consume(p);
                *at_line_end = true;
            } else if (c == '\n') {
                parser_consume(p);
                *at_line_end = true;
            } else {
                rt_string_unref(result);
                csv_parse_error(p, "Csv.Parse: invalid character after closing quote");
                return NULL;
            }
        } else {
            *at_line_end = true;
        }

        return result;
    } else {
        // Unquoted field - read until delimiter or line end
        size_t start = p->pos;
        while (!parser_eof(p)) {
            char c = parser_peek(p);
            if (c == p->delim || c == '\r' || c == '\n')
                break;
            if (c == '"') {
                csv_parse_error(p, "Csv.Parse: quote in unquoted field");
                return NULL;
            }
            parser_consume(p);
        }
        size_t field_len = p->pos - start;
        rt_string result = rt_string_from_bytes(p->input + start, field_len);

        // Handle delimiter or line end
        if (!parser_eof(p)) {
            char c = parser_peek(p);
            if (c == p->delim) {
                parser_consume(p);
            } else if (c == '\r') {
                parser_consume(p);
                if (parser_peek(p) == '\n')
                    parser_consume(p);
                *at_line_end = true;
            } else if (c == '\n') {
                parser_consume(p);
                *at_line_end = true;
            }
        } else {
            *at_line_end = true;
        }

        return result;
    }
}

/// @brief Parse a single row (line) of CSV.
/// @details Always attempts at least one field, so an empty record produces one
///          empty string. On a field error, returns the partially built
///          element-owning row with @c p->has_error set for the outer parser to
///          discard.
/// @param p Parser positioned at the start of a row.
/// @return Caller-owned Seq of field strings, possibly partial after a recorded
///         parse failure, or NULL when row allocation fails.
static void *parse_row(csv_parser *p) {
    void *row = rt_seq_new();
    if (!row) {
        rt_trap("Csv.Parse: memory allocation failed");
        return NULL;
    }
    rt_seq_set_owns_elements(row, 1);
    bool at_line_end = false;

    // Use do-while to ensure we process trailing empty fields after delimiter
    do {
        rt_string field = parse_field(p, &at_line_end);
        if (!field) {
            if (!p->has_error)
                csv_parse_error(p, "Csv.Parse: memory allocation failed");
            return row;
        }
        rt_seq_push(row, (void *)field);
        if (field)
            rt_string_unref(field);
    } while (!at_line_end);

    return row;
}

/// @brief Validate and consume one CSV field without allocating its value.
/// @param p Parser positioned at a field start.
/// @param at_line_end Receives true when the field ends the current record.
/// @return True for valid quoting/termination syntax, otherwise false.
static bool validate_field(csv_parser *p, bool *at_line_end) {
    *at_line_end = false;

    if (parser_eof(p)) {
        *at_line_end = true;
        return true;
    }

    if (parser_peek(p) == '"') {
        parser_consume(p);
        bool closed = false;
        while (!parser_eof(p)) {
            char c = parser_consume(p);
            if (c == '"') {
                if (parser_peek(p) == '"') {
                    parser_consume(p);
                } else {
                    closed = true;
                    break;
                }
            }
        }
        if (!closed)
            return false;

        if (!parser_eof(p)) {
            char c = parser_peek(p);
            if (c == p->delim) {
                parser_consume(p);
            } else if (c == '\r') {
                parser_consume(p);
                if (parser_peek(p) == '\n')
                    parser_consume(p);
                *at_line_end = true;
            } else if (c == '\n') {
                parser_consume(p);
                *at_line_end = true;
            } else {
                return false;
            }
        } else {
            *at_line_end = true;
        }
        return true;
    }

    while (!parser_eof(p)) {
        char c = parser_peek(p);
        if (c == p->delim || c == '\r' || c == '\n')
            break;
        if (c == '"')
            return false;
        parser_consume(p);
    }

    if (!parser_eof(p)) {
        char c = parser_peek(p);
        if (c == p->delim) {
            parser_consume(p);
        } else if (c == '\r') {
            parser_consume(p);
            if (parser_peek(p) == '\n')
                parser_consume(p);
            *at_line_end = true;
        } else if (c == '\n') {
            parser_consume(p);
            *at_line_end = true;
        }
    } else {
        *at_line_end = true;
    }
    return true;
}

/// @brief Validate and consume one complete CSV row.
/// @param p Parser positioned at a row start.
/// @return True when every field through the row terminator is valid.
static bool validate_row(csv_parser *p) {
    bool at_line_end = false;
    do {
        if (!validate_field(p, &at_line_end))
            return false;
    } while (!at_line_end);
    return true;
}

//=============================================================================
// Formatting Implementation
//=============================================================================

/// @brief Format a single field for CSV output.
/// @param field Field string.
/// @param field_len Field byte count.
/// @param delim Delimiter character.
/// @param out Output buffer (must have enough space).
/// @return Number of bytes written.
static size_t format_field(const char *field, size_t field_len, char delim, char *out) {
    if (!needs_quoting(field, field_len, delim)) {
        // No quoting needed
        memcpy(out, field, field_len);
        return field_len;
    }

    // Need quoting
    size_t o = 0;
    out[o++] = '"';
    for (size_t i = 0; i < field_len; i++) {
        char c = field[i];
        if (c == '"') {
            out[o++] = '"';
            out[o++] = '"';
        } else {
            out[o++] = c;
        }
    }
    out[o++] = '"';
    return o;
}

/// @brief Calculate output size for a formatted field.
/// @param field Borrowed field bytes.
/// @param field_len Field byte count.
/// @param delim Active delimiter byte.
/// @return Exact formatted byte count, or SIZE_MAX after an overflow trap.
static size_t calc_field_size(const char *field, size_t field_len, char delim) {
    if (!needs_quoting(field, field_len, delim))
        return field_len;

    // 2 for quotes + escaped quotes
    size_t size = 2;
    for (size_t i = 0; i < field_len; i++) {
        size_t add = (field[i] == '"') ? 2 : 1;
        if (size > SIZE_MAX - add) {
            rt_trap("Csv.Format: output length overflow");
            return SIZE_MAX;
        }
        size += add;
    }
    return size;
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Parses a single line of CSV into a Seq of strings.
///
/// Parses one CSV record (line) using comma as the default delimiter. The
/// result is a Seq where each element is a string representing one field.
///
/// **Parsing rules:**
/// - Commas separate fields
/// - Quoted fields (`"..."`) preserve commas and can contain newlines
/// - Doubled quotes (`""`) within quoted fields become single quotes
///
/// **Example:**
/// ```
/// Dim fields = Csv.ParseLine("Alice,30,NYC")
/// ' fields = Seq ["Alice", "30", "NYC"]
///
/// Dim fields2 = Csv.ParseLine("name,""age"",""New York, NY""")
/// ' fields2 = Seq ["name", "age", "New York, NY"]
/// ```
///
/// @param line The CSV line to parse.
///
/// @return A caller-owned Seq containing parsed field strings, an empty Seq for
///         NULL input, or NULL after a parse/allocation failure.
///
/// @note Uses comma (`,`) as the delimiter. For other delimiters, use
///       rt_csv_parse_line_with.
/// @note O(n) time complexity where n is the line length.
///
/// @see rt_csv_parse_line_with For custom delimiters
/// @see rt_csv_parse For parsing multiple lines
/// @see rt_csv_format_line For the inverse operation
void *rt_csv_parse_line(rt_string line) {
    return rt_csv_parse_line_with(line, NULL);
}

/// @brief Parses a single line of CSV with a custom delimiter.
///
/// Parses one CSV record using the specified delimiter character. The first
/// character of the delimiter string is used; if empty, defaults to comma.
///
/// **Common delimiters:**
/// | Delimiter | Description           | Use Case              |
/// |-----------|-----------------------|-----------------------|
/// | `,`       | Comma (default)       | Standard CSV          |
/// | `\t`      | Tab                   | TSV files             |
/// | `;`       | Semicolon             | European CSV          |
/// | `|`       | Pipe                  | Log files             |
///
/// **Example:**
/// ```
/// ' Tab-separated values
/// Dim fields = Csv.ParseLineWith("Alice\t30\tNYC", "\t")
/// ' fields = Seq ["Alice", "30", "NYC"]
///
/// ' Semicolon-separated (European format)
/// Dim fields2 = Csv.ParseLineWith("name;age;city", ";")
/// ```
///
/// @param line The CSV line to parse.
/// @param delim Exactly one delimiter byte. Empty or NULL selects comma; longer
///              values and quote/CR/LF/NUL delimiters trap.
///
/// @return A caller-owned Seq containing parsed field strings, an empty Seq for
///         NULL input, or NULL after a parse/allocation failure.
///
/// @note The delimiter is validated as exactly one permitted byte.
/// @note O(n) time complexity where n is the line length.
///
/// @see rt_csv_parse_line For the default comma delimiter
/// @see rt_csv_parse_with For parsing multiple lines
void *rt_csv_parse_line_with(rt_string line, rt_string delim) {
    if (!line)
        return rt_seq_new();

    const char *input = rt_string_cstr(line);
    size_t len = (size_t)rt_str_len(line);
    char d = get_delim(delim);

    csv_parser p;
    parser_init(&p, input, len, d);

    void *row = parse_row(&p);
    if (p.has_error) {
        release_local_obj(row);
        return NULL;
    }
    if (!parser_eof(&p))
        csv_parse_error(&p, "Csv.ParseLine: expected a single CSV record");
    if (p.has_error) {
        release_local_obj(row);
        return NULL;
    }
    return row;
}

/// @brief Parses multi-line CSV text into a Seq of Seqs.
///
/// Parses complete CSV content containing multiple rows, using comma as the
/// default delimiter. The result is a Seq where each element is itself a Seq
/// of strings representing one row.
///
/// **Example:**
/// ```
/// Dim csv = "name,age,city" & vbLf & "Alice,30,NYC" & vbLf & "Bob,25,LA"
/// Dim rows = Csv.Parse(csv)
///
/// ' rows = Seq [
/// '   Seq ["name", "age", "city"],
/// '   Seq ["Alice", "30", "NYC"],
/// '   Seq ["Bob", "25", "LA"]
/// ' ]
///
/// For Each row In rows
///     For Each field In row
///         Print field & " | ";
///     Next
///     Print
/// Next
/// ```
///
/// **Line ending handling:**
/// - LF (`\n`): Unix/Linux/macOS
/// - CR (`\r`): Classic Mac
/// - CRLF (`\r\n`): Windows
///
/// @param text The CSV text containing one or more rows.
///
/// @return A caller-owned Seq of element-owning row Seqs, an empty Seq for NULL
///         or empty input, or NULL after a parse/allocation failure.
///
/// @note Uses comma (`,`) as the delimiter. For other delimiters, use
///       rt_csv_parse_with.
/// @note O(n) time complexity where n is the total text length.
///
/// @see rt_csv_parse_with For custom delimiters
/// @see rt_csv_parse_line For parsing a single line
/// @see rt_csv_format For the inverse operation
void *rt_csv_parse(rt_string text) {
    return rt_csv_parse_with(text, NULL);
}

/// @brief Parses multi-line CSV text with a custom delimiter.
///
/// Parses complete CSV content containing multiple rows, using the specified
/// delimiter character. The result is a Seq where each element is itself a
/// Seq of strings representing one row.
///
/// **Example:**
/// ```
/// ' Tab-separated values file
/// Dim tsv = "name\tage\tcity" & vbLf & "Alice\t30\tNYC"
/// Dim rows = Csv.ParseWith(tsv, "\t")
///
/// ' rows = Seq [
/// '   Seq ["name", "age", "city"],
/// '   Seq ["Alice", "30", "NYC"]
/// ' ]
/// ```
///
/// @param text The CSV text containing one or more rows.
/// @param delim Exactly one delimiter byte. Empty or NULL selects comma; longer
///              values and quote/CR/LF/NUL delimiters trap.
///
/// @return A caller-owned Seq of element-owning row Seqs, an empty Seq for NULL
///         or empty input, or NULL after a parse/allocation failure.
///
/// @note The delimiter is validated as exactly one permitted byte.
/// @note O(n) time complexity where n is the total text length.
///
/// @see rt_csv_parse For the default comma delimiter
/// @see rt_csv_parse_line_with For parsing a single line
void *rt_csv_parse_with(rt_string text, rt_string delim) {
    if (!text)
        return rt_seq_new();

    const char *input = rt_string_cstr(text);
    size_t len = (size_t)rt_str_len(text);
    if (len == 0)
        return rt_seq_new();

    char d = get_delim(delim);

    csv_parser p;
    parser_init(&p, input, len, d);

    void *rows = rt_seq_new();
    if (!rows)
        return NULL;
    rt_seq_set_owns_elements(rows, 1);

    while (!parser_eof(&p)) {
        void *row = parse_row(&p);
        if (p.has_error) {
            release_local_obj(row);
            release_local_obj(rows);
            return NULL;
        }
        rt_seq_push(rows, row);
        if (row && rt_obj_release_check0(row))
            rt_obj_free(row);
    }

    return rows;
}

/// @brief Validate comma-delimited CSV syntax without constructing rows.
/// @details Accepts CRLF, LF, and CR record endings and the same quoted-field
///          grammar as the parser. A NULL handle is invalid, while empty text is
///          a valid empty document.
/// @param text CSV text to inspect.
/// @return 1 when syntactically valid, otherwise 0.
int8_t rt_csv_is_valid(rt_string text) {
    if (!text)
        return 0;

    const char *input = rt_string_cstr(text);
    size_t len = (size_t)rt_str_len(text);
    if (len == 0)
        return 1;

    csv_parser p;
    parser_init(&p, input, len, DEFAULT_DELIMITER);
    while (!parser_eof(&p)) {
        if (!validate_row(&p))
            return 0;
    }
    return 1;
}

/// @brief Formats a Seq of strings as a single CSV line.
///
/// Converts a sequence of field strings into a properly formatted CSV line.
/// Fields containing special characters (commas, quotes, newlines) are
/// automatically quoted, and internal quotes are escaped.
///
/// **Automatic quoting:**
/// A field is quoted if it contains:
/// - The delimiter character (`,`)
/// - Double-quote (`"`)
/// - Newline (`\n`) or carriage return (`\r`)
///
/// **Example:**
/// ```
/// Dim fields = Seq.Of("Alice", "30", "New York, NY")
/// Dim line = Csv.FormatLine(fields)
/// ' line = "Alice,30,\"New York, NY\""
///
/// Dim fields2 = Seq.Of("say \"hi\"", "normal", "with\nnewline")
/// Dim line2 = Csv.FormatLine(fields2)
/// ' line2 = "\"say \"\"hi\"\"\",normal,\"with\nnewline\""
/// ```
///
/// @param fields A Seq of strings to format as CSV.
///
/// @return A CSV-formatted string representing one row. Returns an empty
///         string if fields is NULL or empty.
///
/// @note Uses comma (`,`) as the delimiter. For other delimiters, use
///       rt_csv_format_line_with.
/// @note The returned string does NOT include a trailing newline.
/// @note O(n) time complexity where n is total character count.
///
/// @see rt_csv_format_line_with For custom delimiters
/// @see rt_csv_format For formatting multiple rows
/// @see rt_csv_parse_line For the inverse operation
rt_string rt_csv_format_line(void *fields) {
    return rt_csv_format_line_with(fields, NULL);
}

/// @brief Formats a Seq of strings as a CSV line with custom delimiter.
///
/// Converts a sequence of field strings into a properly formatted CSV line
/// using the specified delimiter. Fields containing special characters are
/// automatically quoted.
///
/// **Example:**
/// ```
/// ' Create tab-separated output
/// Dim fields = Seq.Of("Alice", "30", "NYC")
/// Dim line = Csv.FormatLineWith(fields, "\t")
/// ' line = "Alice\t30\tNYC"
///
/// ' Semicolon-separated (European format)
/// Dim line2 = Csv.FormatLineWith(Seq.Of("1.5", "2.5"), ";")
/// ' line2 = "1.5;2.5"
/// ```
///
/// @param fields A Seq of strings to format as CSV.
/// @param delim Exactly one delimiter byte. Empty or NULL selects comma; longer
///              values and quote/CR/LF/NUL delimiters trap.
///
/// @return A CSV-formatted string representing one row. Returns an empty
///         string if fields is NULL or empty.
///
/// @note The delimiter is validated as exactly one permitted byte.
/// @note The returned string does NOT include a trailing newline.
/// @note O(n) time complexity where n is total character count.
///
/// @see rt_csv_format_line For the default comma delimiter
/// @see rt_csv_format_with For formatting multiple rows
rt_string rt_csv_format_line_with(void *fields, rt_string delim) {
    if (!fields)
        return rt_string_from_bytes("", 0);
    if (!csv_is_seq(fields)) {
        rt_trap("Csv.FormatLine: invalid fields");
        return rt_string_from_bytes("", 0);
    }

    char d = get_delim(delim);
    int64_t count = rt_seq_len(fields);

    if (count == 0)
        return rt_string_from_bytes("", 0);

    // Calculate total output size
    size_t total_size = 0;
    for (int64_t i = 0; i < count; i++) {
        bool owned = false;
        rt_string field = csv_value_to_string(rt_seq_get(fields, i), &owned);
        const char *str = field ? rt_string_cstr(field) : "";
        size_t field_len = field ? (size_t)rt_str_len(field) : 0;
        size_t field_size = calc_field_size(str, field_len, d);
        if (field_size == SIZE_MAX ||
            !csv_checked_add(
                &total_size, field_size, "Csv.FormatLine: output length overflow")) {
            if (owned)
                rt_string_unref(field);
            return rt_string_from_bytes("", 0);
        }
        if (i < count - 1 &&
            !csv_checked_add(&total_size, 1, "Csv.FormatLine: output length overflow")) {
            if (owned)
                rt_string_unref(field);
            return rt_string_from_bytes("", 0);
        }
        if (owned)
            rt_string_unref(field);
    }
    if (total_size == SIZE_MAX) {
        rt_trap("Csv.FormatLine: output length overflow");
        return rt_string_from_bytes("", 0);
    }

    char *out = (char *)malloc(total_size + 1);
    if (!out) {
        rt_trap("Csv.FormatLine: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    size_t pos = 0;
    for (int64_t i = 0; i < count; i++) {
        bool owned = false;
        rt_string field = csv_value_to_string(rt_seq_get(fields, i), &owned);
        const char *str = field ? rt_string_cstr(field) : "";
        size_t field_len = field ? (size_t)rt_str_len(field) : 0;
        pos += format_field(str, field_len, d, out + pos);
        if (i < count - 1)
            out[pos++] = d;
        if (owned)
            rt_string_unref(field);
    }
    out[pos] = '\0';

    rt_string result = rt_string_from_bytes(out, pos);
    free(out);
    return result;
}

/// @brief Formats a Seq of Seqs as complete CSV text.
///
/// Converts a two-dimensional structure (rows of fields) into properly
/// formatted CSV text. Each row becomes a line in the output, with rows
/// separated by newline characters.
///
/// **Example:**
/// ```
/// Dim rows = Seq.New()
/// rows.Push(Seq.Of("name", "age", "city"))
/// rows.Push(Seq.Of("Alice", "30", "NYC"))
/// rows.Push(Seq.Of("Bob", "25", "LA"))
///
/// Dim csv = Csv.Format(rows)
/// ' csv = "name,age,city\nAlice,30,NYC\nBob,25,LA\n"
///
/// ' Write to file
/// Dim writer = LineWriter.Open("data.csv")
/// writer.Write(csv)
/// writer.Close()
/// ```
///
/// @param rows A Seq of Seqs, where each inner Seq contains the fields of
///             one row.
///
/// @return Complete CSV text with rows separated by newlines. Returns an
///         empty string if rows is NULL or empty.
///
/// @note Uses comma (`,`) as the delimiter. For other delimiters, use
///       rt_csv_format_with.
/// @note Each row ends with a newline character (`\n`).
/// @note O(n) time complexity where n is total character count.
///
/// @see rt_csv_format_with For custom delimiters
/// @see rt_csv_format_line For formatting a single row
/// @see rt_csv_parse For the inverse operation
rt_string rt_csv_format(void *rows) {
    return rt_csv_format_with(rows, NULL);
}

/// @brief Formats a Seq of Seqs as CSV text with custom delimiter.
///
/// Converts a two-dimensional structure (rows of fields) into properly
/// formatted CSV text using the specified delimiter.
///
/// **Example:**
/// ```
/// ' Create tab-separated values file
/// Dim rows = Seq.New()
/// rows.Push(Seq.Of("name", "age", "city"))
/// rows.Push(Seq.Of("Alice", "30", "NYC"))
///
/// Dim tsv = Csv.FormatWith(rows, "\t")
/// ' tsv = "name\tage\tcity\nAlice\t30\tNYC\n"
///
/// ' Create semicolon-separated file (European format)
/// Dim euCsv = Csv.FormatWith(rows, ";")
/// ' euCsv = "name;age;city\nAlice;30;NYC\n"
/// ```
///
/// @param rows A Seq of Seqs, where each inner Seq contains the fields of
///             one row.
/// @param delim Exactly one delimiter byte. Empty or NULL selects comma; longer
///              values and quote/CR/LF/NUL delimiters trap.
///
/// @return Complete CSV text with rows separated by newlines. Returns an
///         empty string if rows is NULL or empty.
///
/// @note The delimiter is validated as exactly one permitted byte.
/// @note Each row ends with a newline character (`\n`).
/// @note O(n) time complexity where n is total character count.
///
/// @see rt_csv_format For the default comma delimiter
/// @see rt_csv_format_line_with For formatting a single row
/// @see rt_csv_parse_with For the inverse operation
rt_string rt_csv_format_with(void *rows, rt_string delim) {
    if (!rows)
        return rt_string_from_bytes("", 0);
    if (!csv_is_seq(rows)) {
        rt_trap("Csv.Format: invalid rows");
        return rt_string_from_bytes("", 0);
    }

    char d = get_delim(delim);
    int64_t row_count = rt_seq_len(rows);

    if (row_count == 0)
        return rt_string_from_bytes("", 0);

    // Calculate total output size
    size_t total_size = 0;
    for (int64_t r = 0; r < row_count; r++) {
        void *row = rt_seq_get(rows, r);
        if (!row)
            continue;
        if (!csv_is_seq(row)) {
            rt_trap("Csv.Format: invalid row");
            return rt_string_from_bytes("", 0);
        }

        int64_t count = rt_seq_len(row);
        for (int64_t i = 0; i < count; i++) {
            bool owned = false;
            rt_string field = csv_value_to_string(rt_seq_get(row, i), &owned);
            const char *str = field ? rt_string_cstr(field) : "";
            size_t field_len = field ? (size_t)rt_str_len(field) : 0;
            size_t field_size = calc_field_size(str, field_len, d);
            if (field_size == SIZE_MAX ||
                !csv_checked_add(&total_size, field_size, "Csv.Format: output length overflow")) {
                if (owned)
                    rt_string_unref(field);
                return rt_string_from_bytes("", 0);
            }
            if (i < count - 1 &&
                !csv_checked_add(&total_size, 1, "Csv.Format: output length overflow")) {
                if (owned)
                    rt_string_unref(field);
                return rt_string_from_bytes("", 0);
            }
            if (owned)
                rt_string_unref(field);
        }
        if (!csv_checked_add(&total_size, 1, "Csv.Format: output length overflow"))
            return rt_string_from_bytes("", 0);
    }
    if (total_size == SIZE_MAX) {
        rt_trap("Csv.Format: output length overflow");
        return rt_string_from_bytes("", 0);
    }

    char *out = (char *)malloc(total_size + 1);
    if (!out) {
        rt_trap("Csv.Format: memory allocation failed");
        return rt_string_from_bytes("", 0);
    }

    size_t pos = 0;
    for (int64_t r = 0; r < row_count; r++) {
        void *row = rt_seq_get(rows, r);
        if (!row) {
            out[pos++] = '\n';
            continue;
        }
        if (!csv_is_seq(row)) {
            free(out);
            rt_trap("Csv.Format: invalid row");
            return rt_string_from_bytes("", 0);
        }

        int64_t count = rt_seq_len(row);
        for (int64_t i = 0; i < count; i++) {
            bool owned = false;
            rt_string field = csv_value_to_string(rt_seq_get(row, i), &owned);
            const char *str = field ? rt_string_cstr(field) : "";
            size_t field_len = field ? (size_t)rt_str_len(field) : 0;
            pos += format_field(str, field_len, d, out + pos);
            if (i < count - 1)
                out[pos++] = d;
            if (owned)
                rt_string_unref(field);
        }
        out[pos++] = '\n';
    }
    out[pos] = '\0';

    rt_string result = rt_string_from_bytes(out, pos);
    free(out);
    return result;
}
