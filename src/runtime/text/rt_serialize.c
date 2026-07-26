//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/text/rt_serialize.c
// Purpose: Implements a unified serialization facade for the Zanna.Data.Serialize
//          class. Dispatches Parse/Format/Convert calls to format-specific
//          implementations based on the requested format enum.
//
// Key invariants:
//   - Supported formats: JSON, XML, YAML, TOML, and CSV.
//   - Unknown format enums return an empty string, null, zero, or -1 according
//     to the operation; parse/format operations record a thread-local error.
//   - Serialization produces a string; deserialization parses a string into
//     Zanna maps, sequences, XML nodes, strings, boxed primitives, or NULL.
//   - Cross-format conversion uses the native backends plus generic projections.
//   - Error state is thread-local.
//
// Ownership/Lifetime:
//   - Returned serialized strings, Results, and deserialized values are owned
//     by the caller according to their runtime type.
//   - Input strings are borrowed for the duration of the call.
//
// Links: src/runtime/text/rt_serialize.h (public API),
//        src/runtime/text/rt_json.h, rt_xml.h, rt_toml.h, rt_yaml.h (backends)
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_serialize.c
 * @brief Implements the unified multi-format serialization facade.
 * @details The module dispatches JSON, XML, YAML, TOML, and CSV parsing or
 *          formatting, converts through generic managed projections, detects
 *          likely source formats, captures thread-local diagnostics, and
 *          provides nullable and Result-returning failure surfaces.
 */

#include "rt_serialize.h"
#include "rt_format.h"

#include "rt_box.h"
#include "rt_csv.h"
#include "rt_heap.h"
#include "rt_internal.h"
#include "rt_json.h"
#include "rt_map.h"
#include "rt_object.h"
#include "rt_result.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_toml.h"
#include "rt_xml.h"
#include "rt_yaml.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

#include "rt_trap.h"

/// Minimum payload bytes read by Seq/Map public APIs during generic projection.
#define SERIALIZE_SEQ_MIN_PAYLOAD (sizeof(int64_t) * 2 + sizeof(void *) + sizeof(int8_t))
#define SERIALIZE_MAP_MIN_PAYLOAD (sizeof(void *) * 2 + sizeof(size_t) * 2)

/// Thread-local error message.
static _Thread_local rt_string g_last_error = NULL;

/// @brief Replace the thread-local error string with `msg` (refcounts the previous one off).
/// @details Stored per-thread (`_Thread_local`) so concurrent
///          serialize calls in different threads don't clobber each
///          other's diagnostics. Released via `clear_error` at the
///          start of every public entry point so stale errors from
///          a prior call don't leak across operations.
/// @param msg Required null-terminated diagnostic text to copy.
static void set_error(const char *msg) {
    if (g_last_error)
        rt_string_unref(g_last_error);
    g_last_error = rt_string_from_bytes(msg, strlen(msg));
}

/// @brief Drop the current thread-local error string (called at the start of each entry point).
static void clear_error(void) {
    if (g_last_error)
        rt_string_unref(g_last_error);
    g_last_error = NULL;
}

/// @brief Return 1 if the thread-local error string is currently non-empty.
/// @return Nonzero when the current thread has a nonempty serialization error.
static int has_error(void) {
    return g_last_error && rt_str_len(g_last_error) > 0;
}

/// @brief Set the thread-local error to `msg` if non-empty, else to `fallback`.
/// @param msg Borrowed runtime diagnostic string.
/// @param fallback Required null-terminated fallback text.
static void set_error_from_string(rt_string msg, const char *fallback) {
    if (g_last_error)
        rt_string_unref(g_last_error);
    if (msg && rt_str_len(msg) > 0)
        g_last_error = rt_string_ref(msg);
    else
        g_last_error = rt_string_from_bytes(fallback, strlen(fallback));
}

/// @brief Release a GC object reference, freeing it if the refcount drops to zero.
/// @param obj Runtime-managed object reference; null is ignored.
static void release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Convert the latest serialize parse attempt into a Result.
/// @details A NULL parse value is successful when the thread-local serialize
///          error is empty, preserving valid null-document semantics. Non-empty
///          error text becomes `Err(message)`.
/// @param value Parsed value returned by a serialize parse API.
/// @param fallback Fallback error message used only if an error has no text.
/// @return Owned `Zanna.Result` carrying @p value or an error string.
static void *serialize_parse_value_to_result(void *value, const char *fallback) {
    rt_string err = rt_serialize_error();
    if (!value && err && rt_str_len(err) > 0) {
        void *result = rt_result_err_str(err);
        rt_str_release_maybe(err);
        return result;
    }
    if (!value && has_error()) {
        rt_str_release_maybe(err);
        return rt_result_err_str(rt_const_cstr(fallback ? fallback : "Serialize parse failed"));
    }
    rt_str_release_maybe(err);

    void *result = rt_result_ok(value);
    release_obj(value);
    return result;
}

