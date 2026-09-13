//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam_social.c
// Purpose: Binds the Steam provider's platform UI features: rich presence and
//          overlay pages (ISteamFriends), overlay status and notification
//          placement, and the floating and full-screen gamepad keyboards
//          (ISteamUtils).
// Key invariants:
//   - Each feature group is usable only when every export it calls resolved;
//     otherwise one diagnostic names the first missing export.
//   - At most one full-screen text input request is pending; its completion
//     arrives as GamepadTextInputDismissed_t, not as a call result.
//   - Pixel and length arguments outside int32 are rejected with a
//     diagnostic before calling Steam.
// Ownership/Lifetime:
//   - Submitted text is copied into the completed request.
// Links: src/runtime/services/steam/rt_steam_internal.h,
//        src/runtime/services/rt_services_social.h,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam_social.c
 * @brief Implements Steam rich presence, overlay control, and text input.
 */

#include "rt_platform.h"
#include "rt_services.h"
#include "rt_services_provider.h"
#include "rt_services_social.h"
#include "rt_steam_abi.h"
#include "rt_steam_internal.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @brief Largest submitted text length the provider reads back, in bytes.
#define STEAM_MAX_SUBMITTED_TEXT 65536

//===----------------------------------------------------------------------===//
// Binding
//===----------------------------------------------------------------------===//

/// @brief Resolve one export into a typed field, remembering the first missing name.
#define STEAM_BIND(field, type, name, missing)                                                     \
    do {                                                                                           \
        void *symbol_ = rt_services_steam_symbol(name);                                            \
        if (!symbol_ && !(missing))                                                                \
            (missing) = (name);                                                                    \
        (field) = symbol_ ? RT_FN_PTR_CAST((type)symbol_) : NULL;                                  \
    } while (0)

/// @brief Bind the presence, overlay, name, and text input groups.
void rt_services_steam_bind_social(void) {
    steam_friends_api *friends = &g_steam.friends;
    steam_utils_api *utils = &g_steam.utils;
    const char *missing = NULL;

    if (friends->self) {
        STEAM_BIND(friends->set_rich_presence,
                   rt_steam_self_str_str_bool_fn,
                   RT_STEAM_SYMBOL_FRIENDS_SET_RICH_PRESENCE,
                   missing);
        STEAM_BIND(friends->clear_rich_presence,
                   rt_steam_self_void_fn,
                   RT_STEAM_SYMBOL_FRIENDS_CLEAR_RICH_PRESENCE,
                   missing);
        friends->presence_ready = missing == NULL;
        if (missing)
            rt_services_steam_report_missing(missing, "rich presence");

        missing = NULL;
        STEAM_BIND(friends->activate_overlay,
                   rt_steam_activate_overlay_fn,
                   RT_STEAM_SYMBOL_FRIENDS_ACTIVATE_OVERLAY,
                   missing);
        STEAM_BIND(friends->activate_web_page,
                   rt_steam_activate_web_page_fn,
                   RT_STEAM_SYMBOL_FRIENDS_ACTIVATE_WEB_PAGE,
                   missing);
        STEAM_BIND(friends->activate_store,
                   rt_steam_activate_store_fn,
                   RT_STEAM_SYMBOL_FRIENDS_ACTIVATE_STORE,
                   missing);
        friends->overlay_ready = missing == NULL;
        if (missing)
            rt_services_steam_report_missing(missing, "overlay pages");

        missing = NULL;
        STEAM_BIND(friends->friend_persona_name,
                   rt_steam_friend_persona_name_fn,
                   RT_STEAM_SYMBOL_FRIENDS_FRIEND_PERSONA_NAME,
                   missing);
        STEAM_BIND(friends->request_user_information,
                   rt_steam_request_user_information_fn,
                   RT_STEAM_SYMBOL_FRIENDS_REQUEST_USER_INFO,
                   missing);
        friends->names_ready = missing == NULL;
        if (missing)
            rt_services_steam_report_missing(missing, "leaderboard user names");
    }

    if (utils->self) {
        missing = NULL;
        STEAM_BIND(utils->overlay_enabled,
                   rt_steam_self_bool_fn,
                   RT_STEAM_SYMBOL_UTILS_OVERLAY_ENABLED,
                   missing);
        STEAM_BIND(utils->notification_position,
                   rt_steam_notification_position_fn,
                   RT_STEAM_SYMBOL_UTILS_NOTIFICATION_POSITION,
                   missing);
        STEAM_BIND(utils->notification_inset,
                   rt_steam_notification_inset_fn,
                   RT_STEAM_SYMBOL_UTILS_NOTIFICATION_INSET,
                   missing);
        utils->overlay_ready = missing == NULL;
        if (missing)
            rt_services_steam_report_missing(missing, "overlay status and notification placement");

        missing = NULL;
        STEAM_BIND(utils->show_floating,
                   rt_steam_show_floating_input_fn,
                   RT_STEAM_SYMBOL_UTILS_SHOW_FLOATING_INPUT,
                   missing);
        STEAM_BIND(utils->dismiss_floating,
                   rt_steam_self_bool_fn,
                   RT_STEAM_SYMBOL_UTILS_DISMISS_FLOATING_INPUT,
                   missing);
        STEAM_BIND(utils->show_gamepad_input,
                   rt_steam_show_gamepad_input_fn,
                   RT_STEAM_SYMBOL_UTILS_SHOW_GAMEPAD_INPUT,
                   missing);
        STEAM_BIND(utils->entered_text_length,
                   rt_steam_self_u32_fn,
                   RT_STEAM_SYMBOL_UTILS_ENTERED_TEXT_LENGTH,
                   missing);
        STEAM_BIND(utils->entered_text,
                   rt_steam_entered_gamepad_text_fn,
                   RT_STEAM_SYMBOL_UTILS_ENTERED_TEXT,
                   missing);
        utils->text_input_ready = missing == NULL;
        if (missing)
            rt_services_steam_report_missing(missing, "text input");
    }
}

