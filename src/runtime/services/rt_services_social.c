//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_social.c
// Purpose: Implements the provider-neutral Zanna.Services.Presence, Overlay,
//          and TextInput classes and their constant classes on top of the
//          active provider's operation tables.
// Key invariants:
//   - Every stateful member checks the main thread first, then validates
//     provider-independent argument rules (trapping on empty keys, URLs, and
//     product ids, unknown constants, and out-of-range text lengths), then
//     delegates.
//   - Negative floating keyboard sizes return false with a diagnostic, since
//     they come from run-time layout values.
//   - Without an active provider exposing the operation, members return false
//     and OnScreenKeyboard.RequestText completes as failed.
// Ownership/Lifetime:
//   - OnScreenKeyboard.RequestText returns a caller-owned Zanna.Services.Request.
// Links: src/runtime/services/rt_services_social.h,
//        src/runtime/services/rt_services_internal.h,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_social.c
 * @brief Implements Zanna.Services.Presence, Overlay, and TextInput.
 */

#include "rt_services_social.h"

#include "rt_services.h"
#include "rt_services_internal.h"
#include "rt_services_provider.h"
#include "rt_string.h"

#include <string.h>

//===----------------------------------------------------------------------===//
// Helpers
//===----------------------------------------------------------------------===//

/// @brief Read the active provider's presence operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_presence_ops *social_presence_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->presence : NULL;
}

/// @brief Read the active provider's overlay operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_overlay_ops *social_overlay_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->overlay : NULL;
}

/// @brief Read the active provider's text input operations.
/// @return Operation table, or NULL when unavailable.
static const rt_services_text_input_ops *social_text_input_ops(void) {
    const rt_services_provider *provider = rt_services_internal_active_provider();
    return provider ? provider->text_input : NULL;
}

/// @brief Trap unless @p mode is a TextInputMode value.
/// @param member Class-qualified member name.
/// @param mode Candidate mode.
/// @return 1 when valid, 0 after reporting the trap.
static int social_require_text_mode(const char *member, int64_t mode) {
    if (mode >= RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE &&
        mode <= RT_SERVICES_TEXT_INPUT_MODE_PASSWORD)
        return 1;
    rt_services_internal_trap_argument(
        member, "mode must be a TextInputMode value (got %lld)", (long long)mode);
    return 0;
}

//===----------------------------------------------------------------------===//
// Zanna.Services.Presence
//===----------------------------------------------------------------------===//

/// @brief Publish one rich presence key.
/// @param key Presence key.
/// @param value Presence value; empty removes the key.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_presence_set(rt_string key, rt_string value) {
    static const char member[] = "Presence.Set";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *key_text = rt_services_internal_require_name(key, member, "key");
    if (!key_text)
        return 0;
    const rt_services_presence_ops *ops = social_presence_ops();
    return (ops && ops->set && ops->set(key_text, rt_services_internal_cstr(value))) ? 1 : 0;
}

/// @brief Remove every rich presence key.
void rt_services_presence_clear(void) {
    if (!rt_services_provider_require_main_thread("Presence.Clear"))
        return;
    const rt_services_presence_ops *ops = social_presence_ops();
    if (ops && ops->clear)
        ops->clear();
}

//===----------------------------------------------------------------------===//
// Zanna.Services.Overlay
//===----------------------------------------------------------------------===//

/// @brief Report whether the overlay is usable now.
/// @return 1 when enabled, otherwise 0.
int8_t rt_services_overlay_get_is_enabled(void) {
    if (!rt_services_provider_require_main_thread("Overlay.IsEnabled"))
        return 0;
    const rt_services_overlay_ops *ops = social_overlay_ops();
    return (ops && ops->is_enabled && ops->is_enabled()) ? 1 : 0;
}

/// @brief Open an overlay page.
/// @param page OverlayPage value.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_overlay_open(int64_t page) {
    static const char member[] = "Overlay.Open";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    if (page < RT_SERVICES_OVERLAY_PAGE_FRIENDS || page > RT_SERVICES_OVERLAY_PAGE_ACHIEVEMENTS) {
        rt_services_internal_trap_argument(
            member, "page must be an OverlayPage value (got %lld)", (long long)page);
        return 0;
    }
    const rt_services_overlay_ops *ops = social_overlay_ops();
    return (ops && ops->open_page && ops->open_page(page)) ? 1 : 0;
}

