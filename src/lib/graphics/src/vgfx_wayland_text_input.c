//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_text_input.c
// Purpose: Implement optional text-input-v3 composition and surrounding-text exchange.
// Key invariants: See vgfx_wayland_text_input.h.
// Ownership/Lifetime: See vgfx_wayland_text_input.h.
// Links: src/lib/graphics/src/vgfx_wayland_text_input.h
//
//===----------------------------------------------------------------------===//

#define _POSIX_C_SOURCE 200809L

#include "vgfx_wayland_text_input.h"

#include "vgfx_internal.h"

#include <stdlib.h>
#include <string.h>

/// @file
/// @brief Implements text-input-v3 state publication and composition event batching.

/// @brief Text-input-v3 opcodes, protocol values, and local bounds.
enum {
    /// @brief Opcode for `zwp_text_input_manager_v3.get_text_input`.
    ZWP_TEXT_INPUT_MANAGER_GET_TEXT_INPUT = 1,

    /// @brief Opcode for `zwp_text_input_v3.destroy`.
    ZWP_TEXT_INPUT_DESTROY = 0,

    /// @brief Opcode for `zwp_text_input_v3.enable`.
    ZWP_TEXT_INPUT_ENABLE = 1,

    /// @brief Opcode for `zwp_text_input_v3.disable`.
    ZWP_TEXT_INPUT_DISABLE = 2,

    /// @brief Opcode for `zwp_text_input_v3.set_surrounding_text`.
    ZWP_TEXT_INPUT_SET_SURROUNDING_TEXT = 3,

    /// @brief Opcode for `zwp_text_input_v3.set_text_change_cause`.
    ZWP_TEXT_INPUT_SET_TEXT_CHANGE_CAUSE = 4,

    /// @brief Opcode for `zwp_text_input_v3.set_content_type`.
    ZWP_TEXT_INPUT_SET_CONTENT_TYPE = 5,

    /// @brief Opcode for `zwp_text_input_v3.set_cursor_rectangle`.
    ZWP_TEXT_INPUT_SET_CURSOR_RECTANGLE = 6,

    /// @brief Opcode for `zwp_text_input_v3.commit`.
    ZWP_TEXT_INPUT_COMMIT = 7,

    /// @brief Protocol cause used when the input method produced the latest text change.
    ZWP_TEXT_CHANGE_CAUSE_INPUT_METHOD = 0,

    /// @brief Content hint requesting hidden visual presentation.
    ZWP_CONTENT_HINT_HIDDEN_TEXT = 1 << 6,

    /// @brief Content hint marking surrounding text as sensitive.
    ZWP_CONTENT_HINT_SENSITIVE_DATA = 1 << 7,

    /// @brief Generic text content purpose.
    ZWP_CONTENT_PURPOSE_NORMAL = 0,

    /// @brief Numeric content purpose.
    ZWP_CONTENT_PURPOSE_NUMBER = 3,

    /// @brief Telephone-number content purpose.
    ZWP_CONTENT_PURPOSE_PHONE = 4,

    /// @brief URL content purpose.
    ZWP_CONTENT_PURPOSE_URL = 5,

    /// @brief Email-address content purpose.
    ZWP_CONTENT_PURPOSE_EMAIL = 6,

    /// @brief Password content purpose.
    ZWP_CONTENT_PURPOSE_PASSWORD = 8,

    /// @brief Marshal flag that destroys a proxy after sending its destructor.
    WL_MARSHAL_FLAG_DESTROY = 1,

    /// @brief Maximum UTF-8 byte count published as surrounding text.
    VGFX_TEXT_SURROUNDING_LIMIT = 4000,
};

/// @brief ABI callback table for all text-input-v3 events.
typedef struct {
    /// @brief Report input-method entry into a surface.
    void (*enter)(void *, struct zwp_text_input_v3 *, struct wl_proxy *);

    /// @brief Report input-method departure from a surface.
    void (*leave)(void *, struct zwp_text_input_v3 *, struct wl_proxy *);

    /// @brief Stage a preedit string and its byte-oriented cursor range.
    void (*preedit_string)(void *, struct zwp_text_input_v3 *, const char *, int32_t, int32_t);

    /// @brief Stage text committed by the input method.
    void (*commit_string)(void *, struct zwp_text_input_v3 *, const char *);

    /// @brief Stage a surrounding-text deletion around the cursor.
    void (*delete_surrounding_text)(void *, struct zwp_text_input_v3 *, uint32_t, uint32_t);

    /// @brief Publish all events staged in the current protocol batch.
    void (*done)(void *, struct zwp_text_input_v3 *, uint32_t);
} vgfx_zwp_text_input_listener_t;

