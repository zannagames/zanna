//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/2d/rt_action_io.c
/// @file
/// @brief Implements JSON serialization and transactional parsing for the
///        global input-action registry.
// Purpose: Input-action serialization: save the current action/binding set to JSON
//   and load it back, reconstructing actions and bindings via the action core.
//
// Key invariants:
//   - Save emits one top-level "actions" array in current linked-list order.
//   - Load constructs a temporary registry while retaining the previous list.
//     A syntactically/structurally invalid document destroys the temporary
//     nodes and restores the previous registry; successful parsing replaces it.
//   - Unknown object members and binding type tags are ignored for forward
//     compatibility. Recognized fields are unique, schema-complete, finite,
//     range-checked, and compatible with their enclosing action kind.
//
// Ownership/Lifetime:
//   - Save returns a newly allocated runtime string.
//   - Load borrows its input string, owns its parser and pending-binding buffer,
//     and transfers successfully materialized nodes to the global registry.
//
// Links: rt_action.h (public API), rt_action_internal.h (shared model),
//        rt_json_stream.h (streaming JSON parser), rt_action.c (action core)
//
//===----------------------------------------------------------------------===//

#include "rt_action.h"
#include "rt_action_internal.h"
#include "rt_input.h"
#include "rt_internal.h"
#include "rt_json_stream.h"
#include "rt_object.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_string_builder.h"
#include "rt_trap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/// @brief Extract an int64 value from a borrowed runtime box.
extern int64_t rt_unbox_i64(void *box);

/// @brief Map a `BindingType` enum to the JSON serialization tag.
/// @param type Internal binding kind.
/// @return Borrowed static tag string, or `"unknown"` for unsupported values.
static const char *binding_type_name(BindingType type) {
    switch (type) {
        case BIND_KEY:
            return "key";
        case BIND_MOUSE_BUTTON:
            return "mouse";
        case BIND_MOUSE_X:
            return "mouse_x";
        case BIND_MOUSE_Y:
            return "mouse_y";
        case BIND_SCROLL_X:
            return "scroll_x";
        case BIND_SCROLL_Y:
            return "scroll_y";
        case BIND_PAD_BUTTON:
            return "pad_button";
        case BIND_PAD_AXIS:
            return "pad_axis";
        case BIND_PAD_BUTTON_AXIS:
            return "pad_button_axis";
        case BIND_CHORD:
            return "chord";
        default:
            return "unknown";
    }
}

/// @brief Release a streaming JSON parser object created by rt_json_stream_new.
/// @details Releases one reference and frees the object if that was the final
///          reference.
/// @param parser Owned parser handle; `NULL` is ignored.
static void action_release_json_parser(void *parser) {
    if (parser && rt_obj_release_check0(parser))
        rt_obj_free(parser);
}

/// @brief Append a JSON-quoted-string-literal version of `str` to a builder.
///
/// Handles the standard JSON escape characters (`"`, `\\`, `\\n`,
/// `\\r`, `\\t`) and emits `\\u00XX` escapes for the remaining control bytes.
/// Non-ASCII bytes pass through as-is — the persistence format assumes UTF-8
/// throughout.
/// @param sb Borrowed destination builder.
/// @param str Borrowed non-null, null-terminated text to quote.
/// @return `1` on success; `0` when a builder append fails.
static int sb_append_json_span(rt_string_builder *sb, const char *str, size_t len) {
    static const char hex[] = "0123456789ABCDEF";
    if (rt_sb_append_cstr(sb, "\"") != RT_SB_OK)
        return 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)str[i];
        switch (c) {
            case '"':
                if (rt_sb_append_cstr(sb, "\\\"") != RT_SB_OK)
                    return 0;
                break;
            case '\\':
                if (rt_sb_append_cstr(sb, "\\\\") != RT_SB_OK)
                    return 0;
                break;
            case '\n':
                if (rt_sb_append_cstr(sb, "\\n") != RT_SB_OK)
                    return 0;
                break;
            case '\r':
                if (rt_sb_append_cstr(sb, "\\r") != RT_SB_OK)
                    return 0;
                break;
            case '\t':
                if (rt_sb_append_cstr(sb, "\\t") != RT_SB_OK)
                    return 0;
                break;
            default:
                if (c < 0x20u) {
                    char esc[6] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xFu], hex[c & 0xFu]};
                    if (rt_sb_append_bytes(sb, esc, sizeof(esc)) != RT_SB_OK)
                        return 0;
                } else {
                    char byte = (char)c;
                    if (rt_sb_append_bytes(sb, &byte, 1) != RT_SB_OK)
                        return 0;
                }
                break;
        }
    }
    return rt_sb_append_cstr(sb, "\"") == RT_SB_OK;
}

