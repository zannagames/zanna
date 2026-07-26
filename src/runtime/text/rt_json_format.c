//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE in the project root for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_json_format.c
// Purpose: JSON serialization and value-type classification for the
//          Zanna.Data.Json class per RFC 8259 §7. Walks a Zanna value tree
//          (Map/Seq/String/boxed primitives) and emits compact or
//          pretty-printed JSON text.
//
// Key invariants:
//   - Format produces compact JSON (no whitespace); FormatPretty indents.
//   - NaN and Infinity serialize as `null` (JSON has no representation for them).
//   - Cyclic object graphs trap; nesting beyond JSON_MAX_DEPTH traps.
//   - No global mutable state; fully thread-safe.
//
// Ownership/Lifetime:
//   - Formatted JSON strings are fresh rt_string allocations owned by caller.
//   - The scratch string_builder buffer is freed by sb_finish.
//
// Links: src/runtime/text/rt_json.h (public API),
//        src/runtime/text/rt_json_internal.h (JSON_MAX_DEPTH),
//        src/runtime/text/rt_json_parse.c (inverse: JSON text → value)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_json_format.c
 * @brief Implements JSON serialization and managed-value type classification.
 * @details The serializer walks Maps, sequences, runtime Strings, and boxed
 *          primitives, escapes RFC 8259 text, formats finite numbers
 *          locale-independently, supports compact and indented output, and
 *          rejects cyclic or excessively nested object graphs.
 */

#include "rt_json.h"

#include "rt_box.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_json_internal.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <locale.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <xlocale.h>
#endif

/// Minimum payload size required before treating an object as a Seq.
#define JSON_SEQ_MIN_PAYLOAD (sizeof(int64_t) * 2 + sizeof(void *) + sizeof(int8_t))
/// Minimum payload size required before treating an object as a Map.
#define JSON_MAP_MIN_PAYLOAD (sizeof(void *) * 2 + sizeof(size_t) * 2)

//=============================================================================
// String Formatting Helpers
//=============================================================================

/// Growable, NUL-terminated scratch buffer for serialized JSON bytes.
typedef struct {
    char *buf;  ///< Heap allocation containing accumulated bytes.
    size_t len; ///< Number of content bytes, excluding the terminator.
    size_t cap; ///< Total allocated capacity in bytes.
    int failed; ///< Sticky allocation, overflow, or graph-error flag.
} string_builder;

/// Active-container stack used for cycle and nesting-depth detection.
typedef struct {
    void **items; ///< Borrowed Map/Seq pointers on the current recursion path.
    size_t len;   ///< Number of active entries.
    size_t cap;   ///< Allocated pointer capacity.
    int failed;   ///< Sticky graph/depth/allocation error flag.
} format_context;

// ---------------------------------------------------------------------------
// `string_builder` — a simple growing-buffer used to assemble the
// JSON output. Doubles capacity on overflow; the final `sb_finish`
// converts it into an `rt_string` and frees the scratch buffer.
// ---------------------------------------------------------------------------

/// @brief Initialize an empty builder with a 256-byte scratch allocation.
/// @details Allocation failure marks the builder failed and raises a runtime
///          trap, allowing later append helpers to become no-ops.
/// @param sb Writable builder state.
static void sb_init(string_builder *sb) {
    sb->cap = 256;
    sb->len = 0;
    sb->failed = 0;
    sb->buf = (char *)malloc(sb->cap);
    if (!sb->buf) {
        sb->failed = 1;
        rt_trap("Json.Format: memory allocation failed");
        return;
    }
    sb->buf[0] = '\0';
}

/// @brief Ensure the builder has room for at least `needed` more bytes; doubles capacity.
/// @details Detects length/capacity overflow. Reallocation failure releases the
///          old buffer, marks the builder failed, and raises a trap.
/// @param sb Initialized builder to grow.
/// @param needed Additional capacity required, including any requested
///               terminator reserve.
static void sb_grow(string_builder *sb, size_t needed) {
    if (sb->failed)
        return;
    if (needed > SIZE_MAX - sb->len) {
        sb->failed = 1;
        rt_trap("Json.Format: output length overflow");
        return;
    }
    size_t required = sb->len + needed;
    if (required < sb->cap)
        return;

    while (sb->cap <= required) {
        if (sb->cap > SIZE_MAX / 2) {
            sb->failed = 1;
            rt_trap("Json.Format: output length overflow");
            return;
        }
        sb->cap *= 2;
    }

    char *tmp = (char *)realloc(sb->buf, sb->cap);
    if (!tmp) {
        free(sb->buf);
        sb->buf = NULL;
        sb->len = 0;
        sb->cap = 0;
        sb->failed = 1;
        rt_trap("Json.Format: memory allocation failed");
        return;
    }
    sb->buf = tmp;
}