/// @brief Count UTF-8 codepoint starts within a bounded byte span.
/// @details The helper assumes ordinary UTF-8 width markers and treats a truncated final
/// sequence as one codepoint, matching the bridge's defensive offset conversion.
/// @param text UTF-8 bytes to inspect, or NULL.
/// @param bytes Maximum number of bytes to consume.
/// @return Number of codepoint-sized sequences encountered.
static uint32_t vgfx_text_codepoints(const char *text, size_t bytes) {
    uint32_t count = 0;
    for (size_t i = 0; text && i < bytes;) {
        unsigned char lead = (unsigned char)text[i];
        size_t width = lead < 0x80 ? 1u : lead < 0xE0 ? 2u : lead < 0xF0 ? 3u : 4u;
        if (width > bytes - i)
            width = 1;
        i += width;
        count++;
    }
    return count;
}

/// @brief Construct and enqueue one public composition event.
/// @param text_input Bridge providing window, modifiers, and timing context.
/// @param type Composition event kind.
/// @param text UTF-8 event text, or NULL for an empty string.
/// @param selection_start Selection start in codepoints.
/// @param selection_length Selection length in codepoints.
/// @param replacement_start Surrounding-text replacement start in codepoints, or -1.
/// @param replacement_length Surrounding-text replacement length in codepoints, or -1.
static void vgfx_text_emit(vgfx_wayland_text_input_t *text_input,
                           vgfx_event_type_t type,
                           const char *text,
                           int32_t selection_start,
                           int32_t selection_length,
                           int32_t replacement_start,
                           int32_t replacement_length) {
    vgfx_event_t event;
    const char *safe = text ? text : "";
    if (vgfx_internal_init_composition_event(&event,
                                             type,
                                             vgfx_platform_now_ms(),
                                             safe,
                                             strlen(safe),
                                             selection_start,
                                             selection_length,
                                             replacement_start,
                                             replacement_length,
                                             text_input->input->modifiers))
        vgfx_internal_enqueue_event(text_input->window, &event);
}

/// @brief Commit accumulated client-side state changes to the input method.
/// @param text_input Bridge with an optional text-input proxy.
static void vgfx_text_commit_request(vgfx_wayland_text_input_t *text_input) {
    if (!text_input || !text_input->proxy)
        return;
    (void)text_input->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)text_input->proxy,
        ZWP_TEXT_INPUT_COMMIT,
        NULL,
        1,
        0);
    text_input->commit_serial++;
}

/// @brief Publish the current surrounding text, purpose, and cursor rectangle.
/// @details The request sequence is emitted only while the protocol context is enabled and
/// ends with a text-input commit.
/// @param text_input Enabled bridge whose public state should be published.
static void vgfx_text_publish_state(vgfx_wayland_text_input_t *text_input) {
    if (!text_input || !text_input->proxy || !text_input->protocol_enabled)
        return;
    const char *surrounding = text_input->surrounding ? text_input->surrounding : "";
    (void)text_input->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)text_input->proxy,
        ZWP_TEXT_INPUT_SET_SURROUNDING_TEXT,
        NULL,
        1,
        0,
        surrounding,
        text_input->cursor_byte,
        text_input->anchor_byte);
    (void)text_input->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)text_input->proxy,
        ZWP_TEXT_INPUT_SET_TEXT_CHANGE_CAUSE,
        NULL,
        1,
        0,
        ZWP_TEXT_CHANGE_CAUSE_INPUT_METHOD);
    uint32_t hints = 0;
    uint32_t purpose = ZWP_CONTENT_PURPOSE_NORMAL;
    switch (text_input->state.purpose) {
    case VGFX_TEXT_INPUT_PASSWORD:
        hints = ZWP_CONTENT_HINT_HIDDEN_TEXT | ZWP_CONTENT_HINT_SENSITIVE_DATA;
        purpose = ZWP_CONTENT_PURPOSE_PASSWORD;
        break;
    case VGFX_TEXT_INPUT_EMAIL: purpose = ZWP_CONTENT_PURPOSE_EMAIL; break;
    case VGFX_TEXT_INPUT_NUMBER: purpose = ZWP_CONTENT_PURPOSE_NUMBER; break;
    case VGFX_TEXT_INPUT_PHONE: purpose = ZWP_CONTENT_PURPOSE_PHONE; break;
    case VGFX_TEXT_INPUT_URL: purpose = ZWP_CONTENT_PURPOSE_URL; break;
    default: break;
    }
    (void)text_input->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)text_input->proxy,
        ZWP_TEXT_INPUT_SET_CONTENT_TYPE,
        NULL,
        1,
        0,
        hints,
        purpose);
    (void)text_input->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)text_input->proxy,
        ZWP_TEXT_INPUT_SET_CURSOR_RECTANGLE,
        NULL,
        1,
        0,
        text_input->state.cursor_x,
        text_input->state.cursor_y,
        text_input->state.cursor_width,
        text_input->state.cursor_height);
    vgfx_text_commit_request(text_input);
}

