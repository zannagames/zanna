//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/rt_services_input.h
// Purpose: Public C ABI for Zanna.Services.ActionInput, the provider-neutral
//          action-based controller input of a distribution platform (Steam
//          Input first): action manifests, connected controllers, action sets
//          and layers, digital and analog actions, binding origins with their
//          labels and glyphs, rumble, controller lights, and the binding
//          panel, plus the ControllerType and GlyphSize constants.
// Key invariants:
//   - Stateful entry points must run on the main thread; constant getters may
//     run on any thread.
//   - An empty controller id means every connected controller: activation
//     reaches controllers connected later, IsPressed and IsActionActive ask
//     whether any controller qualifies, AnalogX and AnalogY read the
//     controller whose vector is longest, Vibrate and the light members reach
//     each connected controller, and the remaining members use the first
//     connected controller.
//   - Empty action, action set, and layer names, negative origins, unknown
//     GlyphSize values, strengths outside 0..1, and color components outside
//     0..255 trap whether or not a provider is started.
//   - Without a started provider exposing action input every member returns
//     false, 0, -1 (GamepadIndex), or "".
//   - Constant ordinals are stable public values documented in ADR 0365.
// Ownership/Lifetime:
//   - Returned strings are caller-owned.
// Links: src/runtime/services/rt_services_input.c,
//        src/runtime/services/rt_services_provider.h,
//        docs/zannalib/services.md,
//        docs/adr/0365-platform-services-action-input.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_services_input.h
 * @brief Declares Zanna.Services.ActionInput and its constant classes.
 * @details Platforms with action-based input let the game name what the player
 *          does ("swing", "aim") in an action manifest while the player binds
 *          those actions to any controller in the platform's own interface.
 *          The game reads actions by name, groups them into action sets (a
 *          batting set, a menu set) with layers on top, and shows the glyph of
 *          whatever input the player bound.
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

/// @brief The controller type is unknown.
#define RT_SERVICES_CONTROLLER_TYPE_UNKNOWN INT64_C(0)
/// @brief Steam Controller (2015).
#define RT_SERVICES_CONTROLLER_TYPE_STEAM_CONTROLLER INT64_C(1)
/// @brief Xbox 360 controller.
#define RT_SERVICES_CONTROLLER_TYPE_XBOX360 INT64_C(2)
/// @brief Xbox One or Xbox Series controller.
#define RT_SERVICES_CONTROLLER_TYPE_XBOX_ONE INT64_C(3)
/// @brief Generic (DirectInput) gamepad.
#define RT_SERVICES_CONTROLLER_TYPE_GENERIC_GAMEPAD INT64_C(4)
/// @brief PlayStation 4 controller.
#define RT_SERVICES_CONTROLLER_TYPE_PLAYSTATION4 INT64_C(5)
/// @brief Apple MFi controller.
#define RT_SERVICES_CONTROLLER_TYPE_APPLE_MFI INT64_C(6)
/// @brief Android controller.
#define RT_SERVICES_CONTROLLER_TYPE_ANDROID INT64_C(7)
/// @brief A pair of Nintendo Switch Joy-Cons.
#define RT_SERVICES_CONTROLLER_TYPE_SWITCH_JOY_CON_PAIR INT64_C(8)
/// @brief A single Nintendo Switch Joy-Con.
#define RT_SERVICES_CONTROLLER_TYPE_SWITCH_JOY_CON_SINGLE INT64_C(9)
/// @brief Nintendo Switch Pro controller.
#define RT_SERVICES_CONTROLLER_TYPE_SWITCH_PRO INT64_C(10)
/// @brief On-screen touch controller (Steam Link).
#define RT_SERVICES_CONTROLLER_TYPE_MOBILE_TOUCH INT64_C(11)
/// @brief PlayStation 3 controller.
#define RT_SERVICES_CONTROLLER_TYPE_PLAYSTATION3 INT64_C(12)
/// @brief PlayStation 5 controller.
#define RT_SERVICES_CONTROLLER_TYPE_PLAYSTATION5 INT64_C(13)
/// @brief Steam Deck built-in controls.
#define RT_SERVICES_CONTROLLER_TYPE_STEAM_DECK INT64_C(14)
/// @brief Built-in controls of another SteamOS handheld.
#define RT_SERVICES_CONTROLLER_TYPE_STEAMOS_HANDHELD INT64_C(15)
/// @brief Nintendo Switch 2 Pro controller.
#define RT_SERVICES_CONTROLLER_TYPE_SWITCH2_PRO INT64_C(16)
/// @brief Steam Controller (2026).
#define RT_SERVICES_CONTROLLER_TYPE_STEAM_CONTROLLER_2026 INT64_C(17)
/// @brief Steam Frame controller pair.
#define RT_SERVICES_CONTROLLER_TYPE_STEAM_FRAME_CONTROLLER_PAIR INT64_C(18)

