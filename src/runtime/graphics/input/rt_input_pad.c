//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/graphics/input/rt_input_pad.c
// Purpose: Gamepad and controller input backend for Zanna.Input. Manages state
//   for up to ZANNA_PAD_MAX (4) simultaneously connected controllers, polling
//   button press/release/held edges, analog stick axes, and triggers each frame.
//   Provides platform-specific backends for macOS (IOKit HID), Linux (evdev),
//   and Windows (XInput), with a vibration API for force-feedback motors.
//
// Key invariants:
//   - rt_pad_poll_frame() must be called once per frame to latch pressed/released
//     edges; edges are valid only for the frame they are read.
//   - Analog stick values are in [-1.0, 1.0]; trigger values are in [0.0, 1.0].
//   - A configurable deadzone (default 0.1) is applied to stick axes before
//     returning values; inputs within the deadzone read as 0.0.
//   - Controller indices are in [0, ZANNA_PAD_MAX); out-of-range indices return
//     safe zero/false values without trapping.
//   - Button codes map to the ZANNA_PAD_BUTTON_* enum; axis codes to
//     ZANNA_PAD_AXIS_*.
//
// Ownership/Lifetime:
//   - All state is stored in static globals (g_pads, g_pad_deadzone,
//     g_pad_initialized); no heap allocation is required for normal operation.
//   - Platform HID resources (IOKit manager on macOS) are allocated at init and
//     released by rt_pad_shutdown().
//
// Links: src/runtime/graphics/input/rt_input.h (keyboard/mouse layer, frame lifecycle),
//        src/runtime/graphics/2d/rt_action.c (action mapping layer),
//        src/runtime/graphics/input/rt_inputmgr.h (high-level input manager)
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Implements cross-platform gamepad state, polling, and vibration.
 *
 * @details Up to four logical controller slots expose frame-latched button
 *          edges, held state, normalized sticks and triggers, deadzone
 *          processing, names, and rumble. Platform adapters translate IOKit
 *          HID, Linux evdev, or XInput devices into the same stable public
 *          button and axis vocabulary.
 */

#include "rt_box.h"
#include "rt_input.h"
#include "rt_platform.h"
#include "rt_seq.h"
#include "rt_string.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//=============================================================================
// Gamepad/Controller Input Implementation
//=============================================================================

#include <math.h>

/// @brief Frame-coherent public state for one logical gamepad slot.
typedef struct {
    bool connected; ///< Whether a platform device currently occupies the slot.
    char name[64];  ///< NUL-terminated display name for the connected device.

    // Current button state
    bool buttons[ZANNA_PAD_BUTTON_MAX]; ///< Current held state by public button code.

    // Button events this frame
    bool pressed[ZANNA_PAD_BUTTON_MAX];  ///< Button-down edges latched this frame.
    bool released[ZANNA_PAD_BUTTON_MAX]; ///< Button-up edges latched this frame.

    // Analog stick values (-1.0 to 1.0)
    double left_x;  ///< Normalized left-stick horizontal axis.
    double left_y;  ///< Normalized left-stick vertical axis.
    double right_x; ///< Normalized right-stick horizontal axis.
    double right_y; ///< Normalized right-stick vertical axis.

    // Trigger values (0.0 to 1.0)
    double left_trigger;  ///< Normalized left-trigger value.
    double right_trigger; ///< Normalized right-trigger value.

    // Vibration state
    double vibration_left;  ///< Last requested low-frequency motor strength.
    double vibration_right; ///< Last requested high-frequency motor strength.
} rt_pad_state;

/// @brief Logical state for every public controller index.
static rt_pad_state g_pads[ZANNA_PAD_MAX];

/// @brief Shared radial/axis deadzone threshold applied to stick queries.
static double g_pad_deadzone = 0.1;

/// @brief Whether platform gamepad resources have been initialized.
static bool g_pad_initialized = false;

/// @brief Clear one public gamepad slot to its disconnected zero state.
/// @details Platform backends call this when an OS device disappears or a slot has no
///          backing device. Keeping the clear operation shared ensures stale buttons,
///          axes, names, and vibration values do not survive disconnects.
/// @param index Public gamepad slot in [0, `ZANNA_PAD_MAX`); invalid indices are ignored.
static void pad_clear_logical_state(int index) {
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return;
    rt_pad_state zero = {0};
    g_pads[index] = zero;
}

/// @brief Clamp a rumble motor strength to [0, 1], treating NaN/inf as 0.
/// @details Platform force-feedback APIs ultimately convert the value to an
///          integer motor magnitude. Casting NaN or infinity to an integer is
///          undefined, so the public and backend vibration paths share this
///          sanitizer before storing state or issuing OS calls.
/// @param value Caller-supplied motor strength.
/// @return A finite normalized motor strength.
static double pad_clamp_unit_finite(double value) {
    if (!isfinite(value))
        return 0.0;
    if (value < 0.0)
        return 0.0;
    if (value > 1.0)
        return 1.0;
    return value;
}

//=============================================================================
// Platform-Specific Gamepad Backend
//=============================================================================

// ===========================================================================
// Each platform (macOS / Linux / Windows) provides its own
// `platform_pad_poll` and `platform_pad_vibrate` implementations. The
// public API in `rt_pad_*` calls the platform variant via these
// forward declarations; only one definition is compiled in per build.
// ===========================================================================

/// @brief Platform-specific gamepad poll — refresh button/axis state into `g_pads[]`.
static void platform_pad_poll(void);

/// @brief Platform-specific rumble — set left/right motor strengths in [0,1].
/// @param index Logical controller slot.
/// @param left Normalized low-frequency motor strength.
/// @param right Normalized high-frequency motor strength.
static void platform_pad_vibrate(int64_t index, double left, double right);

#if RT_PLATFORM_MACOS
//-----------------------------------------------------------------------------
// macOS Implementation (IOKit HID Manager)
//-----------------------------------------------------------------------------

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDKeys.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDUsageTables.h>

typedef struct {
    IOHIDElementRef element;
    CFIndex min;
    CFIndex max;
} mac_axis;

typedef struct {
    IOHIDDeviceRef device;
    mac_axis left_x;
    mac_axis left_y;
    mac_axis right_x;
    mac_axis right_y;
    mac_axis left_trigger;
    mac_axis right_trigger;
    IOHIDElementRef hat;
    CFIndex hat_min;
    CFIndex hat_max;
    IOHIDElementRef buttons[ZANNA_PAD_BUTTON_MAX];
} mac_pad;

static IOHIDManagerRef g_hid_manager = NULL;
static mac_pad g_mac_pads[ZANNA_PAD_MAX];
static bool g_mac_initialized = false;

// ---------------------------------------------------------------------------
// macOS HID helpers — all of the `mac_*` functions wrap CoreFoundation /
// IOKit ref-counted objects (devices, axis elements, button elements).
// `mac_clear_pad` is the reverse of `mac_scan_devices`: it walks every
// retained element on a pad and releases it so we don't leak between
// device scans.
// ---------------------------------------------------------------------------

/// @brief Release an axis's HID element ref and zero its calibration min/max.
/// @param axis Axis binding to release; must be valid storage.
static void mac_release_axis(mac_axis *axis) {
    if (axis->element)
        CFRelease(axis->element);
    axis->element = NULL;
    axis->min = 0;
    axis->max = 0;
}

/// @brief Drop every IOKit ref retained for one pad slot (device, axes, hat, buttons).
/// @details Run before each rescan so re-plugging does not leak the prior device.
/// @param pad macOS gamepad slot to clear.
static void mac_clear_pad(mac_pad *pad) {
    if (pad->device)
        CFRelease(pad->device);
    pad->device = NULL;
    mac_release_axis(&pad->left_x);
    mac_release_axis(&pad->left_y);
    mac_release_axis(&pad->right_x);
    mac_release_axis(&pad->right_y);
    mac_release_axis(&pad->left_trigger);
    mac_release_axis(&pad->right_trigger);
    if (pad->hat)
        CFRelease(pad->hat);
    pad->hat = NULL;
    pad->hat_min = 0;
    pad->hat_max = 0;
    for (int i = 0; i < ZANNA_PAD_BUTTON_MAX; ++i) {
        if (pad->buttons[i])
            CFRelease(pad->buttons[i]);
        pad->buttons[i] = NULL;
    }
}