/// @brief Return 1 if `obj` is a runtime Seq container.
/// @param obj Borrowed candidate runtime object.
/// @return Nonzero when @p obj has the Seq class and minimum payload layout.
static int is_seq_obj(void *obj) {
    return rt_obj_is_instance(obj, RT_SEQ_CLASS_ID, SERIALIZE_SEQ_MIN_PAYLOAD);
}

/// @brief Return 1 if `obj` is a runtime Map container.
/// @param obj Borrowed candidate runtime object.
/// @return Nonzero when @p obj has the Map class and minimum payload layout.
static int is_map_obj(void *obj) {
    return rt_obj_is_instance(obj, RT_MAP_CLASS_ID, SERIALIZE_MAP_MIN_PAYLOAD);
}

/// @brief Allocate a fresh `rt_string` from the null-terminated C string `s`.
/// @param s Required null-terminated bytes to copy.
/// @return Newly allocated runtime string.
static rt_string make_cstr(const char *s) {
    return rt_string_from_bytes(s, strlen(s));
}

/// @brief Set a map key (given as a C literal) to `value`, releasing the temporary key string.
/// @param map Borrowed destination Map.
/// @param key Required null-terminated key text.
/// @param value Borrowed value passed to the Map setter.
static void map_set_cstr(void *map, const char *key, void *value) {
    rt_string k = make_cstr(key);
    rt_map_set(map, k, value);
    rt_string_unref(k);
}

/// @brief Return 1 if the `rt_string` `s` equals the null-terminated C string `cstr`.
/// @details Compares the full runtime byte length, so a string whose bytes
///          continue past an embedded NUL never aliases `cstr` (VDOC-043).
/// @param s Borrowed runtime string.
/// @param cstr Required null-terminated comparison text.
/// @return Nonzero when lengths and bytes are identical.
static int str_eq_cstr(rt_string s, const char *cstr) {
    if (!s || !cstr)
        return 0;
    size_t clen = strlen(cstr);
    int64_t slen = rt_str_len(s);
    return slen >= 0 && (size_t)slen == clen && memcmp(rt_string_cstr(s), cstr, clen) == 0;
}

/// @brief Convert any Zanna value to a plain string for use as XML text/attribute content.
///        NULL→"null", bool→"true"/"false", int/float→numeric, boxed str→unwrapped,
///        anything else→JSON-formatted fallback.
/// @param obj Borrowed generic runtime value.
/// @return Caller-owned scalar string representation.
static rt_string scalar_to_string(void *obj) {
    if (!obj)
        return make_cstr("null");
    if (rt_string_is_handle(obj))
        return rt_string_ref((rt_string)obj);

    int64_t box_type = rt_box_type(obj);
    char buf[96];
    if (box_type == RT_BOX_I1)
        return make_cstr(rt_unbox_i1(obj) ? "true" : "false");
    if (box_type == RT_BOX_I64) {
        snprintf(buf, sizeof(buf), "%lld", (long long)rt_unbox_i64(obj));
        return rt_string_from_bytes(buf, strlen(buf));
    }
    if (box_type == RT_BOX_F64) {
        double v = rt_unbox_f64(obj);
        if (isnan(v))
            return make_cstr(".nan");
        if (isinf(v))
            return make_cstr(v > 0 ? ".inf" : "-.inf");
        // Locale-independent exact formatting (VDOC-041).
        rt_format_f64_roundtrip(v, buf, sizeof(buf));
        return rt_string_from_bytes(buf, strlen(buf));
    }
    if (box_type == RT_BOX_STR)
        return rt_unbox_str(obj);

    return rt_json_format(obj);
}

/// @brief Return 1 if `c` is a valid first character for an XML name (letter, `_`, or `:`).
/// @param c Byte value to classify using the C ctype locale.
/// @return Nonzero for an accepted initial XML-name byte.
static int xml_name_start(int c) {
    return isalpha((unsigned char)c) || c == '_' || c == ':';
}

/// @brief Return 1 if `c` is a valid continuation character for an XML name.
/// @param c Byte value to classify using the C ctype locale.
/// @return Nonzero for an accepted continuation XML-name byte.
static int xml_name_char(int c) {
    return isalnum((unsigned char)c) || c == '_' || c == ':' || c == '-' || c == '.';
}

/// @brief Produce a valid XML element name from `name`, prefixing `fallback` if the first char is
/// invalid.
///        Invalid continuation characters are replaced with `_`.
/// @param name Candidate name bytes, or null.
/// @param name_len Number of candidate bytes.
/// @param fallback Required null-terminated fallback/prefix.
/// @return Newly allocated sanitized runtime string.
static rt_string sanitized_xml_name(const char *name, size_t name_len, const char *fallback) {
    const char *src = (name && name_len > 0) ? name : fallback;
    size_t len = (name && name_len > 0) ? name_len : strlen(fallback);
    size_t cap = len + strlen(fallback) + 8;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        rt_trap("Serialize: memory allocation failed");
        return rt_string_from_bytes(src, len);
    }
    size_t out = 0;
    if (!xml_name_start((unsigned char)src[0])) {
        for (const char *p = fallback; *p; ++p)
            buf[out++] = *p;
        buf[out++] = '_';
    }
    for (size_t i = 0; i < len; i++)
        buf[out++] = (char)(xml_name_char((unsigned char)src[i]) ? src[i] : '_');
    rt_string result = rt_string_from_bytes(buf, out);
    free(buf);
    return result;
}

