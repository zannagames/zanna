//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/runtime/RTServicesInputTests.cpp
// Purpose: Verify Zanna.Services.ActionInput (ADR 0365) against the fake
//          steam_api libraries: the neutral contract without a provider,
//          argument traps, Steam Input start and stop, the controller
//          snapshot and device events, action sets and layers, digital and
//          analog actions for one or every controller, origins, glyphs,
//          feedback, and the core-only and SDK 1.61 redistributables.
// Key invariants:
//   - Every test starts from a shut-down session and a reset fake library.
//   - Manifest files are written to the system temporary directory and
//     removed again.
// Ownership/Lifetime:
//   - Tests release every runtime string they receive.
// Links: src/runtime/services/rt_services_input.c,
//        src/runtime/services/steam/rt_steam_input.c,
//        src/tests/runtime/RTServicesFakeSteamApi.c,
//        docs/adr/0365-platform-services-action-input.md
//
//===----------------------------------------------------------------------===//

#include "RTServicesTestSupport.hpp"

#include "rt_services_input.h"
#include "rt_steam_abi.h"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace services_test;

namespace {

/// @brief Id text of the fake's first controller (an Xbox One controller).
const char *const kFirst = "18000000000000000001";
/// @brief Id text of the fake's second controller (a PlayStation 5 controller).
const char *const kSecond = "18000000000000000002";

/// @brief Start Steam against the SDK 1.65 fake with scripted startup events disabled.
const FakeSteam &startSteam() {
    const FakeSteam &fake = fake165();
    useFake(fake);
    fake.setScripted(0);
    InitOutcome outcome = init("steam", "480");
    EXPECT_TRUE(outcome.ok);
    return fake;
}

/// @brief Temporary manifest file removed when the object goes away.
struct ManifestFile {
    std::filesystem::path path;

    explicit ManifestFile(const char *name, const char *contents) {
        path = std::filesystem::temp_directory_path() / name;
        std::ofstream out(path, std::ios::binary);
        out << contents;
    }

