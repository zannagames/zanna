---
status: accepted
audience: contributors
last-verified: 2026-09-15
---

# ADR 0365: Platform Services Action Input (Steam Input First)

## Status

Accepted. Adds `Zanna.Services.ActionInput` to the platform services layer of
[ADR 0352](0352-platform-services-runtime-loaded-providers.md). The policy, provider model, event
queue, and neutral-value rules there apply unchanged.

## Context

Games sold on Steam are expected to support the controllers Steam supports, and the Steam Deck
depends on it. Steam Input lets a game name what the player does ("swing", "aim") in an *action
manifest* while the player binds those actions to any controller in Steam's own interface. Steam
also supplies the button labels and images a game needs for prompts that match the player's
bindings.

Zanna's own gamepad input reads physical buttons. It cannot follow Steam's binding interface,
show the player's bindings, or read controllers whose inputs Steam remaps.

The flat `ISteamInput` API is `SteamInput_v006` in SDK 1.61 through 1.64 and `SteamInput_v007` in
SDK 1.65. Every method this ADR binds has the same signature in both. The action data comes back
*by value* in one-byte-packed structures: `InputDigitalActionData_t` (2 bytes) and
`InputAnalogActionData_t` (13 bytes). Device and configuration callbacks follow the usual
callback packing.

## Decision

### Neutral surface

`Zanna.Services.ActionInput` is a static class. Members are main-thread only and neutral without a
provider. Constant getters are readable from any thread.

| Member | Signature | Ownership | Contract |
|---|---|---|---|
| `Start(manifestPath)` | `i1(str)` | | Start with a manifest file (relative to the working directory), or `""` for the configuration published with the platform |
| `Stop()` | `i1()` | | Stop; true when it was started |
| `IsStarted` | `i1` | | Action input runs |
| `ControllerCount` | `i64` | | Connected controllers, at most 16, refreshed by each pump |
| `ControllerIdAt(index)` | `str(i64)` | owned | Controller id, or `""` outside the range |
| `ControllerType(controllerId)` | `i64(str)` | | `ControllerType` value |
| `GamepadIndex(controllerId)` | `i64(str)` | | Emulated gamepad slot, or -1 |
| `ActivateActionSet(controllerId, actionSet)` | `i1(str,str)` | | Select the action set |
| `ActivateLayer` / `DeactivateLayer(controllerId, layer)` | `i1(str,str)` | | Stack or remove a layer |
| `DeactivateAllLayers(controllerId)` | `i1(str)` | | Remove every layer |
| `IsPressed(controllerId, action)` | `i1(str,str)` | | Digital action held and available |
| `AnalogX` / `AnalogY(controllerId, action)` | `f64(str,str)` | | Analog action axes, 0 when unavailable |
| `IsActionActive(controllerId, action)` | `i1(str,str)` | | Action available in the active set and layers |
| `ActionLabel(action)` | `str(str)` | owned | Localized action name |
| `OriginCount(controllerId, actionSet, action)` | `i64(str,str,str)` | | Bound inputs, at most 8; `""` set means the current set |
| `OriginAt(controllerId, actionSet, action, index)` | `i64(str,str,str,i64)` | | Provider origin id, or 0 |
| `OriginLabel(origin)` | `str(i64)` | owned | Localized input name |
| `OriginGlyphPath(origin, size)` | `str(i64,i64)` | owned | PNG file of the input at a `GlyphSize` |
| `Vibrate(controllerId, left, right)` | `i1(str,f64,f64)` | | Motor strengths in 0..1 |
| `SetLedColor(controllerId, red, green, blue)` | `i1(str,i64,i64,i64)` | | Components in 0..255 |
| `ResetLedColor(controllerId)` | `i1(str)` | | Restore the player's color |
| `ShowBindingPanel(controllerId)` | `i1(str)` | | Open the platform's binding screen |

**The empty controller id.** An empty id means every connected controller:

- Activation members pass it to the provider, so a set or layer also reaches controllers
  connected later.
- `IsPressed` and `IsActionActive` ask whether any controller qualifies.
- `AnalogX` and `AnalogY` read the controller whose vector is longest, so both axes describe the
  same stick.