#undef STEAM_BIND

/// @brief Report whether a pixel value fits the flat API's int parameters.
/// @param value Candidate value.
/// @return 1 when it fits in int32.
static int steam_fits_int(int64_t value) {
    return value >= INT32_MIN && value <= INT32_MAX;
}

//===----------------------------------------------------------------------===//
// Presence
//===----------------------------------------------------------------------===//

/// @brief Set one rich presence key.
/// @param key Non-empty key.
/// @param value Value; empty removes the key.
/// @return 1 when accepted, otherwise 0.
static int8_t steam_presence_set(const char *key, const char *value) {
    if (!g_steam.started || !g_steam.friends.presence_ready)
        return 0;
    if (g_steam.friends.set_rich_presence(g_steam.friends.self, key, value))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: SetRichPresence('%s') failed; keys must be shorter than 64 bytes, values shorter "
        "than 256 bytes, and at most 30 keys may be set",
        key);
    return 0;
}

/// @brief Remove every rich presence key.
static void steam_presence_clear(void) {
    if (g_steam.started && g_steam.friends.presence_ready)
        g_steam.friends.clear_rich_presence(g_steam.friends.self);
}

/// @brief Steam presence operations.
const rt_services_presence_ops rt_services_steam_presence_ops = {
    .set = steam_presence_set,
    .clear = steam_presence_clear,
};

//===----------------------------------------------------------------------===//
// Overlay
//===----------------------------------------------------------------------===//

/// @brief Report whether the overlay is attached and usable.
/// @return 1 when enabled, otherwise 0.
static int8_t steam_overlay_is_enabled(void) {
    if (!g_steam.started || !g_steam.utils.overlay_ready)
        return 0;
    return g_steam.utils.overlay_enabled(g_steam.utils.self) ? 1 : 0;
}

/// @brief Open an overlay page.
/// @param page RT_SERVICES_OVERLAY_PAGE_* value.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_overlay_open_page(int64_t page) {
    static const char *const dialogs[] = {"friends",
                                          "community",
                                          "players",
                                          "settings",
                                          "officialgamegroup",
                                          "stats",
                                          "achievements"};
    if (!g_steam.started || !g_steam.friends.overlay_ready)
        return 0;
    if (page < RT_SERVICES_OVERLAY_PAGE_FRIENDS || page > RT_SERVICES_OVERLAY_PAGE_ACHIEVEMENTS)
        return 0;
    g_steam.friends.activate_overlay(g_steam.friends.self,
                                     dialogs[page - RT_SERVICES_OVERLAY_PAGE_FRIENDS]);
    return 1;
}

/// @brief Open a web page in the overlay browser.
/// @param url Non-empty URL.
/// @param modal Nonzero for the modal browser.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_overlay_open_web_page(const char *url, int8_t modal) {
    if (!g_steam.started || !g_steam.friends.overlay_ready)
        return 0;
    g_steam.friends.activate_web_page(g_steam.friends.self,
                                      url,
                                      modal ? RT_STEAM_WEB_PAGE_MODE_MODAL
                                            : RT_STEAM_WEB_PAGE_MODE_DEFAULT);
    return 1;
}