/// @brief Append a NUL-terminated string as a JSON string literal.
static int sb_append_json_string(rt_string_builder *sb, const char *str) {
    return str && sb_append_json_span(sb, str, strlen(str));
}

/// @brief `Action.Save` — serialize all action+binding state to JSON.
///
/// Format: `{"actions":[{"name","type","bindings":[{"type","code","pad","value","keys":[...]}]}]}`.
/// `keys` is only present for chord bindings. The result is a fresh
/// `rt_string` that can be saved to disk and round-tripped via
/// `rt_action_load`. Actions and bindings retain current linked-list order.
/// @return Newly owned JSON runtime string. Builder failure returns a newly
///         allocated empty string when possible; final string allocation can
///         return `NULL`.
rt_string rt_action_save(void) {
    RT_ASSERT_MAIN_THREAD();
    rt_string_builder sb;
    int8_t first_action;
    rt_sb_init(&sb);

    first_action = 1;
    if (rt_sb_append_cstr(&sb, "{\"actions\":[") != RT_SB_OK)
        goto save_error;

    {
        Action *a = g_actions;
        while (a) {
            int8_t first_binding;
            int64_t serialized_binding_count = 0;
            if (!first_action) {
                if (rt_sb_append_cstr(&sb, ",") != RT_SB_OK)
                    goto save_error;
            }
            first_action = 0;

            if (!a->name || a->name_len <= 0 || (uint64_t)a->name_len > SIZE_MAX ||
                strlen(a->name) != (size_t)a->name_len ||
                rt_sb_append_cstr(&sb, "{\"name\":") != RT_SB_OK ||
                !sb_append_json_span(&sb, a->name, (size_t)a->name_len) ||
                rt_sb_append_cstr(&sb, ",\"type\":") != RT_SB_OK ||
                rt_sb_append_cstr(&sb, a->is_axis ? "\"axis\"" : "\"button\"") != RT_SB_OK ||
                rt_sb_append_cstr(&sb, ",\"bindings\":[") != RT_SB_OK)
                goto save_error;

            first_binding = 1;
            {
                Binding *b = a->bindings;
                while (b) {
                    if (!action_binding_valid(b, a->is_axis))
                        goto save_error;
                    serialized_binding_count++;
                    if (!first_binding) {
                        if (rt_sb_append_cstr(&sb, ",") != RT_SB_OK)
                            goto save_error;
                    }
                    first_binding = 0;

                    if (rt_sb_append_cstr(&sb, "{\"type\":") != RT_SB_OK ||
                        !sb_append_json_string(&sb, binding_type_name(b->type)) ||
                        rt_sb_append_cstr(&sb, ",\"code\":") != RT_SB_OK ||
                        rt_sb_append_int(&sb, b->code) != RT_SB_OK ||
                        rt_sb_append_cstr(&sb, ",\"pad\":") != RT_SB_OK ||
                        rt_sb_append_int(&sb, b->pad_index) != RT_SB_OK ||
                        rt_sb_append_cstr(&sb, ",\"value\":") != RT_SB_OK ||
                        rt_sb_append_double(&sb, b->value) != RT_SB_OK)
                        goto save_error;
                    if (b->type == BIND_CHORD && b->chord_len > 0) {
                        int32_t ci;
                        if (rt_sb_append_cstr(&sb, ",\"keys\":[") != RT_SB_OK)
                            goto save_error;
                        for (ci = 0; ci < b->chord_len; ci++) {
                            if (ci > 0) {
                                if (rt_sb_append_cstr(&sb, ",") != RT_SB_OK)
                                    goto save_error;
                            }
                            if (rt_sb_append_int(&sb, b->chord_keys[ci]) != RT_SB_OK)
                                goto save_error;
                        }
                        if (rt_sb_append_cstr(&sb, "]") != RT_SB_OK)
                            goto save_error;
                    }
                    if (rt_sb_append_cstr(&sb, "}") != RT_SB_OK)
                        goto save_error;
                    b = b->next;
                }
            }
            if (serialized_binding_count != a->binding_count)
                goto save_error;

            if (rt_sb_append_cstr(&sb, "]}") != RT_SB_OK)
                goto save_error;
            a = a->next;
        }
    }

    if (rt_sb_append_cstr(&sb, "]}") != RT_SB_OK)
        goto save_error;

    {
        rt_string result = rt_string_from_bytes(sb.data, sb.len);
        rt_sb_free(&sb);
        if (result)
            return result;
        rt_trap("Action.Save: result allocation failed");
        return rt_str_empty();
    }

save_error:
    rt_sb_free(&sb);
    rt_trap("Action.Save: invalid registry state or allocation failure");
    return rt_str_empty();
}