- `Vibrate`, `SetLedColor`, and `ResetLedColor` reach each connected controller.
- The remaining members use the first connected controller.

**Constants.**

- `ControllerType` values 0..18 follow `ESteamInputType`: `Unknown`, `SteamController`, `Xbox360`,
  `XboxOne`, `GenericGamepad`, `PlayStation4`, `AppleMfi`, `Android`, `SwitchJoyConPair`,
  `SwitchJoyConSingle`, `SwitchPro`, `MobileTouch`, `PlayStation3`, `PlayStation5`, `SteamDeck`,
  `SteamOSHandheld`, `Switch2Pro`, `SteamController2026`, and `SteamFrameControllerPair`. A
  provider value outside that range reads as `Unknown`.
- `GlyphSize` is `Small` 0, `Medium` 1, and `Large` 2.
- `EventKind` gains `ControllerConnected` 12 and `ControllerDisconnected` 13 (both with the
  controller id in `EventText`), and `ControllerConfigured` 14 (`EventFlag`: the bindings use
  actions; `EventValue`: the major binding revision). They are queued only while action input runs.
- `Feature.ActionInput` is 17.

**Traps and diagnostics.**

- These trap whether or not a provider is started, as `Services.ActionInput.<Member>: <detail>`:
  - empty action, set, and layer names;
  - negative origins;
  - unknown `GlyphSize` values;
  - strengths outside 0..1, NaN included;
  - color components outside 0..255.
- Calling a member before `Start` records `Services: ActionInput.<Member> needs ActionInput.Start
  first`.
- An undefined action records `Services: ActionInput.<Member> found no digital action '<name>' in
  the action manifest` (or `analog action`, or `action`).
- A missing manifest file records `Services: ActionInput.Start found no action manifest at
  '<path>'` and starts nothing.

### Provider contract

`rt_services_action_input_ops` holds these operations:

- `start`, `stop`, and `is_started`.
- `check_controller_id(member, id)`, which traps on an id the provider cannot use.
- `controller_count`, `controller_id_at`, `controller_type`, and `gamepad_index`.
- `activate_action_set`, `set_layer_active`, and `deactivate_all_layers`. Only these accept the
  empty id.
- `digital_action` and `analog_action`. Each returns 0 without a diagnostic for a name it does not
  define, so the core can probe digital before analog.
- `action_label`, `action_origins`, `origin_label`, and `origin_glyph_path`.
- `vibrate`, `set_led_color` (with a restore flag), and `show_binding_panel`.

When the platform refuses the manifest, `start` returns 0 while `is_started` reports 1. The
provider table gains `.action_input`.

### Steam binding

- **Binding.** The accessor is `SteamInput_v007`, falling back to `v006`. The group needs the
  accessor and 28 exports. A missing piece records `Steam: interface ... unavailable; action input
  disabled` (or `export ...`).
- **Start and frames.** `Start` calls `Init(true)` and `EnableDeviceCallbacks`, then the manifest.
  Each provider pump calls `RunFrame` and refreshes `GetConnectedControllers` before dispatching
  callbacks. `Stop`, `Platform.Shutdown`, and provider stop call `ISteamInput::Shutdown` before
  `SteamAPI_Shutdown`.
- **Refused manifests.** Steam accepts a manifest only after it holds a controller mapping for the
  app. Until then `SetInputActionManifestFilePath` prints "Timed out waiting for game mapping!",
  blocks about 1.1 seconds, and returns false. The binding then:
  - keeps Steam Input running;
  - returns 0 with a diagnostic explaining the refusal;
  - remembers the path, and applies it again when `SteamInputDeviceConnected_t` arrives, before
    reporting `ControllerConnected`, so a game reacting to the event sees the manifest's
    actions.
- **Controller ids.** Ids are decimal `InputHandle_t` values. The empty id becomes
  `STEAM_INPUT_HANDLE_ALL_CONTROLLERS`. Text that does not parse, `0`, or the all-controllers value
  itself traps with `Services.ActionInput.<Member>: Steam controller id '<id>' must be an integer in
  1..18446744073709551614`.