/// @brief Reconcile desired enablement with input-method surface focus.
/// @details Protocol enablement requires both application intent and a matching `enter` event.
/// Disabling commits the request and cancels any public composition still in progress.
/// @param text_input Bridge whose enable state should be reconciled.
static void vgfx_text_apply_enabled(vgfx_wayland_text_input_t *text_input) {
    int should_enable = text_input->desired_enabled && text_input->entered;
    if (should_enable == text_input->protocol_enabled || !text_input->proxy)
        return;
    (void)text_input->connection->api.proxy_marshal_flags(
        (struct wl_proxy *)text_input->proxy,
        should_enable ? ZWP_TEXT_INPUT_ENABLE : ZWP_TEXT_INPUT_DISABLE,
        NULL,
        1,
        0);
    text_input->protocol_enabled = should_enable;
    if (should_enable)
        vgfx_text_publish_state(text_input);
    else {
        vgfx_text_commit_request(text_input);
        if (text_input->composition_active)
            vgfx_text_emit(text_input, VGFX_EVENT_COMPOSITION_CANCEL, "", 0, 0, -1, -1);
        text_input->composition_active = 0;
    }
}

/// @brief Record input-method entry into the window surface and reconcile enablement.
/// @param data Registered @ref vgfx_wayland_text_input_t callback context.
/// @param proxy Text-input proxy that emitted the event.
/// @param surface Surface entered by the input method.
static void vgfx_text_enter(void *data,
                            struct zwp_text_input_v3 *proxy,
                            struct wl_proxy *surface) {
    (void)proxy;
    vgfx_wayland_text_input_t *text_input = data;
    if (text_input && surface == text_input->input->surface) {
        text_input->entered = 1;
        vgfx_text_apply_enabled(text_input);
    }
}

/// @brief Clear protocol enablement when the input method leaves the surface.
/// @details Any active public composition is cancelled because further updates no longer
/// belong to this window.
/// @param data Registered @ref vgfx_wayland_text_input_t callback context.
/// @param proxy Text-input proxy that emitted the event.
/// @param surface Surface left by the input method.
static void vgfx_text_leave(void *data,
                            struct zwp_text_input_v3 *proxy,
                            struct wl_proxy *surface) {
    (void)proxy;
    (void)surface;
    vgfx_wayland_text_input_t *text_input = data;
    if (!text_input)
        return;
    text_input->entered = 0;
    text_input->protocol_enabled = 0;
    if (text_input->composition_active)
        vgfx_text_emit(text_input, VGFX_EVENT_COMPOSITION_CANCEL, "", 0, 0, -1, -1);
    text_input->composition_active = 0;
}

/// @brief Replace the preedit string staged for the current protocol batch.
/// @param data Registered @ref vgfx_wayland_text_input_t callback context.
/// @param proxy Text-input proxy that emitted the event.
/// @param text Nullable UTF-8 preedit string.
/// @param begin Selection start byte offset, or a negative protocol sentinel.
/// @param end Selection end byte offset, or a negative protocol sentinel.
static void vgfx_text_preedit(void *data,
                              struct zwp_text_input_v3 *proxy,
                              const char *text,
                              int32_t begin,
                              int32_t end) {
    (void)proxy;
    vgfx_wayland_text_input_t *text_input = data;
    if (!text_input)
        return;
    free(text_input->pending_preedit);
    text_input->pending_preedit = strdup(text ? text : "");
    text_input->preedit_begin_byte = begin;
    text_input->preedit_end_byte = end;
    text_input->have_preedit = text_input->pending_preedit != NULL;
}

/// @brief Replace the committed text staged for the current protocol batch.
/// @param data Registered @ref vgfx_wayland_text_input_t callback context.
/// @param proxy Text-input proxy that emitted the event.
/// @param text Nullable UTF-8 committed string.
static void vgfx_text_commit(void *data,
                             struct zwp_text_input_v3 *proxy,
                             const char *text) {
    (void)proxy;
    vgfx_wayland_text_input_t *text_input = data;
    if (!text_input)
        return;
    free(text_input->pending_commit);
    text_input->pending_commit = strdup(text ? text : "");
    text_input->have_commit = text_input->pending_commit != NULL;
}