    ~ManifestFile() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::string text() const {
        return path.string();
    }
};

/// @brief Write a valid action manifest and return it.
ManifestFile validManifest() {
    return ManifestFile("zanna_services_input_manifest.vdf",
                        "\"Action Manifest\"\n{\n    \"actions\"\n    {\n    }\n}\n");
}

/// @brief Start action input with @p manifest and drain the events it produced.
void startInput(const ManifestFile &manifest) {
    Str path(manifest.text().c_str());
    ASSERT_EQ(rt_services_action_input_start(path), 1);
    rt_services_platform_update();
    while (rt_services_platform_poll_event() != RT_SERVICES_EVENT_NONE) {
    }
}

/// @brief Report whether @p text ends with @p suffix.
bool endsWith(const std::string &text, const std::string &suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// @brief Report whether a diagnostic starts with @p prefix and ends with @p suffix.
/// @details Paths inside diagnostics are normalized by the runtime, so tests
///          match around them.
bool hasDiagnosticAround(const std::string &prefix, const std::string &suffix) {
    for (const std::string &message : diagnostics()) {
        if (message.compare(0, prefix.size(), prefix) == 0 && endsWith(message, suffix))
            return true;
    }
    return false;
}

} // namespace

TEST(ServicesInput, ConstantsMatchAdr) {
    EXPECT_EQ(rt_services_event_kind_controller_connected(), 12);
    EXPECT_EQ(rt_services_event_kind_controller_disconnected(), 13);
    EXPECT_EQ(rt_services_event_kind_controller_configured(), 14);
    EXPECT_EQ(rt_services_feature_action_input(), 17);
    EXPECT_EQ(rt_services_controller_type_unknown(), 0);
    EXPECT_EQ(rt_services_controller_type_steam_controller(), 1);
    EXPECT_EQ(rt_services_controller_type_xbox360(), 2);
    EXPECT_EQ(rt_services_controller_type_xbox_one(), 3);
    EXPECT_EQ(rt_services_controller_type_generic_gamepad(), 4);
    EXPECT_EQ(rt_services_controller_type_playstation4(), 5);
    EXPECT_EQ(rt_services_controller_type_apple_mfi(), 6);
    EXPECT_EQ(rt_services_controller_type_android(), 7);
    EXPECT_EQ(rt_services_controller_type_switch_joy_con_pair(), 8);
    EXPECT_EQ(rt_services_controller_type_switch_joy_con_single(), 9);
    EXPECT_EQ(rt_services_controller_type_switch_pro(), 10);
    EXPECT_EQ(rt_services_controller_type_mobile_touch(), 11);
    EXPECT_EQ(rt_services_controller_type_playstation3(), 12);
    EXPECT_EQ(rt_services_controller_type_playstation5(), 13);
    EXPECT_EQ(rt_services_controller_type_steam_deck(), 14);
    EXPECT_EQ(rt_services_controller_type_steamos_handheld(), 15);
    EXPECT_EQ(rt_services_controller_type_switch2_pro(), 16);
    EXPECT_EQ(rt_services_controller_type_steam_controller_2026(), 17);
    EXPECT_EQ(rt_services_controller_type_steam_frame_controller_pair(), 18);
    EXPECT_EQ(rt_services_glyph_size_small(), 0);
    EXPECT_EQ(rt_services_glyph_size_medium(), 1);
    EXPECT_EQ(rt_services_glyph_size_large(), 2);
}

TEST(ServicesInput, LayoutsMatchRedistributablePacking) {
    const bool pack8 = RT_STEAM_CALLBACK_PACK == 8;
    EXPECT_EQ(sizeof(rt_steam_input_device), 8u);
    EXPECT_EQ(sizeof(rt_steam_input_configuration_loaded), pack8 ? 40u : 32u);
    EXPECT_EQ(offsetof(rt_steam_input_configuration_loaded, device), pack8 ? 8u : 4u);
    EXPECT_EQ(offsetof(rt_steam_input_configuration_loaded, mapping_creator), pack8 ? 16u : 12u);
    EXPECT_EQ(offsetof(rt_steam_input_configuration_loaded, major_revision), pack8 ? 24u : 20u);
    EXPECT_EQ(offsetof(rt_steam_input_configuration_loaded, minor_revision), pack8 ? 28u : 24u);
    EXPECT_EQ(offsetof(rt_steam_input_configuration_loaded, uses_input_api), pack8 ? 32u : 28u);
    EXPECT_EQ(offsetof(rt_steam_input_configuration_loaded, uses_gamepad_api), pack8 ? 33u : 29u);
    EXPECT_EQ(sizeof(rt_steam_input_digital_data), 2u);
    EXPECT_EQ(sizeof(rt_steam_input_analog_data), 13u);
    EXPECT_EQ(offsetof(rt_steam_input_analog_data, x), 4u);
    EXPECT_EQ(offsetof(rt_steam_input_analog_data, y), 8u);
    EXPECT_EQ(offsetof(rt_steam_input_analog_data, active), 12u);
}

TEST(ServicesInput, NeutralWithoutProvider) {
    rt_services_platform_shutdown();
    Str empty("");
    Str id("abc");
    Str swing("swing");
    Str batting("batting");
    Str manifest("missing.vdf");
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACTION_INPUT), 0);
    EXPECT_EQ(rt_services_action_input_start(manifest), 0);
    EXPECT_EQ(rt_services_action_input_start(empty), 0);
    EXPECT_EQ(rt_services_action_input_stop(), 0);
    EXPECT_EQ(rt_services_action_input_get_is_started(), 0);
    EXPECT_EQ(rt_services_action_input_get_controller_count(), 0);
    EXPECT_EQ(take(rt_services_action_input_controller_id_at(0)), std::string(""));
    EXPECT_EQ(rt_services_action_input_controller_type(empty), RT_SERVICES_CONTROLLER_TYPE_UNKNOWN);
    // A provider-specific controller id format is only checked while that provider is active.
    EXPECT_EQ(rt_services_action_input_gamepad_index(id), -1);
    EXPECT_EQ(rt_services_action_input_activate_action_set(empty, batting), 0);
    EXPECT_EQ(rt_services_action_input_activate_layer(empty, batting), 0);
    EXPECT_EQ(rt_services_action_input_deactivate_layer(id, batting), 0);
    EXPECT_EQ(rt_services_action_input_deactivate_all_layers(empty), 0);
    EXPECT_EQ(rt_services_action_input_is_pressed(empty, swing), 0);
    EXPECT_EQ(rt_services_action_input_analog_x(id, swing), 0.0);
    EXPECT_EQ(rt_services_action_input_analog_y(empty, swing), 0.0);
    EXPECT_EQ(rt_services_action_input_is_action_active(empty, swing), 0);
    EXPECT_EQ(take(rt_services_action_input_action_label(swing)), std::string(""));
    EXPECT_EQ(rt_services_action_input_origin_count(empty, empty, swing), 0);
    EXPECT_EQ(rt_services_action_input_origin_at(empty, batting, swing, 0), 0);
    EXPECT_EQ(take(rt_services_action_input_origin_label(114)), std::string(""));
    EXPECT_EQ(take(rt_services_action_input_origin_glyph_path(114, RT_SERVICES_GLYPH_SIZE_SMALL)),
              std::string(""));
    EXPECT_EQ(rt_services_action_input_vibrate(empty, 0.5, 0.5), 0);
    EXPECT_EQ(rt_services_action_input_set_led_color(empty, 1, 2, 3), 0);
    EXPECT_EQ(rt_services_action_input_reset_led_color(id), 0);
    EXPECT_EQ(rt_services_action_input_show_binding_panel(empty), 0);
}