/// @brief Recursively convert any Zanna value into an XML element named `name`.
///        Map keys become child elements; `@attrs` keys become XML attributes;
///        `@text`/`#text` keys become text content; Seq items become `<item>` children;
///        scalars become element text content.
/// @param name Requested element-name bytes.
/// @param name_len Number of bytes in @p name.
/// @param obj Borrowed generic value to project.
/// @return Caller-owned XML element node, or null on construction failure.
static void *generic_to_xml_element(const char *name, size_t name_len, void *obj) {
    rt_string tag = sanitized_xml_name(name, name_len, "item");
    void *elem = rt_xml_element(tag);
    rt_string_unref(tag);
    if (!elem)
        return NULL;

    if (is_map_obj(obj)) {
        rt_string attrs_key = make_cstr("@attrs");
        void *attrs = rt_map_get(obj, attrs_key);
        rt_string_unref(attrs_key);
        if (is_map_obj(attrs)) {
            void *attr_names = rt_map_keys(attrs);
            int64_t nattrs = rt_seq_len(attr_names);
            for (int64_t i = 0; i < nattrs; i++) {
                rt_string attr_name = (rt_string)rt_seq_get(attr_names, i);
                rt_string attr_value = scalar_to_string(rt_map_get(attrs, attr_name));
                rt_xml_set_attr(elem, attr_name, attr_value);
                rt_string_unref(attr_value);
            }
            release_obj(attr_names);
        }

        rt_string text_key = make_cstr("@text");
        void *text_value = rt_map_get(obj, text_key);
        rt_string_unref(text_key);
        if (!text_value) {
            rt_string legacy_text_key = make_cstr("#text");
            text_value = rt_map_get(obj, legacy_text_key);
            rt_string_unref(legacy_text_key);
        }
        if (text_value) {
            rt_string text = scalar_to_string(text_value);
            rt_xml_set_text(elem, text);
            rt_string_unref(text);
        }

        void *keys = rt_map_keys(obj);
        int64_t nkeys = rt_seq_len(keys);
        for (int64_t i = 0; i < nkeys; i++) {
            rt_string key = (rt_string)rt_seq_get(keys, i);
            if (str_eq_cstr(key, "@attrs") || str_eq_cstr(key, "@text") ||
                str_eq_cstr(key, "#text"))
                continue;
            int64_t key_len = rt_str_len(key);
            void *child = generic_to_xml_element(
                rt_string_cstr(key), key_len < 0 ? 0 : (size_t)key_len, rt_map_get(obj, key));
            if (child) {
                rt_xml_append(elem, child);
                release_obj(child);
            }
        }
        release_obj(keys);
        return elem;
    }

    if (is_seq_obj(obj)) {
        int64_t len = rt_seq_len(obj);
        for (int64_t i = 0; i < len; i++) {
            void *child = generic_to_xml_element("item", 4, rt_seq_get(obj, i));
            if (child) {
                rt_xml_append(elem, child);
                release_obj(child);
            }
        }
        return elem;
    }

    rt_string text = scalar_to_string(obj);
    rt_xml_set_text(elem, text);
    rt_string_unref(text);
    return elem;
}

/// @brief Serialize `obj` as XML, wrapping non-XML-node values in a `<root>` element first.
///        `indent > 0` produces pretty-printed output; `indent == 0` produces compact output.
/// @param obj Borrowed XML node or generic runtime value.
/// @param indent Pretty-print width; nonpositive selects compact output.
/// @return Caller-owned XML text, or an empty string after projection failure.
static rt_string format_xml_from_generic(void *obj, int64_t indent) {
    if (rt_xml_is_node(obj))
        return indent > 0 ? rt_xml_format_pretty(obj, indent) : rt_xml_format(obj);

    void *root = generic_to_xml_element("root", 4, obj);
    if (!root) {
        set_error("format XML: cannot build XML tree");
        return rt_string_from_bytes("", 0);
    }
    rt_string out = indent > 0 ? rt_xml_format_pretty(root, indent) : rt_xml_format(root);
    release_obj(root);
    return out;
}

/// @brief Insert `value` under `key` in `map`, promoting to a Seq on the second occurrence.
///        This implements the XML-to-JSON grouping convention: repeated same-tag siblings
///        are collected into a sequence rather than silently overwriting the first entry.
/// @param map Borrowed destination Map.
/// @param key Borrowed runtime key.
/// @param value Borrowed value inserted or appended under @p key.
static void add_grouped_child(void *map, rt_string key, void *value) {
    void *existing = rt_map_get(map, key);
    if (!existing) {
        rt_map_set(map, key, value);
        return;
    }

    if (is_seq_obj(existing)) {
        rt_seq_push(existing, value);
        return;
    }

    void *seq = rt_seq_new();
    rt_seq_set_owns_elements(seq, 1);
    rt_seq_push(seq, existing);
    rt_seq_push(seq, value);
    rt_map_set(map, key, seq);
    release_obj(seq);
}