/// @brief Stage a surrounding-text deletion for the current protocol batch.
/// @param data Registered @ref vgfx_wayland_text_input_t callback context.
/// @param proxy Text-input proxy that emitted the event.
/// @param before Number of UTF-8 bytes to remove before the cursor.
/// @param after Number of UTF-8 bytes to remove after the cursor.
static void vgfx_text_delete(void *data,
                             struct zwp_text_input_v3 *proxy,
                             uint32_t before,
                             uint32_t after) {
    (void)proxy;
    vgfx_wayland_text_input_t *text_input = data;
    if (!text_input)
        return;
    text_input->delete_before_bytes = before;
    text_input->delete_after_bytes = after;
    text_input->have_delete = 1;
}

/// @brief Translate and publish the complete compositor event batch.
/// @details Deletion byte counts are converted into codepoint replacement ranges. Commit text
/// takes precedence over preedit in the same batch; preedit starts, updates, or cancels the
/// public composition lifecycle as needed. All owned staging buffers and flags are cleared.
/// @param data Registered @ref vgfx_wayland_text_input_t callback context.
/// @param proxy Text-input proxy that emitted the event.
/// @param serial Compositor batch serial, currently not used for client policy.
static void vgfx_text_done(void *data, struct zwp_text_input_v3 *proxy, uint32_t serial) {
    (void)proxy;
    (void)serial;
    vgfx_wayland_text_input_t *text_input = data;
    if (!text_input)
        return;
    int32_t replacement_start = -1;
    int32_t replacement_length = -1;
    if (text_input->have_delete && text_input->surrounding) {
        size_t length = strlen(text_input->surrounding);
        size_t cursor = (size_t)text_input->cursor_byte;
        size_t start = text_input->delete_before_bytes > cursor
                           ? 0
                           : cursor - text_input->delete_before_bytes;
        size_t end = cursor + text_input->delete_after_bytes;
        if (end > length)
            end = length;
        replacement_start = (int32_t)vgfx_text_codepoints(text_input->surrounding, start);
        replacement_length =
            (int32_t)vgfx_text_codepoints(text_input->surrounding + start, end - start);
    }
    if (text_input->have_commit) {
        if (!text_input->composition_active)
            vgfx_text_emit(text_input,
                           VGFX_EVENT_COMPOSITION_START,
                           "",
                           0,
                           0,
                           replacement_start,
                           replacement_length);
        vgfx_text_emit(text_input,
                       VGFX_EVENT_COMPOSITION_COMMIT,
                       text_input->pending_commit,
                       0,
                       0,
                       replacement_start,
                       replacement_length);
        text_input->composition_active = 0;
    } else if (text_input->have_preedit) {
        const char *preedit = text_input->pending_preedit ? text_input->pending_preedit : "";
        if (preedit[0]) {
            if (!text_input->composition_active)
                vgfx_text_emit(text_input,
                               VGFX_EVENT_COMPOSITION_START,
                               "",
                               0,
                               0,
                               replacement_start,
                               replacement_length);
            size_t length = strlen(preedit);
            size_t begin = text_input->preedit_begin_byte < 0
                               ? length
                               : (size_t)text_input->preedit_begin_byte;
            size_t end = text_input->preedit_end_byte < 0
                             ? begin
                             : (size_t)text_input->preedit_end_byte;
            if (begin > length) begin = length;
            if (end > length) end = length;
            int32_t selection_start = (int32_t)vgfx_text_codepoints(preedit, begin);
            int32_t selection_end = (int32_t)vgfx_text_codepoints(preedit, end);
            vgfx_text_emit(text_input,
                           VGFX_EVENT_COMPOSITION_UPDATE,
                           preedit,
                           selection_start,
                           selection_end >= selection_start ? selection_end - selection_start : 0,
                           replacement_start,
                           replacement_length);
            text_input->composition_active = 1;
        } else if (text_input->composition_active) {
            vgfx_text_emit(text_input, VGFX_EVENT_COMPOSITION_CANCEL, "", 0, 0, -1, -1);
            text_input->composition_active = 0;
        }
    }
    free(text_input->pending_preedit);
    free(text_input->pending_commit);
    text_input->pending_preedit = NULL;
    text_input->pending_commit = NULL;
    text_input->have_preedit = 0;
    text_input->have_commit = 0;
    text_input->have_delete = 0;
}