/// @brief Append a NUL-terminated C string to the builder.
/// @param sb Initialized destination builder.
/// @param s Borrowed non-null NUL-terminated source.
static void sb_append(string_builder *sb, const char *s) {
    if (sb->failed)
        return;
    size_t slen = strlen(s);
    sb_grow(sb, slen + 1);
    if (sb->failed)
        return;
    memcpy(sb->buf + sb->len, s, slen);
    sb->len += slen;
    sb->buf[sb->len] = '\0';
}

/// @brief Append a single byte to the builder.
/// @param sb Initialized destination builder.
/// @param c Byte to append before the maintained NUL terminator.
static void sb_append_char(string_builder *sb, char c) {
    if (sb->failed)
        return;
    sb_grow(sb, 2);
    if (sb->failed)
        return;
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

/// @brief Append `indent * level` literal spaces (pretty-print indentation).
/// @details Non-positive indentation performs no work. Negative levels and
///          multiplication overflow mark the builder failed and trap.
/// @param sb Initialized destination builder.
/// @param indent Spaces per nesting level.
/// @param level Current non-negative nesting level.
static void sb_append_indent(string_builder *sb, int64_t indent, int64_t level) {
    if (sb->failed)
        return;
    if (indent <= 0)
        return;
    if (level < 0 || (level > 0 && indent > INT64_MAX / level)) {
        sb->failed = 1;
        rt_trap("Json.Format: indentation overflow");
        return;
    }

    int64_t spaces = indent * level;
    sb_grow(sb, (size_t)spaces + 1);
    if (sb->failed)
        return;
    for (int64_t i = 0; i < spaces; i++)
        sb->buf[sb->len++] = ' ';
    sb->buf[sb->len] = '\0';
}

/// @brief Convert the builder's contents to an `rt_string` and free the scratch buffer.
/// @details A failed or missing buffer produces an owned empty fallback string.
/// @param sb Builder to consume; its scratch buffer is invalidated.
/// @return Newly allocated runtime string containing the accumulated bytes, or
///         an empty fallback after an earlier failure.
static rt_string sb_finish(string_builder *sb) {
    if (sb->failed || !sb->buf) {
        free(sb->buf);
        sb->buf = NULL;
        return rt_const_cstr("");
    }
    rt_string result = rt_string_from_bytes(sb->buf, sb->len);
    free(sb->buf);
    return result;
}

/// @brief Format text with C-locale numeric punctuation.
/// @details Uses `_vsnprintf_l` on Windows and a temporary thread-local locale
///          on POSIX, preventing the process locale from introducing a decimal
///          comma into JSON numbers.
/// @param buffer Writable output buffer.
/// @param size Total capacity of @p buffer.
/// @param fmt Borrowed printf-compatible format string.
/// @param args Variadic arguments matching @p fmt.
/// @return Number of formatted bytes as reported by the platform function, or
///         `-1` when arguments or locale setup are invalid.
static int json_vsnprintf_c_locale(char *buffer, size_t size, const char *fmt, va_list args) {
    if (!buffer || size == 0 || !fmt)
        return -1;

#if defined(_WIN32)
    _locale_t c_locale = _create_locale(LC_NUMERIC, "C");
    if (!c_locale)
        return -1;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int written = _vsnprintf_l(buffer, size, fmt, c_locale, args);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    _free_locale(c_locale);
    return written;
#else
    locale_t c_locale = newlocale(LC_NUMERIC_MASK, "C", (locale_t)0);
    if (!c_locale)
        return -1;
    locale_t previous = uselocale(c_locale);
    if (previous == (locale_t)0) {
        freelocale(c_locale);
        return -1;
    }
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
    int written = vsnprintf(buffer, size, fmt, args);
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
    uselocale(previous);
    freelocale(c_locale);
    return written;
#endif
}

/// @brief Variadic wrapper around `json_vsnprintf_c_locale`.
/// @param buffer Writable output buffer.
/// @param size Total capacity of @p buffer.
/// @param fmt Borrowed printf-compatible format string followed by its values.
/// @return Number of formatted bytes, or `-1` on failure.
static int json_snprintf_c_locale(char *buffer, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int written = json_vsnprintf_c_locale(buffer, size, fmt, args);
    va_end(args);
    return written;
}

/// @brief Initialize an empty serialization recursion context.
/// @param ctx Writable context state.
static void format_ctx_init(format_context *ctx) {
    ctx->items = NULL;
    ctx->len = 0;
    ctx->cap = 0;
    ctx->failed = 0;
}

/// @brief Release a recursion context's pointer stack and reset its fields.
/// @param ctx Context to invalidate.
static void format_ctx_free(format_context *ctx) {
    free(ctx->items);
    ctx->items = NULL;
    ctx->len = 0;
    ctx->cap = 0;
    ctx->failed = 0;
}

/// @brief Push a container onto the active serialization path.
/// @details Rejects repeated active pointers as cycles, enforces
///          `JSON_MAX_DEPTH`, and grows the pointer stack geometrically. Shared
///          acyclic children are permitted once a prior path has exited.
/// @param ctx Active recursion context.
/// @param obj Borrowed Map or Seq pointer to enter; null is a no-op.
/// @return `1` when entered or null, otherwise `0` after trapping and setting
///         the sticky failure flag.
static int format_ctx_enter(format_context *ctx, void *obj) {
    if (ctx->failed)
        return 0;
    if (!obj)
        return 1;
    for (size_t i = 0; i < ctx->len; i++) {
        if (ctx->items[i] == obj) {
            ctx->failed = 1;
            rt_trap("Json.Format: cyclic object graph");
            return 0;
        }
    }
    if (ctx->len >= JSON_MAX_DEPTH) {
        ctx->failed = 1;
        rt_trap("Json.Format: maximum nesting depth exceeded");
        return 0;
    }
    if (ctx->len == ctx->cap) {
        size_t new_cap = ctx->cap == 0 ? 16 : ctx->cap * 2;
        if (new_cap < ctx->cap || new_cap > SIZE_MAX / sizeof(void *)) {
            ctx->failed = 1;
            rt_trap("Json.Format: nesting stack overflow");
            return 0;
        }
        void **tmp = (void **)realloc(ctx->items, new_cap * sizeof(void *));
        if (!tmp) {
            ctx->failed = 1;
            rt_trap("Json.Format: memory allocation failed");
            return 0;
        }
        ctx->items = tmp;
        ctx->cap = new_cap;
    }
    ctx->items[ctx->len++] = obj;
    return 1;
}

/// @brief Remove a container from the active serialization path.
/// @details Normally pops the final entry; a defensive reverse search also
///          removes an out-of-order match while preserving the other entries.
/// @param ctx Active recursion context.
/// @param obj Borrowed container pointer to remove.
static void format_ctx_exit(format_context *ctx, void *obj) {
    if (!obj || ctx->len == 0)
        return;
    if (ctx->items[ctx->len - 1] == obj) {
        ctx->len--;
        return;
    }
    for (size_t i = ctx->len; i > 0; i--) {
        if (ctx->items[i - 1] == obj) {
            memmove(ctx->items + i - 1, ctx->items + i, (ctx->len - i) * sizeof(void *));
            ctx->len--;
            return;
        }
    }
}

//=============================================================================
// JSON String Escaping
//=============================================================================

/// @brief Emit `s` as a JSON string literal (with quotes and escapes).
///
/// Escapes per RFC 8259 §7: `\\`, `\"`, `\b`, `\f`, `\n`, `\r`,
/// `\t`. Other control characters (0x00-0x1F) become `\u00XX`.
/// Non-ASCII bytes pass through verbatim; callers are responsible for ensuring
/// they form valid UTF-8. A null string is emitted as an empty JSON string.
/// @param sb Initialized destination builder.
/// @param s Borrowed runtime string to quote and escape.
static void format_string(string_builder *sb, rt_string s) {
    sb_append_char(sb, '"');

    if (!s) {
        sb_append_char(sb, '"');
        return;
    }

    const char *str = rt_string_cstr(s);
    size_t len = (size_t)rt_str_len(s);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];

        switch (c) {
            case '"':
                sb_append(sb, "\\\"");
                break;
            case '\\':
                sb_append(sb, "\\\\");
                break;
            case '\b':
                sb_append(sb, "\\b");
                break;
            case '\f':
                sb_append(sb, "\\f");
                break;
            case '\n':
                sb_append(sb, "\\n");
                break;
            case '\r':
                sb_append(sb, "\\r");
                break;
            case '\t':
                sb_append(sb, "\\t");
                break;
            default:
                if (c < 0x20) {
                    // Escape control characters as \uXXXX
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    sb_append(sb, esc);
                } else {
                    sb_append_char(sb, (char)c);
                }
                break;
        }
    }

    sb_append_char(sb, '"');
}