/// @brief Small glyph (32 by 32 pixels on Steam).
#define RT_SERVICES_GLYPH_SIZE_SMALL INT64_C(0)
/// @brief Medium glyph (128 by 128 pixels on Steam).
#define RT_SERVICES_GLYPH_SIZE_MEDIUM INT64_C(1)
/// @brief Large glyph (256 by 256 pixels on Steam).
#define RT_SERVICES_GLYPH_SIZE_LARGE INT64_C(2)

/// @brief Most controllers ActionInput reports at once.
#define RT_SERVICES_ACTION_INPUT_MAX_CONTROLLERS 16
/// @brief Most origins ActionInput reports for one action.
#define RT_SERVICES_ACTION_INPUT_MAX_ORIGINS 8
/// @brief Capacity, in bytes including the terminator, of a controller id.
#define RT_SERVICES_CONTROLLER_ID_CAPACITY 32

//===----------------------------------------------------------------------===//
// Zanna.Services.ActionInput
//===----------------------------------------------------------------------===//

/// @brief Start action input.
/// @details Relative manifest paths resolve against the working directory. A
///          manifest file that does not exist returns false with a diagnostic
///          and starts nothing. When the platform refuses the manifest, action
///          input still starts (IsStarted is true), false is returned with a
///          diagnostic, and the provider applies the manifest once the platform
///          accepts it (on Steam, when a controller connects).
/// @param manifest_path Action manifest file, or "" to use the configuration
///        published with the platform.
/// @return 1 when started with the manifest accepted, otherwise 0.
int8_t rt_services_action_input_start(rt_string manifest_path);

/// @brief Stop action input; controllers are no longer reported.
/// @return 1 when action input was started, otherwise 0.
int8_t rt_services_action_input_stop(void);

/// @brief Report whether action input is started.
/// @return 1 when started, otherwise 0.
int8_t rt_services_action_input_get_is_started(void);

/// @brief Count connected controllers (updated by each platform pump).
/// @return 0..16.
int64_t rt_services_action_input_get_controller_count(void);

/// @brief Read the id of the connected controller at an index.
/// @param index Index in 0..ControllerCount-1.
/// @return Caller-owned controller id, or "" outside the range.
rt_string rt_services_action_input_controller_id_at(int64_t index);

/// @brief Read a controller's type.
/// @param controller_id Controller id, or "" for the first connected controller.
/// @return ControllerType value; Unknown when unavailable.
int64_t rt_services_action_input_controller_type(rt_string controller_id);

/// @brief Read the gamepad slot a controller emulates.
/// @param controller_id Controller id, or "" for the first connected controller.
/// @return Slot index, or -1 when the controller emulates no gamepad.
int64_t rt_services_action_input_gamepad_index(rt_string controller_id);

/// @brief Activate an action set.
/// @param controller_id Controller id, or "" for every controller.
/// @param action_set Non-empty action set name from the manifest.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_activate_action_set(rt_string controller_id, rt_string action_set);

/// @brief Activate an action set layer on top of the active set.
/// @param controller_id Controller id, or "" for every controller.
/// @param layer Non-empty layer name from the manifest.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_activate_layer(rt_string controller_id, rt_string layer);