static const vgfx_zwp_text_input_listener_t g_text_listener = {
    vgfx_text_enter, vgfx_text_leave, vgfx_text_preedit,
    vgfx_text_commit, vgfx_text_delete, vgfx_text_done};

/// @copydoc vgfx_wayland_text_input_open
int vgfx_wayland_text_input_open(vgfx_wayland_text_input_t *text_input,
                                 vgfx_wayland_connection_t *connection,
                                 vgfx_wayland_input_t *input,
                                 struct vgfx_window *window) {
    if (!text_input || !connection || !input || !window)
        return 0;
    memset(text_input, 0, sizeof(*text_input));
    text_input->connection = connection;
    text_input->input = input;
    text_input->window = window;
    if (!connection->text_input_manager_v3)
        return 1;
    text_input->proxy =
        (struct zwp_text_input_v3 *)connection->api.proxy_marshal_flags(
            (struct wl_proxy *)connection->text_input_manager_v3,
            ZWP_TEXT_INPUT_MANAGER_GET_TEXT_INPUT,
            &vgfx_zwp_text_input_v3_interface,
            1,
            0,
            NULL,
            connection->seat);
    return text_input->proxy &&
           connection->api.proxy_add_listener((struct wl_proxy *)text_input->proxy,
                                              (void (**)(void))(void *)&g_text_listener,
                                              text_input) == 0;
}

/// @copydoc vgfx_wayland_text_input_close
void vgfx_wayland_text_input_close(vgfx_wayland_text_input_t *text_input) {
    if (!text_input)
        return;
    if (text_input->proxy && text_input->connection)
        (void)text_input->connection->api.proxy_marshal_flags(
            (struct wl_proxy *)text_input->proxy,
            ZWP_TEXT_INPUT_DESTROY,
            NULL,
            1,
            WL_MARSHAL_FLAG_DESTROY);
    free(text_input->surrounding);
    free(text_input->pending_preedit);
    free(text_input->pending_commit);
    memset(text_input, 0, sizeof(*text_input));
}

/// @copydoc vgfx_wayland_text_input_set_enabled
int vgfx_wayland_text_input_set_enabled(vgfx_wayland_text_input_t *text_input, int32_t enabled) {
    if (!text_input)
        return 0;
    text_input->desired_enabled = enabled ? 1 : 0;
    vgfx_text_apply_enabled(text_input);
    return 1;
}

/// @copydoc vgfx_wayland_text_input_set_state
int vgfx_wayland_text_input_set_state(vgfx_wayland_text_input_t *text_input,
                                      const vgfx_text_input_state_t *state) {
    if (!text_input || !state)
        return 0;
    const char *source = state->surrounding_text ? state->surrounding_text : "";
    size_t length = strlen(source);
    size_t cursor = state->cursor_byte < 0 ? 0 : (size_t)state->cursor_byte;
    size_t anchor = state->anchor_byte < 0 ? 0 : (size_t)state->anchor_byte;
    if (cursor > length)
        cursor = length;
    if (anchor > length)
        anchor = length;
    while (cursor > 0 && cursor < length &&
           ((unsigned char)source[cursor] & 0xC0u) == 0x80u)
        cursor--;
    while (anchor > 0 && anchor < length &&
           ((unsigned char)source[anchor] & 0xC0u) == 0x80u)
        anchor--;
    size_t start = 0;
    size_t end = length;
    if (length > VGFX_TEXT_SURROUNDING_LIMIT) {
        start = cursor > VGFX_TEXT_SURROUNDING_LIMIT / 2
                    ? cursor - VGFX_TEXT_SURROUNDING_LIMIT / 2
                    : 0;
        while (start < length && ((unsigned char)source[start] & 0xC0u) == 0x80u)
            start++;
        end = start + VGFX_TEXT_SURROUNDING_LIMIT;
        if (end > length)
            end = length;
        while (end > start && end < length &&
               ((unsigned char)source[end] & 0xC0u) == 0x80u)
            end--;
        if (anchor < start || anchor > end)
            anchor = cursor;
    }
    char *copy = malloc(end - start + 1);
    if (!copy)
        return 0;
    memcpy(copy, source + start, end - start);
    copy[end - start] = '\0';
    free(text_input->surrounding);
    text_input->surrounding = copy;
    text_input->cursor_byte = (int32_t)(cursor - start);
    text_input->anchor_byte = (int32_t)(anchor - start);
    text_input->state = *state;
    text_input->state.surrounding_text = text_input->surrounding;
    vgfx_text_publish_state(text_input);
    return 1;
}