TEST(ServicesInput, MalformedArgumentsTrap) {
    rt_services_platform_shutdown();
    Str empty("");
    Str id("");
    EXPECT_TRAP_MESSAGE(
        rt_services_action_input_activate_action_set(id, empty),
        "Services.ActionInput.ActivateActionSet: action set name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_activate_layer(id, nullptr),
                        "Services.ActionInput.ActivateLayer: layer name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_deactivate_layer(id, empty),
                        "Services.ActionInput.DeactivateLayer: layer name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_is_pressed(id, empty),
                        "Services.ActionInput.IsPressed: action name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_analog_x(id, empty),
                        "Services.ActionInput.AnalogX: action name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_analog_y(id, empty),
                        "Services.ActionInput.AnalogY: action name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_is_action_active(id, empty),
                        "Services.ActionInput.IsActionActive: action name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_action_label(empty),
                        "Services.ActionInput.ActionLabel: action name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_origin_count(id, empty, empty),
                        "Services.ActionInput.OriginCount: action name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_origin_at(id, empty, empty, 0),
                        "Services.ActionInput.OriginAt: action name must not be empty");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_origin_label(-1),
                        "Services.ActionInput.OriginLabel: origin must not be negative (got -1)");
    EXPECT_TRAP_MESSAGE(
        rt_services_action_input_origin_glyph_path(-5, RT_SERVICES_GLYPH_SIZE_SMALL),
        "Services.ActionInput.OriginGlyphPath: origin must not be negative (got -5)");
    EXPECT_TRAP_MESSAGE(
        rt_services_action_input_origin_glyph_path(114, 3),
        "Services.ActionInput.OriginGlyphPath: size must be a GlyphSize value (got 3)");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_vibrate(id, -0.5, 0.0),
                        "Services.ActionInput.Vibrate: left must be in 0..1 (got -0.5)");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_vibrate(id, 0.0, 1.5),
                        "Services.ActionInput.Vibrate: right must be in 0..1 (got 1.5)");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_vibrate(id, std::nan(""), 0.0),
                        "Services.ActionInput.Vibrate: left must be in 0..1 (got nan)");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_set_led_color(id, 256, 0, 0),
                        "Services.ActionInput.SetLedColor: red must be in 0..255 (got 256)");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_set_led_color(id, 0, -1, 0),
                        "Services.ActionInput.SetLedColor: green must be in 0..255 (got -1)");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_set_led_color(id, 0, 0, 300),
                        "Services.ActionInput.SetLedColor: blue must be in 0..255 (got 300)");
}