/// @brief Compare a runtime string with one ASCII schema literal exactly.
static int action_string_equals(rt_string string, const char *literal) {
    if (!string || !literal)
        return 0;
    size_t literal_len = strlen(literal);
    int64_t string_len = rt_str_len(string);
    return string_len >= 0 && (uint64_t)string_len == literal_len &&
           memcmp(string->data, literal, literal_len) == 0;
}

/// @brief Convert an external JSON number to an exact in-range int64.
/// @param value Parsed double value.
/// @param out Borrowed destination.
/// @return Nonzero only for finite, integral values in the int64 domain.
static int action_json_i64(double value, int64_t *out) {
    if (!out || !isfinite(value) || trunc(value) != value || value >= 9223372036854775808.0 ||
        value < -9223372036854775808.0)
        return 0;
    *out = (int64_t)value;
    return 1;
}

/// @brief Parse a binding type tag without relying on embedded-NUL C strings.
static BindingType binding_type_from_string(rt_string string) {
    if (action_string_equals(string, "key"))
        return BIND_KEY;
    if (action_string_equals(string, "mouse"))
        return BIND_MOUSE_BUTTON;
    if (action_string_equals(string, "mouse_x"))
        return BIND_MOUSE_X;
    if (action_string_equals(string, "mouse_y"))
        return BIND_MOUSE_Y;
    if (action_string_equals(string, "scroll_x"))
        return BIND_SCROLL_X;
    if (action_string_equals(string, "scroll_y"))
        return BIND_SCROLL_Y;
    if (action_string_equals(string, "pad_button"))
        return BIND_PAD_BUTTON;
    if (action_string_equals(string, "pad_axis"))
        return BIND_PAD_AXIS;
    if (action_string_equals(string, "pad_button_axis"))
        return BIND_PAD_BUTTON_AXIS;
    if (action_string_equals(string, "chord"))
        return BIND_CHORD;
    return BIND_NONE;
}

/// @brief One parsed binding, buffered until the enclosing action object closes so
///        that "name"/"type"/"bindings" may appear in any order (JSON is unordered).
typedef struct {
    /// Parsed source kind; BIND_NONE causes the object to be ignored.
    BindingType btype;
    /// Parsed key, button, or axis identifier.
    int64_t code;
    /// Parsed controller index.
    int64_t pad;
    /// Parsed digital contribution or analog scale.
    double value;
    /// Parsed ordered chord keys.
    int64_t chord_keys[MAX_CHORD_KEYS];
    /// Number of populated @ref chord_keys entries.
    int32_t chord_len;
} action_pending_binding;