//=============================================================================
// Value Formatting
//=============================================================================

/// @brief Forward declaration: serialise any Zanna value as JSON.
/// @param sb Destination builder.
/// @param obj Borrowed runtime value, or null for JSON null.
/// @param indent Spaces per level, or zero for compact output.
/// @param level Current nesting level.
/// @param ctx Active recursion context.
static void format_value(
    string_builder *sb, void *obj, int64_t indent, int64_t level, format_context *ctx);

/// @brief Emit a Seq as a JSON array, optionally pretty-printed.
///
/// `indent == 0` emits compactly (`[1,2,3]`); `indent > 0` puts
/// each element on its own line at `(level + 1) * indent` spaces.
/// @param sb Destination builder.
/// @param seq Borrowed runtime Seq.
/// @param indent Spaces per level, or zero for compact output.
/// @param level Current container nesting level.
/// @param ctx Active recursion context used for cycle/depth checks.
static void format_array(
    string_builder *sb, void *seq, int64_t indent, int64_t level, format_context *ctx) {
    if (!format_ctx_enter(ctx, seq)) {
        sb->failed = 1;
        return;
    }
    int64_t len = rt_seq_len(seq);

    if (len == 0) {
        sb_append(sb, "[]");
        format_ctx_exit(ctx, seq);
        return;
    }

    sb_append_char(sb, '[');
    if (indent > 0)
        sb_append_char(sb, '\n');

    for (int64_t i = 0; i < len; i++) {
        if (indent > 0)
            sb_append_indent(sb, indent, level + 1);

        void *item = rt_seq_get(seq, i);
        format_value(sb, item, indent, level + 1, ctx);
        if (sb->failed || ctx->failed)
            break;

        if (i < len - 1)
            sb_append_char(sb, ',');
        if (indent > 0)
            sb_append_char(sb, '\n');
    }

    if (indent > 0)
        sb_append_indent(sb, indent, level);
    sb_append_char(sb, ']');
    format_ctx_exit(ctx, seq);
}