static void *xml_to_generic_value(void *node);

/// @brief Convert an XML document node to a generic `Map{tag: value}` representation.
///        Finds the root element, converts its subtree, and wraps it under the root tag name.
/// @param doc Borrowed XML document node.
/// @return Caller-owned generic Map, or null when the document has no root.
static void *xml_document_to_generic(void *doc) {
    void *root = rt_xml_root(doc);
    if (!root)
        return NULL;
    void *map = rt_map_new();
    rt_string tag = rt_xml_tag(root);
    void *value = xml_to_generic_value(root);
    rt_map_set(map, tag, value);
    release_obj(value);
    rt_string_unref(tag);
    return map;
}

/// @brief Recursively convert an XML node to a generic Zanna value (Map/Seq/String).
///        Element attributes go into `@attrs`; mixed text goes into `@text`;
///        child elements become map entries (grouped into Seq on repeated tags).
///        Text-only elements with no attributes return the text string directly.
/// @param node Borrowed XML document, element, text, or CDATA node.
/// @return Caller-owned generic Map, Seq-compatible value, string, or null for
///         unsupported nodes and conversion failure.
static void *xml_to_generic_value(void *node) {
    int64_t type = rt_xml_node_type(node);
    if (type == XML_NODE_DOCUMENT)
        return xml_document_to_generic(node);
    if (type == XML_NODE_TEXT || type == XML_NODE_CDATA)
        return (void *)rt_xml_content(node);
    if (type != XML_NODE_ELEMENT)
        return NULL;

    void *attrs = rt_xml_attr_names(node);
    void *children = rt_xml_children(node);
    int64_t attr_count = rt_seq_len(attrs);
    int64_t child_count = rt_seq_len(children);
    int64_t element_children = 0;
    rt_string_builder text;
    rt_sb_init(&text);

    void *map = rt_map_new();
    if (!map) {
        release_obj(attrs);
        release_obj(children);
        rt_sb_free(&text);
        return NULL;
    }

    if (attr_count > 0) {
        void *attr_map = rt_map_new();
        if (!attr_map)
            goto convert_error;
        for (int64_t i = 0; i < attr_count; i++) {
            rt_string name = (rt_string)rt_seq_get(attrs, i);
            rt_string val = rt_xml_attr(node, name);
            rt_map_set(attr_map, name, (void *)val);
            rt_string_unref(val);
        }
        map_set_cstr(map, "@attrs", attr_map);
        release_obj(attr_map);
    }

    for (int64_t i = 0; i < child_count; i++) {
        void *child = rt_seq_get(children, i);
        int64_t child_type = rt_xml_node_type(child);
        if (child_type == XML_NODE_ELEMENT) {
            element_children++;
            rt_string tag = rt_xml_tag(child);
            void *value = xml_to_generic_value(child);
            add_grouped_child(map, tag, value);
            release_obj(value);
            rt_string_unref(tag);
        } else if (child_type == XML_NODE_TEXT || child_type == XML_NODE_CDATA) {
            rt_string content = rt_xml_content(child);
            int64_t content_len = content ? rt_str_len(content) : 0;
            const char *content_data = content ? rt_string_cstr(content) : NULL;
            if (content_len < 0 ||
                rt_sb_append_bytes(&text, content_data, (size_t)content_len) != RT_SB_OK) {
                rt_string_unref(content);
                goto convert_error;
            }
            rt_string_unref(content);
        }
    }

    release_obj(attrs);
    release_obj(children);

    if (attr_count == 0 && element_children == 0) {
        rt_string scalar = rt_string_from_bytes(text.data ? text.data : "", text.len);
        rt_sb_free(&text);
        release_obj(map);
        return scalar;
    }

    if (text.len > 0) {
        rt_string scalar = rt_string_from_bytes(text.data, text.len);
        map_set_cstr(map, "@text", (void *)scalar);
        rt_string_unref(scalar);
    }
    rt_sb_free(&text);
    return map;

convert_error:
    release_obj(attrs);
    release_obj(children);
    release_obj(map);
    rt_sb_free(&text);
    return NULL;
}

/// @brief Serialize `obj` as TOML. Non-map objects are wrapped in a synthetic map
///        (`items` key for sequences, `value` key for scalars) so TOML's top-level
///        table requirement is satisfied.
/// @param obj Borrowed generic runtime value.
/// @return Caller-owned TOML text.
static rt_string format_toml_from_generic(void *obj) {
    if (is_map_obj(obj))
        return rt_toml_format(obj);
    void *map = rt_map_new();
    map_set_cstr(map, is_seq_obj(obj) ? "items" : "value", obj);
    rt_string out = rt_toml_format(map);
    release_obj(map);
    return out;
}