/// @brief Open an app's store page.
/// @param product_id Decimal Steam app id; a malformed id traps.
/// @param add_to_cart Nonzero to add the app to the cart and show the page.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_overlay_open_store(const char *product_id, int8_t add_to_cart) {
    uint32_t app_id = 0;
    if (!rt_services_steam_parse_id(product_id, &app_id)) {
        char message[256];
        snprintf(message,
                 sizeof(message),
                 "Services.Overlay.OpenStore: Steam app id '%s' must be an integer in "
                 "1..4294967295",
                 product_id);
        rt_trap(message);
        return 0;
    }
    if (!g_steam.started || !g_steam.friends.overlay_ready)
        return 0;
    g_steam.friends.activate_store(g_steam.friends.self,
                                   app_id,
                                   add_to_cart ? RT_STEAM_STORE_FLAG_ADD_TO_CART_AND_SHOW
                                               : RT_STEAM_STORE_FLAG_NONE);
    return 1;
}

/// @brief Set the notification corner.
/// @param position RT_SERVICES_NOTIFICATION_POSITION_* value (same ordinals as
/// ENotificationPosition).
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_overlay_set_notification_position(int64_t position) {
    if (!g_steam.started || !g_steam.utils.overlay_ready)
        return 0;
    g_steam.utils.notification_position(g_steam.utils.self, (int)position);
    return 1;
}

/// @brief Set the notification inset.
/// @param horizontal Horizontal inset in pixels.
/// @param vertical Vertical inset in pixels.
/// @return 1 when passed to Steam, otherwise 0.
static int8_t steam_overlay_set_notification_inset(int64_t horizontal, int64_t vertical) {
    if (!g_steam.started || !g_steam.utils.overlay_ready)
        return 0;
    if (!steam_fits_int(horizontal) || !steam_fits_int(vertical)) {
        rt_services_provider_add_diagnostic(
            "Steam: notification inset %lldx%lld is outside the int32 range",
            (long long)horizontal,
            (long long)vertical);
        return 0;
    }
    g_steam.utils.notification_inset(g_steam.utils.self, (int)horizontal, (int)vertical);
    return 1;
}

/// @brief Steam overlay operations.
const rt_services_overlay_ops rt_services_steam_overlay_ops = {
    .is_enabled = steam_overlay_is_enabled,
    .open_page = steam_overlay_open_page,
    .open_web_page = steam_overlay_open_web_page,
    .open_store = steam_overlay_open_store,
    .set_notification_position = steam_overlay_set_notification_position,
    .set_notification_inset = steam_overlay_set_notification_inset,
};

//===----------------------------------------------------------------------===//
// Text input
//===----------------------------------------------------------------------===//

/// @brief Show the floating keyboard.
/// @param mode RT_SERVICES_TEXT_INPUT_MODE_* value.
/// @param x Text field left edge.
/// @param y Text field top edge.
/// @param width Text field width.
/// @param height Text field height.
/// @return 1 when shown, otherwise 0.
static int8_t steam_text_input_show_floating(
    int64_t mode, int64_t x, int64_t y, int64_t width, int64_t height) {
    if (!g_steam.started || !g_steam.utils.text_input_ready)
        return 0;
    if (!steam_fits_int(x) || !steam_fits_int(y) || !steam_fits_int(width) ||
        !steam_fits_int(height)) {
        rt_services_provider_add_diagnostic(
            "Steam: floating keyboard field %lld,%lld %lldx%lld is outside the int32 range",
            (long long)x,
            (long long)y,
            (long long)width,
            (long long)height);
        return 0;
    }
    int keyboard_mode = RT_STEAM_FLOATING_INPUT_SINGLE_LINE;
    if (mode == RT_SERVICES_TEXT_INPUT_MODE_MULTI_LINE)
        keyboard_mode = RT_STEAM_FLOATING_INPUT_MULTIPLE_LINES;
    else if (mode == RT_SERVICES_TEXT_INPUT_MODE_EMAIL)
        keyboard_mode = RT_STEAM_FLOATING_INPUT_EMAIL;
    else if (mode == RT_SERVICES_TEXT_INPUT_MODE_NUMERIC)
        keyboard_mode = RT_STEAM_FLOATING_INPUT_NUMERIC;
    if (g_steam.utils.show_floating(
            g_steam.utils.self, keyboard_mode, (int)x, (int)y, (int)width, (int)height))
        return 1;
    rt_services_provider_add_diagnostic(
        "Steam: ShowFloatingGamepadTextInput returned false; the floating keyboard needs Steam "
        "Deck or Big Picture mode");
    return 0;
}

/// @brief Dismiss the floating keyboard.
/// @return 1 when accepted, otherwise 0.
static int8_t steam_text_input_dismiss_floating(void) {
    if (!g_steam.started || !g_steam.utils.text_input_ready)
        return 0;
    return g_steam.utils.dismiss_floating(g_steam.utils.self) ? 1 : 0;
}

/// @brief Steam text input operations.
const rt_services_text_input_ops rt_services_steam_text_input_ops = {
    .show_floating = steam_text_input_show_floating,
    .dismiss_floating = steam_text_input_dismiss_floating,
};