/// @brief Emit a Map as a JSON object, optionally pretty-printed.
///
/// Iterates a copied key snapshot in the implementation-defined Map order.
/// Runtime Maps expose string keys, which are escaped with `format_string`.
/// @param sb Destination builder.
/// @param map Borrowed runtime Map.
/// @param indent Spaces per level, or zero for compact output.
/// @param level Current container nesting level.
/// @param ctx Active recursion context used for cycle/depth checks.
static void format_object(
    string_builder *sb, void *map, int64_t indent, int64_t level, format_context *ctx) {
    if (!format_ctx_enter(ctx, map)) {
        sb->failed = 1;
        return;
    }
    int64_t len = rt_map_len(map);

    if (len == 0) {
        sb_append(sb, "{}");
        format_ctx_exit(ctx, map);
        return;
    }

    sb_append_char(sb, '{');
    if (indent > 0)
        sb_append_char(sb, '\n');

    void *keys = rt_map_keys(map);
    int64_t keys_len = rt_seq_len(keys);

    for (int64_t i = 0; i < keys_len; i++) {
        if (indent > 0)
            sb_append_indent(sb, indent, level + 1);

        rt_string key = (rt_string)rt_seq_get(keys, i);
        format_string(sb, key);

        sb_append_char(sb, ':');
        if (indent > 0)
            sb_append_char(sb, ' ');

        void *value = rt_map_get(map, key);
        format_value(sb, value, indent, level + 1, ctx);
        if (sb->failed || ctx->failed)
            break;

        if (i < keys_len - 1)
            sb_append_char(sb, ',');
        if (indent > 0)
            sb_append_char(sb, '\n');
    }

    if (indent > 0)
        sb_append_indent(sb, indent, level);
    sb_append_char(sb, '}');
    if (keys && rt_obj_release_check0(keys))
        rt_obj_free(keys);
    format_ctx_exit(ctx, map);
}