TEST(ServicesInput, StartReportsControllersAndDeviceEvents) {
    const FakeSteam &fake = startSteam();
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACTION_INPUT), 1);
    Str empty("");
    Str swing("swing");
    EXPECT_EQ(rt_services_action_input_is_pressed(empty, swing), 0);
    EXPECT_TRUE(hasDiagnostic("Services: ActionInput.IsPressed needs ActionInput.Start first"));
    EXPECT_EQ(rt_services_action_input_get_controller_count(), 0);

    ManifestFile manifest = validManifest();
    Str path(manifest.text().c_str());
    ASSERT_EQ(rt_services_action_input_start(path), 1);
    EXPECT_EQ(rt_services_action_input_get_is_started(), 1);
    // Started with explicit frames and device callbacks; Start ran the first frame.
    EXPECT_EQ(fake.inputState(), std::string("1,1,1,1"));
    EXPECT_TRUE(endsWith(fake.inputManifestPath(), "zanna_services_input_manifest.vdf"));

    ASSERT_EQ(rt_services_action_input_get_controller_count(), 2);
    EXPECT_EQ(take(rt_services_action_input_controller_id_at(0)), std::string(kFirst));
    EXPECT_EQ(take(rt_services_action_input_controller_id_at(1)), std::string(kSecond));
    EXPECT_EQ(take(rt_services_action_input_controller_id_at(2)), std::string(""));
    EXPECT_EQ(take(rt_services_action_input_controller_id_at(-1)), std::string(""));
    Str first(kFirst);
    Str second(kSecond);
    EXPECT_EQ(rt_services_action_input_controller_type(first),
              RT_SERVICES_CONTROLLER_TYPE_XBOX_ONE);
    EXPECT_EQ(rt_services_action_input_controller_type(second),
              RT_SERVICES_CONTROLLER_TYPE_PLAYSTATION5);
    EXPECT_EQ(rt_services_action_input_controller_type(empty),
              RT_SERVICES_CONTROLLER_TYPE_XBOX_ONE);
    EXPECT_EQ(rt_services_action_input_gamepad_index(second), 1);
    Str stranger("42");
    EXPECT_EQ(rt_services_action_input_controller_type(stranger),
              RT_SERVICES_CONTROLLER_TYPE_UNKNOWN);
    EXPECT_EQ(rt_services_action_input_gamepad_index(stranger), -1);

    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_CONTROLLER_CONNECTED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kFirst));
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_CONTROLLER_CONNECTED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kSecond));
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_CONTROLLER_CONFIGURED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kFirst));
    EXPECT_EQ(rt_services_platform_get_event_flag(), 1);
    EXPECT_EQ(rt_services_platform_get_event_value(), 3);
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_CONTROLLER_CONFIGURED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kSecond));
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);

    fake.setControllerConnected(1, 0);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_CONTROLLER_DISCONNECTED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kSecond));
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_NONE);
    EXPECT_EQ(rt_services_action_input_get_controller_count(), 1);
    EXPECT_EQ(fake.inputState(), std::string("1,1,1,3"));
    rt_services_platform_shutdown();
}