/// @brief Return 1 if `obj` is a Seq whose every element is also a Seq (i.e., a 2-D table).
///        An empty Seq is considered valid. Used to decide whether CSV format can be applied
///        directly.
/// @param obj Borrowed candidate runtime value.
/// @return Nonzero for an empty or entirely row-Seq outer Seq.
static int seq_rows_are_sequences(void *obj) {
    if (!is_seq_obj(obj))
        return 0;
    int64_t len = rt_seq_len(obj);
    if (len == 0)
        return 1;
    for (int64_t i = 0; i < len; i++) {
        void *row = rt_seq_get(obj, i);
        if (row && !is_seq_obj(row))
            return 0;
    }
    return 1;
}

/// @brief Convert `value` to a string and push it as the next cell in the CSV `row` Seq.
/// @param row Borrowed destination row Seq.
/// @param value Borrowed generic cell value.
static void seq_push_string_cell(void *row, void *value) {
    rt_string s = scalar_to_string(value);
    rt_seq_push(row, (void *)s);
    rt_string_unref(s);
}

/// @brief Serialize `obj` as CSV. A 2-D Seq passes through directly; a Map becomes
///        two-column key-value rows; a flat Seq becomes single-column rows; a scalar
///        becomes one row with one cell.
/// @param obj Borrowed generic runtime value.
/// @return Caller-owned CSV text.
static rt_string format_csv_from_generic(void *obj) {
    if (seq_rows_are_sequences(obj))
        return rt_csv_format(obj);

    void *rows = rt_seq_new();
    rt_seq_set_owns_elements(rows, 1);

    if (is_map_obj(obj)) {
        void *keys = rt_map_keys(obj);
        int64_t nkeys = rt_seq_len(keys);
        for (int64_t i = 0; i < nkeys; i++) {
            rt_string key = (rt_string)rt_seq_get(keys, i);
            void *row = rt_seq_new();
            rt_seq_set_owns_elements(row, 1);
            rt_seq_push(row, (void *)key);
            seq_push_string_cell(row, rt_map_get(obj, key));
            rt_seq_push(rows, row);
            release_obj(row);
        }
        release_obj(keys);
    } else if (is_seq_obj(obj)) {
        int64_t len = rt_seq_len(obj);
        for (int64_t i = 0; i < len; i++) {
            void *row = rt_seq_new();
            rt_seq_set_owns_elements(row, 1);
            seq_push_string_cell(row, rt_seq_get(obj, i));
            rt_seq_push(rows, row);
            release_obj(row);
        }
    } else {
        void *row = rt_seq_new();
        rt_seq_set_owns_elements(row, 1);
        seq_push_string_cell(row, obj);
        rt_seq_push(rows, row);
        release_obj(row);
    }

    rt_string out = rt_csv_format(rows);
    release_obj(rows);
    return out;
}

//=============================================================================
// Unified Parse
//=============================================================================

/// @brief `Serialize.Parse(text, format)` — parse `text` in the given format.
///        Delegates to the appropriate backend: rt_json_parse, rt_xml_parse, rt_yaml_parse,
///        rt_toml_parse, or rt_csv_parse. Returns NULL on parse failure; call
///        `rt_serialize_error()` to retrieve the diagnostic message.
/// @param text Borrowed serialized input.
/// @param format One of the `rt_format_t` values.
/// @return Caller-owned parsed value, or null for either a valid null document
///         or failure; inspect `rt_serialize_error()` to distinguish them.
void *rt_serialize_parse(rt_string text, int64_t format) {
    clear_error();
    if (!text) {
        set_error("parse: nil input");
        return NULL;
    }

    switch ((rt_format_t)format) {
        case RT_FORMAT_JSON:
            if (!rt_json_is_valid(text)) {
                set_error("JSON parse error");
                return NULL;
            }
            return rt_json_parse(text);

        case RT_FORMAT_XML: {
            void *result = rt_xml_parse(text);
            if (!result) {
                rt_string err = rt_xml_error();
                set_error_from_string(err, "XML parse error");
                rt_string_unref(err);
            }
            return result;
        }

        case RT_FORMAT_YAML: {
            void *result = rt_yaml_parse(text);
            if (!result) {
                rt_string err = rt_yaml_error();
                if (err && rt_str_len(err) > 0)
                    set_error_from_string(err, "YAML parse error");
                rt_string_unref(err);
            }
            return result;
        }

        case RT_FORMAT_TOML:
            if (!rt_toml_is_valid(text)) {
                set_error("TOML parse error");
                return NULL;
            }
            return rt_toml_parse(text);

        case RT_FORMAT_CSV:
            if (!rt_csv_is_valid(text)) {
                set_error("CSV parse error");
                return NULL;
            }
            return rt_csv_parse(text);

        default:
            set_error("parse: unknown format");
            return NULL;
    }
}