/// @brief Recursive JSON emitter for any Zanna value.
///
/// Dispatches by type:
///   - NULL                  → `null`
///   - boxed bool/int/float  → `true`/`false`/`123`/`1.5`
///   - rt_string             → quoted JSON string
///   - Seq[Any]              → JSON array via `format_array`
///   - Map[Any,Any]          → JSON object via `format_object`
/// `indent == 0` produces compact output; positive values trigger
/// pretty printing with `indent` spaces per level. Non-finite F64 values,
/// unknown objects, and invalid float-formatting results become `null`.
/// @param sb Destination builder.
/// @param obj Borrowed runtime value, or null for JSON null.
/// @param indent Spaces per level, or zero for compact output.
/// @param level Current nesting level.
/// @param ctx Active recursion context.
static void format_value(
    string_builder *sb, void *obj, int64_t indent, int64_t level, format_context *ctx) {
    if (sb->failed || ctx->failed)
        return;

    // null
    if (!obj) {
        sb_append(sb, "null");
        return;
    }

    // Check if it's a string handle (most common case for strings)
    if (rt_string_is_handle(obj)) {
        format_string(sb, (rt_string)obj);
        return;
    }

    // Distinguish between boxes and collections using the heap header's
    // class_id field. Seq objects have RT_SEQ_CLASS_ID (2), Map objects have
    // RT_MAP_CLASS_ID (3), and boxed primitives have class_id 0.

    rt_heap_info_t heap_info;
    if (rt_heap_get_info(obj, &heap_info)) {
        // Check collection types by class_id first
        if (rt_obj_is_instance(obj, RT_SEQ_CLASS_ID, JSON_SEQ_MIN_PAYLOAD)) {
            format_array(sb, obj, indent, level, ctx);
            return;
        }

        if (rt_obj_is_instance(obj, RT_MAP_CLASS_ID, JSON_MAP_MIN_PAYLOAD)) {
            format_object(sb, obj, indent, level, ctx);
            return;
        }

        // Box (class_id == 0) - check its type tag
        int64_t box_type = rt_box_type(obj);

        if (box_type == RT_BOX_I64) {
            int64_t val = rt_unbox_i64(obj);
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", (long long)val);
            sb_append(sb, buf);
            return;
        }

        if (box_type == RT_BOX_F64) {
            double val = rt_unbox_f64(obj);
            if (isnan(val)) {
                sb_append(sb, "null");
                return;
            }
            if (isinf(val)) {
                sb_append(sb, "null");
                return;
            }
            char buf[64];
            int written = json_snprintf_c_locale(buf, sizeof(buf), "%.17g", val);
            if (written < 0 || (size_t)written >= sizeof(buf)) {
                sb_append(sb, "null");
                return;
            }
            sb_append(sb, buf);
            return;
        }

        if (box_type == RT_BOX_I1) {
            int8_t val = (int8_t)rt_unbox_i1(obj);
            sb_append(sb, val ? "true" : "false");
            return;
        }

        if (box_type == RT_BOX_STR) {
            rt_string str = rt_unbox_str(obj);
            format_string(sb, str);
            rt_string_unref(str);
            return;
        }
    }

    // Unknown or invalid object - format as null
    sb_append(sb, "null");
}

//=============================================================================
// Public API
//=============================================================================

/// @brief Formats a Zanna value as compact JSON.
///
/// Converts a Zanna value to its JSON representation without extra whitespace.
/// Produces minimal, single-line output suitable for APIs and storage.
///
/// **Type mappings:**
/// - Map -> JSON object
/// - Seq -> JSON array
/// - String -> JSON string
/// - Boxed f64 -> JSON number
/// - Boxed i64 -> JSON number (only boxed i1 formats as a JSON boolean)
/// - NULL -> JSON null
///
/// **Example:**
/// ```
/// Dim obj = Map.New()
/// obj.Set("name", "Alice")
/// obj.Set("age", Box.F64(30.0))
/// Print Json.Format(obj)
/// ' Output: {"name":"Alice","age":30}
/// ```
///
/// @param obj Borrowed Zanna value to format, or null for JSON null.
///
/// @return Newly allocated compact JSON string owned by the caller, or an empty
///         fallback after a trapped serialization failure.
///
/// @note O(n) time complexity where n is the total data size.
/// @note NaN and Infinity are formatted as null.
///
/// @see rt_json_format_pretty For human-readable output
/// @see rt_json_parse For the inverse operation
rt_string rt_json_format(void *obj) {
    string_builder sb;
    format_context ctx;
    sb_init(&sb);
    format_ctx_init(&ctx);
    format_value(&sb, obj, 0, 0, &ctx);
    if (ctx.failed)
        sb.failed = 1;
    format_ctx_free(&ctx);
    return sb_finish(&sb);
}