/// @brief Deactivate an action set layer.
/// @param controller_id Controller id, or "" for every controller.
/// @param layer Non-empty layer name from the manifest.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_deactivate_layer(rt_string controller_id, rt_string layer);

/// @brief Deactivate every action set layer.
/// @param controller_id Controller id, or "" for every controller.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_deactivate_all_layers(rt_string controller_id);

/// @brief Report whether a digital action is pressed.
/// @param controller_id Controller id, or "" for any connected controller.
/// @param action Non-empty digital action name.
/// @return 1 while pressed and available in the active set, otherwise 0.
int8_t rt_services_action_input_is_pressed(rt_string controller_id, rt_string action);

/// @brief Read the horizontal value of an analog action.
/// @param controller_id Controller id, or "" for the controller whose vector is longest.
/// @param action Non-empty analog action name.
/// @return Value (-1..1 for sticks, deltas for mouse-like inputs), or 0.
double rt_services_action_input_analog_x(rt_string controller_id, rt_string action);

/// @brief Read the vertical value of an analog action.
/// @param controller_id Controller id, or "" for the controller whose vector is longest.
/// @param action Non-empty analog action name.
/// @return Value, or 0.
double rt_services_action_input_analog_y(rt_string controller_id, rt_string action);

/// @brief Report whether an action is available in the active set.
/// @param controller_id Controller id, or "" for any connected controller.
/// @param action Non-empty digital or analog action name.
/// @return 1 when available, otherwise 0.
int8_t rt_services_action_input_is_action_active(rt_string controller_id, rt_string action);

/// @brief Read an action's localized name from the manifest.
/// @param action Non-empty digital or analog action name.
/// @return Caller-owned name, or "".
rt_string rt_services_action_input_action_label(rt_string action);

/// @brief Count the physical inputs bound to an action.
/// @param controller_id Controller id, or "" for the first connected controller.
/// @param action_set Action set name, or "" for the controller's current set.
/// @param action Non-empty digital or analog action name.
/// @return 0..8.
int64_t rt_services_action_input_origin_count(rt_string controller_id,
                                              rt_string action_set,
                                              rt_string action);

/// @brief Read one physical input bound to an action.
/// @param controller_id Controller id, or "" for the first connected controller.
/// @param action_set Action set name, or "" for the controller's current set.
/// @param action Non-empty digital or analog action name.
/// @param index Index in 0..OriginCount-1.
/// @return Provider-defined origin id, or 0 outside the range.
int64_t rt_services_action_input_origin_at(rt_string controller_id,
                                           rt_string action_set,
                                           rt_string action,
                                           int64_t index);

/// @brief Read an origin's localized name (for example "A Button").
/// @param origin Non-negative origin id.
/// @return Caller-owned name, or "".
rt_string rt_services_action_input_origin_label(int64_t origin);

/// @brief Read the image file of an origin's glyph.
/// @param origin Non-negative origin id.
/// @param size GlyphSize value; other values trap.
/// @return Caller-owned path of a PNG file, or "".
rt_string rt_services_action_input_origin_glyph_path(int64_t origin, int64_t size);

/// @brief Run a controller's rumble motors.
/// @param controller_id Controller id, or "" for every connected controller.
/// @param left Left (low-frequency) motor strength in 0..1; 0 stops it.
/// @param right Right (high-frequency) motor strength in 0..1; 0 stops it.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_vibrate(rt_string controller_id, double left, double right);

/// @brief Set a controller's light color.
/// @param controller_id Controller id, or "" for every connected controller.
/// @param red Red component in 0..255.
/// @param green Green component in 0..255.
/// @param blue Blue component in 0..255.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_set_led_color(rt_string controller_id,
                                              int64_t red,
                                              int64_t green,
                                              int64_t blue);

/// @brief Restore the light color the player chose.
/// @param controller_id Controller id, or "" for every connected controller.
/// @return 1 when passed to the platform, otherwise 0.
int8_t rt_services_action_input_reset_led_color(rt_string controller_id);