/// @brief Open a web page in the overlay browser.
/// @param url Absolute URL.
/// @param modal Nonzero for the modal browser.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_overlay_open_web_page(rt_string url, int8_t modal) {
    static const char member[] = "Overlay.OpenWebPage";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *text = rt_services_internal_require_name(url, member, "url");
    if (!text)
        return 0;
    const rt_services_overlay_ops *ops = social_overlay_ops();
    return (ops && ops->open_web_page && ops->open_web_page(text, modal ? 1 : 0)) ? 1 : 0;
}

/// @brief Open a product's store page.
/// @param product_id Provider-defined product id.
/// @param add_to_cart Nonzero to add the product to the cart.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_overlay_open_store(rt_string product_id, int8_t add_to_cart) {
    static const char member[] = "Overlay.OpenStore";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    const char *text = rt_services_internal_require_name(product_id, member, "product id");
    if (!text)
        return 0;
    const rt_services_overlay_ops *ops = social_overlay_ops();
    return (ops && ops->open_store && ops->open_store(text, add_to_cart ? 1 : 0)) ? 1 : 0;
}

/// @brief Choose the overlay notification corner.
/// @param position NotificationPosition value.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_overlay_set_notification_position(int64_t position) {
    static const char member[] = "Overlay.SetNotificationPosition";
    if (!rt_services_provider_require_main_thread(member))
        return 0;
    if (position < RT_SERVICES_NOTIFICATION_POSITION_TOP_LEFT ||
        position > RT_SERVICES_NOTIFICATION_POSITION_BOTTOM_RIGHT) {
        rt_services_internal_trap_argument(member,
                                           "position must be a NotificationPosition value (got "
                                           "%lld)",
                                           (long long)position);
        return 0;
    }
    const rt_services_overlay_ops *ops = social_overlay_ops();
    return (ops && ops->set_notification_position && ops->set_notification_position(position)) ? 1
                                                                                               : 0;
}

/// @brief Offset overlay notifications from their corner.
/// @param horizontal Horizontal inset in pixels.
/// @param vertical Vertical inset in pixels.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_overlay_set_notification_inset(int64_t horizontal, int64_t vertical) {
    if (!rt_services_provider_require_main_thread("Overlay.SetNotificationInset"))
        return 0;
    const rt_services_overlay_ops *ops = social_overlay_ops();
    return (ops && ops->set_notification_inset && ops->set_notification_inset(horizontal, vertical))
               ? 1
               : 0;
}

//===----------------------------------------------------------------------===//
// Zanna.Services.OnScreenKeyboard
//===----------------------------------------------------------------------===//

/// @brief Show the floating on-screen keyboard.
/// @param mode TextInputMode value.
/// @param x Text field left edge.
/// @param y Text field top edge.
/// @param width Text field width.
/// @param height Text field height.
/// @return 1 when shown, otherwise 0.
int8_t rt_services_on_screen_keyboard_show_floating(
    int64_t mode, int64_t x, int64_t y, int64_t width, int64_t height) {
    static const char member[] = "OnScreenKeyboard.ShowFloating";
    if (!rt_services_provider_require_main_thread(member) ||
        !social_require_text_mode(member, mode))
        return 0;
    if (width < 0 || height < 0) {
        rt_services_provider_add_diagnostic(
            "Services: OnScreenKeyboard.ShowFloating needs a non-negative width and height (got "
            "%lldx%lld)",
            (long long)width,
            (long long)height);
        return 0;
    }
    const rt_services_text_input_ops *ops = social_text_input_ops();
    return (ops && ops->show_floating && ops->show_floating(mode, x, y, width, height)) ? 1 : 0;
}

/// @brief Close the floating on-screen keyboard.
/// @return 1 when accepted, otherwise 0.
int8_t rt_services_on_screen_keyboard_dismiss_floating(void) {
    if (!rt_services_provider_require_main_thread("OnScreenKeyboard.DismissFloating"))
        return 0;
    const rt_services_text_input_ops *ops = social_text_input_ops();
    return (ops && ops->dismiss_floating && ops->dismiss_floating()) ? 1 : 0;
}