/// @brief `Serialize.ParseResult(text, format)` — parse into a Result object.
///
/// Success returns `Ok(parsedValue)`. Parse failures, nil input, and unknown
/// formats return `Err(message)` using the same diagnostic text exposed by
/// `Serialize.Error()`.
///
/// @param text Input text.
/// @param format Serialization format enum.
/// @return Owned `Zanna.Result` carrying the parsed value or an error string.
void *rt_serialize_parse_result(rt_string text, int64_t format) {
    void *value = rt_serialize_parse(text, format);
    return serialize_parse_value_to_result(value, "Serialize.Parse failed");
}

//=============================================================================
// Unified Format
//=============================================================================

/// @brief Serialize `obj` into the requested text format (compact output).
/// @details Dispatches to the per-format formatter (`rt_json_format`,
///          `rt_xml_format`, etc.) based on the `format` enum.
///          Returns an empty string on unknown format. Use
///          `rt_serialize_format_pretty` for indented output.
/// @param obj Borrowed generic value or XML node.
/// @param format Target `rt_format_t` value.
/// @return Caller-owned serialized text, or empty text with a recorded error
///         for an unknown target or failed generic projection.
rt_string rt_serialize_format(void *obj, int64_t format) {
    clear_error();

    if (rt_xml_is_node(obj) && format != RT_FORMAT_XML) {
        void *generic = xml_to_generic_value(obj);
        rt_string out = rt_serialize_format(generic, format);
        release_obj(generic);
        return out;
    }

    switch ((rt_format_t)format) {
        case RT_FORMAT_JSON:
            return rt_json_format(obj);

        case RT_FORMAT_XML:
            return format_xml_from_generic(obj, 0);

        case RT_FORMAT_YAML:
            return rt_yaml_format(obj);

        case RT_FORMAT_TOML:
            return format_toml_from_generic(obj);

        case RT_FORMAT_CSV:
            return format_csv_from_generic(obj);

        default:
            set_error("format: unknown format");
            return rt_string_from_bytes("", 0);
    }
}

/// @brief Serialize an object to a pretty-printed string in the specified format (JSON/XML/YAML).
/// @details Indents less than one become two. TOML and CSV ignore indentation
///          and use their compact formatters.
/// @param obj Borrowed generic value or XML node.
/// @param format Target `rt_format_t` value.
/// @param indent Requested spaces per nesting level.
/// @return Caller-owned serialized text, or empty text with a recorded error
///         for an unknown target or failed projection.
rt_string rt_serialize_format_pretty(void *obj, int64_t format, int64_t indent) {
    clear_error();

    if (indent < 1)
        indent = 2;

    if (rt_xml_is_node(obj) && format != RT_FORMAT_XML) {
        void *generic = xml_to_generic_value(obj);
        rt_string out = rt_serialize_format_pretty(generic, format, indent);
        release_obj(generic);
        return out;
    }

    switch ((rt_format_t)format) {
        case RT_FORMAT_JSON:
            return rt_json_format_pretty(obj, indent);

        case RT_FORMAT_XML:
            return format_xml_from_generic(obj, indent);

        case RT_FORMAT_YAML:
            return rt_yaml_format_indent(obj, indent);

        case RT_FORMAT_TOML:
            return format_toml_from_generic(obj); /* TOML has no indent option */

        case RT_FORMAT_CSV:
            return format_csv_from_generic(obj); /* CSV has no indent option */

        default:
            set_error("format_pretty: unknown format");
            return rt_string_from_bytes("", 0);
    }
}

//=============================================================================
// Validation
//=============================================================================

/// @brief Check whether text is valid in any supported serialization format.
/// @details A null input records an error. Unknown format values return zero
///          without setting an additional diagnostic.
/// @param text Borrowed serialized input.
/// @param format Candidate `rt_format_t` value.
/// @return `1` when the selected backend accepts the text, otherwise `0`.
int8_t rt_serialize_is_valid(rt_string text, int64_t format) {
    clear_error();
    if (!text) {
        set_error("is_valid: nil input");
        return 0;
    }

    switch ((rt_format_t)format) {
        case RT_FORMAT_JSON:
            return rt_json_is_valid(text);

        case RT_FORMAT_XML:
            return rt_xml_is_valid(text);

        case RT_FORMAT_YAML:
            return rt_yaml_is_valid(text);

        case RT_FORMAT_TOML:
            return rt_toml_is_valid(text);

        case RT_FORMAT_CSV:
            return rt_csv_is_valid(text);

        default:
            return 0;
    }
}

//=============================================================================
// Auto-Detection
//=============================================================================

/// @brief Skip leading ASCII whitespace and return a pointer to the first non-space byte.
/// @details This detector-specific rule skips every non-null byte at or below
///          ASCII space.
/// @param s Required null-terminated input cursor.
/// @return Pointer to the first byte greater than ASCII space or the terminator.
static const char *skip_ws(const char *s) {
    while (*s && ((unsigned char)*s <= ' '))
        s++;
    return s;
}

