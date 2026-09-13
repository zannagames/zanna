//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_social.h
// Purpose: Public C ABI for the provider-neutral platform UI classes of
//          Zanna.Services: Presence, Overlay, and TextInput, plus the
//          OverlayPage, NotificationPosition, and TextInputMode constants.
// Key invariants:
//   - Stateful entry points must run on the main thread.
//   - Arguments that are malformed for every provider (empty keys or URLs,
//     unknown constant values, negative sizes) trap even without a started
//     provider; provider-specific limits return false and record a diagnostic.
//   - Without a provider that supports the feature every member returns false
//     and OnScreenKeyboard.RequestText completes as failed.
//   - Constant ordinals are stable public values documented in ADR 0353.
// Ownership/Lifetime:
//   - OnScreenKeyboard.RequestText returns a caller-owned Zanna.Services.Request.
// Links: src/runtime/services/rt_services_social.c,
//        src/runtime/services/rt_services.h,
//        docs/zannalib/services.md,
//        docs/adr/0353-platform-services-player-features.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_social.h
 * @brief Declares Zanna.Services.Presence, Overlay, and TextInput.
 * @details Presence publishes what the player is doing to friends. Overlay
 *          opens the platform's in-game overlay pages and positions its
 *          notifications. TextInput brings up on-screen keyboards for
 *          controller-only devices such as Steam Deck.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

//===----------------------------------------------------------------------===//
// Stable constants
//===----------------------------------------------------------------------===//

/// @brief Overlay page listing the user's friends.
#define RT_SERVICES_OVERLAY_PAGE_FRIENDS INT64_C(1)
/// @brief Overlay page for the platform community.
#define RT_SERVICES_OVERLAY_PAGE_COMMUNITY INT64_C(2)
/// @brief Overlay page listing players recently played with.
#define RT_SERVICES_OVERLAY_PAGE_PLAYERS INT64_C(3)
/// @brief Overlay settings page.
#define RT_SERVICES_OVERLAY_PAGE_SETTINGS INT64_C(4)
/// @brief Overlay page for the game's official group.
#define RT_SERVICES_OVERLAY_PAGE_OFFICIAL_GROUP INT64_C(5)
/// @brief Overlay page showing the user's stats for the game.
#define RT_SERVICES_OVERLAY_PAGE_STATS INT64_C(6)
/// @brief Overlay page showing the user's achievements for the game.
#define RT_SERVICES_OVERLAY_PAGE_ACHIEVEMENTS INT64_C(7)

/// @brief Overlay notifications appear in the top-left corner.
#define RT_SERVICES_NOTIFICATION_POSITION_TOP_LEFT INT64_C(0)
/// @brief Overlay notifications appear in the top-right corner.
#define RT_SERVICES_NOTIFICATION_POSITION_TOP_RIGHT INT64_C(1)
/// @brief Overlay notifications appear in the bottom-left corner.
#define RT_SERVICES_NOTIFICATION_POSITION_BOTTOM_LEFT INT64_C(2)
/// @brief Overlay notifications appear in the bottom-right corner.
#define RT_SERVICES_NOTIFICATION_POSITION_BOTTOM_RIGHT INT64_C(3)

/// @brief One line of text; Enter finishes entry.
#define RT_SERVICES_TEXT_INPUT_MODE_SINGLE_LINE INT64_C(0)
/// @brief Several lines of text; the user closes the keyboard explicitly.
#define RT_SERVICES_TEXT_INPUT_MODE_MULTI_LINE INT64_C(1)
/// @brief An email address layout.
#define RT_SERVICES_TEXT_INPUT_MODE_EMAIL INT64_C(2)
/// @brief A numeric layout.
#define RT_SERVICES_TEXT_INPUT_MODE_NUMERIC INT64_C(3)
/// @brief Masked single-line entry (providers without masking use SingleLine).
#define RT_SERVICES_TEXT_INPUT_MODE_PASSWORD INT64_C(4)

/// @brief Largest maximum length accepted by OnScreenKeyboard.RequestText, in bytes.
#define RT_SERVICES_TEXT_INPUT_MAX_LENGTH 4096

//===----------------------------------------------------------------------===//
// Zanna.Services.Presence
//===----------------------------------------------------------------------===//

/// @brief Publish one rich presence key for the signed-in user.
/// @param key Presence key; empty traps. Keys are provider-defined (Steam:
///            "status", "steam_display", ...).
/// @param value Presence value; the empty string removes the key.
/// @return 1 when the provider accepted the pair, otherwise 0.
int8_t rt_services_presence_set(rt_string key, rt_string value);

/// @brief Remove every rich presence key for the signed-in user.
void rt_services_presence_clear(void);

//===----------------------------------------------------------------------===//
// Zanna.Services.Overlay
//===----------------------------------------------------------------------===//

/// @brief Report whether the platform overlay is attached and usable now.
/// @return 1 when enabled, otherwise 0.
int8_t rt_services_overlay_get_is_enabled(void);

/// @brief Open an overlay page.
/// @param page OverlayPage value; other values trap.
/// @return 1 when the request was passed to the platform, otherwise 0.
int8_t rt_services_overlay_open(int64_t page);

/// @brief Open a web page in the overlay browser.
/// @param url Absolute URL including its scheme; empty traps.
/// @param modal Nonzero to show the browser alone, without other overlay windows.
/// @return 1 when the request was passed to the platform, otherwise 0.
int8_t rt_services_overlay_open_web_page(rt_string url, int8_t modal);