TEST(ServicesInput, ActionSetsLayersAndActions) {
    const FakeSteam &fake = startSteam();
    ManifestFile manifest = validManifest();
    startInput(manifest);
    Str empty("");
    Str first(kFirst);
    Str second(kSecond);
    Str swing("swing");
    Str bunt("bunt");
    Str select("select");
    Str aim("aim");
    Str menu("menu");
    Str batting("batting");
    Str layer("bunt_layer");

    // The first action set is active by default.
    EXPECT_EQ(rt_services_action_input_is_action_active(first, swing), 1);
    EXPECT_EQ(rt_services_action_input_is_action_active(first, aim), 1);
    EXPECT_EQ(rt_services_action_input_is_action_active(first, select), 0);

    fake.setDigitalAction(0, "swing", 1);
    EXPECT_EQ(rt_services_action_input_is_pressed(first, swing), 1);
    EXPECT_EQ(rt_services_action_input_is_pressed(second, swing), 0);
    EXPECT_EQ(rt_services_action_input_is_pressed(empty, swing), 1);

    // Handles are looked up once.
    const int lookups = fake.inputHandleLookups();
    for (int i = 0; i < 5; ++i)
        EXPECT_EQ(rt_services_action_input_is_pressed(first, swing), 1);
    EXPECT_EQ(fake.inputHandleLookups(), lookups);

    fake.setDigitalAction(0, "bunt", 1);
    EXPECT_EQ(rt_services_action_input_is_pressed(first, bunt), 0);
    EXPECT_EQ(rt_services_action_input_activate_layer(empty, layer), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ActivateActionSetLayer(18446744073709551615,201)"));
    EXPECT_EQ(rt_services_action_input_is_pressed(first, bunt), 1);
    EXPECT_EQ(rt_services_action_input_deactivate_layer(first, layer), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("DeactivateActionSetLayer(18000000000000000001,201)"));
    EXPECT_EQ(rt_services_action_input_is_pressed(first, bunt), 0);
    EXPECT_EQ(rt_services_action_input_deactivate_all_layers(empty), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("DeactivateAllActionSetLayers(18446744073709551615)"));

    fake.setAnalogAction(0, "aim", 0.25, -0.5);
    fake.setAnalogAction(1, "aim", -0.75, 0.5);
    EXPECT_EQ(rt_services_action_input_analog_x(first, aim), 0.25);
    EXPECT_EQ(rt_services_action_input_analog_y(first, aim), -0.5);
    // With every controller, both axes come from the longest vector.
    EXPECT_EQ(rt_services_action_input_analog_x(empty, aim), -0.75);
    EXPECT_EQ(rt_services_action_input_analog_y(empty, aim), 0.5);

    EXPECT_EQ(rt_services_action_input_activate_action_set(first, menu), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ActivateActionSet(18000000000000000001,102)"));
    EXPECT_EQ(rt_services_action_input_is_action_active(first, swing), 0);
    EXPECT_EQ(rt_services_action_input_is_action_active(first, select), 1);
    EXPECT_EQ(rt_services_action_input_is_action_active(empty, select), 1);
    EXPECT_EQ(rt_services_action_input_is_pressed(first, swing), 0);
    EXPECT_EQ(rt_services_action_input_analog_x(first, aim), 0.0);
    EXPECT_EQ(rt_services_action_input_analog_x(empty, aim), -0.75);
    EXPECT_EQ(rt_services_action_input_activate_action_set(empty, batting), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ActivateActionSet(18446744073709551615,101)"));
    EXPECT_EQ(rt_services_action_input_is_pressed(first, swing), 1);

    EXPECT_EQ(take(rt_services_action_input_action_label(swing)), std::string("Swing"));
    EXPECT_EQ(take(rt_services_action_input_action_label(aim)), std::string("Aim"));
    Str jump("jump");
    EXPECT_EQ(take(rt_services_action_input_action_label(jump)), std::string(""));

    Str fielding("fielding");
    Str steal("steal_layer");
    EXPECT_EQ(rt_services_action_input_activate_action_set(empty, fielding), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: action set 'fielding' is not in the action manifest"));
    EXPECT_EQ(rt_services_action_input_activate_layer(empty, steal), 0);
    EXPECT_TRUE(
        hasDiagnostic("Steam: action set layer 'steal_layer' is not in the action manifest"));
    EXPECT_EQ(rt_services_action_input_is_pressed(first, jump), 0);
    EXPECT_TRUE(hasDiagnostic(
        "Services: ActionInput.IsPressed found no digital action 'jump' in the action manifest"));
    EXPECT_EQ(rt_services_action_input_analog_x(first, swing), 0.0);
    EXPECT_TRUE(hasDiagnostic(
        "Services: ActionInput.AnalogX found no analog action 'swing' in the action manifest"));
    EXPECT_EQ(rt_services_action_input_is_action_active(empty, jump), 0);
    EXPECT_TRUE(hasDiagnostic(
        "Services: ActionInput.IsActionActive found no action 'jump' in the action manifest"));
    rt_services_platform_shutdown();
}

TEST(ServicesInput, OriginsLabelsAndGlyphs) {
    const FakeSteam &fake = startSteam();
    (void)fake;
    ManifestFile manifest = validManifest();
    startInput(manifest);
    Str empty("");
    Str first(kFirst);
    Str second(kSecond);
    Str swing("swing");
    Str aim("aim");
    Str batting("batting");
    Str menu("menu");

    EXPECT_EQ(rt_services_action_input_origin_count(first, batting, swing), 1);
    EXPECT_EQ(rt_services_action_input_origin_at(first, batting, swing, 0), 114);
    EXPECT_EQ(rt_services_action_input_origin_at(first, batting, swing, 1), 0);
    EXPECT_EQ(rt_services_action_input_origin_at(first, batting, swing, -1), 0);
    EXPECT_EQ(rt_services_action_input_origin_count(second, empty, aim), 2);
    EXPECT_EQ(rt_services_action_input_origin_at(second, empty, aim, 1), 275);
    EXPECT_EQ(rt_services_action_input_origin_count(empty, batting, swing), 1);
    EXPECT_EQ(rt_services_action_input_origin_count(empty, menu, swing), 0);
    Str fielding("fielding");
    EXPECT_EQ(rt_services_action_input_origin_count(empty, fielding, swing), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: action set 'fielding' is not in the action manifest"));

    EXPECT_EQ(take(rt_services_action_input_origin_label(114)), std::string("A Button"));
    EXPECT_EQ(take(rt_services_action_input_origin_label(258)), std::string("Cross Button"));
    EXPECT_EQ(take(rt_services_action_input_origin_label(0)), std::string(""));
    EXPECT_EQ(take(rt_services_action_input_origin_label(40000)), std::string(""));
    // Outside Windows the backslashes Steam for macOS mixes into glyph paths become '/'.
#if RT_PLATFORM_WINDOWS
    const std::string glyph = "/fake/steam\\controller_base\\glyphs/origin_258_size_2_flags_0.png";
#else
    const std::string glyph = "/fake/steam/controller_base/glyphs/origin_258_size_2_flags_0.png";
#endif
    EXPECT_EQ(take(rt_services_action_input_origin_glyph_path(258, RT_SERVICES_GLYPH_SIZE_LARGE)),
              glyph);
    rt_services_platform_shutdown();
}

TEST(ServicesInput, FeedbackAndBindingPanel) {
    const FakeSteam &fake = startSteam();
    ManifestFile manifest = validManifest();
    startInput(manifest);
    Str empty("");
    Str first(kFirst);
    Str second(kSecond);

    EXPECT_EQ(rt_services_action_input_vibrate(first, 1.0, 0.5), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("TriggerVibration(18000000000000000001,65535,32768)"));
    EXPECT_EQ(rt_services_action_input_vibrate(empty, 0.0, 0.0), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("TriggerVibration(18000000000000000002,0,0)"));
    EXPECT_EQ(rt_services_action_input_set_led_color(second, 255, 128, 0), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("SetLEDColor(18000000000000000002,255,128,0,0)"));
    EXPECT_EQ(rt_services_action_input_reset_led_color(first), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("SetLEDColor(18000000000000000001,0,0,0,1)"));
    EXPECT_EQ(rt_services_action_input_show_binding_panel(empty), 1);
    EXPECT_EQ(fake.lastUiCall(), std::string("ShowBindingPanel(18000000000000000001)"));

    fake.setOverlayEnabled(0);
    EXPECT_EQ(rt_services_action_input_show_binding_panel(second), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: ShowBindingPanel failed; the Steam overlay must be available "
                              "or Steam must be in Big Picture mode"));

    fake.setControllerConnected(0, 0);
    fake.setControllerConnected(1, 0);
    rt_services_platform_update();
    EXPECT_EQ(rt_services_action_input_get_controller_count(), 0);
    EXPECT_EQ(rt_services_action_input_vibrate(empty, 1.0, 1.0), 0);
    EXPECT_EQ(rt_services_action_input_reset_led_color(empty), 0);
    EXPECT_EQ(rt_services_action_input_show_binding_panel(empty), 0);
    rt_services_platform_shutdown();
}

TEST(ServicesInput, SteamControllerIdsTrapWhileActive) {
    startSteam();
    Str swing("swing");
    Str malformed("abc");
    Str all("18446744073709551615");
    Str zero("0");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_is_pressed(malformed, swing),
                        "Services.ActionInput.IsPressed: Steam controller id 'abc' must be an "
                        "integer in 1..18446744073709551614");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_vibrate(all, 0.0, 0.0),
                        "Services.ActionInput.Vibrate: Steam controller id '18446744073709551615' "
                        "must be an integer in 1..18446744073709551614");
    EXPECT_TRAP_MESSAGE(rt_services_action_input_controller_type(zero),
                        "Services.ActionInput.ControllerType: Steam controller id '0' must be an "
                        "integer in 1..18446744073709551614");
    rt_services_platform_shutdown();
}

TEST(ServicesInput, StartFailuresStopAndShutdown) {
    const FakeSteam &fake = startSteam();
    const std::string missing_path =
        (std::filesystem::temp_directory_path() / "zanna_services_input_absent.vdf").string();
    Str missing(missing_path.c_str());
    EXPECT_EQ(rt_services_action_input_start(missing), 0);
    EXPECT_TRUE(hasDiagnosticAround("Services: ActionInput.Start found no action manifest at '",
                                    "zanna_services_input_absent.vdf'"));
    EXPECT_EQ(rt_services_action_input_get_is_started(), 0);
    EXPECT_EQ(fake.inputState().substr(0, 1), std::string("0"));

    // A file Steam refuses still starts action input, and Start reports the refusal.
    ManifestFile rejected("zanna_services_input_rejected.vdf", "not a manifest\n");
    Str rejected_path(rejected.text().c_str());
    EXPECT_EQ(rt_services_action_input_start(rejected_path), 0);
    EXPECT_TRUE(hasDiagnosticAround("Steam: SetInputActionManifestFilePath('",
                                    "zanna_services_input_rejected.vdf') failed; Steam accepts a "
                                    "manifest once it has a controller mapping for the app, so "
                                    "the manifest is retried when a controller connects (also "
                                    "check that the file is a Steam Input action manifest)"));
    EXPECT_EQ(rt_services_action_input_get_is_started(), 1);
    EXPECT_EQ(fake.inputState().substr(0, 1), std::string("1"));
    EXPECT_EQ(rt_services_action_input_stop(), 1);

    Str empty("");
    EXPECT_EQ(rt_services_action_input_start(empty), 1);
    EXPECT_EQ(rt_services_action_input_get_is_started(), 1);
    EXPECT_EQ(rt_services_action_input_stop(), 1);
    EXPECT_EQ(rt_services_action_input_get_is_started(), 0);
    EXPECT_EQ(fake.inputState().substr(0, 1), std::string("0"));
    EXPECT_EQ(rt_services_action_input_stop(), 0);

    fake.setInputInitResult(0);
    EXPECT_EQ(rt_services_action_input_start(empty), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: ISteamInput Init failed"));
    fake.setInputInitResult(1);

    ManifestFile manifest = validManifest();
    Str path(manifest.text().c_str());
    EXPECT_EQ(rt_services_action_input_start(path), 1);
    rt_services_platform_shutdown();
    EXPECT_EQ(fake.inputState().substr(0, 1), std::string("0"));
    EXPECT_EQ(rt_services_action_input_get_is_started(), 0);
}

TEST(ServicesInput, RefusedManifestAppliesWhenAControllerConnects) {
    const FakeSteam &fake = startSteam();
    fake.setControllerConnected(0, 0);
    fake.setControllerConnected(1, 0);
    fake.setInputMapping(0);
    ManifestFile manifest = validManifest();
    Str path(manifest.text().c_str());
    Str empty("");
    Str batting("batting");
    EXPECT_EQ(rt_services_action_input_start(path), 0);
    EXPECT_EQ(rt_services_action_input_get_is_started(), 1);
    EXPECT_EQ(fake.inputManifestPath(), std::string(""));
    EXPECT_EQ(rt_services_action_input_activate_action_set(empty, batting), 0);

    // The connection gives Steam a mapping; the manifest applies before the event is reported.
    fake.setControllerConnected(0, 1);
    rt_services_platform_update();
    EXPECT_TRUE(endsWith(fake.inputManifestPath(), "zanna_services_input_manifest.vdf"));
    EXPECT_EQ(rt_services_platform_poll_event(), RT_SERVICES_EVENT_CONTROLLER_CONNECTED);
    EXPECT_EQ(take(rt_services_platform_get_event_text()), std::string(kFirst));
    EXPECT_EQ(rt_services_action_input_activate_action_set(empty, batting), 1);
    rt_services_platform_shutdown();
}

TEST(ServicesInput, MissingFromCoreOnlyRedistributable) {
    useFake(fakeCore());
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACTION_INPUT), 0);
    EXPECT_TRUE(hasDiagnostic("Steam: interface SteamAPI_SteamInput_v007 or "
                              "SteamAPI_SteamInput_v006 unavailable; action input disabled"));
    Str empty("");
    Str swing("swing");
    EXPECT_EQ(rt_services_action_input_start(empty), 0);
    EXPECT_EQ(rt_services_action_input_is_pressed(empty, swing), 0);
    EXPECT_TRUE(hasDiagnostic("Services: ActionInput.IsPressed needs ActionInput.Start first"));
    rt_services_platform_shutdown();
}

TEST(ServicesInput, Sdk161RedistributableBindsVersion006) {
    const FakeSteam &fake = fake164();
    useFake(fake);
    fake.setScripted(0);
    ASSERT_TRUE(init("steam", "480").ok);
    EXPECT_EQ(rt_services_platform_has_feature(RT_SERVICES_FEATURE_ACTION_INPUT), 1);
    ManifestFile manifest = validManifest();
    startInput(manifest);
    EXPECT_EQ(rt_services_action_input_get_controller_count(), 2);
    fake.setDigitalAction(1, "swing", 1);
    Str empty("");
    Str swing("swing");
    EXPECT_EQ(rt_services_action_input_is_pressed(empty, swing), 1);
    rt_services_platform_shutdown();
}

TEST(ServicesInput, StatefulMembersRequireMainThread) {
    rt_services_platform_shutdown();
    g_trap_jump = false;
    g_last_trap.clear();
    int8_t pressed = -1;
    std::thread worker([&pressed]() {
        Str empty("");
        Str swing("swing");
        pressed = rt_services_action_input_is_pressed(empty, swing);
    });
    worker.join();
    EXPECT_EQ(g_last_trap,
              std::string("Services: ActionInput.IsPressed must be called on the main thread"));
    EXPECT_EQ(pressed, 0);
    g_last_trap.clear();
    int64_t size = -1;
    std::thread constant_worker([&size]() { size = rt_services_glyph_size_large(); });
    constant_worker.join();
    EXPECT_EQ(size, 2);
    EXPECT_EQ(g_last_trap, std::string(""));
}

int main(int argc, char **argv) {
    rt_set_main_thread();
    zanna_test::init(&argc, argv);
    return zanna_test::run_all_tests();
}