/// @brief Sniff a text blob and return the most likely serialization format.
/// @details Heuristic dispatch on the first non-whitespace character(s):
///          - `{` or `[` → JSON.
///          - `<` → XML.
///          - `---` → YAML document marker.
///          - `[name]` on first line → TOML section.
///          - `key = ...` on first line → TOML key/value.
///          - `key: ...` on first line → YAML.
///          - first-line comma → CSV.
///          Returns the matching `RT_FORMAT_*` constant, or -1 for
///          NULL/empty/unknown input. Never throws — this is meant to be a
///          conservative best-effort guess used as a fallback.
/// @param text Borrowed serialized input.
/// @return Detected `rt_format_t` value, or `-1` when no heuristic matches.
int64_t rt_serialize_detect(rt_string text) {
    const char *s;
    const char *line;

    if (!text)
        return -1;

    s = rt_string_cstr(text);
    if (!s || *s == '\0')
        return -1;

    s = skip_ws(s);
    if (*s == '\0')
        return -1;

    /* JSON: valid object or array. Prefer valid JSON arrays over TOML quoted table names. */
    if ((*s == '{' || *s == '[') && rt_json_is_valid(text))
        return RT_FORMAT_JSON;

    /* TOML: [section] on first line. */
    if (*s == '[' && s[1] != '\0') {
        const char *p = s + 1;
        int has_comma = 0;
        while (*p && *p != '\n' && *p != ']') {
            if (*p == ',')
                has_comma = 1;
            p++;
        }
        if (*p == ']' && p > s + 1 && !has_comma &&
            (isalpha((unsigned char)s[1]) || s[1] == '_' || s[1] == '"' || s[1] == '\'')) {
            const char *after = p + 1;
            while (*after == ' ' || *after == '\t')
                after++;
            if ((*after == '\0' || *after == '\n' || *after == '#') && rt_toml_is_valid(text))
                return RT_FORMAT_TOML;
        }
    }

    /* JSON-looking but invalid still most likely intended as JSON. */
    if (*s == '{' || *s == '[')
        return RT_FORMAT_JSON;

    /* XML: starts with < */
    if (*s == '<')
        return RT_FORMAT_XML;

    /* YAML: starts with --- */
    if (s[0] == '-' && s[1] == '-' && s[2] == '-')
        return RT_FORMAT_YAML;

    /* TOML: look for key = value on first line */
    line = s;
    {
        const char *eq = line;
        while (*eq && *eq != '\n' && *eq != '=')
            eq++;
        if (*eq == '=' && eq > line && rt_toml_is_valid(text))
            return RT_FORMAT_TOML;
    }

    /* YAML: mapping-style first line with a colon followed by whitespace/EOL. */
    {
        const char *colon = line;
        while (*colon && *colon != '\n' && *colon != ':')
            colon++;
        if (*colon == ':' && colon > line &&
            (colon[1] == '\0' || colon[1] == '\n' || colon[1] == ' ' || colon[1] == '\t') &&
            rt_yaml_is_valid(text))
            return RT_FORMAT_YAML;
    }

    /* CSV: comma-separated first line */
    {
        const char *comma = line;
        while (*comma && *comma != '\n' && *comma != ',')
            comma++;
        if (*comma == ',')
            return RT_FORMAT_CSV;
    }

    return -1;
}

/// @brief `Serialize.AutoParse(text)` — detect the format heuristically and parse with it.
///        Convenience for "load this file without knowing its format" workflows.
///        Sets an error and returns NULL if the format cannot be determined.
/// @param text Borrowed serialized input.
/// @return Caller-owned parsed value, or null for a valid null document or
///         failure; inspect `rt_serialize_error()` for failure.
void *rt_serialize_auto_parse(rt_string text) {
    int64_t format;
    clear_error();
    if (!text) {
        set_error("auto_parse: nil input");
        return NULL;
    }

    format = rt_serialize_detect(text);
    if (format < 0) {
        set_error("auto_parse: cannot detect format");
        return NULL;
    }

    return rt_serialize_parse(text, format);
}

/// @brief `Serialize.AutoParseResult(text)` — detect format, parse, and return a Result.
///
/// Success returns `Ok(parsedValue)`. Detection failures and parse failures
/// return `Err(message)` with the same diagnostic text exposed by
/// `Serialize.Error()`.
///
/// @param text Input text.
/// @return Owned `Zanna.Result` carrying the parsed value or an error string.
void *rt_serialize_auto_parse_result(rt_string text) {
    void *value = rt_serialize_auto_parse(text);
    return serialize_parse_value_to_result(value, "Serialize.AutoParse failed");
}

//=============================================================================
// Round-Trip Conversion
//=============================================================================