/// @brief Build a HID match dictionary for a given GenericDesktop usage (Joystick / GamePad).
/// @details Used to constrain `IOHIDManagerSetDeviceMatchingMultiple` to gamepad-shaped devices.
/// @param usage GenericDesktop usage identifier to match.
/// @return Owned mutable CoreFoundation dictionary, or NULL if any allocation fails.
static CFMutableDictionaryRef mac_make_match(uint32_t usage) {
    CFMutableDictionaryRef dict = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int page = kHIDPage_GenericDesktop;
    CFNumberRef page_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
    CFNumberRef usage_num = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    if (!dict || !page_num || !usage_num) {
        if (usage_num)
            CFRelease(usage_num);
        if (page_num)
            CFRelease(page_num);
        if (dict)
            CFRelease(dict);
        return NULL;
    }
    CFDictionarySetValue(dict, CFSTR(kIOHIDDeviceUsagePageKey), page_num);
    CFDictionarySetValue(dict, CFSTR(kIOHIDDeviceUsageKey), usage_num);
    CFRelease(page_num);
    CFRelease(usage_num);
    return dict;
}

/// @brief Capture an axis HID element + its calibration range for later normalisation.
/// @param axis Destination binding; any previously retained element is released.
/// @param element HID axis element to retain and inspect.
static void mac_store_axis(mac_axis *axis, IOHIDElementRef element) {
    if (axis->element)
        CFRelease(axis->element);
    axis->element = element;
    CFRetain(element);
    axis->min = IOHIDElementGetLogicalMin(element);
    axis->max = IOHIDElementGetLogicalMax(element);
}

/// @brief Map a HID Button usage code (1..11) to a Zanna `ZANNA_PAD_*` button index.
/// @param usage HID Button-page usage code.
/// @return Public gamepad-button index, or -1 outside the standard 11-button layout.
static int mac_button_index(uint32_t usage) {
    switch (usage) {
        case 1:
            return ZANNA_PAD_A;
        case 2:
            return ZANNA_PAD_B;
        case 3:
            return ZANNA_PAD_X;
        case 4:
            return ZANNA_PAD_Y;
        case 5:
            return ZANNA_PAD_LB;
        case 6:
            return ZANNA_PAD_RB;
        case 7:
            return ZANNA_PAD_BACK;
        case 8:
            return ZANNA_PAD_START;
        case 9:
            return ZANNA_PAD_LSTICK;
        case 10:
            return ZANNA_PAD_RSTICK;
        case 11:
            return ZANNA_PAD_GUIDE;
        default:
            return -1;
    }
}

/// @brief Enumerate matched HID gamepads and bind their elements into `g_mac_pads[]`.
///
/// Replaces the previous device list — every reattach event re-runs
/// this. For each detected device we walk its element list once and
/// classify each element by its GenericDesktop / Button HID usage:
/// X/Y → left stick, Rx/Ry → right stick, Z/Rz → triggers, Hatswitch
/// → D-pad, Slider → fallback trigger, Button N → button table.
static void mac_scan_devices(void) {
    for (int i = 0; i < ZANNA_PAD_MAX; ++i) {
        mac_clear_pad(&g_mac_pads[i]);
        pad_clear_logical_state(i);
    }

    if (!g_hid_manager)
        return;

    CFSetRef devices = IOHIDManagerCopyDevices(g_hid_manager);
    if (!devices)
        return;

    CFIndex count = CFSetGetCount(devices);
    if (count <= 0) {
        CFRelease(devices);
        return;
    }
    if ((uint64_t)count > (uint64_t)SIZE_MAX / sizeof(IOHIDDeviceRef)) {
        CFRelease(devices);
        return;
    }

    IOHIDDeviceRef *device_list = (IOHIDDeviceRef *)calloc((size_t)count, sizeof(IOHIDDeviceRef));
    if (!device_list) {
        CFRelease(devices);
        return;
    }
    CFSetGetValues(devices, (const void **)(void *)device_list);

    int pad_index = 0;
    for (CFIndex i = 0; i < count && pad_index < ZANNA_PAD_MAX; ++i) {
        IOHIDDeviceRef device = device_list[i];
        if (!device)
            continue;

        mac_pad *pad = &g_mac_pads[pad_index];
        pad->device = device;
        CFRetain(device);

        CFTypeRef product = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDProductKey));
        if (product && CFGetTypeID(product) == CFStringGetTypeID()) {
            if (!CFStringGetCString((CFStringRef)product,
                                    g_pads[pad_index].name,
                                    sizeof(g_pads[pad_index].name),
                                    kCFStringEncodingUTF8)) {
                snprintf(g_pads[pad_index].name,
                         sizeof(g_pads[pad_index].name),
                         "HID Gamepad %d",
                         pad_index);
            }
        } else {
            snprintf(g_pads[pad_index].name,
                     sizeof(g_pads[pad_index].name),
                     "HID Gamepad %d",
                     pad_index);
        }
        g_pads[pad_index].connected = true;

        CFArrayRef elements = IOHIDDeviceCopyMatchingElements(device, NULL, kIOHIDOptionsTypeNone);
        if (elements) {
            CFIndex elem_count = CFArrayGetCount(elements);
            for (CFIndex e = 0; e < elem_count; ++e) {
                IOHIDElementRef elem =
                    (IOHIDElementRef)(uintptr_t)CFArrayGetValueAtIndex(elements, e);
                if (!elem)
                    continue;

                IOHIDElementType type = IOHIDElementGetType(elem);
                if (type != kIOHIDElementTypeInput_Button && type != kIOHIDElementTypeInput_Misc &&
                    type != kIOHIDElementTypeInput_Axis) {
                    continue;
                }

                uint32_t page = IOHIDElementGetUsagePage(elem);
                uint32_t usage = IOHIDElementGetUsage(elem);
                if (page == kHIDPage_GenericDesktop) {
                    switch (usage) {
                        case kHIDUsage_GD_X:
                            mac_store_axis(&pad->left_x, elem);
                            break;
                        case kHIDUsage_GD_Y:
                            mac_store_axis(&pad->left_y, elem);
                            break;
                        case kHIDUsage_GD_Rx:
                            mac_store_axis(&pad->right_x, elem);
                            break;
                        case kHIDUsage_GD_Ry:
                            mac_store_axis(&pad->right_y, elem);
                            break;
                        case kHIDUsage_GD_Z:
                            mac_store_axis(&pad->left_trigger, elem);
                            break;
                        case kHIDUsage_GD_Rz:
                            mac_store_axis(&pad->right_trigger, elem);
                            break;
                        case kHIDUsage_GD_Hatswitch:
                            pad->hat = elem;
                            CFRetain(elem);
                            pad->hat_min = IOHIDElementGetLogicalMin(elem);
                            pad->hat_max = IOHIDElementGetLogicalMax(elem);
                            break;
                        case kHIDUsage_GD_Slider:
                            if (!pad->right_trigger.element)
                                mac_store_axis(&pad->right_trigger, elem);
                            else if (!pad->left_trigger.element)
                                mac_store_axis(&pad->left_trigger, elem);
                            break;
                        default:
                            break;
                    }
                } else if (page == kHIDPage_Button) {
                    int index = mac_button_index(usage);
                    if (index >= 0 && index < ZANNA_PAD_BUTTON_MAX && !pad->buttons[index]) {
                        pad->buttons[index] = elem;
                        CFRetain(elem);
                    }
                }
            }
            CFRelease(elements);
        }

        pad_index++;
    }

    free(device_list);
    CFRelease(devices);
}