/// @brief Open a product's store page in the overlay.
/// @param product_id Provider-defined product id (Steam: decimal app id); empty
///                   traps, and a malformed id traps while that provider is active.
/// @param add_to_cart Nonzero to add the product to the cart as well.
/// @return 1 when the request was passed to the platform, otherwise 0.
int8_t rt_services_overlay_open_store(rt_string product_id, int8_t add_to_cart);

/// @brief Choose the screen corner where overlay notifications appear.
/// @param position NotificationPosition value; other values trap.
/// @return 1 when the setting was passed to the platform, otherwise 0.
int8_t rt_services_overlay_set_notification_position(int64_t position);

/// @brief Offset overlay notifications from their corner.
/// @param horizontal Horizontal inset in pixels.
/// @param vertical Vertical inset in pixels.
/// @return 1 when the setting was passed to the platform, otherwise 0.
int8_t rt_services_overlay_set_notification_inset(int64_t horizontal, int64_t vertical);

//===----------------------------------------------------------------------===//
// Zanna.Services.OnScreenKeyboard
//===----------------------------------------------------------------------===//

/// @brief Show a floating on-screen keyboard that types into the game's own text field.
/// @details Keys arrive as ordinary keyboard input. EventKind.TextInputDismissed
///          reports when the keyboard closes.
/// @param mode TextInputMode value; other values trap.
/// @param x Text field left edge in window pixels.
/// @param y Text field top edge in window pixels.
/// @param width Text field width in pixels; negative traps.
/// @param height Text field height in pixels; negative traps.
/// @return 1 when the keyboard was shown, otherwise 0 (for example on a desktop
///         without a controller-oriented shell).
int8_t rt_services_on_screen_keyboard_show_floating(
    int64_t mode, int64_t x, int64_t y, int64_t width, int64_t height);

/// @brief Close the floating on-screen keyboard.
/// @return 1 when the platform accepted the request, otherwise 0.
int8_t rt_services_on_screen_keyboard_dismiss_floating(void);

/// @brief Ask the user for text through the platform's full-screen text entry.
/// @details The request completes when the user submits or cancels: Succeeded
///          and Text hold the submitted text; cancelling fails the request.
/// @param prompt Description shown above the text field.
/// @param initial_text Text the field starts with.
/// @param max_length Maximum text length in bytes, 1..RT_SERVICES_TEXT_INPUT_MAX_LENGTH;
///                   other values trap.
/// @param mode TextInputMode value; other values trap.
/// @return Caller-owned Zanna.Services.Request of kind TextInput.
void *rt_services_on_screen_keyboard_request_text(rt_string prompt,
                                                  rt_string initial_text,
                                                  int64_t max_length,
                                                  int64_t mode);

//===----------------------------------------------------------------------===//
// Constant classes
//===----------------------------------------------------------------------===//

/// @brief Return `Zanna.Services.OverlayPage.Friends`.
/// @return Stable ordinal 1.
int64_t rt_services_overlay_page_friends(void);
/// @brief Return `Zanna.Services.OverlayPage.Community`.
/// @return Stable ordinal 2.
int64_t rt_services_overlay_page_community(void);
/// @brief Return `Zanna.Services.OverlayPage.Players`.
/// @return Stable ordinal 3.
int64_t rt_services_overlay_page_players(void);
/// @brief Return `Zanna.Services.OverlayPage.Settings`.
/// @return Stable ordinal 4.
int64_t rt_services_overlay_page_settings(void);
/// @brief Return `Zanna.Services.OverlayPage.OfficialGroup`.
/// @return Stable ordinal 5.
int64_t rt_services_overlay_page_official_group(void);
/// @brief Return `Zanna.Services.OverlayPage.Stats`.
/// @return Stable ordinal 6.
int64_t rt_services_overlay_page_stats(void);
/// @brief Return `Zanna.Services.OverlayPage.Achievements`.
/// @return Stable ordinal 7.
int64_t rt_services_overlay_page_achievements(void);

/// @brief Return `Zanna.Services.NotificationPosition.TopLeft`.
/// @return Stable ordinal 0.
int64_t rt_services_notification_position_top_left(void);
/// @brief Return `Zanna.Services.NotificationPosition.TopRight`.
/// @return Stable ordinal 1.
int64_t rt_services_notification_position_top_right(void);
/// @brief Return `Zanna.Services.NotificationPosition.BottomLeft`.
/// @return Stable ordinal 2.
int64_t rt_services_notification_position_bottom_left(void);
/// @brief Return `Zanna.Services.NotificationPosition.BottomRight`.
/// @return Stable ordinal 3.
int64_t rt_services_notification_position_bottom_right(void);

/// @brief Return `Zanna.Services.TextInputMode.SingleLine`.
/// @return Stable ordinal 0.
int64_t rt_services_text_input_mode_single_line(void);
/// @brief Return `Zanna.Services.TextInputMode.MultiLine`.
/// @return Stable ordinal 1.
int64_t rt_services_text_input_mode_multi_line(void);
/// @brief Return `Zanna.Services.TextInputMode.Email`.
/// @return Stable ordinal 2.
int64_t rt_services_text_input_mode_email(void);
/// @brief Return `Zanna.Services.TextInputMode.Numeric`.
/// @return Stable ordinal 3.
int64_t rt_services_text_input_mode_numeric(void);
/// @brief Return `Zanna.Services.TextInputMode.Password`.
/// @return Stable ordinal 4.
int64_t rt_services_text_input_mode_password(void);

#ifdef __cplusplus
}
#endif
