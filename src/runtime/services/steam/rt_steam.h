//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/services/steam/rt_steam.h
// Purpose: Public C ABI for Zanna.Services.Steam, the Steam-specific extension
//          class of the platform services layer, and its SteamHardware
//          constants.
// Key invariants:
//   - Every query returns a neutral value unless Steam is the active provider,
//     except RestartAppIfNecessary, which works before Platform.Init.
//   - Stateful entry points must run on the main thread.
//   - SteamHardware ordinals are stable and match ESteamHardwareType, with
//     Unknown (-1) reserved for "Steam is not active".
// Ownership/Lifetime:
//   - String results are new caller-owned references.
// Links: src/runtime/services/steam/rt_steam_provider.c,
//        src/runtime/services/rt_services.h,
//        docs/zannalib/services.md
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_steam.h
 * @brief Declares the Zanna.Services.Steam extension surface.
 * @details Neutral features (identity, licensing, events, requests) live on
 *          Zanna.Services.Platform. This header covers what only Steam
 *          offers: relaunching through the Steam client, the numeric
 *          SteamID64, Steam hardware and Proton detection, Big Picture mode,
 *          and the path of the loaded redistributable.
 */

#pragma once

#include "rt_string.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief SteamHardware value reported while Steam is not the active provider.
#define RT_SERVICES_STEAM_HARDWARE_UNKNOWN INT64_C(-1)
/// @brief Not running on Steam hardware.
#define RT_SERVICES_STEAM_HARDWARE_NONE INT64_C(0)
/// @brief Running on a Steam Deck.
#define RT_SERVICES_STEAM_HARDWARE_STEAM_DECK INT64_C(1)
/// @brief Running on a Steam Machine.
#define RT_SERVICES_STEAM_HARDWARE_STEAM_MACHINE INT64_C(2)
/// @brief Running on a Steam Frame.
#define RT_SERVICES_STEAM_HARDWARE_STEAM_FRAME INT64_C(3)

/// @brief Relaunch the game through the Steam client when it was started outside Steam.
/// @details Loads the redistributable (without setting SteamAppId) and asks it
///          whether the process was launched by Steam. When it returns 1,
///          Steam is relaunching the game and the caller should exit
///          immediately. Returns 0 when launched by Steam, when a development
///          steam_appid.txt is present, or when the library cannot be loaded
///          (the reason is recorded in Platform.Diagnostics).
/// @param app_id Steam app id in 1..4294967295; other values trap.
/// @return 1 when the caller must exit, otherwise 0.
int8_t rt_services_steam_restart_app_if_necessary(int64_t app_id);

/// @brief Report whether Steam is the active platform services provider.
/// @return 1 while the Steam provider is started, otherwise 0.
int8_t rt_services_steam_get_is_active(void);

/// @brief Read the signed-in user's SteamID64.
/// @return SteamID64 (always below 2^63 for individual accounts), or 0 when unavailable.
int64_t rt_services_steam_get_steam_id(void);

/// @brief Read the Steam hardware the game runs on.
/// @details Redistributables from SDK 1.65 report the full ESteamHardwareType;
///          older supported redistributables distinguish only Steam Deck from
///          none.
/// @return RT_SERVICES_STEAM_HARDWARE_* value; Unknown when Steam is not active.
int64_t rt_services_steam_get_hardware_type(void);

/// @brief Report whether the game runs under the Proton compatibility layer.
/// @details Requires an SDK 1.65 redistributable; older ones report 0.
/// @return 1 under Proton, otherwise 0.
int8_t rt_services_steam_get_is_under_proton(void);

/// @brief Report whether Steam is running in Big Picture mode.
/// @return 1 in Big Picture mode, otherwise 0.
int8_t rt_services_steam_get_is_big_picture(void);

/// @brief Read the path of the loaded steam_api redistributable.
/// @return Caller-owned path, or the empty string when no library has been loaded.
rt_string rt_services_steam_get_library_path(void);

/// @brief Return `Zanna.Services.SteamHardware.Unknown`.
/// @return Stable ordinal -1.
int64_t rt_services_steam_hardware_unknown(void);
/// @brief Return `Zanna.Services.SteamHardware.None`.
/// @return Stable ordinal 0.
int64_t rt_services_steam_hardware_none(void);
/// @brief Return `Zanna.Services.SteamHardware.SteamDeck`.
/// @return Stable ordinal 1.
int64_t rt_services_steam_hardware_steam_deck(void);
/// @brief Return `Zanna.Services.SteamHardware.SteamMachine`.
/// @return Stable ordinal 2.
int64_t rt_services_steam_hardware_steam_machine(void);
/// @brief Return `Zanna.Services.SteamHardware.SteamFrame`.
/// @return Stable ordinal 3.
int64_t rt_services_steam_hardware_steam_frame(void);

#ifdef __cplusplus
}
#endif