/// @brief Round-trip text through a parse → re-format cycle to convert between formats.
/// @details Implementation: parse `text` as `from_format` to a tree,
///          then re-serialize that tree as `to_format`. Lossy in both
///          directions for some pairs (XML attributes ↔ JSON keys,
///          TOML datetimes ↔ JSON strings) — caller should expect
///          structural fidelity, not byte-for-byte equivalence.
/// @param text Borrowed source-format text.
/// @param from_format Source `rt_format_t` value.
/// @param to_format Target `rt_format_t` value.
/// @return Caller-owned converted text, or empty text with a diagnostic on failure.
rt_string rt_serialize_convert(rt_string text, int64_t from_format, int64_t to_format) {
    void *parsed;
    void *value;
    int parsed_is_xml;

    clear_error();
    if (!text) {
        set_error("convert: nil input");
        return rt_string_from_bytes("", 0);
    }

    if (!rt_serialize_is_valid(text, from_format)) {
        if (!has_error())
            set_error("convert: source text is not valid for format");
        return rt_string_from_bytes("", 0);
    }

    parsed = rt_serialize_parse(text, from_format);
    if (!parsed && has_error())
        return rt_string_from_bytes("", 0);

    parsed_is_xml = rt_xml_is_node(parsed);
    value = parsed;
    if (parsed_is_xml && to_format != RT_FORMAT_XML)
        value = xml_to_generic_value(parsed);

    rt_string out = rt_serialize_format(value, to_format);

    if (value != parsed)
        release_obj(value);
    release_obj(parsed);
    return out;
}

//=============================================================================
// Format Metadata
//=============================================================================

/// @brief Return the human-readable name of a format constant (e.g., "JSON", "XML").
/// @param format Candidate `rt_format_t` value.
/// @return Newly allocated lowercase format name, or `"unknown"`.
rt_string rt_serialize_format_name(int64_t format) {
    switch ((rt_format_t)format) {
        case RT_FORMAT_JSON:
            return rt_string_from_bytes("json", 4);
        case RT_FORMAT_XML:
            return rt_string_from_bytes("xml", 3);
        case RT_FORMAT_YAML:
            return rt_string_from_bytes("yaml", 4);
        case RT_FORMAT_TOML:
            return rt_string_from_bytes("toml", 4);
        case RT_FORMAT_CSV:
            return rt_string_from_bytes("csv", 3);
        default:
            return rt_string_from_bytes("unknown", 7);
    }
}

/// @brief Return the MIME content-type string for a format (e.g., "application/json").
/// @param format Candidate `rt_format_t` value.
/// @return Newly allocated MIME type, or `"application/octet-stream"`.
rt_string rt_serialize_mime_type(int64_t format) {
    switch ((rt_format_t)format) {
        case RT_FORMAT_JSON:
            return rt_string_from_bytes("application/json", 16);
        case RT_FORMAT_XML:
            return rt_string_from_bytes("application/xml", 15);
        case RT_FORMAT_YAML:
            return rt_string_from_bytes("application/yaml", 16);
        case RT_FORMAT_TOML:
            return rt_string_from_bytes("application/toml", 16);
        case RT_FORMAT_CSV:
            return rt_string_from_bytes("text/csv", 8);
        default:
            return rt_string_from_bytes("application/octet-stream", 24);
    }
}

/// @brief Look up a format constant by its short name (case-insensitive: "json", "xml", ...).
/// @details Inverse of `rt_serialize_format_name`. Accepts both
///          "yaml" and "yml" for YAML. Returns -1 for unknown names.
///          Embedded null bytes are rejected.
/// @param name Borrowed case-insensitive format name.
/// @return Matching `rt_format_t` value, or `-1`.
int64_t rt_serialize_format_from_name(rt_string name) {
    const char *s;
    if (!name)
        return -1;

    s = rt_string_cstr(name);
    if (!s)
        return -1;

    /* Names with bytes past an embedded NUL are not valid format names; the
     * case-insensitive C-string comparisons below would otherwise let
     * "json\0suffix" alias "json" (VDOC-043). */
    int64_t name_len = rt_str_len(name);
    if (name_len < 0 || (size_t)name_len != strlen(s))
        return -1;

    /* Case-insensitive comparison */
    if (strcasecmp(s, "json") == 0)
        return RT_FORMAT_JSON;
    if (strcasecmp(s, "xml") == 0)
        return RT_FORMAT_XML;
    if (strcasecmp(s, "yaml") == 0)
        return RT_FORMAT_YAML;
    if (strcasecmp(s, "yml") == 0)
        return RT_FORMAT_YAML;
    if (strcasecmp(s, "toml") == 0)
        return RT_FORMAT_TOML;
    if (strcasecmp(s, "csv") == 0)
        return RT_FORMAT_CSV;

    return -1;
}

//=============================================================================
// Error Handling
//=============================================================================

/// @brief Return the most recent thread-local error message (empty string if none).
/// @details Stored per-thread, so concurrent serialize calls don't
///          step on each other's diagnostics. Cleared at the start of
///          every public entry point.
/// @return Retained thread-local diagnostic string, or a newly allocated empty
///         string when no error is recorded.
rt_string rt_serialize_error(void) {
    if (g_last_error)
        return rt_string_ref(g_last_error);
    return rt_string_from_bytes("", 0);
}