/// @brief Lazily create the IOHID manager and register it for the standard gamepad usages.
///
/// One-time setup. Sets up a matching dictionary array for both
/// `Joystick` and `GamePad` GenericDesktop usages so we catch
/// X-Input controllers (which advertise as `GamePad`) as well as
/// classic joysticks. After this returns, `mac_scan_devices` can
/// enumerate the manager's bound device list.
static void mac_init_manager(void) {
    if (g_mac_initialized)
        return;

    g_hid_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (g_hid_manager) {
        CFMutableDictionaryRef matches[3];
        matches[0] = mac_make_match(kHIDUsage_GD_GamePad);
        matches[1] = mac_make_match(kHIDUsage_GD_Joystick);
        matches[2] = mac_make_match(kHIDUsage_GD_MultiAxisController);

        if (matches[0] && matches[1] && matches[2]) {
            CFArrayRef match_array = CFArrayCreate(
                kCFAllocatorDefault, (const void **)(void *)matches, 3, &kCFTypeArrayCallBacks);
            if (match_array) {
                IOHIDManagerSetDeviceMatchingMultiple(g_hid_manager, match_array);
                IOReturn open_result = IOHIDManagerOpen(g_hid_manager, kIOHIDOptionsTypeNone);
                CFRelease(match_array);
                if (open_result != kIOReturnSuccess) {
                    CFRelease(g_hid_manager);
                    g_hid_manager = NULL;
                }
            } else {
                CFRelease(g_hid_manager);
                g_hid_manager = NULL;
            }
        } else {
            CFRelease(g_hid_manager);
            g_hid_manager = NULL;
        }

        for (int i = 0; i < 3; ++i)
            if (matches[i])
                CFRelease(matches[i]);
    }

    mac_scan_devices();
    g_mac_initialized = true;
}

/// @brief Map a raw HID axis reading to the [-1, +1] range using device calibration.
/// @param value Raw HID axis sample.
/// @param min Calibrated logical minimum.
/// @param max Calibrated logical maximum.
/// @return Normalized axis value, or zero for a degenerate range.
static double mac_normalize_axis(CFIndex value, CFIndex min, CFIndex max) {
    if (max == min)
        return 0.0;
    double norm = ((double)value - (double)min) / ((double)max - (double)min);
    return norm * 2.0 - 1.0;
}

/// @brief Map a raw trigger HID reading to the [0, 1] range (triggers don't go negative).
/// @param value Raw HID trigger sample.
/// @param min Calibrated logical minimum.
/// @param max Calibrated logical maximum.
/// @return Trigger value clamped to [0,1], or zero for a degenerate range.
static double mac_normalize_trigger(CFIndex value, CFIndex min, CFIndex max) {
    if (max == min)
        return 0.0;
    double norm = ((double)value - (double)min) / ((double)max - (double)min);
    if (norm < 0.0)
        norm = 0.0;
    if (norm > 1.0)
        norm = 1.0;
    return norm;
}

/// @brief Read the current integer value of one HID element.
/// @details Wraps `IOHIDDeviceGetValue` and copies out the integer payload.
/// @param device Borrowed HID device containing the element.
/// @param element Borrowed HID element; NULL fails.
/// @param out Destination for the integer payload on success.
/// @return true on success, false if the element couldn't be read.
static bool mac_read_value(IOHIDDeviceRef device, IOHIDElementRef element, CFIndex *out) {
    IOHIDValueRef value_ref = NULL;
    if (!element)
        return false;
    if (IOHIDDeviceGetValue(device, element, &value_ref) != kIOReturnSuccess || !value_ref)
        return false;
    *out = IOHIDValueGetIntegerValue(value_ref);
    return true;
}

/// @brief Translate a HID hat-switch value (0–7, 8=center) into D-pad button bits.
///
/// HID hats encode 8 directions clockwise starting from "up = 0".
/// We unpack each into the corresponding `up/down/left/right` button
/// flags so callers don't see the raw hat encoding.
/// @param pad Logical gamepad state to update.
/// @param hat_value HID hat value, with values outside [0,7] treated as centered.
static void mac_apply_hat(rt_pad_state *pad, int hat_value) {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;

    switch (hat_value) {
        case 0: // Up
            up = true;
            break;
        case 1: // Up-right
            up = true;
            right = true;
            break;
        case 2: // Right
            right = true;
            break;
        case 3: // Down-right
            down = true;
            right = true;
            break;
        case 4: // Down
            down = true;
            break;
        case 5: // Down-left
            down = true;
            left = true;
            break;
        case 6: // Left
            left = true;
            break;
        case 7: // Up-left
            up = true;
            left = true;
            break;
        default:
            break;
    }

    pad->buttons[ZANNA_PAD_UP] = up;
    pad->buttons[ZANNA_PAD_DOWN] = down;
    pad->buttons[ZANNA_PAD_LEFT] = left;
    pad->buttons[ZANNA_PAD_RIGHT] = right;
}

/// @brief macOS IOKit HID implementation of platform_pad_poll.
/// @details Lazily initializes the IOKit HID manager on first call, then scans
///   for newly connected devices if no pad is currently marked connected.
///   For each registered device slot, this reads the current value of every
///   axis element (left stick, right stick, triggers) via mac_read_value, maps
///   raw hardware ranges to [-1, +1] / [0, 1] using mac_normalize_axis and
///   mac_normalize_trigger, reads all mapped button elements, and converts any
///   hat-switch element into the four discrete D-pad button flags.
///   The results are written directly into the shared g_pads[] array so that
///   the public rt_pad_* API functions can read consistent state.
static void platform_pad_poll(void) {
    if (!g_mac_initialized)
        mac_init_manager();

    if (!g_hid_manager)
        return;

    bool has_free_slot = false;
    for (int i = 0; i < ZANNA_PAD_MAX; ++i) {
        if (!g_mac_pads[i].device) {
            has_free_slot = true;
            break;
        }
    }
    if (has_free_slot)
        mac_scan_devices();

    bool need_rescan = false;
    for (int i = 0; i < ZANNA_PAD_MAX; ++i) {
        mac_pad *pad = &g_mac_pads[i];
        if (!pad->device) {
            pad_clear_logical_state(i);
            continue;
        }

        g_pads[i].connected = true;
        for (int b = 0; b < ZANNA_PAD_BUTTON_MAX; ++b)
            g_pads[i].buttons[b] = false;
        g_pads[i].left_x = 0.0;
        g_pads[i].left_y = 0.0;
        g_pads[i].right_x = 0.0;
        g_pads[i].right_y = 0.0;
        g_pads[i].left_trigger = 0.0;
        g_pads[i].right_trigger = 0.0;

        CFIndex value = 0;
        int read_attempts = 0;
        int read_failures = 0;
#define MAC_READ_AXIS(axis_member, target_expr, normalizer_expr)                                   \
    do {                                                                                           \
        if ((axis_member).element) {                                                               \
            read_attempts++;                                                                       \
            if (mac_read_value(pad->device, (axis_member).element, &value))                        \
                (target_expr) = (normalizer_expr);                                                 \
            else                                                                                   \
                read_failures++;                                                                   \
        }                                                                                          \
    } while (0)

        MAC_READ_AXIS(pad->left_x,
                      g_pads[i].left_x,
                      mac_normalize_axis(value, pad->left_x.min, pad->left_x.max));
        MAC_READ_AXIS(pad->left_y,
                      g_pads[i].left_y,
                      mac_normalize_axis(value, pad->left_y.min, pad->left_y.max));
        MAC_READ_AXIS(pad->right_x,
                      g_pads[i].right_x,
                      mac_normalize_axis(value, pad->right_x.min, pad->right_x.max));
        MAC_READ_AXIS(pad->right_y,
                      g_pads[i].right_y,
                      mac_normalize_axis(value, pad->right_y.min, pad->right_y.max));
        MAC_READ_AXIS(pad->left_trigger,
                      g_pads[i].left_trigger,
                      mac_normalize_trigger(value, pad->left_trigger.min, pad->left_trigger.max));
        MAC_READ_AXIS(pad->right_trigger,
                      g_pads[i].right_trigger,
                      mac_normalize_trigger(value, pad->right_trigger.min, pad->right_trigger.max));
#undef MAC_READ_AXIS

        for (int b = 0; b < ZANNA_PAD_BUTTON_MAX; ++b) {
            if (!pad->buttons[b])
                continue;
            read_attempts++;
            CFIndex btn_value = 0;
            if (mac_read_value(pad->device, pad->buttons[b], &btn_value))
                g_pads[i].buttons[b] = btn_value != 0;
            else
                read_failures++;
        }

        if (pad->hat) {
            read_attempts++;
            if (mac_read_value(pad->device, pad->hat, &value))
                mac_apply_hat(&g_pads[i], (int)value);
            else
                read_failures++;
        }
        if (read_attempts > 0 && read_failures == read_attempts) {
            mac_clear_pad(pad);
            pad_clear_logical_state(i);
            need_rescan = true;
        }
    }
    if (need_rescan)
        mac_scan_devices();
}