/// @brief Start a full-screen gamepad text input request.
/// @param args Validated TextInput arguments.
/// @param out_handle Receives the provider token.
/// @param message Receives the failure message.
/// @param message_capacity Size of @p message in bytes.
/// @return 1 when the keyboard was shown, otherwise 0.
int8_t rt_services_steam_begin_text_input(const rt_services_request_args *args,
                                          uint64_t *out_handle,
                                          char *message,
                                          size_t message_capacity) {
    if (!g_steam.started || !g_steam.utils.text_input_ready) {
        snprintf(message,
                 message_capacity,
                 "Steam: text input is unavailable (see Platform.Diagnostics)");
        return 0;
    }
    if (rt_services_steam_op_find_stage(STEAM_OP_TEXT_INPUT)) {
        snprintf(message, message_capacity, "Steam: a text input request is already pending");
        return 0;
    }
    steam_request_op *op = rt_services_steam_op_alloc(STEAM_OP_TEXT_INPUT);
    if (!op) {
        snprintf(message, message_capacity, "Steam: too many pending requests");
        return 0;
    }
    const int password = args->text_mode == RT_SERVICES_TEXT_INPUT_MODE_PASSWORD;
    const int multi_line = args->text_mode == RT_SERVICES_TEXT_INPUT_MODE_MULTI_LINE;
    if (!g_steam.utils.show_gamepad_input(
            g_steam.utils.self,
            password ? RT_STEAM_GAMEPAD_INPUT_PASSWORD : RT_STEAM_GAMEPAD_INPUT_NORMAL,
            multi_line ? RT_STEAM_GAMEPAD_LINE_MULTIPLE : RT_STEAM_GAMEPAD_LINE_SINGLE,
            args->name,
            (uint32_t)args->max_length,
            args->text)) {
        rt_services_steam_op_free(op);
        snprintf(message,
                 message_capacity,
                 "Steam: the gamepad text input could not be shown; it needs Steam Deck or Big "
                 "Picture mode");
        return 0;
    }
    *out_handle = op->token;
    return 1;
}

/// @brief Complete the pending text input request from GamepadTextInputDismissed_t.
/// @param payload Decoded dismissal record.
static void steam_text_input_dismissed(const rt_steam_gamepad_text_input_dismissed *payload) {
    steam_request_op *op = rt_services_steam_op_find_stage(STEAM_OP_TEXT_INPUT);
    if (!op)
        return;
    if (!payload->submitted) {
        rt_services_steam_op_fail(op, "Steam: text input was cancelled");
        return;
    }
    uint32_t length = g_steam.utils.entered_text_length(g_steam.utils.self);
    if (length > STEAM_MAX_SUBMITTED_TEXT) {
        rt_services_steam_op_fail(op, "Steam: the submitted text is too long to read");
        return;
    }
    // Allocate one byte beyond the reported length so the text is terminated
    // whether or not Steam counts the terminator.
    size_t capacity = (size_t)length + 1u;
    char *text = (char *)calloc(capacity, 1);
    if (!text) {
        rt_services_steam_op_fail(op, "Steam: out of memory reading the submitted text");
        return;
    }
    if (!g_steam.utils.entered_text(g_steam.utils.self, text, (uint32_t)capacity)) {
        free(text);
        rt_services_steam_op_fail(op, "Steam: the submitted text could not be read");
        return;
    }
    text[capacity - 1] = '\0';
    rt_services_request_result result;
    memset(&result, 0, sizeof(result));
    result.succeeded = 1;
    result.result_code = RT_STEAM_RESULT_OK;
    result.value = (int64_t)strlen(text);
    result.text = text;
    rt_services_provider_finish_request(op->token, &result);
    rt_services_steam_op_free(op);
    free(text);
}

/// @brief Decode text input callbacks.
/// @param msg Dispatched callback.
/// @return 1 when @p msg was a text input callback, otherwise 0.
int rt_services_steam_social_callback(const rt_steam_callback_msg *msg) {
    switch (msg->callback_id) {
        case RT_STEAM_CB_GAMEPAD_TEXT_INPUT_DISMISSED: {
            rt_steam_gamepad_text_input_dismissed payload;
            if (rt_services_steam_payload_matches(msg, sizeof(payload))) {
                memcpy(&payload, msg->param, sizeof(payload));
                steam_text_input_dismissed(&payload);
            }
            return 1;
        }
        case RT_STEAM_CB_FLOATING_GAMEPAD_TEXT_INPUT_DISMISSED:
            rt_services_provider_emit_event(RT_SERVICES_EVENT_TEXT_INPUT_DISMISSED, 0, 0, 0, NULL);
            return 1;
        default:
            return 0;
    }
}