/// @brief `Action.Load(json)` — restore action and binding state from JSON.
/// @details Uses the streaming parser rather than constructing a JSON tree.
///          The previous global list remains recoverable until the complete
///          top-level object and its single `"actions"` array parse cleanly.
///          Unknown members are skipped and unknown binding types are omitted.
///          Recognized members are unique and schema-complete; names, numbers,
///          source identifiers, binding/action compatibility, and resource
///          limits are validated. Input order is preserved. Any allocation
///          failure rejects the complete transaction.
/// @param json Borrowed runtime string containing the JSON document.
/// @return `1` after a structurally valid complete document replaces the
///         registry; `0` on null input, parser creation failure, malformed
///         structure, excessive chord/name length, or pending-buffer failure.
int8_t rt_action_load(rt_string json) {
    RT_ASSERT_MAIN_THREAD();
    void *parser = NULL;
    int64_t tok = RT_JSON_TOK_ERROR;
    int8_t success = 0;
    int8_t saw_actions = 0;
    Action *saved_actions = NULL;
    Action *loaded_tail = NULL;
    Action *building_action = NULL;
    rt_string pending_name = NULL;
    /* Per-action-object binding buffer, reused across objects and freed once at
     * cleanup so the many `goto cleanup` sites need no per-branch free. */
    action_pending_binding *pending = NULL;
    size_t pending_count = 0;
    size_t pending_cap = 0;
    size_t total_binding_count = 0;
    int64_t action_count = 0;

    if (!json)
        return 0;

    parser = rt_json_stream_new(json);
    if (!parser)
        return 0;

    if (!g_initialized)
        rt_action_init();

    saved_actions = g_actions;
    g_actions = NULL;

    tok = rt_json_stream_next(parser);
    if (tok != RT_JSON_TOK_OBJECT_START)
        goto cleanup;

    tok = rt_json_stream_next(parser);
    while (tok == RT_JSON_TOK_KEY) {
        rt_string top_key = rt_json_stream_string_value(parser);
        if (!top_key)
            goto cleanup;
        int is_actions_key = action_string_equals(top_key, "actions");
        rt_string_unref(top_key);
        tok = rt_json_stream_next(parser);
        if (!is_actions_key) {
            rt_json_stream_skip(parser);
            tok = rt_json_stream_next(parser);
            continue;
        }
        if (saw_actions || tok != RT_JSON_TOK_ARRAY_START)
            goto cleanup;
        saw_actions = 1;

        tok = rt_json_stream_next(parser);
        while (tok == RT_JSON_TOK_OBJECT_START) {
            enum {
                ACTION_SEEN_NAME = 1u << 0,
                ACTION_SEEN_TYPE = 1u << 1,
                ACTION_SEEN_BINDINGS = 1u << 2
            };

            unsigned action_seen = 0;
            int8_t is_axis = 0;
            if (++action_count > ACTION_MAX_ACTIONS)
                goto cleanup;
            /* Buffer this object's bindings and materialize the action at OBJECT_END,
             * so "name"/"type"/"bindings" may appear in any order — JSON members are
             * unordered, and a third-party file need not match rt_action_save's order.
             * Reuse the function-scope buffer across objects. */
            pending_count = 0;
            if (pending_name) {
                rt_string_unref(pending_name);
                pending_name = NULL;
            }

            tok = rt_json_stream_next(parser);
            while (tok == RT_JSON_TOK_KEY) {
                rt_string key = rt_json_stream_string_value(parser);
                if (!key)
                    goto cleanup;
                int key_name = action_string_equals(key, "name");
                int key_type = action_string_equals(key, "type");
                int key_bindings = action_string_equals(key, "bindings");
                rt_string_unref(key);
                tok = rt_json_stream_next(parser);

                if (key_name) {
                    if ((action_seen & ACTION_SEEN_NAME) || tok != RT_JSON_TOK_STRING)
                        goto cleanup;
                    pending_name = rt_json_stream_string_value(parser);
                    if (!pending_name)
                        goto cleanup;
                    int64_t name_len = rt_str_len(pending_name);
                    if (name_len <= 0 || (uint64_t)name_len > SIZE_MAX - 1u ||
                        memchr(pending_name->data, '\0', (size_t)name_len) != NULL)
                        goto cleanup;
                    action_seen |= ACTION_SEEN_NAME;
                } else if (key_type) {
                    if ((action_seen & ACTION_SEEN_TYPE) || tok != RT_JSON_TOK_STRING)
                        goto cleanup;
                    rt_string val = rt_json_stream_string_value(parser);
                    if (!val)
                        goto cleanup;
                    if (action_string_equals(val, "axis"))
                        is_axis = 1;
                    else if (action_string_equals(val, "button"))
                        is_axis = 0;
                    else {
                        rt_string_unref(val);
                        goto cleanup;
                    }
                    rt_string_unref(val);
                    action_seen |= ACTION_SEEN_TYPE;
                } else if (key_bindings) {
                    if ((action_seen & ACTION_SEEN_BINDINGS) || tok != RT_JSON_TOK_ARRAY_START)
                        goto cleanup;
                    action_seen |= ACTION_SEEN_BINDINGS;

                    tok = rt_json_stream_next(parser);
                    while (tok == RT_JSON_TOK_OBJECT_START) {
                        enum {
                            BINDING_SEEN_TYPE = 1u << 0,
                            BINDING_SEEN_CODE = 1u << 1,
                            BINDING_SEEN_PAD = 1u << 2,
                            BINDING_SEEN_VALUE = 1u << 3,
                            BINDING_SEEN_KEYS = 1u << 4
                        };

                        action_pending_binding pb;
                        unsigned binding_seen = 0;
                        pb.btype = BIND_NONE;
                        pb.code = 0;
                        pb.pad = 0;
                        pb.value = 0.0;
                        pb.chord_len = 0;

                        tok = rt_json_stream_next(parser);
                        while (tok == RT_JSON_TOK_KEY) {
                            rt_string bkey = rt_json_stream_string_value(parser);
                            if (!bkey)
                                goto cleanup;
                            int bk_type = action_string_equals(bkey, "type");
                            int bk_code = action_string_equals(bkey, "code");
                            int bk_pad = action_string_equals(bkey, "pad");
                            int bk_value = action_string_equals(bkey, "value");
                            int bk_keys = action_string_equals(bkey, "keys");
                            rt_string_unref(bkey);
                            tok = rt_json_stream_next(parser);

                            if (bk_type) {
                                if ((binding_seen & BINDING_SEEN_TYPE) || tok != RT_JSON_TOK_STRING)
                                    goto cleanup;
                                rt_string bval = rt_json_stream_string_value(parser);
                                if (!bval)
                                    goto cleanup;
                                pb.btype = binding_type_from_string(bval);
                                rt_string_unref(bval);
                                binding_seen |= BINDING_SEEN_TYPE;
                            } else if (bk_code) {
                                if ((binding_seen & BINDING_SEEN_CODE) ||
                                    tok != RT_JSON_TOK_NUMBER ||
                                    !action_json_i64(rt_json_stream_number_value(parser), &pb.code))
                                    goto cleanup;
                                binding_seen |= BINDING_SEEN_CODE;
                            } else if (bk_pad) {
                                if ((binding_seen & BINDING_SEEN_PAD) ||
                                    tok != RT_JSON_TOK_NUMBER ||
                                    !action_json_i64(rt_json_stream_number_value(parser), &pb.pad))
                                    goto cleanup;
                                binding_seen |= BINDING_SEEN_PAD;
                            } else if (bk_value) {
                                if ((binding_seen & BINDING_SEEN_VALUE) ||
                                    tok != RT_JSON_TOK_NUMBER)
                                    goto cleanup;
                                pb.value = rt_json_stream_number_value(parser);
                                if (!isfinite(pb.value))
                                    goto cleanup;
                                binding_seen |= BINDING_SEEN_VALUE;
                            } else if (bk_keys) {
                                if ((binding_seen & BINDING_SEEN_KEYS) ||
                                    tok != RT_JSON_TOK_ARRAY_START)
                                    goto cleanup;
                                binding_seen |= BINDING_SEEN_KEYS;
                                pb.chord_len = 0;
                                tok = rt_json_stream_next(parser);
                                while (tok == RT_JSON_TOK_NUMBER) {
                                    if (pb.chord_len >= MAX_CHORD_KEYS)
                                        goto cleanup;
                                    if (!action_json_i64(rt_json_stream_number_value(parser),
                                                         &pb.chord_keys[pb.chord_len]))
                                        goto cleanup;
                                    pb.chord_len++;
                                    tok = rt_json_stream_next(parser);
                                }
                                if (tok != RT_JSON_TOK_ARRAY_END)
                                    goto cleanup;
                            } else {
                                rt_json_stream_skip(parser);
                            }

                            tok = rt_json_stream_next(parser);
                        }
                        if (tok != RT_JSON_TOK_OBJECT_END)
                            goto cleanup;

                        if (pb.btype != BIND_NONE) {
                            const unsigned required = BINDING_SEEN_TYPE | BINDING_SEEN_CODE |
                                                      BINDING_SEEN_PAD | BINDING_SEEN_VALUE;
                            if ((binding_seen & required) != required ||
                                (pb.btype == BIND_CHORD) !=
                                    ((binding_seen & BINDING_SEEN_KEYS) != 0) ||
                                pending_count >= ACTION_MAX_BINDINGS_PER_ACTION ||
                                total_binding_count >= ACTION_LOAD_MAX_TOTAL_BINDINGS)
                                goto cleanup;
                            if (pending_count >= pending_cap) {
                                size_t new_cap = pending_cap ? pending_cap : 8u;
                                if (new_cap <= pending_count) {
                                    if (new_cap > ACTION_MAX_BINDINGS_PER_ACTION / 2u)
                                        new_cap = ACTION_MAX_BINDINGS_PER_ACTION;
                                    else
                                        new_cap *= 2u;
                                }
                                if (new_cap <= pending_count ||
                                    new_cap > SIZE_MAX / sizeof(*pending))
                                    goto cleanup;
                                action_pending_binding *grown = (action_pending_binding *)realloc(
                                    pending, new_cap * sizeof(*pending));
                                if (!grown)
                                    goto cleanup;
                                pending = grown;
                                pending_cap = new_cap;
                            }
                            pending[pending_count++] = pb;
                            total_binding_count++;
                        }
                        tok = rt_json_stream_next(parser);
                    }
                    if (tok != RT_JSON_TOK_ARRAY_END)
                        goto cleanup;
                } else {
                    rt_json_stream_skip(parser);
                }
                tok = rt_json_stream_next(parser);
            }
            if (tok != RT_JSON_TOK_OBJECT_END)
                goto cleanup;

            if (action_seen != (ACTION_SEEN_NAME | ACTION_SEEN_TYPE | ACTION_SEEN_BINDINGS))
                goto cleanup;

            int64_t name_len = rt_str_len(pending_name);
            for (Action *existing = g_actions; existing; existing = existing->next) {
                if (existing->name_len == name_len &&
                    memcmp(existing->name, pending_name->data, (size_t)name_len) == 0)
                    goto cleanup;
            }

            building_action = (Action *)calloc(1, sizeof(Action));
            if (!building_action)
                goto cleanup;
            building_action->name = (char *)malloc((size_t)name_len + 1u);
            if (!building_action->name)
                goto cleanup;
            memcpy(building_action->name, pending_name->data, (size_t)name_len);
            building_action->name[name_len] = '\0';
            building_action->name_len = name_len;
            building_action->is_axis = is_axis;

            Binding *binding_tail = NULL;
            for (size_t pi = 0; pi < pending_count; ++pi) {
                Binding *binding = create_binding(
                    pending[pi].btype, pending[pi].code, pending[pi].pad, pending[pi].value);
                if (!binding)
                    goto cleanup;
                binding->chord_len = pending[pi].chord_len;
                memcpy(binding->chord_keys,
                       pending[pi].chord_keys,
                       (size_t)pending[pi].chord_len * sizeof(pending[pi].chord_keys[0]));

                int canonical = 1;
                switch (binding->type) {
                    case BIND_KEY:
                        canonical = binding->pad_index == 0;
                        if (!is_axis)
                            canonical = canonical && binding->value == 1.0;
                        break;
                    case BIND_MOUSE_BUTTON:
                        canonical = binding->pad_index == 0 && binding->value == 1.0;
                        break;
                    case BIND_CHORD:
                        canonical =
                            binding->code == 0 && binding->pad_index == 0 && binding->value == 1.0;
                        break;
                    case BIND_MOUSE_X:
                    case BIND_MOUSE_Y:
                    case BIND_SCROLL_X:
                    case BIND_SCROLL_Y:
                        canonical = binding->code == 0 && binding->pad_index == 0;
                        break;
                    case BIND_PAD_BUTTON:
                        canonical = binding->value == 1.0;
                        break;
                    case BIND_PAD_AXIS:
                    case BIND_PAD_BUTTON_AXIS:
                        break;
                    default:
                        canonical = 0;
                        break;
                }
                if (!canonical || !action_binding_valid(binding, is_axis)) {
                    free(binding);
                    goto cleanup;
                }
                append_binding(&building_action->bindings, &binding_tail, binding);
                building_action->binding_count++;
            }

            building_action->next = NULL;
            if (loaded_tail)
                loaded_tail->next = building_action;
            else
                g_actions = building_action;
            loaded_tail = building_action;
            building_action = NULL;
            rt_string_unref(pending_name);
            pending_name = NULL;
            tok = rt_json_stream_next(parser);
        }
        if (tok != RT_JSON_TOK_ARRAY_END)
            goto cleanup;
        tok = rt_json_stream_next(parser);
    }
    if (tok != RT_JSON_TOK_OBJECT_END)
        goto cleanup;
    tok = rt_json_stream_next(parser);
    if (tok == RT_JSON_TOK_END && saw_actions)
        success = 1;

cleanup:
    if (pending_name)
        rt_string_unref(pending_name);
    action_free_node(building_action);
    free(pending);
    action_release_json_parser(parser);
    if (success) {
        action_free_list(saved_actions);
    } else {
        action_free_list(g_actions);
        g_actions = saved_actions;
    }
    return success;
}