- **Handle cache.** Set, layer, and action handles are looked up once per name, unknown names
  included, in growable heap caches. The caches are cleared by `Start`, `Stop`, a controller
  connection, and a configuration load, since each can make names known.
- **Action data by value.** `rt_steam_input_digital_data` and `rt_steam_input_analog_data` are
  declared under `#pragma pack(1)`. Every field of the 13-byte analog structure sits at a multiple
  of its own alignment, so the compiler's by-value return convention matches the SDK's C++
  declaration on SysV x86-64, AArch64, and Windows x64. Non-finite axes read as 0.
- **Callbacks.**
  - `SteamInputDeviceConnected_t` and `SteamInputDeviceDisconnected_t` (2801, 2802): 8 bytes.
  - `SteamInputConfigurationLoaded_t` (2803): 32 bytes under pack 4 and 40 under pack 8. The
    device is at 4 or 8, the one-byte-aligned `CSteamID` creator at 12 or 16, the revisions at
    20/24 or 24/28, and the flags at 28/29 or 32/33.
  - `SteamInputGamepadSlotChange_t` (2804) is ignored.
- **Glyph paths.** Steam for macOS returns glyph paths that join their last directories with
  backslashes, such as `.../MacOS\controller_base\images\api\knockout/ps_button_x_lg.png`.
  Outside Windows the binding replaces backslashes with `/`, so the path opens.
- **Rumble and lights.** Strengths map to `uint16` speeds as `round(strength * 65535)`.
  `SetLedColor` uses `k_ESteamInputLEDFlag_SetColor`, and `ResetLedColor` uses
  `k_ESteamInputLEDFlag_RestoreUserDefault`.
- **Binding panel.** A refused `ShowBindingPanel` records `Steam: ShowBindingPanel failed; the
  Steam overlay must be available or Steam must be in Big Picture mode`.

Pack-4 layouts and the origin ordinals were checked against both the SDK 1.61 and SDK 1.65
headers.

## Consequences

- A game reads controllers the way the player bound them, shows matching prompts, and runs the
  same code without Steam: every member is neutral.
- Motion data, action-event callbacks, trigger effects, haptic events, and Xbox-origin
  translation are left out. They can be appended later without changing this surface.
- Tests use a new `test_rt_services_input` binary against the fake redistributable. The fake
  models Steam Input with:
  - two controllers whose handles exceed `INT64_MAX`;
  - two action sets, a layer, and four digital and two analog actions;
  - origins, labels, and backslash glyph paths;
  - device and configuration callbacks under both packings;
  - manifests refused until a controller mapping exists;
  - the SDK 1.61 accessor and the core-only profile.

  The Zia and BASIC fixtures cover the class on the VM and in native binaries.
- **Live check (2026-09-15, macOS arm64, Spacewar 480, SDK 1.61 and the 1.65-generation library,
  no controller attached):**
  - Both redistributables bound the group and initialized Steam Input.
  - Steam refused the manifest as described, and `Start` reported it while action input kept
    running.
  - Origin labels read "A Button" and "X Button".
  - Glyph PNGs loaded at 32, 128, and 256 pixels after the separator fix.
  - `Stop` shut Steam Input down.
  - Actions, per-controller origins, rumble, lights, the binding panel, device events, and a
    manifest applied on connection still need a controller.

## Alternatives Considered

- **Extend Zanna's gamepad input instead.** Rejected. Physical-button input cannot follow the
  player's Steam bindings or show them, and it would tie a store feature to the input layer.
- **Handle-based API (look up handles, then query by handle).** Rejected. Handles are Steam's
  model. Name-based calls with a cache in the binding cost the same per frame.
- **Shut Steam Input down when the manifest is refused.** Rejected. Once stopped, no connection
  event could arrive to retry, so a game started before its controller was picked up would never
  get its actions in that session.
- **Retry the manifest every frame.** Rejected. Each refusal blocks about a second. A connection
  is the event that gives Steam the mapping.
- **Expose `EInputSourceMode`.** Rejected. The manifest already tells the game whether an action
  is a stick or mouse-like input, and the enum is Steam-specific.