/// @brief macOS IOKit HID implementation of platform_pad_vibrate — no-op.
/// @details The generic IOKit HID API does not expose force-feedback output
///   reports for gamepads.  Rumble motors are device-specific and require
///   vendor extensions (e.g. the MFi FFBDevice protocol or Sony's DualShock
///   HID reports) that are not available through the generic IOHIDManager
///   interface used by this backend.  Parameters are silenced to avoid
///   compiler warnings.
/// @param index Ignored public gamepad slot.
/// @param left Ignored left-motor strength.
/// @param right Ignored right-motor strength.
static void platform_pad_vibrate(int64_t index, double left, double right) {
    (void)index;
    (void)left;
    (void)right;
    // Vibration is not available via generic HID APIs on macOS.
}

#elif RT_PLATFORM_LINUX
//-----------------------------------------------------------------------------
// Linux Implementation (evdev)
//-----------------------------------------------------------------------------

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int fd;
    bool has_rumble;
    int rumble_id;
    dev_t device_rdev;
    ino_t device_ino;
    int abs_min[ABS_MAX + 1];
    int abs_max[ABS_MAX + 1];
} linux_pad;

static linux_pad g_linux_pads[ZANNA_PAD_MAX];
static bool g_linux_initialized = false;

// ---------------------------------------------------------------------------
// Linux evdev gamepad helpers — read raw events from /dev/input/eventN
// devices. Each `linux_*` function below is the analogue of the
// corresponding `mac_*` helper above, but for the Linux input subsystem.
// ---------------------------------------------------------------------------

/// @brief Test bit `bit` in a Linux ioctl bitset (EVIOCGBIT-shaped buffer).
/// @param bits Borrowed ioctl bitset.
/// @param bit Non-negative bit index to test.
/// @return true when the requested capability bit is set.
static bool linux_test_bit(const unsigned long *bits, int bit) {
    return (bits[bit / (int)(8 * sizeof(unsigned long))] >>
            (bit % (int)(8 * sizeof(unsigned long)))) &
           1UL;
}

/// @brief Heuristic — does this input device look like a gamepad/joystick?
///
/// Requires both EV_KEY (button events) and EV_ABS (axes) capabilities,
/// plus at least one of the standard gamepad button keys
/// (BTN_GAMEPAD/BTN_JOYSTICK/BTN_SOUTH/BTN_NORTH). Filters out
/// keyboards, mice, and touchpads that share the /dev/input namespace.
/// @param fd Open evdev file descriptor.
/// @return true when capability queries identify a gamepad-like device.
static bool linux_is_gamepad(int fd) {
    unsigned long ev_bits[(EV_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
    unsigned long key_bits[(KEY_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];

    memset(ev_bits, 0, sizeof(ev_bits));
    memset(key_bits, 0, sizeof(key_bits));

    if (ioctl(fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0)
        return false;
    if (!linux_test_bit(ev_bits, EV_KEY) || !linux_test_bit(ev_bits, EV_ABS))
        return false;
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0)
        return false;

    return linux_test_bit(key_bits, BTN_GAMEPAD) || linux_test_bit(key_bits, BTN_JOYSTICK) ||
           linux_test_bit(key_bits, BTN_SOUTH) || linux_test_bit(key_bits, BTN_NORTH);
}

/// @brief Close the device fd (if open) and reset the pad's calibration to defaults.
/// @param pad Linux backend slot to close and reset.
static void linux_reset_pad(linux_pad *pad) {
    if (pad->fd >= 0) {
        if (pad->rumble_id >= 0)
            (void)ioctl(pad->fd, EVIOCRMFF, pad->rumble_id);
        close(pad->fd);
    }
    pad->fd = -1;
    pad->has_rumble = false;
    pad->rumble_id = -1;
    pad->device_rdev = 0;
    pad->device_ino = 0;
    for (int i = 0; i <= ABS_MAX; ++i) {
        pad->abs_min[i] = -32768;
        pad->abs_max[i] = 32767;
    }
}

/// @brief Clear the public logical state for one Linux gamepad slot.
/// @details Used for initialization and disconnect handling so stale buttons, axes, triggers, and
///          names do not remain visible after the underlying evdev fd disappears.
/// @param index Slot index in @c g_pads.
static void linux_clear_logical_pad(int index) {
    pad_clear_logical_state(index);
}

/// @brief Return whether an evdev device identity is already bound to a slot.
/// @param rdev Device special-file id from stat/fstat.
/// @param ino Inode from stat/fstat.
/// @return true if any open Linux pad slot already owns the same device.
static bool linux_device_already_bound(dev_t rdev, ino_t ino) {
    for (int i = 0; i < ZANNA_PAD_MAX; ++i) {
        if (g_linux_pads[i].fd >= 0 && g_linux_pads[i].device_rdev == rdev &&
            g_linux_pads[i].device_ino == ino)
            return true;
    }
    return false;
}

/// @brief Find the first unbound Linux gamepad slot.
/// @return Slot index in [0, ZANNA_PAD_MAX), or -1 when all slots are occupied.
static int linux_find_free_pad_slot(void) {
    for (int i = 0; i < ZANNA_PAD_MAX; ++i) {
        if (g_linux_pads[i].fd < 0)
            return i;
    }
    return -1;
}

/// @brief Open an evdev path non-blocking and close-on-exec.
/// @details Uses O_CLOEXEC when available and falls back to FD_CLOEXEC through
///          fcntl on older libc/kernel combinations.
/// @param path `/dev/input/event*` path.
/// @param writable Non-zero to request read/write access for force feedback.
/// @return File descriptor on success; -1 on open failure.
static int linux_open_evdev_nonblocking(const char *path, int writable) {
    int flags = (writable ? O_RDWR : O_RDONLY) | O_NONBLOCK;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path, flags);
#if !defined(O_CLOEXEC) && defined(FD_CLOEXEC)
    if (fd >= 0) {
        int fd_flags = fcntl(fd, F_GETFD);
        if (fd_flags >= 0)
            (void)fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
#endif
    return fd;
}

/// @brief Map a raw evdev axis reading to the [-1, +1] range using its absinfo range.
/// @param value Raw evdev axis sample.
/// @param min Device-reported absolute minimum.
/// @param max Device-reported absolute maximum.
/// @return Normalized axis value, or zero for a degenerate range.
static double linux_normalize_axis(int value, int min, int max) {
    if (max == min)
        return 0.0;
    double norm = ((double)value - (double)min) / ((double)max - (double)min);
    return norm * 2.0 - 1.0;
}

/// @brief Map a raw evdev trigger reading to [0, 1] (clamped — triggers can't go negative).
/// @param value Raw evdev trigger sample.
/// @param min Device-reported absolute minimum.
/// @param max Device-reported absolute maximum.
/// @return Trigger value clamped to [0,1], or zero for a degenerate range.
static double linux_normalize_trigger(int value, int min, int max) {
    if (max == min)
        return 0.0;
    double norm = ((double)value - (double)min) / ((double)max - (double)min);
    if (norm < 0.0)
        norm = 0.0;
    if (norm > 1.0)
        norm = 1.0;
    return norm;
}

/// @brief Translate an evdev D-pad axis (`ABS_HAT0X` or `ABS_HAT0Y`) into directional buttons.
/// @param pad Logical gamepad state to update.
/// @param value Signed hat-axis value; negative and positive select opposite directions.
/// @param is_x true for left/right, false for up/down.
static void linux_apply_hat(rt_pad_state *pad, int value, bool is_x) {
    if (is_x) {
        pad->buttons[ZANNA_PAD_LEFT] = value < 0;
        pad->buttons[ZANNA_PAD_RIGHT] = value > 0;
    } else {
        pad->buttons[ZANNA_PAD_UP] = value < 0;
        pad->buttons[ZANNA_PAD_DOWN] = value > 0;
    }
}

/// @brief Scan `/dev/input/event*` and bind newly discovered gamepads into free slots.
/// @details Already-open devices are skipped by comparing their device identity. This makes the
///          scan safe to call after initialization for hotplug support without duplicating existing
///          controllers into empty slots.
static void linux_scan_gamepads(void) {
    DIR *dir = opendir("/dev/input");
    if (!dir)
        return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        int pad_index = linux_find_free_pad_slot();
        if (pad_index < 0)
            break;
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        char path[256];
        int written = snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        if (written < 0 || (size_t)written >= sizeof(path))
            continue;

        struct stat path_stat;
        if (stat(path, &path_stat) != 0)
            continue;
        if (linux_device_already_bound(path_stat.st_rdev, path_stat.st_ino))
            continue;

        int fd = linux_open_evdev_nonblocking(path, 1);
        bool fd_writable = fd >= 0;
        if (fd < 0) {
            fd = linux_open_evdev_nonblocking(path, 0);
            fd_writable = false;
        }
        if (fd < 0)
            continue;

        struct stat fd_stat;
        if (fstat(fd, &fd_stat) != 0 ||
            linux_device_already_bound(fd_stat.st_rdev, fd_stat.st_ino)) {
            close(fd);
            continue;
        }

        if (!linux_is_gamepad(fd)) {
            close(fd);
            continue;
        }

        linux_pad *pad = &g_linux_pads[pad_index];
        linux_reset_pad(pad);
        pad->fd = fd;
        pad->device_rdev = fd_stat.st_rdev;
        pad->device_ino = fd_stat.st_ino;
        linux_clear_logical_pad(pad_index);

        char name[64];
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0) {
            name[sizeof(name) - 1u] = '\0';
            snprintf(g_pads[pad_index].name, sizeof(g_pads[pad_index].name), "%s", name);
        } else {
            snprintf(g_pads[pad_index].name,
                     sizeof(g_pads[pad_index].name),
                     "Linux Gamepad %d",
                     pad_index);
        }

        unsigned long ff_bits[(FF_MAX + 8 * sizeof(unsigned long)) / (8 * sizeof(unsigned long))];
        memset(ff_bits, 0, sizeof(ff_bits));
        if (fd_writable && ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ff_bits)), ff_bits) >= 0 &&
            linux_test_bit(ff_bits, FF_RUMBLE)) {
            pad->has_rumble = true;
        }

        int abs_codes[] = {ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
        for (size_t a = 0; a < sizeof(abs_codes) / sizeof(abs_codes[0]); ++a) {
            struct input_absinfo absinfo;
            if (ioctl(fd, EVIOCGABS(abs_codes[a]), &absinfo) == 0) {
                pad->abs_min[abs_codes[a]] = absinfo.minimum;
                pad->abs_max[abs_codes[a]] = absinfo.maximum;
            }
        }

        g_pads[pad_index].connected = true;
    }

    closedir(dir);
}