/// @brief Formats a Zanna value as pretty-printed JSON.
///
/// Converts a Zanna value to its JSON representation with indentation and
/// newlines for human readability. Each nesting level is indented by the
/// specified number of spaces.
///
/// **Example:**
/// ```
/// Dim obj = Map.New()
/// obj.Set("name", "Alice")
/// Dim items = Seq.New()
/// items.Push(Box.F64(1))
/// items.Push(Box.F64(2))
/// obj.Set("items", items)
/// Print Json.FormatPretty(obj, 2)
/// ' Output:
/// ' {
/// '   "name": "Alice",
/// '   "items": [
/// '     1,
/// '     2
/// '   ]
/// ' }
/// ```
///
/// @param obj Borrowed Zanna value to format, or null for JSON null.
/// @param indent Number of spaces per indentation level (typically 2 or 4).
///
/// @return Newly allocated pretty-printed JSON string owned by the caller, or
///         an empty fallback after a trapped serialization failure.
///
/// @note O(n) time complexity where n is the total data size.
/// @note If indent <= 0, behaves like rt_json_format (compact output).
///
/// @see rt_json_format For compact output
rt_string rt_json_format_pretty(void *obj, int64_t indent) {
    if (indent <= 0)
        return rt_json_format(obj);

    string_builder sb;
    format_context ctx;
    sb_init(&sb);
    format_ctx_init(&ctx);
    format_value(&sb, obj, indent, 0, &ctx);
    if (ctx.failed)
        sb.failed = 1;
    format_ctx_free(&ctx);
    return sb_finish(&sb);
}

/// @brief Gets the JSON type of a parsed value.
///
/// Returns a string describing the JSON type of a value. Useful for
/// type checking before accessing specific methods.
///
/// **Possible return values:**
/// - "null" - for NULL values
/// - "boolean" - for boxed i1 (boolean) values
/// - "number" - for boxed i64 and f64 values
/// - "string" - for String values
/// - "array" - for Seq values
/// - "object" - for Map values
/// - "unknown" - for every other runtime object
///
/// **Example:**
/// ```
/// Dim obj = Json.Parse("[1, \"hello\", null]")
/// Print Json.TypeOf(obj)           ' "array"
/// Print Json.TypeOf(obj.Get(0))    ' "number"
/// Print Json.TypeOf(obj.Get(1))    ' "string"
/// Print Json.TypeOf(obj.Get(2))    ' "null"
/// ```
///
/// @param obj Borrowed parsed or format-compatible runtime value.
///
/// @return Newly allocated string naming the classified JSON/runtime type.
rt_string rt_json_type_of(void *obj) {
    if (!obj)
        return rt_string_from_bytes("null", 4);

    if (rt_string_is_handle(obj))
        return rt_string_from_bytes("string", 6);

    // Use class_id to distinguish between collections and boxes
    rt_heap_info_t heap_info;
    if (rt_heap_get_info(obj, &heap_info)) {
        if (rt_obj_is_instance(obj, RT_SEQ_CLASS_ID, JSON_SEQ_MIN_PAYLOAD))
            return rt_string_from_bytes("array", 5);

        if (rt_obj_is_instance(obj, RT_MAP_CLASS_ID, JSON_MAP_MIN_PAYLOAD))
            return rt_string_from_bytes("object", 6);

        // Box (class_id == 0) - check type tag
        int64_t box_type = rt_box_type(obj);
        if (box_type == RT_BOX_I64 || box_type == RT_BOX_F64)
            return rt_string_from_bytes("number", 6);
        if (box_type == RT_BOX_I1)
            return rt_string_from_bytes("boolean", 7);
        if (box_type == RT_BOX_STR)
            return rt_string_from_bytes("string", 6);
    }

    return rt_string_from_bytes("unknown", 7);
}