/// @brief Open the platform's binding panel.
/// @param controller_id Controller id, or "" for the first connected controller.
/// @return 1 when the panel opened, otherwise 0.
int8_t rt_services_action_input_show_binding_panel(rt_string controller_id);

//===----------------------------------------------------------------------===//
// Constant classes: Zanna.Services.ControllerType / GlyphSize
//===----------------------------------------------------------------------===//

/// @brief Return `Zanna.Services.ControllerType.Unknown`. @return Stable ordinal 0.
int64_t rt_services_controller_type_unknown(void);
/// @brief Return `Zanna.Services.ControllerType.SteamController`. @return Stable ordinal 1.
int64_t rt_services_controller_type_steam_controller(void);
/// @brief Return `Zanna.Services.ControllerType.Xbox360`. @return Stable ordinal 2.
int64_t rt_services_controller_type_xbox360(void);
/// @brief Return `Zanna.Services.ControllerType.XboxOne`. @return Stable ordinal 3.
int64_t rt_services_controller_type_xbox_one(void);
/// @brief Return `Zanna.Services.ControllerType.GenericGamepad`. @return Stable ordinal 4.
int64_t rt_services_controller_type_generic_gamepad(void);
/// @brief Return `Zanna.Services.ControllerType.PlayStation4`. @return Stable ordinal 5.
int64_t rt_services_controller_type_playstation4(void);
/// @brief Return `Zanna.Services.ControllerType.AppleMfi`. @return Stable ordinal 6.
int64_t rt_services_controller_type_apple_mfi(void);
/// @brief Return `Zanna.Services.ControllerType.Android`. @return Stable ordinal 7.
int64_t rt_services_controller_type_android(void);
/// @brief Return `Zanna.Services.ControllerType.SwitchJoyConPair`. @return Stable ordinal 8.
int64_t rt_services_controller_type_switch_joy_con_pair(void);
/// @brief Return `Zanna.Services.ControllerType.SwitchJoyConSingle`. @return Stable ordinal 9.
int64_t rt_services_controller_type_switch_joy_con_single(void);
/// @brief Return `Zanna.Services.ControllerType.SwitchPro`. @return Stable ordinal 10.
int64_t rt_services_controller_type_switch_pro(void);
/// @brief Return `Zanna.Services.ControllerType.MobileTouch`. @return Stable ordinal 11.
int64_t rt_services_controller_type_mobile_touch(void);
/// @brief Return `Zanna.Services.ControllerType.PlayStation3`. @return Stable ordinal 12.
int64_t rt_services_controller_type_playstation3(void);
/// @brief Return `Zanna.Services.ControllerType.PlayStation5`. @return Stable ordinal 13.
int64_t rt_services_controller_type_playstation5(void);
/// @brief Return `Zanna.Services.ControllerType.SteamDeck`. @return Stable ordinal 14.
int64_t rt_services_controller_type_steam_deck(void);
/// @brief Return `Zanna.Services.ControllerType.SteamOSHandheld`. @return Stable ordinal 15.
int64_t rt_services_controller_type_steamos_handheld(void);
/// @brief Return `Zanna.Services.ControllerType.Switch2Pro`. @return Stable ordinal 16.
int64_t rt_services_controller_type_switch2_pro(void);
/// @brief Return `Zanna.Services.ControllerType.SteamController2026`. @return Stable ordinal 17.
int64_t rt_services_controller_type_steam_controller_2026(void);
/// @brief Return `Zanna.Services.ControllerType.SteamFrameControllerPair`. @return Stable ordinal
/// 18.
int64_t rt_services_controller_type_steam_frame_controller_pair(void);

/// @brief Return `Zanna.Services.GlyphSize.Small`. @return Stable ordinal 0.
int64_t rt_services_glyph_size_small(void);
/// @brief Return `Zanna.Services.GlyphSize.Medium`. @return Stable ordinal 1.
int64_t rt_services_glyph_size_medium(void);
/// @brief Return `Zanna.Services.GlyphSize.Large`. @return Stable ordinal 2.
int64_t rt_services_glyph_size_large(void);

#ifdef __cplusplus
}
#endif