/// @brief Ask the user for text through full-screen text entry.
/// @param prompt Description shown above the field.
/// @param initial_text Starting text.
/// @param max_length Most characters the user may enter.
/// @param mode TextInputMode value.
/// @return Caller-owned request of kind TextInput.
void *rt_services_on_screen_keyboard_request_text(rt_string prompt,
                                                  rt_string initial_text,
                                                  int64_t max_length,
                                                  int64_t mode) {
    static const char member[] = "OnScreenKeyboard.RequestText";
    if (!rt_services_provider_require_main_thread(member))
        return NULL;
    if (max_length < 1 || max_length > RT_SERVICES_TEXT_INPUT_MAX_LENGTH) {
        rt_services_internal_trap_argument(member,
                                           "maxLength must be in 1..%d (got %lld)",
                                           RT_SERVICES_TEXT_INPUT_MAX_LENGTH,
                                           (long long)max_length);
        return NULL;
    }
    if (!social_require_text_mode(member, mode))
        return NULL;
    rt_services_request_args args;
    memset(&args, 0, sizeof(args));
    args.kind = RT_SERVICES_REQUEST_TEXT_INPUT;
    args.name = rt_services_internal_cstr(prompt);
    args.text = rt_services_internal_cstr(initial_text);
    args.max_length = max_length;
    args.text_mode = mode;
    return rt_services_internal_begin_request(&args, member);
}

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return OverlayPage.Friends. @return 1.
int64_t rt_services_overlay_page_friends(void) {
    return RT_SERVICES_OVERLAY_PAGE_FRIENDS;
}

/// @brief Return OverlayPage.Community. @return 2.
int64_t rt_services_overlay_page_community(void) {
    return RT_SERVICES_OVERLAY_PAGE_COMMUNITY;
}

/// @brief Return OverlayPage.Players. @return 3.
int64_t rt_services_overlay_page_players(void) {
    return RT_SERVICES_OVERLAY_PAGE_PLAYERS;
}

/// @brief Return OverlayPage.Settings. @return 4.
int64_t rt_services_overlay_page_settings(void) {
    return RT_SERVICES_OVERLAY_PAGE_SETTINGS;
}

/// @brief Return OverlayPage.OfficialGroup. @return 5.
int64_t rt_services_overlay_page_official_group(void) {
    return RT_SERVICES_OVERLAY_PAGE_OFFICIAL_GROUP;
}

/// @brief Return OverlayPage.Stats. @return 6.
int64_t rt_services_overlay_page_stats(void) {
    return RT_SERVICES_OVERLAY_PAGE_STATS;
}

/// @brief Return OverlayPage.Achievements. @return 7.
int64_t rt_services_overlay_page_achievements(void) {
    return RT_SERVICES_OVERLAY_PAGE_ACHIEVEMENTS;
}

/// @brief Return NotificationPosition.TopLeft. @return 0.
int64_t rt_services_notification_position_top_left(void) {
    return RT_SERVICES_NOTIFICATION_POSITION_TOP_LEFT;
}

/// @brief Return NotificationPosition.TopRight. @return 1.
int64_t rt_services_notification_position_top_right(void) {
    return RT_SERVICES_NOTIFICATION_POSITION_TOP_RIGHT;
}

/// @brief Return NotificationPosition.BottomLeft. @return 2.
int64_t rt_services_notification_position_bottom_left(void) {
    return RT_SERVICES_NOTIFICATION_POSITION_BOTTOM_LEFT;
}

/// @brief Return NotificationPosition.BottomRight. @return 3.
int64_t rt_services_notification_position_bottom_right(void) {
    return RT_SERVICES_NOTIFICATION_POSITION_BOTTOM_RIGHT;
}

/// @brief Return TextInputMode.SingleLine. @return 0.
int64_t rt_services_text_input_mode_single_line(void) {
    return RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE;
}

/// @brief Return TextInputMode.MultiLine. @return 1.
int64_t rt_services_text_input_mode_multi_line(void) {
    return RT_SERVICES_TEXT_INPUT_MODE_MULTI_LINE;
}

/// @brief Return TextInputMode.Email. @return 2.
int64_t rt_services_text_input_mode_email(void) {
    return RT_SERVICES_TEXT_INPUT_MODE_EMAIL;
}

/// @brief Return TextInputMode.Numeric. @return 3.
int64_t rt_services_text_input_mode_numeric(void) {
    return RT_SERVICES_TEXT_INPUT_MODE_NUMERIC;
}

/// @brief Return TextInputMode.Password. @return 4.
int64_t rt_services_text_input_mode_password(void) {
    return RT_SERVICES_TEXT_INPUT_MODE_PASSWORD;
}