/// @brief Initialize Linux pad slots and perform the first evdev scan.
/// @details The expensive state reset happens once. Device scanning is reusable so later polls can
///          discover hotplugged controllers when slots are available.
static void linux_pad_init(void) {
    if (g_linux_initialized)
        return;

    for (int i = 0; i < ZANNA_PAD_MAX; ++i) {
        g_linux_pads[i].fd = -1;
        linux_reset_pad(&g_linux_pads[i]);
        linux_clear_logical_pad(i);
    }

    linux_scan_gamepads();
    g_linux_initialized = true;
}

/// @brief Linux evdev implementation of platform_pad_poll.
/// @details Calls linux_pad_init to ensure /dev/input/event* devices have been
///   scanned and opened, and rescans free slots for hotplugged devices. For each active pad file
///   descriptor, drains the evdev
///   event queue in a non-blocking read loop: EV_KEY events update button state
///   (BTN_SOUTH/EAST/WEST/NORTH map to A/B/X/Y; shoulder, stick-click, and
///   guide buttons follow), while EV_ABS events update analog axes and triggers
///   (ABS_X/Y for the left stick, ABS_RX/RY for the right stick, ABS_Z/RZ for
///   the triggers, and ABS_HAT0X/Y for D-pad via linux_apply_hat).  If the read
///   returns ENODEV the device has been unplugged, so the pad is reset and
///   marked disconnected.
static void platform_pad_poll(void) {
    linux_pad_init();
    if (linux_find_free_pad_slot() >= 0)
        linux_scan_gamepads();
    bool need_rescan = false;

    for (int i = 0; i < ZANNA_PAD_MAX; ++i) {
        linux_pad *pad = &g_linux_pads[i];
        if (pad->fd < 0) {
            linux_clear_logical_pad(i);
            continue;
        }

        g_pads[i].connected = true;

        struct input_event ev;
        ssize_t n = 0;
        while ((n = read(pad->fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_KEY) {
                bool down = ev.value != 0;
                switch (ev.code) {
                    case BTN_SOUTH:
                        g_pads[i].buttons[ZANNA_PAD_A] = down;
                        break;
                    case BTN_EAST:
                        g_pads[i].buttons[ZANNA_PAD_B] = down;
                        break;
                    case BTN_WEST:
                        g_pads[i].buttons[ZANNA_PAD_X] = down;
                        break;
                    case BTN_NORTH:
                        g_pads[i].buttons[ZANNA_PAD_Y] = down;
                        break;
                    case BTN_TL:
                        g_pads[i].buttons[ZANNA_PAD_LB] = down;
                        break;
                    case BTN_TR:
                        g_pads[i].buttons[ZANNA_PAD_RB] = down;
                        break;
                    case BTN_SELECT:
                        g_pads[i].buttons[ZANNA_PAD_BACK] = down;
                        break;
                    case BTN_START:
                        g_pads[i].buttons[ZANNA_PAD_START] = down;
                        break;
                    case BTN_THUMBL:
                        g_pads[i].buttons[ZANNA_PAD_LSTICK] = down;
                        break;
                    case BTN_THUMBR:
                        g_pads[i].buttons[ZANNA_PAD_RSTICK] = down;
                        break;
                    case BTN_MODE:
                        g_pads[i].buttons[ZANNA_PAD_GUIDE] = down;
                        break;
                    case BTN_DPAD_UP:
                        g_pads[i].buttons[ZANNA_PAD_UP] = down;
                        break;
                    case BTN_DPAD_DOWN:
                        g_pads[i].buttons[ZANNA_PAD_DOWN] = down;
                        break;
                    case BTN_DPAD_LEFT:
                        g_pads[i].buttons[ZANNA_PAD_LEFT] = down;
                        break;
                    case BTN_DPAD_RIGHT:
                        g_pads[i].buttons[ZANNA_PAD_RIGHT] = down;
                        break;
                    default:
                        break;
                }
            } else if (ev.type == EV_ABS) {
                switch (ev.code) {
                    case ABS_X:
                        g_pads[i].left_x = linux_normalize_axis(
                            ev.value, pad->abs_min[ABS_X], pad->abs_max[ABS_X]);
                        break;
                    case ABS_Y:
                        g_pads[i].left_y = linux_normalize_axis(
                            ev.value, pad->abs_min[ABS_Y], pad->abs_max[ABS_Y]);
                        break;
                    case ABS_RX:
                        g_pads[i].right_x = linux_normalize_axis(
                            ev.value, pad->abs_min[ABS_RX], pad->abs_max[ABS_RX]);
                        break;
                    case ABS_RY:
                        g_pads[i].right_y = linux_normalize_axis(
                            ev.value, pad->abs_min[ABS_RY], pad->abs_max[ABS_RY]);
                        break;
                    case ABS_Z:
                        g_pads[i].left_trigger = linux_normalize_trigger(
                            ev.value, pad->abs_min[ABS_Z], pad->abs_max[ABS_Z]);
                        break;
                    case ABS_RZ:
                        g_pads[i].right_trigger = linux_normalize_trigger(
                            ev.value, pad->abs_min[ABS_RZ], pad->abs_max[ABS_RZ]);
                        break;
                    case ABS_HAT0X:
                        linux_apply_hat(&g_pads[i], ev.value, true);
                        break;
                    case ABS_HAT0Y:
                        linux_apply_hat(&g_pads[i], ev.value, false);
                        break;
                    default:
                        break;
                }
            }
        }

        if (n < 0 && errno == ENODEV) {
            linux_reset_pad(pad);
            linux_clear_logical_pad(i);
            need_rescan = true;
        }
    }

    if (need_rescan && linux_find_free_pad_slot() >= 0)
        linux_scan_gamepads();
}

/// @brief Linux evdev FF_RUMBLE implementation of platform_pad_vibrate.
/// @details Submits a force-feedback effect to the evdev device using the
///   kernel's FF_RUMBLE interface.  The caller-supplied [left, right] strengths
///   are clamped to [0, 1], then scaled to the 0–0xFFFF uint16 range expected
///   by ff_effect.u.rumble.strong_magnitude (low-frequency / left motor) and
///   weak_magnitude (high-frequency / right motor).  EVIOCSFF uploads the
///   effect and EV_FF/play activates it for 1000 ms.  If EVIOCSFF fails (e.g.
///   the device was hot-unplugged), has_rumble is cleared so future calls
///   short-circuit immediately.  Index bounds are validated before dereferencing
///   g_linux_pads.
/// @param index Public Linux gamepad slot.
/// @param left Normalized strong/left motor strength.
/// @param right Normalized weak/right motor strength.
static void platform_pad_vibrate(int64_t index, double left, double right) {
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return;
    linux_pad *pad = &g_linux_pads[index];
    if (!pad->has_rumble || pad->fd < 0)
        return;

    struct ff_effect effect;
    memset(&effect, 0, sizeof(effect));
    effect.type = FF_RUMBLE;
    effect.id = pad->rumble_id;
    double left_amp = pad_clamp_unit_finite(left);
    double right_amp = pad_clamp_unit_finite(right);

    effect.u.rumble.strong_magnitude = (uint16_t)(left_amp * 0xFFFF);
    effect.u.rumble.weak_magnitude = (uint16_t)(right_amp * 0xFFFF);
    effect.replay.length = 1000;
    effect.replay.delay = 0;

    if (ioctl(pad->fd, EVIOCSFF, &effect) < 0) {
        pad->has_rumble = false;
        return;
    }
    pad->rumble_id = effect.id;

    struct input_event play;
    memset(&play, 0, sizeof(play));
    play.type = EV_FF;
    play.code = effect.id;
    play.value = 1;
    if (write(pad->fd, &play, sizeof(play)) != (ssize_t)sizeof(play)) {
        if (pad->rumble_id >= 0)
            (void)ioctl(pad->fd, EVIOCRMFF, pad->rumble_id);
        pad->rumble_id = -1;
        pad->has_rumble = false;
        g_pads[index].vibration_left = 0.0;
        g_pads[index].vibration_right = 0.0;
    }
}

#elif RT_PLATFORM_WINDOWS
//-----------------------------------------------------------------------------
// Windows Implementation (XInput)
//-----------------------------------------------------------------------------

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// Ensure architecture is defined before including Windows headers (MSVC C11 workaround)
#if defined(_M_AMD64) && !defined(_AMD64_)
#define _AMD64_
#elif defined(_M_IX86) && !defined(_X86_)
#define _X86_
#elif defined(_M_ARM64) && !defined(_ARM64_)
#define _ARM64_
#endif
#include <Xinput.h>
#include <windows.h>

/// @brief Normalize a signed XInput thumbstick component with the platform deadzone applied.
/// @details XInput devices report small noisy values near center. This helper zeroes
///          values within the SDK-provided deadzone, then rescales the remaining
///          signed range to preserve full [-1, 1] output at the physical extremes.
/// @param value Raw signed XInput thumbstick component.
/// @param deadzone Non-negative SDK deadzone magnitude.
/// @return Rescaled component in [-1,1], or zero inside the deadzone.
static double xinput_normalize_thumb(SHORT value, SHORT deadzone) {
    int raw = (int)value;
    int magnitude = raw < 0 ? -raw : raw;
    int dz = (int)deadzone;
    if (magnitude <= dz)
        return 0.0;
    int max_magnitude = raw < 0 ? 32768 : 32767;
    int span = max_magnitude - dz;
    if (span <= 0)
        return 0.0;
    double normalized = (double)(magnitude - dz) / (double)span;
    return raw < 0 ? -normalized : normalized;
}

/// @brief Windows XInput implementation of platform_pad_poll.
/// @details Calls XInputGetState for each pad slot in [0, ZANNA_PAD_MAX).
///   On ERROR_SUCCESS the pad is marked connected: all digital buttons are
///   extracted from the XINPUT_GAMEPAD.wButtons bitmask, analog sticks are
///   normalized from the signed 16-bit SHORT range to [-1, +1] (using -32768
///   as the negative divisor to handle the asymmetric two's-complement range
///   correctly), and triggers are mapped from BYTE [0, 255] to [0, 1].
///   The guide button is conditionally compiled under XINPUT_GAMEPAD_GUIDE
///   since it is absent from some XInput SDK versions.  When XInputGetState
///   returns any other error the pad is marked disconnected and all state is
///   zeroed.
static void platform_pad_poll(void) {
    for (DWORD i = 0; i < ZANNA_PAD_MAX; ++i) {
        XINPUT_STATE state;
        DWORD result = XInputGetState(i, &state);
        if (result == ERROR_SUCCESS) {
            g_pads[i].connected = true;
            snprintf(g_pads[i].name, sizeof(g_pads[i].name), "XInput Pad %lu", (unsigned long)i);

            WORD buttons = state.Gamepad.wButtons;
            g_pads[i].buttons[ZANNA_PAD_A] = (buttons & XINPUT_GAMEPAD_A) != 0;
            g_pads[i].buttons[ZANNA_PAD_B] = (buttons & XINPUT_GAMEPAD_B) != 0;
            g_pads[i].buttons[ZANNA_PAD_X] = (buttons & XINPUT_GAMEPAD_X) != 0;
            g_pads[i].buttons[ZANNA_PAD_Y] = (buttons & XINPUT_GAMEPAD_Y) != 0;
            g_pads[i].buttons[ZANNA_PAD_LB] = (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
            g_pads[i].buttons[ZANNA_PAD_RB] = (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
            g_pads[i].buttons[ZANNA_PAD_BACK] = (buttons & XINPUT_GAMEPAD_BACK) != 0;
            g_pads[i].buttons[ZANNA_PAD_START] = (buttons & XINPUT_GAMEPAD_START) != 0;
            g_pads[i].buttons[ZANNA_PAD_LSTICK] = (buttons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
            g_pads[i].buttons[ZANNA_PAD_RSTICK] = (buttons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
            g_pads[i].buttons[ZANNA_PAD_UP] = (buttons & XINPUT_GAMEPAD_DPAD_UP) != 0;
            g_pads[i].buttons[ZANNA_PAD_DOWN] = (buttons & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
            g_pads[i].buttons[ZANNA_PAD_LEFT] = (buttons & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
            g_pads[i].buttons[ZANNA_PAD_RIGHT] = (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;
#ifdef XINPUT_GAMEPAD_GUIDE
            g_pads[i].buttons[ZANNA_PAD_GUIDE] = (buttons & XINPUT_GAMEPAD_GUIDE) != 0;
#else
            g_pads[i].buttons[ZANNA_PAD_GUIDE] = false;
#endif

            SHORT lx = state.Gamepad.sThumbLX;
            SHORT ly = state.Gamepad.sThumbLY;
            SHORT rx = state.Gamepad.sThumbRX;
            SHORT ry = state.Gamepad.sThumbRY;

            g_pads[i].left_x = xinput_normalize_thumb(lx, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            g_pads[i].left_y = xinput_normalize_thumb(ly, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
            g_pads[i].right_x = xinput_normalize_thumb(rx, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
            g_pads[i].right_y = xinput_normalize_thumb(ry, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);

            g_pads[i].left_trigger = state.Gamepad.bLeftTrigger / 255.0;
            g_pads[i].right_trigger = state.Gamepad.bRightTrigger / 255.0;
        } else {
            pad_clear_logical_state((int)i);
        }
    }
}

/// @brief Windows XInput implementation of platform_pad_vibrate.
/// @details Clamps the caller-supplied [left, right] strengths to [0, 1], scales
///   them to the 0–65535 WORD range required by XINPUT_VIBRATION, and calls
///   XInputSetState to activate the motors.  The left motor is the large
///   low-frequency rumble motor; the right motor is the small high-frequency
///   vibration motor — mapping left→wLeftMotorSpeed and right→wRightMotorSpeed
///   preserves the conventional strong/weak semantics.  Index bounds are
///   validated before the XInput call.
/// @param index Public XInput gamepad slot.
/// @param left Requested strong/left motor strength.
/// @param right Requested weak/right motor strength.
static void platform_pad_vibrate(int64_t index, double left, double right) {
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return;

    double left_amp = pad_clamp_unit_finite(left);
    double right_amp = pad_clamp_unit_finite(right);

    XINPUT_VIBRATION vib;
    vib.wLeftMotorSpeed = (WORD)(left_amp * 65535.0);
    vib.wRightMotorSpeed = (WORD)(right_amp * 65535.0);
    DWORD result = XInputSetState((DWORD)index, &vib);
    if (result != ERROR_SUCCESS) {
        g_pads[index].vibration_left = 0.0;
        g_pads[index].vibration_right = 0.0;
        if (result == ERROR_DEVICE_NOT_CONNECTED)
            pad_clear_logical_state((int)index);
    }
}

#else
//-----------------------------------------------------------------------------
// Unsupported Platform
//-----------------------------------------------------------------------------

/// @brief Fallback no-op implementation of platform_pad_poll for unsupported platforms.
/// @details Compiled when none of the recognised platform macros (__APPLE__,
///   __linux__, _WIN32) are defined.  All g_pads[] entries remain in their
///   default-initialised disconnected state; no hardware is queried.
static void platform_pad_poll(void) {
    // No gamepad support on this platform
}

/// @brief Fallback no-op implementation of platform_pad_vibrate for unsupported platforms.
/// @details Compiled when none of the recognised platform macros are defined.
///   Vibration output requires platform-specific APIs (IOKit FF, evdev FF_RUMBLE,
///   XInput) that are unavailable here.  Parameters are suppressed to avoid
///   unused-variable warnings.
/// @param index Ignored public gamepad slot.
/// @param left Ignored left-motor strength.
/// @param right Ignored right-motor strength.
static void platform_pad_vibrate(int64_t index, double left, double right) {
    (void)index;
    (void)left;
    (void)right;
}

#endif

//=============================================================================
// Deadzone Application
//=============================================================================

/// @brief Apply a scaled radial deadzone and return one stick component.
/// @details Axial deadzones distort diagonal input because each component is rescaled
///          independently. This helper treats the pair as a vector, removes the inner radius,
///          and rescales the remaining magnitude back to [0, 1].
/// @param x Clamped horizontal stick component.
/// @param y Clamped vertical stick component.
/// @param want_x Non-zero to return the X component; zero to return Y.
/// @return Deadzone-adjusted component in [-1, 1].
static double apply_radial_deadzone_component(double x, double y, int want_x) {
    if (!isfinite(x))
        x = 0.0;
    if (!isfinite(y))
        y = 0.0;
    if (g_pad_deadzone <= 0.0)
        return want_x ? x : y;
    if (g_pad_deadzone >= 1.0)
        return 0.0;

    double mag = sqrt(x * x + y * y);
    if (!isfinite(mag) || mag <= g_pad_deadzone)
        return 0.0;

    double scaled_mag = mag > 1.0 ? 1.0 : mag;
    double scaled = (scaled_mag - g_pad_deadzone) / (1.0 - g_pad_deadzone);
    double component = want_x ? x : y;
    return (component / mag) * scaled;
}

/// @brief Clamp one scalar value to an inclusive range.
/// @param value Value to clamp.
/// @param min_val Inclusive lower bound.
/// @param max_val Inclusive upper bound.
/// @return @p value constrained to [@p min_val, @p max_val].
static double clamp_axis(double value, double min_val, double max_val) {
    if (value < min_val)
        return min_val;
    if (value > max_val)
        return max_val;
    return value;
}

//=============================================================================
// Initialization
//=============================================================================

/// @brief Initialize the gamepad subsystem (zeroes all pad state, sets default deadzone).
void rt_pad_init(void) {
    RT_ASSERT_MAIN_THREAD();
    if (g_pad_initialized)
        return;

    for (int i = 0; i < ZANNA_PAD_MAX; i++) {
        pad_clear_logical_state(i);
    }

    g_pad_deadzone = 0.1;
    g_pad_initialized = true;
}

/// @brief Clear per-frame pressed/released flags for all gamepads. Call once at frame start.
void rt_pad_begin_frame(void) {
    RT_ASSERT_MAIN_THREAD();
    // Clear per-frame event flags
    for (int i = 0; i < ZANNA_PAD_MAX; i++) {
        for (int b = 0; b < ZANNA_PAD_BUTTON_MAX; b++) {
            g_pads[i].pressed[b] = false;
            g_pads[i].released[b] = false;
        }
    }
}

/// @brief Poll all gamepads for current state and detect button press/release edges.
void rt_pad_poll(void) {
    RT_ASSERT_MAIN_THREAD();
    if (!g_pad_initialized)
        rt_pad_init();

    // Store previous button states for edge detection
    bool prev_buttons[ZANNA_PAD_MAX][ZANNA_PAD_BUTTON_MAX];
    for (int i = 0; i < ZANNA_PAD_MAX; i++) {
        for (int b = 0; b < ZANNA_PAD_BUTTON_MAX; b++) {
            prev_buttons[i][b] = g_pads[i].buttons[b];
        }
    }

    // Platform-specific polling updates g_pads state
    platform_pad_poll();

    // Detect button press/release events
    for (int i = 0; i < ZANNA_PAD_MAX; i++) {
        for (int b = 0; b < ZANNA_PAD_BUTTON_MAX; b++) {
            bool was_down = prev_buttons[i][b];
            bool is_down = g_pads[i].buttons[b];

            if (is_down && !was_down)
                g_pads[i].pressed[b] = true;
            else if (!is_down && was_down)
                g_pads[i].released[b] = true;
        }
    }
}

//=============================================================================
// Controller Enumeration
//=============================================================================

/// @brief Get the number of currently connected gamepads.
/// @return Connected public-slot count in [0, `ZANNA_PAD_MAX`].
int64_t rt_pad_count(void) {
    RT_ASSERT_MAIN_THREAD();
    int64_t count = 0;
    for (int i = 0; i < ZANNA_PAD_MAX; i++) {
        if (g_pads[i].connected)
            count++;
    }
    return count;
}

/// @brief Check whether a gamepad is connected at the given slot index.
/// @param index Public controller slot.
/// @return One when the slot is valid and connected, otherwise zero.
int8_t rt_pad_is_connected(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0;
    return g_pads[index].connected ? 1 : 0;
}

/// @brief Get the name of a connected gamepad (e.g., "Xbox Controller").
/// @param index Public controller slot.
/// @return Owned device name, or an owned empty string when disconnected or invalid.
rt_string rt_pad_name(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX || !g_pads[index].connected)
        return rt_string_from_bytes("", 0);

    return rt_string_from_bytes(g_pads[index].name, strlen(g_pads[index].name));
}

//=============================================================================
// Button State (Polling)
//=============================================================================

/// @brief Check whether a gamepad button is currently held down.
/// @param index Public controller slot.
/// @param button Public `ZANNA_PAD_*` button code.
/// @return One when the connected pad's button is down, otherwise zero.
int8_t rt_pad_is_down(int64_t index, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0;
    if (button < 0 || button >= ZANNA_PAD_BUTTON_MAX)
        return 0;
    if (!g_pads[index].connected)
        return 0;

    return g_pads[index].buttons[button] ? 1 : 0;
}

/// @brief Check whether a gamepad button is currently not held down.
/// @param index Public controller slot.
/// @param button Public `ZANNA_PAD_*` button code.
/// @return One when the button is up; invalid and disconnected inputs also report up.
int8_t rt_pad_is_up(int64_t index, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 1;
    if (button < 0 || button >= ZANNA_PAD_BUTTON_MAX)
        return 1;
    if (!g_pads[index].connected)
        return 1;

    return g_pads[index].buttons[button] ? 0 : 1;
}

//=============================================================================
// Button Events (Since Last Poll)
//=============================================================================

/// @brief Check whether a gamepad button was pressed this frame (edge-triggered).
/// @param index Public controller slot.
/// @param button Public `ZANNA_PAD_*` button code.
/// @return One when the button gained its down state this frame, otherwise zero.
int8_t rt_pad_was_pressed(int64_t index, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0;
    if (button < 0 || button >= ZANNA_PAD_BUTTON_MAX)
        return 0;
    if (!g_pads[index].connected)
        return 0;

    return g_pads[index].pressed[button] ? 1 : 0;
}

/// @brief Check whether a gamepad button was released this frame (edge-triggered).
/// @param index Public controller slot.
/// @param button Public `ZANNA_PAD_*` button code.
/// @return One when the button lost its down state this frame, otherwise zero.
int8_t rt_pad_was_released(int64_t index, int64_t button) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0;
    if (button < 0 || button >= ZANNA_PAD_BUTTON_MAX)
        return 0;
    if (!g_pads[index].connected)
        return 0;

    return g_pads[index].released[button] ? 1 : 0;
}

//=============================================================================
// Analog Inputs
//=============================================================================

/// @brief Get the left stick X axis value (-1.0 to 1.0, deadzone applied).
/// @param index Public controller slot.
/// @return Deadzone-adjusted horizontal component, or zero when unavailable.
double rt_pad_left_x(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0.0;
    if (!g_pads[index].connected)
        return 0.0;

    return apply_radial_deadzone_component(clamp_axis(g_pads[index].left_x, -1.0, 1.0),
                                           clamp_axis(g_pads[index].left_y, -1.0, 1.0),
                                           1);
}

/// @brief Get the left stick Y axis value (-1.0 to 1.0, deadzone applied).
/// @param index Public controller slot.
/// @return Deadzone-adjusted vertical component, or zero when unavailable.
double rt_pad_left_y(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0.0;
    if (!g_pads[index].connected)
        return 0.0;

    return apply_radial_deadzone_component(clamp_axis(g_pads[index].left_x, -1.0, 1.0),
                                           clamp_axis(g_pads[index].left_y, -1.0, 1.0),
                                           0);
}

/// @brief Get the right stick X axis value (-1.0 to 1.0, deadzone applied).
/// @param index Public controller slot.
/// @return Deadzone-adjusted horizontal component, or zero when unavailable.
double rt_pad_right_x(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0.0;
    if (!g_pads[index].connected)
        return 0.0;

    return apply_radial_deadzone_component(clamp_axis(g_pads[index].right_x, -1.0, 1.0),
                                           clamp_axis(g_pads[index].right_y, -1.0, 1.0),
                                           1);
}

/// @brief Get the right stick Y axis value (-1.0 to 1.0, deadzone applied).
/// @param index Public controller slot.
/// @return Deadzone-adjusted vertical component, or zero when unavailable.
double rt_pad_right_y(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0.0;
    if (!g_pads[index].connected)
        return 0.0;

    return apply_radial_deadzone_component(clamp_axis(g_pads[index].right_x, -1.0, 1.0),
                                           clamp_axis(g_pads[index].right_y, -1.0, 1.0),
                                           0);
}

/// @brief Get the left trigger value (0.0 = released, 1.0 = fully pressed).
/// @param index Public controller slot.
/// @return Trigger value clamped to [0,1], or zero when unavailable.
double rt_pad_left_trigger(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0.0;
    if (!g_pads[index].connected)
        return 0.0;

    return clamp_axis(g_pads[index].left_trigger, 0.0, 1.0);
}

/// @brief Get the right trigger value (0.0 = released, 1.0 = fully pressed).
/// @param index Public controller slot.
/// @return Trigger value clamped to [0,1], or zero when unavailable.
double rt_pad_right_trigger(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return 0.0;
    if (!g_pads[index].connected)
        return 0.0;

    return clamp_axis(g_pads[index].right_trigger, 0.0, 1.0);
}

//=============================================================================
// Deadzone Handling
//=============================================================================

/// @brief Set the analog stick deadzone radius (0.0 to 1.0, default 0.1).
/// @param radius Requested deadzone, clamped to [0,1].
void rt_pad_set_deadzone(double radius) {
    RT_ASSERT_MAIN_THREAD();
    g_pad_deadzone = clamp_axis(radius, 0.0, 1.0);
}

/// @brief Get the current analog stick deadzone radius.
/// @return Current normalized radial deadzone.
double rt_pad_get_deadzone(void) {
    RT_ASSERT_MAIN_THREAD();
    return g_pad_deadzone;
}

//=============================================================================
// Vibration/Rumble
//=============================================================================

/// @brief Set gamepad vibration motor intensities (0.0 to 1.0; platform-dependent).
/// @param index Public controller slot.
/// @param left_motor Requested strong/left motor strength.
/// @param right_motor Requested weak/right motor strength.
void rt_pad_vibrate(int64_t index, double left_motor, double right_motor) {
    RT_ASSERT_MAIN_THREAD();
    if (index < 0 || index >= ZANNA_PAD_MAX)
        return;
    if (!g_pads[index].connected)
        return;

    double left = pad_clamp_unit_finite(left_motor);
    double right = pad_clamp_unit_finite(right_motor);

    g_pads[index].vibration_left = left;
    g_pads[index].vibration_right = right;

    platform_pad_vibrate(index, left, right);
}

/// @brief Stop all vibration on a gamepad.
/// @param index Public controller slot.
void rt_pad_stop_vibration(int64_t index) {
    RT_ASSERT_MAIN_THREAD();
    rt_pad_vibrate(index, 0.0, 0.0);
}

//=============================================================================
// Button Constant Getters
//=============================================================================

// ===========================================================================
// Button-constant getters — exposed to the language layer so user code
// can write `Pad.IsButtonDown(0, Pad.ButtonA)` without hardcoding the
// internal `ZANNA_PAD_*` enum values. Each is a trivial constant return.
// ===========================================================================

/// @brief Return the integer index for the A (south face) button.
/// @return `ZANNA_PAD_A`.
int64_t rt_pad_button_a(void) {
    return ZANNA_PAD_A;
}

/// @brief Return the integer index for the B (east face) button.
/// @return `ZANNA_PAD_B`.
int64_t rt_pad_button_b(void) {
    return ZANNA_PAD_B;
}

/// @brief Return the integer index for the X (west face) button.
/// @return `ZANNA_PAD_X`.
int64_t rt_pad_button_x(void) {
    return ZANNA_PAD_X;
}

/// @brief Return the integer index for the Y (north face) button.
/// @return `ZANNA_PAD_Y`.
int64_t rt_pad_button_y(void) {
    return ZANNA_PAD_Y;
}

/// @brief Return the integer index for the left bumper (LB / L1) button.
/// @return `ZANNA_PAD_LB`.
int64_t rt_pad_button_lb(void) {
    return ZANNA_PAD_LB;
}

/// @brief Return the integer index for the right bumper (RB / R1) button.
/// @return `ZANNA_PAD_RB`.
int64_t rt_pad_button_rb(void) {
    return ZANNA_PAD_RB;
}

/// @brief Return the integer index for the Back / Select button.
/// @return `ZANNA_PAD_BACK`.
int64_t rt_pad_button_back(void) {
    return ZANNA_PAD_BACK;
}

/// @brief Return the integer index for the Start / Menu button.
/// @return `ZANNA_PAD_START`.
int64_t rt_pad_button_start(void) {
    return ZANNA_PAD_START;
}

/// @brief Return the integer index for the left-stick click (L3) button.
/// @return `ZANNA_PAD_LSTICK`.
int64_t rt_pad_button_lstick(void) {
    return ZANNA_PAD_LSTICK;
}

/// @brief Return the integer index for the right-stick click (R3) button.
/// @return `ZANNA_PAD_RSTICK`.
int64_t rt_pad_button_rstick(void) {
    return ZANNA_PAD_RSTICK;
}

/// @brief Return the integer index for the D-pad Up button.
/// @return `ZANNA_PAD_UP`.
int64_t rt_pad_button_up(void) {
    return ZANNA_PAD_UP;
}

/// @brief Return the integer index for the D-pad Down button.
/// @return `ZANNA_PAD_DOWN`.
int64_t rt_pad_button_down(void) {
    return ZANNA_PAD_DOWN;
}

/// @brief Return the integer index for the D-pad Left button.
/// @return `ZANNA_PAD_LEFT`.
int64_t rt_pad_button_left(void) {
    return ZANNA_PAD_LEFT;
}

/// @brief Return the integer index for the D-pad Right button.
/// @return `ZANNA_PAD_RIGHT`.
int64_t rt_pad_button_right(void) {
    return ZANNA_PAD_RIGHT;
}

/// @brief Return the integer index for the Guide / Home / Xbox button.
/// @return `ZANNA_PAD_GUIDE`.
int64_t rt_pad_button_guide(void) {
    return ZANNA_PAD_GUIDE;
}
