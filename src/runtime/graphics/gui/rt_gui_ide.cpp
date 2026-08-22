//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/gui/rt_gui_ide.cpp
// Purpose: GUI automation, virtualization, and command/accessibility helpers
//          for IDE-style applications.
// Key invariants:
//   - Virtual list and tree IDs are non-empty and unique; duplicate updates
//     trap without mutating model state.
//   - Model/control bindings are non-owning and are invalidated from either
//     side before the pointed-to object is reclaimed.
//   - Bound controls request only viewport slices; stable-ID lookup uses hash
//     indexes and never scans the full logical row count.
// Ownership/Lifetime:
//   - Runtime objects own their C++ state and release it from object finalizers.
//   - Bound ListBox/TreeView controls remain owned by their GUI parent tree.
//   - Strings returned to the runtime are newly referenced managed values.
// Links: src/runtime/graphics/gui/rt_gui_ide.h,
//        src/runtime/graphics/gui/rt_gui_internal.h,
//        src/lib/gui/include/vg_widgets.h,
//        src/lib/gui/include/vg_ide_widgets_tree.h,
//        docs/adr/0290-virtual-tree-bulk-updates.md
//
//===----------------------------------------------------------------------===//

/// @file rt_gui_ide.cpp
/// @brief Implements IDE-oriented GUI automation, virtual models, and command bindings.
///
/// @details
/// Managed runtime handles own C++ state for deterministic automation journals,
/// viewport-backed list and tree models, accessibility checks, and command
/// routing. Native widget bindings remain non-owning and are invalidated before
/// either side is reclaimed.

#include "rt_gui_ide.h"

#include "rt_gui.h" // self-guarding MenuItem/ToolbarItem/Shortcuts/CommandPalette accessors
#ifdef ZANNA_ENABLE_GRAPHICS
#include "../2d/rt_pixels_internal.h"
#include "rt_gui_automation_bridge.h"
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnested-anon-types"
#endif
#include "../../../lib/graphics/include/vgfx.h"
#include "../../../lib/gui/include/vg_ide_widgets_tree.h"
#include "../../../lib/gui/include/vg_widgets.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#endif
#include "rt_map.h"
#include "rt_object.h"
#include "rt_option.h"
#include "rt_seq.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#define RT_GUI_IDE_REQUIRE_OR_RETURN(var, expr, value)                                             \
    auto *var = (expr);                                                                            \
    if (!var)                                                                                      \
    return (value)

#define RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(var, expr)                                               \
    auto *var = (expr);                                                                            \
    if (!var)                                                                                      \
    return

namespace {

/// @brief Copy a managed runtime string into an ordinary byte-preserving C++ string.
/// @param s Runtime string to copy.
/// @return Copied bytes, or an empty string for null or invalid input.
std::string toStd(rt_string s) {
    if (!s)
        return {};
    const char *data = rt_string_cstr(s);
    int64_t len = rt_str_len(s);
    if (!data || len <= 0)
        return {};
    return std::string(data, static_cast<size_t>(len));
}

/// @brief Copy runtime text into a C-renderable UTF-8 string without hidden NUL truncation.
/// @details Each embedded NUL byte becomes U+FFFD. Allocation failure propagates to the caller,
///          which preserves the prior model value.
/// @param value Runtime string whose bytes are normalized.
/// @return UTF-8 text safe for null-terminated GUI APIs.
static std::string toGuiTextStd(rt_string value) {
    std::string input = toStd(value);
    size_t nul_count =
        static_cast<size_t>(std::count(input.begin(), input.end(), static_cast<char>('\0')));
    if (nul_count == 0u)
        return input;
    if (nul_count > (std::numeric_limits<size_t>::max() - input.size()) / 2u)
        throw std::bad_alloc();
    std::string output;
    output.reserve(input.size() + nul_count * 2u);
    for (char ch : input) {
        if (ch == '\0')
            output.append("\xEF\xBF\xBD", 3u);
        else
            output.push_back(ch);
    }
    return output;
}

/// @brief Copy a C++ string into a managed runtime string.
/// @param value Byte sequence to copy.
/// @return Newly referenced runtime string, or `NULL` on allocation failure.
rt_string makeString(const std::string &value) {
    return rt_string_from_bytes(value.data(), value.size());
}

/// @brief Release a managed object and free it when its reference count reaches zero.
/// @param obj Runtime object to release; `NULL` is accepted.
void releaseObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Store a copied C++ string in a managed map.
/// @param map Runtime map receiving the value.
/// @param key Null-terminated constant key.
/// @param value String bytes to copy.
void mapSetStr(void *map, const char *key, const std::string &value) {
    if (!map || !key)
        return;
    rt_string s = makeString(value);
    if (!s)
        return;
    rt_map_set_str(map, rt_const_cstr(key), s);
    rt_string_unref(s);
}

/// @brief Add two signed 64-bit integers with saturation instead of undefined overflow.
/// @details GUI automation helpers accept user-provided coordinates and sizes. Saturating
///          arithmetic lets range comparisons and area estimates remain deterministic even
///          for extreme test inputs that would otherwise overflow `int64_t`.
/// @param a First addend.
/// @param b Second addend.
/// @return Saturated signed sum.
static int64_t saturatingAddI64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Multiply two non-negative `int64_t` dimensions, returning zero on invalid/overflow.
/// @details Used for synthetic snapshot pixel counts. A zero result is a conservative
///          "unknown/empty" value that avoids reporting wrapped negative pixel counts.
/// @param width Non-negative width.
/// @param height Non-negative height.
/// @return Product, or zero when a dimension is non-positive or multiplication would overflow.
static int64_t safeAreaI64(int64_t width, int64_t height) {
    if (width <= 0 || height <= 0 || width > INT64_MAX / height)
        return 0;
    return width * height;
}

/// @brief Return `ceil(numerator / denominator)` for positive dimensions without overflow.
/// @details Avoids the common `(n + d - 1)` idiom because viewport dimensions can be
///          externally supplied and may be close to `INT64_MAX`.
/// @param numerator Positive dividend.
/// @param denominator Positive divisor.
/// @return Ceiling quotient, or zero when either operand is non-positive.
static int64_t ceilDivPositiveI64(int64_t numerator, int64_t denominator) {
    if (numerator <= 0 || denominator <= 0)
        return 0;
    return numerator / denominator + ((numerator % denominator) != 0 ? 1 : 0);
}

/// @brief Check whether two positive-length integer ranges overlap.
/// @details End coordinates are computed with saturating addition so very large
///          rectangles remain well-defined instead of overflowing.
/// @param a Start of the first range.
/// @param a_len Positive length of the first range.
/// @param b Start of the second range.
/// @param b_len Positive length of the second range.
/// @return `true` when the half-open ranges overlap; otherwise `false`.
static bool rangesIntersect(int64_t a, int64_t a_len, int64_t b, int64_t b_len) {
    if (a_len <= 0 || b_len <= 0)
        return false;
    int64_t a_end = saturatingAddI64(a, a_len);
    int64_t b_end = saturatingAddI64(b, b_len);
    return a < b_end && b < a_end;
}

struct WidgetRecord {
    std::string id;
    std::string type;
    std::string name;
    int64_t x{0};
    int64_t y{0};
    int64_t w{0};
    int64_t h{0};
};

struct EventRecord {
    std::string type;
    std::string value;
    int64_t x{0};
    int64_t y{0};
    int64_t button{0};
    int64_t modifiers{0};
    int64_t frame{0};
};

struct HarnessState {
    int64_t frame{0};
    std::vector<WidgetRecord> widgets;
    std::vector<EventRecord> events;
    std::string focus;
#ifdef ZANNA_ENABLE_GRAPHICS
    void *boundApp{nullptr};
    size_t dispatchedEvents{0};
#endif
};

struct HarnessHandle {
    HarnessState *state{nullptr};
};

/// @brief Validate a runtime object as a live TestHarness handle.
/// @param obj Candidate managed object.
/// @return Validated handle, or `nullptr` after trapping on invalid or destroyed input.
HarnessHandle *requireHarness(void *obj) {
    if (!obj || rt_obj_class_id(obj) != RT_GUI_TEST_HARNESS_CLASS_ID) {
        rt_trap("GUI.TestHarness: invalid handle");
        return nullptr;
    }
    auto *h = static_cast<HarnessHandle *>(obj);
    if (!h->state) {
        rt_trap("GUI.TestHarness: destroyed handle");
        return nullptr;
    }
    return h;
}

/// @brief Release native bindings and C++ state owned by a TestHarness object.
/// @param obj Managed TestHarness object being finalized.
void harnessFinalizer(void *obj) {
    auto *h = static_cast<HarnessHandle *>(obj);
#ifdef ZANNA_ENABLE_GRAPHICS
    if (h->state && h->state->boundApp) {
        releaseObject(h->state->boundApp);
        h->state->boundApp = nullptr;
    }
#endif
    delete h->state;
    h->state = nullptr;
}

#ifdef ZANNA_ENABLE_GRAPHICS

/// @brief Return the live app currently retained by an automation harness.
/// @details Explicit app destruction invalidates the app magic even though the harness still
///          owns a managed reference. Validation on every operation therefore prevents stale
///          native windows from being dereferenced while allowing a later Unbind to release the
///          retained runtime allocation safely.
/// @param state Harness state whose optional binding is inspected.
/// @param out_view Destination for the refreshed borrowed automation view.
/// @return True for a live bound app, otherwise false.
static bool harnessBoundApp(HarnessState &state, rt_gui_automation_app_view_t &out_view) {
    return rt_gui_automation_snapshot_app(state.boundApp, &out_view) != 0;
}

/// @brief Clamp a public 64-bit automation coordinate into the platform event domain.
/// @param value Coordinate supplied through the runtime API.
/// @return The nearest representable signed 32-bit coordinate.
static int32_t harnessEventCoordinate(int64_t value) {
    if (value < INT32_MIN)
        return INT32_MIN;
    if (value > INT32_MAX)
        return INT32_MAX;
    return static_cast<int32_t>(value);
}

/// @brief Produce a deterministic timestamp for one app-bound synthetic event.
/// @details Once the app scheduler has been initialized, its deterministic timer clock is used;
///          before the first rendered frame the monotonic GUI clock supplies the timestamp.
///          Values are saturated to the signed platform-event representation.
/// @param app Live app receiving the event.
/// @return Non-negative event time in milliseconds.
static int64_t harnessEventTimeMs(const rt_gui_automation_app_view_t &app) {
    return app.event_time_ms < 0 ? 0 : app.event_time_ms;
}

/// @brief Compare an automation token with an ASCII keyword without locale dependence.
/// @param value Caller-supplied token.
/// @param keyword Lowercase ASCII keyword to match.
/// @return True when the complete strings match ignoring ASCII letter case.
static bool harnessAsciiEqual(const std::string &value, const char *keyword) {
    if (!keyword)
        return false;
    size_t length = std::char_traits<char>::length(keyword);
    if (value.size() != length)
        return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        char lowered =
            (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : static_cast<char>(ch);
        if (lowered != keyword[i])
            return false;
    }
    return true;
}

/// @brief Translate the documented TestHarness key vocabulary into a ZannaGFX key code.
/// @details Named navigation/editing keys are matched case-insensitively. A one-byte printable
///          ASCII token maps to its uppercase key identity while retaining the original byte for
///          the optional text-input event generated by the dispatcher.
/// @param value Automation key token.
/// @param out_text_codepoint Optional destination for a printable text codepoint, or zero.
/// @return Platform key code, or VGFX_KEY_UNKNOWN for unsupported tokens.
static vgfx_key_t harnessKeyCode(const std::string &value, uint32_t *out_text_codepoint) {
    if (out_text_codepoint)
        *out_text_codepoint = 0;

    struct NamedKey {
        const char *name;
        vgfx_key_t key;
    };

    static constexpr NamedKey named[] = {
        {"escape", VGFX_KEY_ESCAPE},  {"enter", VGFX_KEY_ENTER},
        {"return", VGFX_KEY_ENTER},   {"left", VGFX_KEY_LEFT},
        {"right", VGFX_KEY_RIGHT},    {"up", VGFX_KEY_UP},
        {"down", VGFX_KEY_DOWN},      {"backspace", VGFX_KEY_BACKSPACE},
        {"delete", VGFX_KEY_DELETE},  {"tab", VGFX_KEY_TAB},
        {"home", VGFX_KEY_HOME},      {"end", VGFX_KEY_END},
        {"pageup", VGFX_KEY_PAGE_UP}, {"pagedown", VGFX_KEY_PAGE_DOWN},
        {"space", VGFX_KEY_SPACE},    {"f1", VGFX_KEY_F1},
        {"f2", VGFX_KEY_F2},          {"f3", VGFX_KEY_F3},
        {"f4", VGFX_KEY_F4},          {"f5", VGFX_KEY_F5},
        {"f6", VGFX_KEY_F6},          {"f7", VGFX_KEY_F7},
        {"f8", VGFX_KEY_F8},          {"f9", VGFX_KEY_F9},
        {"f10", VGFX_KEY_F10},        {"f11", VGFX_KEY_F11},
        {"f12", VGFX_KEY_F12}};
    for (const NamedKey &entry : named) {
        if (harnessAsciiEqual(value, entry.name)) {
            if (out_text_codepoint && entry.key == VGFX_KEY_SPACE)
                *out_text_codepoint = static_cast<uint32_t>(' ');
            return entry.key;
        }
    }
    if (value.size() != 1u)
        return VGFX_KEY_UNKNOWN;
    unsigned char byte = static_cast<unsigned char>(value[0]);
    if (byte < 0x20u || byte > 0x7Eu)
        return VGFX_KEY_UNKNOWN;
    if (out_text_codepoint)
        *out_text_codepoint = byte;
    if (byte >= 'a' && byte <= 'z')
        byte = static_cast<unsigned char>(byte - 'a' + 'A');
    return static_cast<vgfx_key_t>(byte);
}

/// @brief Post one recorded key action through the app's real platform event queue.
/// @details A key press/release pair is always attempted. Printable keys also receive a native
///          text-input event between the pair unless Ctrl, Alt, or Command is active, matching the
///          separation used by desktop platform adapters. Queue failures are reported without
///          retry so a full queue cannot stall the harness journal indefinitely.
/// @param app Live target app.
/// @param record Recorded key action.
/// @return True only when every required event was accepted.
static bool harnessPostKey(const rt_gui_automation_app_view_t &app, const EventRecord &record) {
    uint32_t text_codepoint = 0;
    vgfx_key_t key = harnessKeyCode(record.value, &text_codepoint);
    vgfx_window_t window = static_cast<vgfx_window_t>(app.window);
    if (!window || key == VGFX_KEY_UNKNOWN)
        return false;

    vgfx_event_t event{};
    event.time_ms = harnessEventTimeMs(app);
    event.type = VGFX_EVENT_KEY_DOWN;
    event.data.key.key = key;
    event.data.key.is_repeat = 0;
    event.data.key.modifiers = static_cast<int>(record.modifiers);
    bool accepted = vgfx_post_event(window, &event) != 0;

    const int command_modifiers = VGFX_MOD_CTRL | VGFX_MOD_ALT | VGFX_MOD_CMD;
    if (text_codepoint != 0u && (record.modifiers & command_modifiers) == 0) {
        event = {};
        event.type = VGFX_EVENT_TEXT_INPUT;
        event.time_ms = harnessEventTimeMs(app);
        event.data.text.codepoint = text_codepoint;
        event.data.text.modifiers = static_cast<int>(record.modifiers);
        accepted = (vgfx_post_event(window, &event) != 0) && accepted;
    }

    event = {};
    event.type = VGFX_EVENT_KEY_UP;
    event.time_ms = harnessEventTimeMs(app);
    event.data.key.key = key;
    event.data.key.is_repeat = 0;
    event.data.key.modifiers = static_cast<int>(record.modifiers);
    return (vgfx_post_event(window, &event) != 0) && accepted;
}

/// @brief Post one recorded mouse action through the app's real platform event queue.
/// @details `move`, `down`/`mousedown`, `up`/`mouseup`, and `click` are accepted
///          case-insensitively. Click expands to an adjacent down/up pair at the same physical
///          framebuffer coordinate. Buttons follow ZannaGFX ordinals: left=0, right=1, middle=2.
/// @param app Live target app.
/// @param record Recorded mouse action.
/// @return True only when the complete requested action was queued.
static bool harnessPostMouse(const rt_gui_automation_app_view_t &app, const EventRecord &record) {
    vgfx_window_t window = static_cast<vgfx_window_t>(app.window);
    if (!window || record.button < VGFX_MOUSE_LEFT || record.button > VGFX_MOUSE_MIDDLE)
        return false;
    int32_t x = harnessEventCoordinate(record.x);
    int32_t y = harnessEventCoordinate(record.y);
    vgfx_event_t event{};
    event.time_ms = harnessEventTimeMs(app);
    if (harnessAsciiEqual(record.type, "move") || harnessAsciiEqual(record.type, "mousemove") ||
        harnessAsciiEqual(record.type, "mouse_move")) {
        event.type = VGFX_EVENT_MOUSE_MOVE;
        event.data.mouse_move.x = x;
        event.data.mouse_move.y = y;
        event.data.mouse_move.modifiers = 0;
        return vgfx_post_event(window, &event) != 0;
    }

    bool click = harnessAsciiEqual(record.type, "click");
    bool down = click || harnessAsciiEqual(record.type, "down") ||
                harnessAsciiEqual(record.type, "mousedown") ||
                harnessAsciiEqual(record.type, "mouse_down");
    bool up = click || harnessAsciiEqual(record.type, "up") ||
              harnessAsciiEqual(record.type, "mouseup") ||
              harnessAsciiEqual(record.type, "mouse_up");
    if (!down && !up)
        return false;
    event.type = down ? VGFX_EVENT_MOUSE_DOWN : VGFX_EVENT_MOUSE_UP;
    event.data.mouse_button.x = x;
    event.data.mouse_button.y = y;
    event.data.mouse_button.button = static_cast<vgfx_mouse_button_t>(record.button);
    event.data.mouse_button.modifiers = 0;
    bool accepted = vgfx_post_event(window, &event) != 0;
    if (click) {
        event.type = VGFX_EVENT_MOUSE_UP;
        accepted = (vgfx_post_event(window, &event) != 0) && accepted;
    }
    return accepted;
}

/// @brief Queue every undispatched journal record for a bound app exactly once.
/// @details The cursor advances even for malformed records or queue overflow, making each call
///          finite and ensuring an invalid event cannot permanently block later automation. The
///          caller chooses whether to poll separately or as part of a deterministic render frame.
/// @param state Harness state containing the journal and dispatch cursor.
/// @param app Live target app.
/// @return Number of journal records whose complete expanded action was queued, saturated to i64.
static int64_t harnessPostPending(HarnessState &state, const rt_gui_automation_app_view_t &app) {
    uint64_t accepted = 0;
    if (state.dispatchedEvents > state.events.size())
        state.dispatchedEvents = state.events.size();
    while (state.dispatchedEvents < state.events.size()) {
        const EventRecord &record = state.events[state.dispatchedEvents++];
        bool posted =
            record.type == "key" ? harnessPostKey(app, record) : harnessPostMouse(app, record);
        if (posted && accepted < static_cast<uint64_t>(INT64_MAX))
            ++accepted;
    }
    return static_cast<int64_t>(accepted);
}

/// @brief Read one requested physical pixel from a framebuffer with transparent clipping.
/// @details Coordinates outside the framebuffer, malformed descriptors, and missing storage all
///          produce transparent black. In-bounds bytes are packed into the runtime's canonical
///          numeric 0xRRGGBBAA representation.
/// @param framebuffer Borrowed framebuffer descriptor valid for the current operation.
/// @param x Requested physical X coordinate.
/// @param y Requested physical Y coordinate.
/// @return Canonical packed RGBA pixel, or zero outside the image.
static uint32_t harnessFramebufferPixel(const vgfx_framebuffer_t &framebuffer,
                                        int64_t x,
                                        int64_t y) {
    if (!framebuffer.pixels || framebuffer.width <= 0 || framebuffer.height <= 0 ||
        framebuffer.stride < 0 ||
        static_cast<int64_t>(framebuffer.stride) < static_cast<int64_t>(framebuffer.width) * 4 ||
        x < 0 || y < 0 || x >= framebuffer.width || y >= framebuffer.height)
        return 0;
    const uint8_t *pixel = framebuffer.pixels + static_cast<size_t>(y) * framebuffer.stride +
                           static_cast<size_t>(x) * 4u;
    return (static_cast<uint32_t>(pixel[0]) << 24u) | (static_cast<uint32_t>(pixel[1]) << 16u) |
           (static_cast<uint32_t>(pixel[2]) << 8u) | static_cast<uint32_t>(pixel[3]);
}

/// @brief Mix one byte into the stable TestHarness framebuffer hash.
/// @param hash Current FNV-1a 64-bit accumulator.
/// @param byte Next byte in the canonical stream.
/// @return Updated accumulator.
static uint64_t harnessHashByte(uint64_t hash, uint8_t byte) {
    return (hash ^ byte) * UINT64_C(1099511628211);
}

/// @brief Mix one signed runtime dimension in a platform-independent little-endian form.
/// @param hash Current FNV-1a accumulator.
/// @param value Dimension or coordinate value to encode.
/// @return Updated accumulator.
static uint64_t harnessHashI64(uint64_t hash, int64_t value) {
    uint64_t bits = static_cast<uint64_t>(value);
    for (unsigned shift = 0; shift < 64u; shift += 8u)
        hash = harnessHashByte(hash, static_cast<uint8_t>(bits >> shift));
    return hash;
}

#endif

/// @brief Convert an optional synthetic widget record to the runtime query-map schema.
/// @param w Widget record to serialize, or `nullptr` for a not-found result.
/// @return New managed map, or `NULL` on allocation failure.
void *widgetToMap(const WidgetRecord *w) {
    void *map = rt_map_new();
    if (!map)
        return nullptr;
    if (!w) {
        rt_map_set_bool(map, rt_const_cstr("found"), 0);
        return map;
    }
    rt_map_set_bool(map, rt_const_cstr("found"), 1);
    mapSetStr(map, "id", w->id);
    mapSetStr(map, "type", w->type);
    mapSetStr(map, "name", w->name);
    rt_map_set_int(map, rt_const_cstr("x"), w->x);
    rt_map_set_int(map, rt_const_cstr("y"), w->y);
    rt_map_set_int(map, rt_const_cstr("width"), w->w);
    rt_map_set_int(map, rt_const_cstr("height"), w->h);
    return map;
}

/// @brief Convert a captured harness event into the map shape returned to runtime callers.
/// @details A NULL event still returns a map with `found=false`, which lets scripts query
///          optional event indices without trapping or special-casing a NULL object result.
/// @param event Event record to serialize, or `nullptr` for a not-found result.
/// @return New managed map, or `NULL` on allocation failure.
void *eventToMap(const EventRecord *event) {
    void *map = rt_map_new();
    if (!map)
        return nullptr;
    if (!event) {
        rt_map_set_bool(map, rt_const_cstr("found"), 0);
        return map;
    }
    rt_map_set_bool(map, rt_const_cstr("found"), 1);
    mapSetStr(map, "type", event->type);
    mapSetStr(map, "value", event->value);
    rt_map_set_int(map, rt_const_cstr("x"), event->x);
    rt_map_set_int(map, rt_const_cstr("y"), event->y);
    rt_map_set_int(map, rt_const_cstr("button"), event->button);
    rt_map_set_int(map, rt_const_cstr("modifiers"), event->modifiers);
    rt_map_set_int(map, rt_const_cstr("frame"), event->frame);
    return map;
}

/// @brief Test whether a synthetic widget bounds intersects a query rectangle.
/// @param w Widget record supplying the first rectangle.
/// @param x Query rectangle X coordinate.
/// @param y Query rectangle Y coordinate.
/// @param width Query rectangle width.
/// @param height Query rectangle height.
/// @return `true` when both axis ranges overlap; otherwise `false`.
bool intersects(const WidgetRecord &w, int64_t x, int64_t y, int64_t width, int64_t height) {
    return rangesIntersect(w.x, w.w, x, width) && rangesIntersect(w.y, w.h, y, height);
}

struct VirtualListState {
    int64_t rowCount{0};
    int64_t rowHeight{20};
    int64_t viewportHeight{100};
    int64_t overscan{2};
    std::unordered_map<int64_t, std::string> rowIds;
    std::unordered_map<std::string, int64_t> idRows;
    std::unordered_map<int64_t, std::string> rowTexts;
    std::string selectedId;
    std::string providerScratch;
    void *boundList{nullptr};
};

struct VirtualListHandle {
    VirtualListState *state{nullptr};
};

/// @brief Validate a runtime object as a live VirtualList handle.
/// @param obj Candidate managed object.
/// @return Validated handle, or `nullptr` after trapping on invalid or destroyed input.
VirtualListHandle *requireList(void *obj) {
    if (!obj || rt_obj_class_id(obj) != RT_GUI_VIRTUAL_LIST_CLASS_ID) {
        rt_trap("GUI.VirtualList: invalid handle");
        return nullptr;
    }
    auto *h = static_cast<VirtualListHandle *>(obj);
    if (!h->state) {
        rt_trap("GUI.VirtualList: destroyed handle");
        return nullptr;
    }
    return h;
}

#ifdef ZANNA_ENABLE_GRAPHICS
/// @brief Return the stable id for one logical row, synthesizing the decimal default when sparse.
/// @details This helper is needed only by the live ListBox selection bridge. Pure model queries use
///          the allocation-free reverse index path and therefore do not compile it into stub
///          builds.
/// @param state Live virtual-list model containing sparse explicit row identifiers.
/// @param row Non-negative logical row whose stable identifier is requested.
/// @return Explicit identifier when assigned, otherwise the canonical decimal row identifier.
static std::string rowId(const VirtualListState &state, int64_t row) {
    auto it = state.rowIds.find(row);
    if (it != state.rowIds.end())
        return it->second;
    return std::to_string(row);
}
#endif

/// @brief Parse a canonical non-negative decimal ID representable by a runtime row index.
/// @details Leading zeroes are rejected except for the id `0`, keeping implicit IDs one-to-one.
/// @param id Candidate stable identifier.
/// @param[out] out_row Optional destination for the parsed row.
/// @return `true` for canonical decimal input in the signed 64-bit range; otherwise `false`.
static bool parseCanonicalRowNumber(const std::string &id, int64_t *out_row) {
    if (id.empty() || (id.size() > 1 && id.front() == '0'))
        return false;
    uint64_t value = 0;
    for (char ch : id) {
        if (ch < '0' || ch > '9')
            return false;
        uint64_t digit = static_cast<uint64_t>(ch - '0');
        if (value > (static_cast<uint64_t>(INT64_MAX) - digit) / 10u)
            return false;
        value = value * 10u + digit;
    }
    int64_t row = static_cast<int64_t>(value);
    if (out_row)
        *out_row = row;
    return true;
}

/// @brief Parse a canonical decimal row id within the current logical list.
/// @param state Virtual-list model providing the row-count bound.
/// @param id Candidate implicit identifier.
/// @param[out] out_row Optional destination for the resolved row.
/// @return `true` when @p id denotes an in-range implicit row; otherwise `false`.
static bool parseImplicitRowId(const VirtualListState &state,
                               const std::string &id,
                               int64_t *out_row) {
    int64_t row = -1;
    if (!parseCanonicalRowNumber(id, &row) || row >= state.rowCount)
        return false;
    if (out_row)
        *out_row = row;
    return true;
}

/// @brief Resolve a stable id through the explicit hash or an unshadowed implicit decimal id.
/// @param state Virtual-list model providing explicit and implicit IDs.
/// @param id Stable identifier to resolve.
/// @param[out] out_row Optional destination for the resolved logical row.
/// @return `true` when the identifier resolves uniquely; otherwise `false`.
static bool findListRowById(const VirtualListState &state,
                            const std::string &id,
                            int64_t *out_row) {
    auto explicit_owner = state.idRows.find(id);
    if (explicit_owner != state.idRows.end()) {
        if (out_row)
            *out_row = explicit_owner->second;
        return true;
    }
    int64_t row = -1;
    if (!parseImplicitRowId(state, id, &row) || state.rowIds.find(row) != state.rowIds.end())
        return false;
    if (out_row)
        *out_row = row;
    return true;
}

/// @brief Report a duplicate stable model id without mutating the model.
/// @param id Duplicate identifier included in the trap message when allocation permits.
static void trapDuplicateModelId(const std::string &id) {
    std::string message = "GUI model ID must be unique: ";
    try {
        message += id;
    } catch (const std::bad_alloc &) {
        rt_trap("GUI model ID must be unique");
        return;
    }
    rt_trap(message.c_str());
}

static void detachVirtualList(VirtualListState &state);

/// @brief Finalize a VirtualList after first severing its non-owning control binding.
/// @param obj Managed VirtualList object being finalized.
static void listFinalizer(void *obj) {
    auto *h = static_cast<VirtualListHandle *>(obj);
    if (h->state)
        detachVirtualList(*h->state);
    delete h->state;
    h->state = nullptr;
}

struct TreeNode {
    std::string id;
    std::string text;
    std::string parent;
    std::vector<std::string> children;
    bool expanded{false};
    bool loaded{false};
    bool declared{true};
    std::string iconVector; ///< vg_icon_vector name projected to viewport rows.
    std::string iconText;   ///< Literal glyph fallback when no vector icon is set.
    uint32_t color{0};      ///< 0xRRGGBB row text override; 0 keeps the theme color.
    bool dim{false};        ///< Blend the row toward the background when true.
};

struct VisibleTreeRow {
    std::string id;
    size_t depth{0};
};

struct VirtualTreeState {
    std::unordered_map<std::string, TreeNode> nodes;
    std::string selectedId;
    std::vector<std::string> selectedIds;          ///< Ordered multi-selection.
    std::unordered_set<std::string> selectedIdSet; ///< Membership index for painting.
    std::vector<VisibleTreeRow> visibleRows;
    std::unordered_map<std::string, size_t> visibleIndexById;
    bool visibleDirty{true};
    uint32_t updateDepth{0}; ///< Nested bulk-update scope depth.
    bool syncPending{false}; ///< Projection invalidation deferred by a bulk scope.
    void *boundTree{nullptr};
    bool dropLatched{false};  ///< A consumed-on-read virtual drop is pending.
    std::string dropSourceId; ///< Dragged node id at latch time.
    std::string dropTargetId; ///< Target node id at latch time.
    int dropPosition{1};      ///< 0 before / 1 into / 2 after.
};

struct VirtualTreeHandle {
    VirtualTreeState *state{nullptr};
};

/// @brief Validate a runtime object as a live VirtualTree handle.
/// @param obj Candidate managed object.
/// @return Validated handle, or `nullptr` after trapping on invalid or destroyed input.
VirtualTreeHandle *requireTree(void *obj) {
    if (!obj || rt_obj_class_id(obj) != RT_GUI_VIRTUAL_TREE_CLASS_ID) {
        rt_trap("GUI.VirtualTree: invalid handle");
        return nullptr;
    }
    auto *h = static_cast<VirtualTreeHandle *>(obj);
    if (!h->state) {
        rt_trap("GUI.VirtualTree: destroyed handle");
        return nullptr;
    }
    return h;
}

static void detachVirtualTree(VirtualTreeState &state);

/// @brief Finalize a VirtualTree after first severing its non-owning control binding.
/// @param obj Managed VirtualTree object being finalized.
static void treeFinalizer(void *obj) {
    auto *h = static_cast<VirtualTreeHandle *>(obj);
    if (h->state)
        detachVirtualTree(*h->state);
    delete h->state;
    h->state = nullptr;
}

struct CommandState {
    std::string id;
    std::string label;
    std::string accessibleLabel;
    std::string accessibleDescription;
    bool enabled{true};
    bool checked{false};
};

struct CommandStateHandle {
    CommandState *state{nullptr};
};

/// @brief Validate a runtime object as a live CommandState handle.
/// @param obj Candidate managed object.
/// @return Validated handle, or `nullptr` after trapping on invalid or destroyed input.
CommandStateHandle *requireCommandState(void *obj) {
    if (!obj || rt_obj_class_id(obj) != RT_GUI_COMMAND_STATE_CLASS_ID) {
        rt_trap("GUI.CommandState: invalid handle");
        return nullptr;
    }
    auto *h = static_cast<CommandStateHandle *>(obj);
    if (!h->state) {
        rt_trap("GUI.CommandState: destroyed handle");
        return nullptr;
    }
    return h;
}

/// @brief Release C++ state owned by a managed CommandState.
/// @param obj Managed CommandState object being finalized.
void commandStateFinalizer(void *obj) {
    auto *h = static_cast<CommandStateHandle *>(obj);
    delete h->state;
    h->state = nullptr;
}

/// @brief Remove one child link while preserving the relative order of all remaining children.
/// @param state Virtual-tree model to update.
/// @param parent Stable parent identifier.
/// @param id Stable child identifier to erase.
static void eraseChildId(VirtualTreeState &state,
                         const std::string &parent,
                         const std::string &id) {
    auto pit = state.nodes.find(parent);
    if (pit == state.nodes.end())
        return;
    auto &children = pit->second.children;
    children.erase(std::remove(children.begin(), children.end(), id), children.end());
}

/// @brief Return true when assigning @p id beneath @p parent would introduce an ancestry cycle.
/// @param state Virtual-tree model containing the current ancestry.
/// @param id Node being moved.
/// @param parent Candidate new parent identifier.
/// @return `true` when the assignment would create a cycle or corrupt ancestry; otherwise `false`.
static bool virtualTreeWouldCycle(const VirtualTreeState &state,
                                  const std::string &id,
                                  const std::string &parent) {
    std::string cursor = parent;
    size_t remaining = state.nodes.size() + 1u;
    while (!cursor.empty() && remaining-- > 0u) {
        if (cursor == id)
            return true;
        auto it = state.nodes.find(cursor);
        if (it == state.nodes.end())
            return false;
        cursor = it->second.parent;
    }
    return remaining == 0u && !cursor.empty();
}

/// @brief Rebuild the flattened visible-row index iteratively, then swap it atomically.
/// @param state Virtual-tree model whose cached projection is ensured.
/// @return `true` when the index is current or rebuilt successfully; `false` on allocation failure.
static bool ensureVisibleTreeIndex(VirtualTreeState &state) {
    if (!state.visibleDirty)
        return true;
    try {
        std::vector<VisibleTreeRow> rows;
        rows.reserve(state.nodes.size() > 0 ? state.nodes.size() - 1u : 0u);
        std::unordered_map<std::string, size_t> indexes;
        indexes.reserve(state.nodes.size() > 0 ? state.nodes.size() - 1u : 0u);
        std::vector<VisibleTreeRow> stack;
        auto root = state.nodes.find("");
        if (root != state.nodes.end()) {
            stack.reserve(root->second.children.size());
            for (auto it = root->second.children.rbegin(); it != root->second.children.rend(); ++it)
                stack.push_back(VisibleTreeRow{*it, 0u});
        }
        size_t remaining = state.nodes.size();
        while (!stack.empty() && remaining-- > 0u) {
            VisibleTreeRow row = std::move(stack.back());
            stack.pop_back();
            auto node_it = state.nodes.find(row.id);
            if (node_it == state.nodes.end())
                continue;
            indexes.emplace(row.id, rows.size());
            rows.push_back(row);
            const TreeNode &node = node_it->second;
            if (!node.expanded)
                continue;
            for (auto child = node.children.rbegin(); child != node.children.rend(); ++child)
                stack.push_back(VisibleTreeRow{*child, row.depth + 1u});
        }
        state.visibleRows.swap(rows);
        state.visibleIndexById.swap(indexes);
        state.visibleDirty = false;
        return true;
    } catch (const std::bad_alloc &) {
        return false;
    }
}

/// @brief Convert one cached visible row into the public runtime map shape.
/// @param state Virtual-tree model containing node details.
/// @param row Cached visible row to serialize.
/// @return New managed row map, or `NULL` if the node is missing or allocation fails.
static void *visibleTreeRowToMap(VirtualTreeState &state, const VisibleTreeRow &row) {
    auto it = state.nodes.find(row.id);
    if (it == state.nodes.end())
        return nullptr;
    const TreeNode &node = it->second;
    void *map = rt_map_new();
    if (!map)
        return nullptr;
    mapSetStr(map, "id", node.id);
    mapSetStr(map, "text", node.text);
    mapSetStr(map, "parentId", node.parent);
    rt_map_set_int(map,
                   rt_const_cstr("depth"),
                   row.depth > static_cast<size_t>(INT64_MAX) ? INT64_MAX
                                                              : static_cast<int64_t>(row.depth));
    rt_map_set_bool(map, rt_const_cstr("expanded"), node.expanded ? 1 : 0);
    rt_map_set_bool(
        map, rt_const_cstr("needsPopulate"), (!node.loaded && node.children.empty()) ? 1 : 0);
    if (!node.iconVector.empty()) {
        try {
            mapSetStr(map, "icon", "vector:" + node.iconVector);
        } catch (const std::bad_alloc &) {
            releaseObject(map);
            return nullptr;
        }
    } else if (!node.iconText.empty()) {
        mapSetStr(map, "icon", node.iconText);
    }
    return map;
}

/// @brief Remove every descendant of @p id iteratively, retaining the named node itself.
/// @param state Virtual-tree model to mutate.
/// @param id Stable identifier of the node whose descendants are removed.
static void removeVirtualTreeDescendants(VirtualTreeState &state, const std::string &id) {
    auto it = state.nodes.find(id);
    if (it == state.nodes.end())
        return;
    std::vector<std::string> stack = it->second.children;
    it->second.children.clear();
    while (!stack.empty()) {
        std::string child = std::move(stack.back());
        stack.pop_back();
        auto child_it = state.nodes.find(child);
        if (child_it == state.nodes.end() || child == id)
            continue;
        for (const auto &grandchild : child_it->second.children)
            stack.push_back(grandchild);
        state.nodes.erase(child_it);
    }
    state.visibleDirty = true;
}

/// @brief Replace the model's multi-selection and keep the membership index coherent.
/// @details Missing, placeholder, empty, and duplicate identifiers are rejected
///          while preserving the first occurrence order of declared nodes.
/// @param state VirtualTree model to update.
/// @param ids New ordered selection; the first entry becomes the primary id.
static void setVirtualTreeSelection(VirtualTreeState &state, std::vector<std::string> ids) {
    std::vector<std::string> accepted;
    accepted.reserve(ids.size());
    std::unordered_set<std::string> seen;
    seen.reserve(ids.size());
    for (auto &entry : ids) {
        auto node = state.nodes.find(entry);
        if (entry.empty() || node == state.nodes.end() || !node->second.declared ||
            !seen.insert(entry).second)
            continue;
        accepted.push_back(std::move(entry));
    }
    state.selectedIds = std::move(accepted);
    state.selectedIdSet.clear();
    for (const auto &entry : state.selectedIds)
        state.selectedIdSet.insert(entry);
    state.selectedId = state.selectedIds.empty() ? std::string() : state.selectedIds.front();
}

/// @brief Drop selection entries whose nodes no longer exist in the model.
/// @param state VirtualTree model whose selection is reconciled.
static void pruneVirtualTreeSelection(VirtualTreeState &state) {
    if (state.selectedIds.empty())
        return;
    std::vector<std::string> kept;
    for (const auto &entry : state.selectedIds) {
        if (state.nodes.find(entry) != state.nodes.end())
            kept.push_back(entry);
    }
    if (kept.size() != state.selectedIds.size())
        setVirtualTreeSelection(state, std::move(kept));
}

#ifdef ZANNA_ENABLE_GRAPHICS
/// @brief Supply borrowed text for one bound ListBox viewport row.
/// @param listbox Lower ListBox requesting data.
/// @param index Zero-based logical row index.
/// @param[out] text Destination for borrowed row text.
/// @param[out] icon Optional icon destination; currently unused.
/// @param user_data Bound VirtualList state.
static void virtualListProvider(
    vg_widget_t *listbox, size_t index, const char **text, struct vg_icon *icon, void *user_data) {
    (void)listbox;
    (void)icon;
    auto *state = static_cast<VirtualListState *>(user_data);
    if (!text)
        return;
    *text = "";
    if (!state || index > static_cast<size_t>(INT64_MAX) ||
        static_cast<int64_t>(index) >= state->rowCount)
        return;
    int64_t row = static_cast<int64_t>(index);
    auto explicit_text = state->rowTexts.find(row);
    if (explicit_text != state->rowTexts.end()) {
        *text = explicit_text->second.c_str();
        return;
    }
    auto explicit_id = state->rowIds.find(row);
    if (explicit_id != state->rowIds.end()) {
        *text = explicit_id->second.c_str();
        return;
    }
    try {
        state->providerScratch = std::to_string(row);
        *text = state->providerScratch.c_str();
    } catch (const std::bad_alloc &) {
        *text = "";
    }
}

/// @brief Clear the model's raw ListBox pointer when the lower control detaches first.
/// @param listbox Lower ListBox being unbound.
/// @param user_data Bound VirtualList state.
static void virtualListUnbound(vg_widget_t *listbox, void *user_data) {
    auto *state = static_cast<VirtualListState *>(user_data);
    if (state && state->boundList == listbox)
        state->boundList = nullptr;
}

/// @brief Refresh a bound ListBox count and cached viewport after a model mutation.
/// @param state VirtualList model whose projection should be invalidated.
static void syncBoundVirtualList(VirtualListState &state) {
    auto *listbox = static_cast<vg_listbox_t *>(state.boundList);
    if (!listbox)
        return;
    size_t count = static_cast<uint64_t>(state.rowCount) > SIZE_MAX
                       ? SIZE_MAX
                       : static_cast<size_t>(state.rowCount);
    vg_listbox_set_total_count(listbox, count);
    vg_listbox_invalidate_items(listbox);
}

/// @brief Supply one borrowed flattened row descriptor to a bound virtual TreeView.
/// @param tree Lower TreeView requesting data.
/// @param index Zero-based visible-row index.
/// @param[out] out_row Destination row descriptor.
/// @param user_data Bound VirtualTree state.
/// @return `true` when a visible row was supplied; otherwise `false`.
static bool virtualTreeProvider(vg_treeview_t *tree,
                                size_t index,
                                vg_treeview_virtual_row_t *out_row,
                                void *user_data) {
    (void)tree;
    auto *state = static_cast<VirtualTreeState *>(user_data);
    if (!state || !out_row || !ensureVisibleTreeIndex(*state) || index >= state->visibleRows.size())
        return false;
    const VisibleTreeRow &visible = state->visibleRows[index];
    auto it = state->nodes.find(visible.id);
    if (it == state->nodes.end())
        return false;
    const TreeNode &node = it->second;
    out_row->text = node.text.c_str();
    out_row->depth = visible.depth;
    out_row->expanded = node.expanded;
    out_row->has_children = !node.children.empty() || !node.loaded;
    out_row->loading = node.expanded && !node.loaded && node.children.empty();
    out_row->icon_name = node.iconVector.empty() ? nullptr : node.iconVector.c_str();
    out_row->icon_text = node.iconText.empty() ? nullptr : node.iconText.c_str();
    out_row->text_color = node.color;
    out_row->flags = 0;
    if (node.dim)
        out_row->flags |= VG_TREEVIEW_VROW_DIM;
    if (state->selectedIdSet.count(visible.id) != 0)
        out_row->flags |= VG_TREEVIEW_VROW_SELECTED;
    return true;
}

/// @brief Synchronize flattened row count, selection, and paint after a tree model mutation.
/// @param state VirtualTree model whose projection should be synchronized.
static void syncBoundVirtualTree(VirtualTreeState &state) {
    if (state.updateDepth > 0) {
        state.syncPending = true;
        return;
    }
    state.syncPending = false;
    auto *tree = static_cast<vg_treeview_t *>(state.boundTree);
    if (!tree || !ensureVisibleTreeIndex(state))
        return;
    vg_treeview_set_virtual_row_count(tree, state.visibleRows.size());
    auto selected = state.visibleIndexById.find(state.selectedId);
    vg_treeview_select_virtual_index(
        tree, selected == state.visibleIndexById.end() ? SIZE_MAX : selected->second);
    vg_treeview_invalidate_virtual_rows(tree);
}

/// @brief Apply one semantic virtual TreeView action to the hash-indexed model.
/// @param tree Bound lower TreeView.
/// @param index Zero-based visible-row index receiving the action.
/// @param action Selection, parent navigation, or expansion action.
/// @param user_data Bound VirtualTree state.
static void virtualTreeAction(vg_treeview_t *tree,
                              size_t index,
                              vg_treeview_virtual_action_t action,
                              void *user_data) {
    auto *state = static_cast<VirtualTreeState *>(user_data);
    if (!state || state->boundTree != tree || !ensureVisibleTreeIndex(*state) ||
        index >= state->visibleRows.size())
        return;

    std::string id;
    try {
        id = state->visibleRows[index].id;
    } catch (const std::bad_alloc &) {
        return;
    }
    auto node = state->nodes.find(id);
    if (node == state->nodes.end())
        return;

    try {
        if (action == VG_TREEVIEW_VIRTUAL_SELECT) {
            setVirtualTreeSelection(*state, {id});
            vg_treeview_invalidate_virtual_rows(tree);
            return;
        }
        if (action == VG_TREEVIEW_VIRTUAL_SELECT_TOGGLE) {
            std::vector<std::string> ids = state->selectedIds;
            auto member = std::find(ids.begin(), ids.end(), id);
            if (member != ids.end())
                ids.erase(member);
            else
                ids.push_back(id);
            setVirtualTreeSelection(*state, std::move(ids));
            if (state->selectedIdSet.count(id) != 0)
                state->selectedId = id;
            vg_treeview_invalidate_virtual_rows(tree);
            return;
        }
        if (action == VG_TREEVIEW_VIRTUAL_SELECT_RANGE) {
            auto anchor = state->visibleIndexById.find(state->selectedId);
            size_t anchorIndex = anchor == state->visibleIndexById.end() ? index : anchor->second;
            size_t lo = anchorIndex < index ? anchorIndex : index;
            size_t hi = anchorIndex < index ? index : anchorIndex;
            std::vector<std::string> ids;
            std::string primary = state->selectedId.empty() ? id : state->selectedId;
            ids.push_back(primary);
            for (size_t row = lo; row <= hi && row < state->visibleRows.size(); ++row) {
                if (state->visibleRows[row].id != primary)
                    ids.push_back(state->visibleRows[row].id);
            }
            setVirtualTreeSelection(*state, std::move(ids));
            vg_treeview_invalidate_virtual_rows(tree);
            return;
        }
        if (action == VG_TREEVIEW_VIRTUAL_PARENT) {
            const std::string &parent = node->second.parent;
            auto parent_index = state->visibleIndexById.find(parent);
            if (!parent.empty() && parent_index != state->visibleIndexById.end()) {
                setVirtualTreeSelection(*state, {parent});
                vg_treeview_select_virtual_index(tree, parent_index->second);
                vg_treeview_invalidate_virtual_rows(tree);
            }
            return;
        }
        if (action == VG_TREEVIEW_VIRTUAL_TOGGLE) {
            node->second.expanded = !node->second.expanded;
            state->visibleDirty = true;
            syncBoundVirtualTree(*state);
        }
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Clear the model's raw TreeView pointer when the lower control detaches first.
/// @param tree Lower TreeView being unbound.
/// @param user_data Bound VirtualTree state.
static void virtualTreeUnbound(vg_treeview_t *tree, void *user_data) {
    auto *state = static_cast<VirtualTreeState *>(user_data);
    if (state && state->boundTree == tree)
        state->boundTree = nullptr;
}
#else
/// @brief Preserve VirtualList mutation semantics when GUI rendering is compiled out.
/// @details A graphics-disabled runtime can still create and query the pure data model, but no
///          lower ListBox exists to invalidate. The no-op keeps every mutation entry point shared
///          between enabled and disabled builds without manufacturing a control handle.
/// @param state Live model state whose data mutation has already completed.
static void syncBoundVirtualList(VirtualListState &state) {
    (void)state;
}

/// @brief Preserve VirtualTree mutation semantics when GUI rendering is compiled out.
/// @details Flattened model state remains fully usable in stub builds; only projection into a
///          TreeView is unavailable. The no-op deliberately performs no lazy flattening so a data
///          mutation has the same allocation behavior regardless of graphics availability.
/// @param state Live model state whose data mutation has already completed.
static void syncBoundVirtualTree(VirtualTreeState &state) {
    if (state.updateDepth > 0) {
        state.syncPending = true;
        return;
    }
    state.syncPending = false;
}
#endif

/// @brief Detach a VirtualList from its non-owning control, if any.
/// @param state VirtualList state whose control link is cleared.
static void detachVirtualList(VirtualListState &state) {
    void *bound = state.boundList;
    state.boundList = nullptr;
#ifdef ZANNA_ENABLE_GRAPHICS
    if (bound)
        vg_listbox_clear_virtual_model(static_cast<vg_listbox_t *>(bound));
#else
    (void)bound;
#endif
}

/// @brief Detach a VirtualTree from its non-owning control, if any.
/// @param state VirtualTree state whose control link is cleared.
static void detachVirtualTree(VirtualTreeState &state) {
    void *bound = state.boundTree;
    state.boundTree = nullptr;
#ifdef ZANNA_ENABLE_GRAPHICS
    if (bound)
        vg_treeview_clear_virtual_model(static_cast<vg_treeview_t *>(bound));
#else
    (void)bound;
#endif
}

/// @brief Convert an 8-bit sRGB channel to linear-light luminance.
/// @param c Channel value; only the low eight bits are used.
/// @return Linearized channel value in the nominal range `[0,1]`.
double channelLuminance(int64_t c) {
    double v = static_cast<double>(c & 0xff) / 255.0;
    return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

/// @brief Compute relative luminance for a packed `0xRRGGBB` color.
/// @param rgb Packed RGB value.
/// @return WCAG relative luminance.
double luminance(int64_t rgb) {
    return 0.2126 * channelLuminance((rgb >> 16) & 0xff) +
           0.7152 * channelLuminance((rgb >> 8) & 0xff) + 0.0722 * channelLuminance(rgb & 0xff);
}

// --- Command / CommandRegistry: bind one UI action to its menu item, toolbar
//     button, keyboard shortcut and palette entry, and route them from one poll. ---

struct CommandData {
    std::string id;
    std::string title;
    std::string shortcut;
    bool enabled{true};
    bool checkable{false};
    bool checked{false};
    bool invoked{false};
    // Foreign widget handles, stored raw and only ever touched through the public
    // self-guarding accessors (which validate liveness and no-op on a dead widget).
    void *menuItem{nullptr};
    void *toolbarItem{nullptr};
};

struct CommandHandle {
    CommandData *state{nullptr};
};

struct CommandRegistryData {
    std::vector<void *> commands; // retained Command handles (registry co-owns them)
    void *palette{nullptr};       // foreign CommandPalette handle, stored raw
};

struct CommandRegistryHandle {
    CommandRegistryData *state{nullptr};
};

/// @brief Trapping accessor for a Zanna.GUI.Command handle (public Command methods).
/// @param obj Candidate managed object.
/// @return Validated live Command handle, or `nullptr` after trapping.
CommandHandle *requireCommand(void *obj) {
    if (!obj || rt_obj_class_id(obj) != RT_GUI_COMMAND_CLASS_ID) {
        rt_trap("GUI.Command: invalid handle");
        return nullptr;
    }
    auto *h = static_cast<CommandHandle *>(obj);
    if (!h->state) {
        rt_trap("GUI.Command: destroyed handle");
        return nullptr;
    }
    return h;
}

/// @brief Non-trapping accessor: returns the command payload, or NULL if @p obj is not a
///        live Command. Used to validate registry entries without aborting.
/// @param obj Candidate managed object.
/// @return Live command payload, or `nullptr` for invalid input.
CommandData *commandDataChecked(void *obj) {
    if (!rt_obj_is_instance(obj, RT_GUI_COMMAND_CLASS_ID, sizeof(CommandHandle)))
        return nullptr;
    return static_cast<CommandHandle *>(obj)->state;
}

/// @brief Validate a runtime object as a live CommandRegistry handle.
/// @param obj Candidate managed object.
/// @return Validated handle, or `nullptr` after trapping on invalid or destroyed input.
CommandRegistryHandle *requireCommandRegistry(void *obj) {
    if (!obj || rt_obj_class_id(obj) != RT_GUI_COMMAND_REGISTRY_CLASS_ID) {
        rt_trap("GUI.CommandRegistry: invalid handle");
        return nullptr;
    }
    auto *h = static_cast<CommandRegistryHandle *>(obj);
    if (!h->state) {
        rt_trap("GUI.CommandRegistry: destroyed handle");
        return nullptr;
    }
    return h;
}

/// @brief Release C++ state owned by a managed Command.
/// @param obj Managed Command object being finalized.
void commandFinalizer(void *obj) {
    auto *h = static_cast<CommandHandle *>(obj);
    delete h->state; // menuItem/toolbarItem are not owned; nothing else to free
    h->state = nullptr;
}

/// @brief Release retained commands and C++ state owned by a CommandRegistry.
/// @param obj Managed CommandRegistry object being finalized.
void commandRegistryFinalizer(void *obj) {
    auto *h = static_cast<CommandRegistryHandle *>(obj);
    if (h->state) {
        for (void *cmd : h->state->commands) {
            if (cmd && rt_obj_release_known_check0(cmd))
                rt_obj_free(cmd);
        }
        delete h->state;
        h->state = nullptr;
    }
}

/// @brief Push the command's enabled/checked state onto its bound widgets.
/// @details The set_* accessors self-guard, so a NULL or destroyed widget is a no-op.
/// @param c Command payload whose state is projected.
void pushCommandState(CommandData &c) {
    if (c.menuItem) {
        rt_menuitem_set_enabled(c.menuItem, c.enabled ? 1 : 0);
        if (c.checkable) {
            rt_menuitem_set_checkable(c.menuItem, 1);
            rt_menuitem_set_checked(c.menuItem, c.checked ? 1 : 0);
        }
    }
    if (c.toolbarItem) {
        rt_toolbaritem_set_enabled(c.toolbarItem, c.enabled ? 1 : 0);
        if (c.checkable)
            rt_toolbaritem_set_toggled(c.toolbarItem, c.checked ? 1 : 0);
    }
}

/// @brief Push state, then read the command's invocation sources for this frame.
/// @param c Command payload to poll and update.
/// @param paletteSelectedId The palette's selected command id this frame ("" if none).
/// @return True if the (enabled) command was invoked. Each click source is read exactly once
///         (menu/toolbar clicks are consumed on read), and the result is cached on the command.
bool pollCommandInto(CommandData &c, const std::string &paletteSelectedId) {
    pushCommandState(c);
    bool clicked = false;
    if (c.menuItem && rt_menuitem_was_clicked(c.menuItem))
        clicked = true;
    if (c.toolbarItem && rt_toolbaritem_was_clicked(c.toolbarItem))
        clicked = true;
    if (!c.shortcut.empty() && !c.id.empty()) {
        rt_string idStr = makeString(c.id);
        if (idStr) {
            if (rt_shortcuts_was_triggered(idStr))
                clicked = true;
            rt_string_unref(idStr);
        }
    }
    if (!paletteSelectedId.empty() && paletteSelectedId == c.id)
        clicked = true;
    c.invoked = clicked && c.enabled; // a disabled command is never invoked
    return c.invoked;
}

} // namespace

extern "C" {

/// @brief Create an empty managed GUI automation harness.
/// @return New TestHarness handle, or `NULL` on allocation failure.
void *rt_gui_test_harness_new(void) {
    auto *h = static_cast<HarnessHandle *>(
        rt_obj_new_i64(RT_GUI_TEST_HARNESS_CLASS_ID, sizeof(HarnessHandle)));
    if (!h)
        return nullptr;
    h->state = new (std::nothrow) HarnessState();
    if (!h->state) {
        releaseObject(h);
        return nullptr;
    }
    rt_obj_set_finalizer(h, harnessFinalizer);
    return h;
}

/// @brief Clear synthetic widgets, events, focus, and dispatch position from a harness.
/// @param harness Managed TestHarness handle.
void rt_gui_test_harness_clear(void *harness) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireHarness(harness));
    h->state->widgets.clear();
    h->state->events.clear();
    h->state->focus.clear();
#ifdef ZANNA_ENABLE_GRAPHICS
    h->state->dispatchedEvents = 0;
#endif
}

/// @brief Retain a live App as the real automation target for a TestHarness.
/// @details Historical synthetic events are not replayed, and failed validation preserves any
///          current binding. Graphics-disabled builds expose the same ABI and return false.
/// @param harness Managed TestHarness object.
/// @param app Live App object to retain.
/// @return 1 when bound, otherwise 0.
int8_t rt_gui_test_harness_bind_app(void *harness, void *app) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), 0);
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_automation_app_view_t validated{};
    if (!rt_gui_automation_snapshot_app(app, &validated))
        return 0;
    if (h->state->boundApp == app)
        return 1;
    rt_obj_retain_known(app);
    void *previous = h->state->boundApp;
    h->state->boundApp = app;
    h->state->dispatchedEvents = h->state->events.size();
    releaseObject(previous);
    return 1;
#else
    (void)app;
    return 0;
#endif
}

/// @brief Release the App retained by a TestHarness without clearing synthetic state.
/// @param harness Managed TestHarness object.
void rt_gui_test_harness_unbind_app(void *harness) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireHarness(harness));
#ifdef ZANNA_ENABLE_GRAPHICS
    void *previous = h->state->boundApp;
    h->state->boundApp = nullptr;
    h->state->dispatchedEvents = h->state->events.size();
    releaseObject(previous);
#endif
}

/// @brief Queue pending authored events and poll the bound App once.
/// @details Every journal record is attempted at most once; the return count includes only records
///          whose full native-event expansion was accepted by the synchronized window queue.
/// @param harness Managed TestHarness object.
/// @return Successfully queued record count, or zero without a live binding.
int64_t rt_gui_test_harness_dispatch_pending(void *harness) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), 0);
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_automation_app_view_t app{};
    if (!harnessBoundApp(*h->state, app))
        return 0;
    int64_t accepted = harnessPostPending(*h->state, app);
    rt_gui_app_poll(h->state->boundApp);
    return accepted;
#else
    return 0;
#endif
}

/// @brief Queue pending authored events and execute one deterministic App frame.
/// @param harness Managed TestHarness object.
/// @param delta_ms Requested scheduler advance in milliseconds.
/// @return 1 while the bound app remains runnable, otherwise 0.
int8_t rt_gui_test_harness_render_frame(void *harness, double delta_ms) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), 0);
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_automation_app_view_t app{};
    if (!harnessBoundApp(*h->state, app))
        return 0;
    (void)harnessPostPending(*h->state, app);
    return rt_gui_app_run_frame_with_delta(h->state->boundApp, delta_ms) ? 1 : 0;
#else
    (void)delta_ms;
    return 0;
#endif
}

/// @brief Deep-copy a requested physical framebuffer rectangle into Pixels storage.
/// @details Out-of-window samples are transparent and successful output is generation-stamped so
///          downstream image caches observe the newly populated content immediately.
/// @param harness Managed TestHarness object with a live app binding.
/// @param x Physical region origin X.
/// @param y Physical region origin Y.
/// @param width Positive capture width.
/// @param height Positive capture height.
/// @return New managed Pixels object, or NULL when unavailable.
void *rt_gui_test_harness_capture_pixels(
    void *harness, int64_t x, int64_t y, int64_t width, int64_t height) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), nullptr);
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_automation_app_view_t app{};
    if (!harnessBoundApp(*h->state, app) || width <= 0 || height <= 0)
        return nullptr;
    vgfx_framebuffer_t framebuffer{};
    if (!vgfx_get_framebuffer(static_cast<vgfx_window_t>(app.window), &framebuffer))
        return nullptr;
    void *pixels = rt_pixels_new(width, height);
    rt_pixels_impl *output = rt_pixels_checked_impl_or_null(pixels);
    if (!output)
        return nullptr;
    for (int64_t row = 0; row < height; ++row) {
        for (int64_t column = 0; column < width; ++column) {
            output->data[static_cast<size_t>(row) * static_cast<size_t>(width) +
                         static_cast<size_t>(column)] =
                harnessFramebufferPixel(
                    framebuffer, saturatingAddI64(x, column), saturatingAddI64(y, row));
        }
    }
    pixels_touch(output);
    return pixels;
#else
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    return nullptr;
#endif
}

/// @brief Compute a stable FNV-1a digest over one physical framebuffer region.
/// @details Dimensions and RGBA channel bytes have explicit encodings, making the 16-character
///          lowercase digest independent of pointer identity, alignment, and host endianness.
/// @param harness Managed TestHarness object with a live app binding.
/// @param x Physical region origin X.
/// @param y Physical region origin Y.
/// @param width Positive capture width.
/// @param height Positive capture height.
/// @return Newly referenced digest, or an empty string when unavailable.
rt_string rt_gui_test_harness_capture_hash(
    void *harness, int64_t x, int64_t y, int64_t width, int64_t height) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), nullptr);
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_automation_app_view_t app{};
    if (!harnessBoundApp(*h->state, app) || width <= 0 || height <= 0)
        return makeString("");
    vgfx_framebuffer_t framebuffer{};
    if (!vgfx_get_framebuffer(static_cast<vgfx_window_t>(app.window), &framebuffer))
        return makeString("");
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = harnessHashI64(hash, width);
    hash = harnessHashI64(hash, height);
    for (int64_t row = 0; row < height; ++row) {
        for (int64_t column = 0; column < width; ++column) {
            uint32_t rgba = harnessFramebufferPixel(
                framebuffer, saturatingAddI64(x, column), saturatingAddI64(y, row));
            hash = harnessHashByte(hash, static_cast<uint8_t>(rgba >> 24u));
            hash = harnessHashByte(hash, static_cast<uint8_t>(rgba >> 16u));
            hash = harnessHashByte(hash, static_cast<uint8_t>(rgba >> 8u));
            hash = harnessHashByte(hash, static_cast<uint8_t>(rgba));
        }
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string encoded(16u, '0');
    for (size_t index = 0; index < encoded.size(); ++index) {
        unsigned shift = static_cast<unsigned>((encoded.size() - index - 1u) * 4u);
        encoded[index] = digits[(hash >> shift) & 0xFu];
    }
    return makeString(encoded);
#else
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    return makeString("");
#endif
}

/// @brief Compare a reference Pixels image with a bound physical framebuffer region.
/// @details Returns a schema-versioned statistics map for both successful and unavailable
///          comparisons. Tolerance is per RGBA channel and is clamped to [0,255].
/// @param harness Managed TestHarness object.
/// @param expected Reference Pixels image.
/// @param x Physical destination origin X.
/// @param y Physical destination origin Y.
/// @param tolerance Maximum accepted absolute channel delta.
/// @return New managed comparison Map, or NULL only when its root allocation fails.
void *rt_gui_test_harness_compare_region(
    void *harness, void *expected, int64_t x, int64_t y, int64_t tolerance) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), nullptr);
    void *comparison = rt_map_new();
    if (!comparison)
        return nullptr;
    rt_map_set_int(comparison, rt_const_cstr("schemaVersion"), 1);
    rt_map_set_bool(comparison, rt_const_cstr("matches"), 0);
    rt_map_set_int(comparison, rt_const_cstr("width"), 0);
    rt_map_set_int(comparison, rt_const_cstr("height"), 0);
    rt_map_set_int(comparison, rt_const_cstr("tolerance"), 0);
    rt_map_set_int(comparison, rt_const_cstr("comparedPixels"), 0);
    rt_map_set_int(comparison, rt_const_cstr("differentPixels"), 0);
    rt_map_set_int(comparison, rt_const_cstr("maxChannelDelta"), 0);
    rt_map_set_float(comparison, rt_const_cstr("meanAbsoluteError"), 0.0);
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_automation_app_view_t app{};
    rt_pixels_impl *reference = rt_pixels_checked_impl_or_null(expected);
    if (!harnessBoundApp(*h->state, app) || !reference)
        return comparison;
    if (tolerance < 0)
        tolerance = 0;
    if (tolerance > 255)
        tolerance = 255;
    rt_map_set_int(comparison, rt_const_cstr("width"), reference->width);
    rt_map_set_int(comparison, rt_const_cstr("height"), reference->height);
    rt_map_set_int(comparison, rt_const_cstr("tolerance"), tolerance);
    vgfx_framebuffer_t framebuffer{};
    if (!vgfx_get_framebuffer(static_cast<vgfx_window_t>(app.window), &framebuffer))
        return comparison;

    uint64_t compared = 0;
    uint64_t different = 0;
    int64_t max_delta = 0;
    long double absolute_error = 0.0L;
    for (int64_t row = 0; row < reference->height; ++row) {
        for (int64_t column = 0; column < reference->width; ++column) {
            uint32_t actual = harnessFramebufferPixel(
                framebuffer, saturatingAddI64(x, column), saturatingAddI64(y, row));
            uint32_t wanted =
                reference->data[static_cast<size_t>(row) * static_cast<size_t>(reference->width) +
                                static_cast<size_t>(column)];
            bool pixel_differs = false;
            for (unsigned shift = 0; shift < 32u; shift += 8u) {
                int lhs = static_cast<int>((actual >> shift) & 0xFFu);
                int rhs = static_cast<int>((wanted >> shift) & 0xFFu);
                int delta = lhs >= rhs ? lhs - rhs : rhs - lhs;
                absolute_error += static_cast<long double>(delta);
                if (delta > max_delta)
                    max_delta = delta;
                if (delta > tolerance)
                    pixel_differs = true;
            }
            if (pixel_differs)
                ++different;
            ++compared;
        }
    }
    int64_t compared_i64 =
        compared > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(compared);
    int64_t different_i64 =
        different > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(different);
    double mean_error =
        compared == 0u
            ? 0.0
            : static_cast<double>(absolute_error / (static_cast<long double>(compared) * 4.0L));
    rt_map_set_bool(comparison, rt_const_cstr("matches"), different == 0u ? 1 : 0);
    rt_map_set_int(comparison, rt_const_cstr("comparedPixels"), compared_i64);
    rt_map_set_int(comparison, rt_const_cstr("differentPixels"), different_i64);
    rt_map_set_int(comparison, rt_const_cstr("maxChannelDelta"), max_delta);
    rt_map_set_float(comparison, rt_const_cstr("meanAbsoluteError"), mean_error);
#else
    (void)expected;
    (void)x;
    (void)y;
    (void)tolerance;
#endif
    return comparison;
}

/// @brief Return the normal public accessibility snapshot for the bound app root.
/// @param harness Managed TestHarness object.
/// @return New managed schema-versioned Map, empty when no live app is bound.
void *rt_gui_test_harness_get_accessibility_snapshot(void *harness) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), nullptr);
#ifdef ZANNA_ENABLE_GRAPHICS
    rt_gui_automation_app_view_t app{};
    return rt_accessibility_snapshot(harnessBoundApp(*h->state, app) ? app.root : nullptr);
#else
    return rt_accessibility_snapshot(nullptr);
#endif
}

/// @brief Advance the synthetic harness frame counter.
/// @param harness Managed TestHarness handle.
/// @param frames Number of frames to add; values below one advance by one.
/// @return Updated synthetic frame number.
int64_t rt_gui_test_harness_tick(void *harness, int64_t frames) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), 0);
    if (frames < 1)
        frames = 1;
    h->state->frame += frames;
    return h->state->frame;
}

/// @brief Add or replace one synthetic widget record by stable identifier.
/// @param harness Managed TestHarness handle.
/// @param id Stable widget identifier.
/// @param type Widget type name.
/// @param name Accessible or display name.
/// @param x Synthetic bounds X coordinate.
/// @param y Synthetic bounds Y coordinate.
/// @param w Synthetic bounds width.
/// @param hgt Synthetic bounds height.
void rt_gui_test_harness_register_widget(void *harness,
                                         rt_string id,
                                         rt_string type,
                                         rt_string name,
                                         int64_t x,
                                         int64_t y,
                                         int64_t w,
                                         int64_t hgt) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireHarness(harness));
    try {
        WidgetRecord rec{toStd(id), toStd(type), toStd(name), x, y, w, hgt};
        /// @brief Match an existing synthetic widget by stable identifier.
        /// @param wrec Candidate widget record.
        /// @return `true` when @p wrec has the replacement record's identifier.
        auto it = std::find_if(h->state->widgets.begin(),
                               h->state->widgets.end(),
                               [&](const WidgetRecord &wrec) { return wrec.id == rec.id; });
        if (it == h->state->widgets.end())
            h->state->widgets.push_back(rec);
        else
            *it = rec;
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Find a synthetic widget by stable identifier.
/// @param harness Managed TestHarness handle.
/// @param id Widget identifier to match.
/// @return New query-result map with a `found` field.
void *rt_gui_test_harness_find_by_id(void *harness, rt_string id) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), widgetToMap(nullptr));
    try {
        std::string key = toStd(id);
        /// @brief Match a synthetic widget by requested identifier.
        /// @param w Candidate widget record.
        /// @return `true` when @p w has identifier @c key.
        auto it = std::find_if(h->state->widgets.begin(),
                               h->state->widgets.end(),
                               [&](const WidgetRecord &w) { return w.id == key; });
        return widgetToMap(it == h->state->widgets.end() ? nullptr : &*it);
    } catch (const std::bad_alloc &) {
        return widgetToMap(nullptr);
    }
}

/// @brief Convert a legacy TestHarness lookup map into an Option.
/// @details The legacy lookup always returns a Map with a `found` flag. The
///          Option wrapper treats `found=false` as absence and releases the
///          temporary map after the Option has retained found records.
/// @param map Map returned by a legacy TestHarness lookup.
/// @return Zanna.Option.Some(Map) for found records, otherwise Zanna.Option.None().
static void *testHarnessLookupOption(void *map) {
    if (!map || rt_map_get_bool(map, rt_const_cstr("found")) == 0) {
        releaseObject(map);
        return rt_option_none();
    }
    void *option = rt_option_some(map);
    releaseObject(map);
    return option;
}

/// @brief Find a widget by id and return an Option-wrapped record.
/// @param harness TestHarness object.
/// @param id Widget id to find.
/// @return Zanna.Option.Some(Map) when found, otherwise Zanna.Option.None().
void *rt_gui_test_harness_find_by_id_option(void *harness, rt_string id) {
    return testHarnessLookupOption(rt_gui_test_harness_find_by_id(harness, id));
}

/// @brief Find the first synthetic widget with a matching name.
/// @param harness Managed TestHarness handle.
/// @param name Widget name to match.
/// @return New query-result map with a `found` field.
void *rt_gui_test_harness_find_by_name(void *harness, rt_string name) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), widgetToMap(nullptr));
    try {
        std::string key = toStd(name);
        /// @brief Match a synthetic widget by requested name.
        /// @param w Candidate widget record.
        /// @return `true` when @p w has name @c key.
        auto it = std::find_if(h->state->widgets.begin(),
                               h->state->widgets.end(),
                               [&](const WidgetRecord &w) { return w.name == key; });
        return widgetToMap(it == h->state->widgets.end() ? nullptr : &*it);
    } catch (const std::bad_alloc &) {
        return widgetToMap(nullptr);
    }
}

/// @brief Find a widget by name and return an Option-wrapped record.
/// @param harness TestHarness object.
/// @param name Widget name to find.
/// @return Zanna.Option.Some(Map) when found, otherwise Zanna.Option.None().
void *rt_gui_test_harness_find_by_name_option(void *harness, rt_string name) {
    return testHarnessLookupOption(rt_gui_test_harness_find_by_name(harness, name));
}

/// @brief Find the first synthetic widget with a matching type.
/// @param harness Managed TestHarness handle.
/// @param type Widget type to match.
/// @return New query-result map with a `found` field.
void *rt_gui_test_harness_find_by_type(void *harness, rt_string type) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), widgetToMap(nullptr));
    try {
        std::string key = toStd(type);
        /// @brief Match a synthetic widget by requested type.
        /// @param w Candidate widget record.
        /// @return `true` when @p w has type @c key.
        auto it = std::find_if(h->state->widgets.begin(),
                               h->state->widgets.end(),
                               [&](const WidgetRecord &w) { return w.type == key; });
        return widgetToMap(it == h->state->widgets.end() ? nullptr : &*it);
    } catch (const std::bad_alloc &) {
        return widgetToMap(nullptr);
    }
}

/// @brief Find a widget by type and return an Option-wrapped record.
/// @param harness TestHarness object.
/// @param type Widget type to find.
/// @return Zanna.Option.Some(Map) when found, otherwise Zanna.Option.None().
void *rt_gui_test_harness_find_by_type_option(void *harness, rt_string type) {
    return testHarnessLookupOption(rt_gui_test_harness_find_by_type(harness, type));
}

/// @brief Append a synthetic key action to the harness journal.
/// @param harness Managed TestHarness handle.
/// @param key Runtime key token.
/// @param modifiers Platform modifier-bit mask.
void rt_gui_test_harness_send_key(void *harness, rt_string key, int64_t modifiers) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireHarness(harness));
    try {
        h->state->events.push_back({"key", toStd(key), 0, 0, 0, modifiers, h->state->frame});
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Append a synthetic mouse action and update synthetic focus by hit testing.
/// @param harness Managed TestHarness handle.
/// @param event_type Runtime mouse action token.
/// @param x Synthetic pointer X coordinate.
/// @param y Synthetic pointer Y coordinate.
/// @param button Platform mouse-button ordinal.
void rt_gui_test_harness_send_mouse(
    void *harness, rt_string event_type, int64_t x, int64_t y, int64_t button) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireHarness(harness));
    try {
        h->state->events.push_back({toStd(event_type), "", x, y, button, 0, h->state->frame});
        for (auto it = h->state->widgets.rbegin(); it != h->state->widgets.rend(); ++it) {
            if (rangesIntersect(it->x, it->w, x, 1) && rangesIntersect(it->y, it->h, y, 1)) {
                h->state->focus = it->id;
                break;
            }
        }
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Return the number of synthetic input events recorded by a GUI test harness.
/// @details Invalid handles return zero through the standard harness guard. Extremely large
///          vectors are saturated to `INT64_MAX` to preserve the public integer contract.
/// @param harness Managed TestHarness handle.
/// @return Recorded event count saturated to `INT64_MAX`.
int64_t rt_gui_test_harness_event_count(void *harness) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), 0);
    if (h->state->events.size() > static_cast<size_t>(INT64_MAX))
        return INT64_MAX;
    return static_cast<int64_t>(h->state->events.size());
}

/// @brief Return one recorded harness event as a map.
/// @details Out-of-range indices and invalid handles return a map with `found=false`; valid
///          events include type, value, coordinates, button, modifiers, and frame fields.
/// @param harness Managed TestHarness handle.
/// @param index Zero-based event index.
/// @return New event-result map with a `found` field.
void *rt_gui_test_harness_event_at(void *harness, int64_t index) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), eventToMap(nullptr));
    if (index < 0 || static_cast<uint64_t>(index) >= h->state->events.size())
        return eventToMap(nullptr);
    return eventToMap(&h->state->events[static_cast<size_t>(index)]);
}

/// @brief Clear all synthetic input events recorded by a GUI test harness.
/// @details This does not alter focus, widgets, virtual lists, or the frame counter; it only
///          resets the event journal exposed through the event inspection helpers.
/// @param harness Managed TestHarness handle.
void rt_gui_test_harness_clear_events(void *harness) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireHarness(harness));
    h->state->events.clear();
#ifdef ZANNA_ENABLE_GRAPHICS
    h->state->dispatchedEvents = 0;
#endif
}

/// @brief Return the stable identifier of the synthetically focused widget.
/// @param harness Managed TestHarness handle.
/// @return Newly referenced focus identifier, possibly empty.
rt_string rt_gui_test_harness_get_focus(void *harness) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), nullptr);
    return makeString(h->state->focus);
}

/// @brief Return synthetic widget identifiers in registration order.
/// @param harness Managed TestHarness handle.
/// @return New owned sequence of identifier strings, or `NULL` on allocation failure.
void *rt_gui_test_harness_focus_order(void *harness) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), nullptr);
    void *seq = rt_seq_new_owned();
    if (!seq)
        return nullptr;
    for (const auto &w : h->state->widgets) {
        rt_string id = makeString(w.id);
        if (id) {
            rt_seq_push(seq, id);
            rt_string_unref(id);
        }
    }
    return seq;
}

/// @brief Produce a synthetic nonblank-region summary from registered widget bounds.
/// @param harness Managed TestHarness handle.
/// @param x Query rectangle X coordinate.
/// @param y Query rectangle Y coordinate.
/// @param w Query rectangle width.
/// @param hgt Query rectangle height.
/// @return New managed snapshot map, or `NULL` on allocation failure.
void *rt_gui_test_harness_capture_region(
    void *harness, int64_t x, int64_t y, int64_t w, int64_t hgt) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireHarness(harness), nullptr);
    void *snapshot = rt_map_new();
    if (!snapshot)
        return nullptr;
    rt_map_set_int(snapshot, rt_const_cstr("x"), x);
    rt_map_set_int(snapshot, rt_const_cstr("y"), y);
    rt_map_set_int(snapshot, rt_const_cstr("width"), w);
    rt_map_set_int(snapshot, rt_const_cstr("height"), hgt);
    int64_t hits = 0;
    for (const auto &widget : h->state->widgets) {
        if (intersects(widget, x, y, w, hgt))
            hits++;
    }
    rt_map_set_int(snapshot, rt_const_cstr("nonBlankPixels"), hits > 0 ? safeAreaI64(w, hgt) : 0);
    rt_map_set_bool(snapshot, rt_const_cstr("nonBlank"), hits > 0 ? 1 : 0);
    return snapshot;
}

/// @brief Read the nonblank assertion flag from a synthetic snapshot map.
/// @param snapshot Runtime map returned by the region capture API.
/// @return `1` when the map reports nonblank content; otherwise `0`.
int8_t rt_gui_test_harness_assert_nonblank(void *snapshot) {
    if (!snapshot || rt_obj_class_id(snapshot) != RT_MAP_CLASS_ID)
        return 0;
    return rt_map_get_bool(snapshot, rt_const_cstr("nonBlank"));
}

/// @brief Create a managed virtual-list model with fixed row and viewport metrics.
/// @param row_count Initial non-negative logical row count.
/// @param row_height Row height clamped to at least one.
/// @param viewport_height Viewport height clamped to at least one.
/// @return New VirtualList handle, or `NULL` on allocation failure.
void *rt_virtual_list_new(int64_t row_count, int64_t row_height, int64_t viewport_height) {
    auto *h = static_cast<VirtualListHandle *>(
        rt_obj_new_i64(RT_GUI_VIRTUAL_LIST_CLASS_ID, sizeof(VirtualListHandle)));
    if (!h)
        return nullptr;
    h->state = new (std::nothrow) VirtualListState();
    if (!h->state) {
        releaseObject(h);
        return nullptr;
    }
    h->state->rowCount = std::max<int64_t>(0, row_count);
    h->state->rowHeight = std::max<int64_t>(1, row_height);
    h->state->viewportHeight = std::max<int64_t>(1, viewport_height);
    rt_obj_set_finalizer(h, listFinalizer);
    return h;
}

/// @brief Synchronize model selection from a bound ListBox's current selected index.
/// @param state VirtualList model whose stable selection is updated.
static void syncVirtualListSelectionFromControl(VirtualListState &state) {
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *listbox = static_cast<vg_listbox_t *>(state.boundList);
    if (!listbox)
        return;
    size_t selected = vg_listbox_get_selected_index(listbox);
    try {
        if (selected == SIZE_MAX || selected > static_cast<size_t>(INT64_MAX) ||
            static_cast<int64_t>(selected) >= state.rowCount)
            state.selectedId.clear();
        else
            state.selectedId = rowId(state, static_cast<int64_t>(selected));
    } catch (const std::bad_alloc &) {
        return;
    }
#else
    (void)state;
#endif
}

/// @brief Resize a sparse VirtualList while preserving unique stable-ID semantics.
/// @param list Managed VirtualList handle.
/// @param row_count New non-negative logical row count.
void rt_virtual_list_set_count(void *list, int64_t row_count) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireList(list));
    VirtualListState &state = *h->state;
    int64_t new_count = std::max<int64_t>(0, row_count);
    if (new_count == state.rowCount)
        return;

    if (new_count > state.rowCount) {
        for (const auto &entry : state.idRows) {
            int64_t implicit_row = -1;
            if (parseCanonicalRowNumber(entry.first, &implicit_row) &&
                implicit_row >= state.rowCount && implicit_row < new_count &&
                entry.second != implicit_row) {
                trapDuplicateModelId(entry.first);
                return;
            }
        }
    }

    if (new_count < state.rowCount) {
        for (auto it = state.rowIds.begin(); it != state.rowIds.end();) {
            if (it->first >= new_count) {
                state.idRows.erase(it->second);
                it = state.rowIds.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = state.rowTexts.begin(); it != state.rowTexts.end();) {
            if (it->first >= new_count)
                it = state.rowTexts.erase(it);
            else
                ++it;
        }
    }

    state.rowCount = new_count;
    int64_t selected_row = -1;
    if (!state.selectedId.empty() && !findListRowById(state, state.selectedId, &selected_row))
        state.selectedId.clear();
    syncBoundVirtualList(state);
}

/// @brief Assign a unique stable ID with rollback on allocation failure.
/// @param list Managed VirtualList handle.
/// @param row Zero-based logical row.
/// @param id Non-empty stable identifier.
void rt_virtual_list_set_row_id(void *list, int64_t row, rt_string id) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireList(list));
    VirtualListState &state = *h->state;
    if (row < 0 || row >= state.rowCount)
        return;
    try {
        std::string desired = toStd(id);
        if (desired.empty()) {
            rt_trap("GUI model ID must not be empty");
            return;
        }
        std::string implicit = std::to_string(row);
        auto current = state.rowIds.find(row);
        std::string old_id = current != state.rowIds.end() ? current->second : implicit;
        if (desired == old_id)
            return;

        int64_t owner = -1;
        if (findListRowById(state, desired, &owner) && owner != row) {
            trapDuplicateModelId(desired);
            return;
        }

        bool follows_selection = state.selectedId == old_id;
        std::string selected_replacement = follows_selection ? desired : std::string();
        if (desired == implicit) {
            if (current != state.rowIds.end()) {
                state.idRows.erase(current->second);
                state.rowIds.erase(current);
            }
        } else {
            state.idRows.reserve(state.idRows.size() + 1u);
            state.rowIds.reserve(state.rowIds.size() + 1u);
            current = state.rowIds.find(row);
            auto inserted_index = state.idRows.emplace(desired, row);
            if (!inserted_index.second && inserted_index.first->second != row) {
                trapDuplicateModelId(desired);
                return;
            }
            std::string row_value = desired;
            if (current != state.rowIds.end()) {
                current->second.swap(row_value);
            } else {
                try {
                    state.rowIds.emplace(row, std::move(row_value));
                } catch (const std::bad_alloc &) {
                    if (inserted_index.second)
                        state.idRows.erase(inserted_index.first);
                    return;
                }
            }
            state.idRows.erase(old_id);
        }
        if (follows_selection)
            state.selectedId.swap(selected_replacement);
#ifdef ZANNA_ENABLE_GRAPHICS
        if (state.boundList) {
            vg_listbox_invalidate_item(static_cast<vg_listbox_t *>(state.boundList),
                                       static_cast<size_t>(row));
            if (follows_selection)
                vg_listbox_select_index(static_cast<vg_listbox_t *>(state.boundList),
                                        static_cast<size_t>(row));
        }
#endif
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Set display text for one sparse VirtualList row without changing its stable ID.
/// @param list Managed VirtualList handle.
/// @param row Zero-based logical row.
/// @param text Runtime display text normalized for GUI rendering.
void rt_virtual_list_set_row_text(void *list, int64_t row, rt_string text) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireList(list));
    VirtualListState &state = *h->state;
    if (row < 0 || row >= state.rowCount)
        return;
    try {
        std::string value = toGuiTextStd(text);
        auto current = state.rowTexts.find(row);
        if (current != state.rowTexts.end()) {
            if (current->second == value)
                return;
            current->second.swap(value);
        } else {
            state.rowTexts.emplace(row, std::move(value));
        }
#ifdef ZANNA_ENABLE_GRAPHICS
        if (state.boundList)
            vg_listbox_invalidate_item(static_cast<vg_listbox_t *>(state.boundList),
                                       static_cast<size_t>(row));
#endif
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Invalidate one bound ListBox row after application-owned backing data changes.
/// @param list Managed VirtualList handle.
/// @param row Zero-based logical row to repaint.
void rt_virtual_list_invalidate_row(void *list, int64_t row) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireList(list));
    if (row < 0 || row >= h->state->rowCount)
        return;
#ifdef ZANNA_ENABLE_GRAPHICS
    if (h->state->boundList)
        vg_listbox_invalidate_item(static_cast<vg_listbox_t *>(h->state->boundList),
                                   static_cast<size_t>(row));
#endif
}

/// @brief Bind a VirtualList to a live ListBox, preserving an old binding on allocation failure.
/// @param list Managed VirtualList handle.
/// @param listbox Live ListBox control.
/// @return `1` when binding succeeds; otherwise `0`.
int8_t rt_virtual_list_bind(void *list, void *listbox) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireList(list), 0);
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control =
        static_cast<vg_listbox_t *>(rt_gui_widget_checked_for_binding(listbox, VG_WIDGET_LISTBOX));
    if (!control)
        return 0;
    VirtualListState &state = *h->state;
    void *old_control = state.boundList;
    if (!vg_listbox_bind_virtual_model(control,
                                       static_cast<size_t>(state.rowCount),
                                       static_cast<float>(state.rowHeight),
                                       virtualListProvider,
                                       &state,
                                       virtualListUnbound))
        return 0;
    state.boundList = control;
    if (old_control && old_control != control)
        vg_listbox_clear_virtual_model(static_cast<vg_listbox_t *>(old_control));
    int64_t selected = -1;
    if (findListRowById(state, state.selectedId, &selected))
        vg_listbox_select_index(control, static_cast<size_t>(selected));
    return 1;
#else
    (void)listbox;
    return 0;
#endif
}

/// @brief Detach a VirtualList from its current ListBox, if any.
/// @param list Managed VirtualList handle.
void rt_virtual_list_unbind(void *list) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireList(list));
    detachVirtualList(*h->state);
}

/// @brief Reverse-form convenience wrapper for `VirtualList.Bind`.
/// @param listbox Live ListBox control.
/// @param model Managed VirtualList handle.
/// @return `1` when binding succeeds; otherwise `0`.
int8_t rt_listbox_set_virtual_model(void *listbox, void *model) {
    return rt_virtual_list_bind(model, listbox);
}

/// @brief Clear a ListBox's external model binding without destroying either object.
/// @param listbox Live ListBox control; invalid handles are ignored.
void rt_listbox_clear_virtual_model(void *listbox) {
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control =
        static_cast<vg_listbox_t *>(rt_gui_widget_checked_for_binding(listbox, VG_WIDGET_LISTBOX));
    if (control)
        vg_listbox_clear_virtual_model(control);
#else
    (void)listbox;
#endif
}

/// @brief Return the first provider-backed ListBox row in the current viewport.
/// @param listbox Live ListBox control.
/// @return First visible logical row saturated to `INT64_MAX`, or zero when unavailable.
int64_t rt_listbox_get_visible_first(void *listbox) {
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control =
        static_cast<vg_listbox_t *>(rt_gui_widget_checked_for_binding(listbox, VG_WIDGET_LISTBOX));
    size_t value = control ? vg_listbox_get_visible_first(control) : 0u;
    return value > static_cast<size_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(value);
#else
    (void)listbox;
    return 0;
#endif
}

/// @brief Return the number of provider-backed ListBox rows materialized for its viewport.
/// @param listbox Live ListBox control.
/// @return Visible materialized row count saturated to `INT64_MAX`, or zero when unavailable.
int64_t rt_listbox_get_visible_count(void *listbox) {
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control =
        static_cast<vg_listbox_t *>(rt_gui_widget_checked_for_binding(listbox, VG_WIDGET_LISTBOX));
    size_t value = control ? vg_listbox_get_visible_count(control) : 0u;
    return value > static_cast<size_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(value);
#else
    (void)listbox;
    return 0;
#endif
}

/// @brief Calculate the overscanned logical row range for a vertical scroll position.
/// @param list Managed VirtualList handle.
/// @param scroll_y Non-negative scroll offset; negative values clamp to zero.
/// @return New map containing half-open `start`, `end`, and `count` values.
void *rt_virtual_list_visible_range(void *list, int64_t scroll_y) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireList(list), nullptr);
    auto &s = *h->state;
    if (scroll_y < 0)
        scroll_y = 0;
    int64_t first = scroll_y / s.rowHeight;
    first = std::max<int64_t>(0, first - s.overscan);
    int64_t visible =
        saturatingAddI64(ceilDivPositiveI64(s.viewportHeight, s.rowHeight), s.overscan * 2);
    int64_t end = std::min<int64_t>(s.rowCount, saturatingAddI64(first, visible));
    void *map = rt_map_new();
    if (!map)
        return nullptr;
    rt_map_set_int(map, rt_const_cstr("start"), first);
    rt_map_set_int(map, rt_const_cstr("end"), end);
    rt_map_set_int(map, rt_const_cstr("count"), std::max<int64_t>(0, end - first));
    return map;
}

/// @brief Select a virtual-list row by stable identifier.
/// @param list Managed VirtualList handle.
/// @param id Stable identifier to select; unresolved values clear bound control selection.
void rt_virtual_list_select_id(void *list, rt_string id) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireList(list));
    try {
        h->state->selectedId = toStd(id);
#ifdef ZANNA_ENABLE_GRAPHICS
        if (h->state->boundList) {
            int64_t row = -1;
            if (findListRowById(*h->state, h->state->selectedId, &row))
                vg_listbox_select_index(static_cast<vg_listbox_t *>(h->state->boundList),
                                        static_cast<size_t>(row));
            else
                vg_listbox_select_index(static_cast<vg_listbox_t *>(h->state->boundList), SIZE_MAX);
        }
#endif
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Return the selected virtual-list stable identifier.
/// @param list Managed VirtualList handle.
/// @return Newly referenced stable identifier, possibly empty.
rt_string rt_virtual_list_get_selected_id(void *list) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireList(list), nullptr);
    syncVirtualListSelectionFromControl(*h->state);
    return makeString(h->state->selectedId);
}

/// @brief Resolve the selected stable identifier to a logical row.
/// @param list Managed VirtualList handle.
/// @return Zero-based selected row, or `-1` when unresolved.
int64_t rt_virtual_list_get_selected_index(void *list) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireList(list), -1);
    syncVirtualListSelectionFromControl(*h->state);
    int64_t row = -1;
    return findListRowById(*h->state, h->state->selectedId, &row) ? row : -1;
}

/// @brief Create an empty managed virtual-tree model with an internal root.
/// @return New VirtualTree handle, or `NULL` on allocation failure.
void *rt_virtual_tree_new(void) {
    auto *h = static_cast<VirtualTreeHandle *>(
        rt_obj_new_i64(RT_GUI_VIRTUAL_TREE_CLASS_ID, sizeof(VirtualTreeHandle)));
    if (!h)
        return nullptr;
    h->state = new (std::nothrow) VirtualTreeState();
    if (!h->state) {
        releaseObject(h);
        return nullptr;
    }
    try {
        h->state->nodes.emplace("", TreeNode{"", "", "", {}, true, true, true, "", "", 0, false});
    } catch (const std::bad_alloc &) {
        delete h->state;
        h->state = nullptr;
        releaseObject(h);
        return nullptr;
    }
    rt_obj_set_finalizer(h, treeFinalizer);
    return h;
}

/// @brief Defer bound-tree projection work across a nested mutation batch.
void rt_virtual_tree_begin_update(void *tree) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
    if (h->state->updateDepth == UINT32_MAX) {
        rt_trap("GUI.VirtualTree: update nesting overflow");
        return;
    }
    h->state->updateDepth++;
}

/// @brief Close one mutation batch and synchronize once at the outer boundary.
void rt_virtual_tree_end_update(void *tree) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
    VirtualTreeState &state = *h->state;
    if (state.updateDepth == 0) {
        rt_trap("GUI.VirtualTree: EndUpdate without BeginUpdate");
        return;
    }
    state.updateDepth--;
    if (state.updateDepth == 0 && state.syncPending)
        syncBoundVirtualTree(state);
}

/// @brief Declare a uniquely identified virtual-tree node beneath a parent.
/// @param tree Managed VirtualTree handle.
/// @param parent_id Stable parent identifier; missing parents become placeholders.
/// @param id_s Non-empty unique node identifier.
/// @param text Runtime display label normalized for GUI rendering.
void rt_virtual_tree_add_node(void *tree, rt_string parent_id, rt_string id_s, rt_string text) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
    try {
        std::string parent = toStd(parent_id);
        std::string id = toStd(id_s);
        std::string node_text = toGuiTextStd(text);
        if (id.empty()) {
            rt_trap("GUI model ID must not be empty");
            return;
        }
        if (id == parent)
            return;
        auto &state = *h->state;
        auto existing = state.nodes.find(id);
        if (existing != state.nodes.end() && existing->second.declared) {
            trapDuplicateModelId(id);
            return;
        }
        if (virtualTreeWouldCycle(state, id, parent))
            return;

        bool declares_placeholder = existing != state.nodes.end();
        TreeNode pending_node;
        std::string pending_child_link;
        std::string placeholder_parent_value;
        std::string placeholder_old_parent;
        if (declares_placeholder) {
            placeholder_parent_value = parent;
            placeholder_old_parent = existing->second.parent;
            if (placeholder_old_parent != parent)
                pending_child_link = id;
        } else {
            pending_node =
                TreeNode{id, node_text, parent, {}, false, false, true, "", "", 0, false};
            pending_child_link = id;
        }

        bool inserted_parent = false;
        bool needs_parent = state.nodes.find(parent) == state.nodes.end();
        // Grow geometrically, never per insert: libc++ rehash(n) also
        // shrinks, so a reserve(size+1) here oscillates bucket counts and
        // turns bulk declaration quadratic (~800x at 65k nodes).
        if (state.nodes.bucket_count() > 0 &&
            static_cast<float>(state.nodes.size() + 2u) >
                state.nodes.max_load_factor() * static_cast<float>(state.nodes.bucket_count()))
            state.nodes.reserve(state.nodes.size() * 2u + 2u);
        if (needs_parent) {
            TreeNode placeholder{parent, parent, "", {}, false, false, false, "", "", 0, false};
            std::string root_link = parent;
            auto parent_result = state.nodes.emplace(parent, std::move(placeholder));
            if (!parent_result.second)
                return;
            try {
                state.nodes.find("")->second.children.push_back(std::move(root_link));
            } catch (const std::bad_alloc &) {
                state.nodes.erase(parent_result.first);
                return;
            }
            inserted_parent = true;
        }

        auto parent_it = state.nodes.find(parent);
        existing = state.nodes.find(id);
        if (existing == state.nodes.end()) {
            auto node_result = state.nodes.emplace(id, std::move(pending_node));
            if (!node_result.second) {
                if (inserted_parent) {
                    auto &roots = state.nodes.find("")->second.children;
                    if (!roots.empty() && roots.back() == parent)
                        roots.pop_back();
                    state.nodes.erase(parent);
                }
                return;
            }
            parent_it = state.nodes.find(parent);
            try {
                parent_it->second.children.push_back(std::move(pending_child_link));
            } catch (const std::bad_alloc &) {
                state.nodes.erase(node_result.first);
                if (inserted_parent) {
                    auto &roots = state.nodes.find("")->second.children;
                    if (!roots.empty() && roots.back() == parent)
                        roots.pop_back();
                    state.nodes.erase(parent);
                }
                return;
            }
        } else {
            if (placeholder_old_parent != parent) {
                try {
                    parent_it->second.children.push_back(std::move(pending_child_link));
                } catch (const std::bad_alloc &) {
                    if (inserted_parent) {
                        auto &roots = state.nodes.find("")->second.children;
                        if (!roots.empty() && roots.back() == parent)
                            roots.pop_back();
                        state.nodes.erase(parent);
                    }
                    return;
                }
            }
            existing->second.text.swap(node_text);
            existing->second.parent.swap(placeholder_parent_value);
            existing->second.declared = true;
            if (placeholder_old_parent != parent)
                eraseChildId(state, placeholder_old_parent, id);
        }

        parent_it = state.nodes.find(parent);
        if (parent_it != state.nodes.end())
            parent_it->second.loaded = true;
        state.visibleDirty = true;
        syncBoundVirtualTree(state);
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Move an existing declared node beneath another existing node without changing its ID.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable identifier of the node to move.
/// @param parent_id Stable identifier of the new parent.
/// @return `1` on success or no-op same-parent movement; otherwise `0`.
int8_t rt_virtual_tree_move_node(void *tree, rt_string id_s, rt_string parent_id) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), 0);
    try {
        std::string id = toStd(id_s);
        std::string parent = toStd(parent_id);
        VirtualTreeState &state = *h->state;
        auto node = state.nodes.find(id);
        auto new_parent = state.nodes.find(parent);
        if (id.empty() || id == parent || node == state.nodes.end() || !node->second.declared ||
            new_parent == state.nodes.end() || virtualTreeWouldCycle(state, id, parent))
            return 0;
        if (node->second.parent == parent)
            return 1;

        std::string child_link = id;
        std::string parent_value = parent;
        new_parent->second.children.push_back(std::move(child_link));
        std::string old_parent = node->second.parent;
        node->second.parent.swap(parent_value);
        eraseChildId(state, old_parent, id);
        new_parent->second.loaded = true;
        state.visibleDirty = true;
        syncBoundVirtualTree(state);
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

/// @brief Replace one declared virtual-tree node label atomically.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
/// @param text Replacement runtime display label.
/// @return `1` when the declared node exists and the operation succeeds; otherwise `0`.
int8_t rt_virtual_tree_set_node_text(void *tree, rt_string id_s, rt_string text) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), 0);
    try {
        std::string id = toStd(id_s);
        std::string value = toGuiTextStd(text);
        auto node = h->state->nodes.find(id);
        if (node == h->state->nodes.end() || !node->second.declared)
            return 0;
        if (node->second.text != value) {
            node->second.text.swap(value);
            syncBoundVirtualTree(*h->state);
        }
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

/// @brief Expand a virtual-tree node and report lazy-population state.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
/// @return New map describing lookup, expansion, and population status.
void *rt_virtual_tree_expand(void *tree, rt_string id_s) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), nullptr);
    void *map = rt_map_new();
    if (!map)
        return nullptr;
    try {
        std::string id = toStd(id_s);
        auto it = h->state->nodes.find(id);
        if (it == h->state->nodes.end()) {
            rt_map_set_bool(map, rt_const_cstr("found"), 0);
            rt_map_set_bool(map, rt_const_cstr("needsPopulate"), 1);
            return map;
        }
        if (!it->second.expanded) {
            it->second.expanded = true;
            h->state->visibleDirty = true;
            syncBoundVirtualTree(*h->state);
        }
        rt_map_set_bool(map, rt_const_cstr("found"), 1);
        rt_map_set_bool(map, rt_const_cstr("expanded"), 1);
        rt_map_set_bool(map,
                        rt_const_cstr("needsPopulate"),
                        (!it->second.loaded && it->second.children.empty()) ? 1 : 0);
    } catch (const std::bad_alloc &) {
        rt_map_set_bool(map, rt_const_cstr("found"), 0);
        rt_map_set_bool(map, rt_const_cstr("needsPopulate"), 1);
    }
    return map;
}

/// @brief Collapse a virtual-tree node if it exists.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
void rt_virtual_tree_collapse(void *tree, rt_string id_s) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
    try {
        auto it = h->state->nodes.find(toStd(id_s));
        if (it != h->state->nodes.end() && it->second.expanded) {
            it->second.expanded = false;
            h->state->visibleDirty = true;
            syncBoundVirtualTree(*h->state);
        }
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Select a virtual-tree node by stable identifier.
/// @param tree Managed VirtualTree handle.
/// @param id Stable identifier to store and project.
void rt_virtual_tree_select_id(void *tree, rt_string id) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
    try {
        std::string next = toStd(id);
        if (next.empty())
            setVirtualTreeSelection(*h->state, {});
        else
            setVirtualTreeSelection(*h->state, {std::move(next)});
        syncBoundVirtualTree(*h->state);
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Return the selected virtual-tree stable identifier.
/// @param tree Managed VirtualTree handle.
/// @return Newly referenced stable identifier, possibly empty.
rt_string rt_virtual_tree_get_selected_id(void *tree) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), nullptr);
    return makeString(h->state->selectedId);
}

/// @brief Materialize every currently visible virtual-tree row.
/// @param tree Managed VirtualTree handle.
/// @return New owned sequence of row maps.
void *rt_virtual_tree_visible_rows(void *tree) {
    return rt_virtual_tree_visible_rows_range(tree, 0, INT64_MAX);
}

/// @brief Materialize only the requested slice of the cached flattened tree order.
/// @param tree Managed VirtualTree handle.
/// @param first Non-negative first visible-row index.
/// @param count Maximum rows to materialize.
/// @return New owned sequence of row maps, possibly empty.
void *rt_virtual_tree_visible_rows_range(void *tree, int64_t first, int64_t count) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), nullptr);
    void *rows = rt_seq_new_owned();
    if (!rows)
        return nullptr;
    if (first < 0)
        first = 0;
    if (count < 0)
        count = 0;
    if (!ensureVisibleTreeIndex(*h->state))
        return rows;
    size_t start = static_cast<uint64_t>(first) > SIZE_MAX ? SIZE_MAX : static_cast<size_t>(first);
    size_t requested =
        static_cast<uint64_t>(count) > SIZE_MAX ? SIZE_MAX : static_cast<size_t>(count);
    if (start >= h->state->visibleRows.size())
        return rows;
    size_t available = h->state->visibleRows.size() - start;
    size_t length = requested < available ? requested : available;
    for (size_t offset = 0; offset < length; offset++) {
        void *map = visibleTreeRowToMap(*h->state, h->state->visibleRows[start + offset]);
        if (!map)
            break;
        rt_seq_push(rows, map);
        releaseObject(map);
    }
    return rows;
}

/// @brief Discard a node's descendants and mark it for lazy repopulation.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable identifier of the retained subtree root.
void rt_virtual_tree_refresh_subtree(void *tree, rt_string id_s) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
    try {
        std::string id = toStd(id_s);
        auto it = h->state->nodes.find(id);
        if (it == h->state->nodes.end())
            return;
        removeVirtualTreeDescendants(*h->state, it->first);
        pruneVirtualTreeSelection(*h->state);
        it = h->state->nodes.find(id);
        if (it == h->state->nodes.end())
            return;
        it->second.loaded = false;
        h->state->visibleDirty = true;
        syncBoundVirtualTree(*h->state);
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Bind a VirtualTree to a live TreeView without constructing retained nodes.
/// @param tree Managed VirtualTree handle.
/// @param treeview Live TreeView control.
/// @return `1` when binding succeeds; otherwise `0`.
int8_t rt_virtual_tree_bind(void *tree, void *treeview) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), 0);
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control = static_cast<vg_treeview_t *>(
        rt_gui_widget_checked_for_binding(treeview, VG_WIDGET_TREEVIEW));
    if (!control || !ensureVisibleTreeIndex(*h->state))
        return 0;
    VirtualTreeState &state = *h->state;
    void *old_control = state.boundTree;
    if (!vg_treeview_bind_virtual_model(control,
                                        state.visibleRows.size(),
                                        virtualTreeProvider,
                                        virtualTreeAction,
                                        &state,
                                        virtualTreeUnbound))
        return 0;
    state.boundTree = control;
    if (old_control && old_control != control)
        vg_treeview_clear_virtual_model(static_cast<vg_treeview_t *>(old_control));
    auto selected = state.visibleIndexById.find(state.selectedId);
    if (selected != state.visibleIndexById.end())
        vg_treeview_select_virtual_index(control, selected->second);
    return 1;
#else
    (void)treeview;
    return 0;
#endif
}

/// @brief Detach a VirtualTree from its current TreeView.
/// @param tree Managed VirtualTree handle.
void rt_virtual_tree_unbind(void *tree) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
    detachVirtualTree(*h->state);
}

/// @brief Reverse-form convenience wrapper for `VirtualTree.Bind`.
/// @param treeview Live TreeView control.
/// @param model Managed VirtualTree handle.
/// @return `1` when binding succeeds; otherwise `0`.
int8_t rt_treeview_set_virtual_model(void *treeview, void *model) {
    return rt_virtual_tree_bind(model, treeview);
}

/// @brief Clear a TreeView's external model binding without destroying either object.
/// @param treeview Live TreeView control; invalid handles are ignored.
void rt_treeview_clear_virtual_model(void *treeview) {
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control = static_cast<vg_treeview_t *>(
        rt_gui_widget_checked_for_binding(treeview, VG_WIDGET_TREEVIEW));
    if (control)
        vg_treeview_clear_virtual_model(control);
#else
    (void)treeview;
#endif
}

/// @brief Remove one virtual-tree node together with its whole subtree.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
/// @return `1` when the node existed and was removed; otherwise `0`.
int8_t rt_virtual_tree_remove_node(void *tree, rt_string id_s) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), 0);
    try {
        std::string id = toStd(id_s);
        auto it = h->state->nodes.find(id);
        if (it == h->state->nodes.end())
            return 0;
        removeVirtualTreeDescendants(*h->state, it->first);
        it = h->state->nodes.find(id);
        if (it != h->state->nodes.end()) {
            eraseChildId(*h->state, it->second.parent, id);
            h->state->nodes.erase(it);
        }
        pruneVirtualTreeSelection(*h->state);
        h->state->visibleDirty = true;
        syncBoundVirtualTree(*h->state);
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

/// @brief Replace the model's ordered multi-selection.
/// @param tree Managed VirtualTree handle.
/// @param ids_seq Borrowed sequence of stable identifiers; the first is primary.
void rt_virtual_tree_select_ids(void *tree, void *ids_seq) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
    try {
        std::vector<std::string> ids;
        int64_t count = ids_seq ? rt_seq_len(ids_seq) : 0;
        for (int64_t i = 0; i < count; ++i) {
            rt_string entry = rt_seq_get_str(ids_seq, i);
            if (!entry)
                continue;
            std::string id = toStd(entry);
            rt_string_unref(entry);
            if (!id.empty())
                ids.push_back(std::move(id));
        }
        setVirtualTreeSelection(*h->state, std::move(ids));
        syncBoundVirtualTree(*h->state);
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Snapshot the model's ordered multi-selection.
/// @param tree Managed VirtualTree handle.
/// @return New owned sequence of stable identifiers; primary first.
void *rt_virtual_tree_get_selected_ids(void *tree) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), nullptr);
    void *seq = rt_seq_new_owned();
    if (!seq)
        return nullptr;
    for (const auto &entry : h->state->selectedIds) {
        rt_string value = makeString(entry);
        if (!value)
            continue;
        rt_seq_push(seq, value);
        rt_string_unref(value);
    }
    return seq;
}

/// @brief Set one node's projected icon from a spec string.
/// @details Specs mirror retained TreeView icons: a `vector:<name>` prefix selects a
///          named scalable icon (an optional `|expanded` variant suffix is accepted and
///          ignored — virtual rows do not switch icons on expansion); any other
///          non-empty text is a literal glyph; empty clears both.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
/// @param spec_s Icon specification.
/// @return `1` when the node existed; otherwise `0`.
int8_t rt_virtual_tree_set_node_icon(void *tree, rt_string id_s, rt_string spec_s) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), 0);
    try {
        auto it = h->state->nodes.find(toStd(id_s));
        if (it == h->state->nodes.end())
            return 0;
        std::string spec = toStd(spec_s);
        it->second.iconVector.clear();
        it->second.iconText.clear();
        const char vectorPrefix[] = "vector:";
        if (spec.compare(0, sizeof(vectorPrefix) - 1, vectorPrefix) == 0) {
            std::string name = spec.substr(sizeof(vectorPrefix) - 1);
            size_t variant = name.find('|');
            if (variant != std::string::npos)
                name.resize(variant);
            it->second.iconVector = std::move(name);
        } else {
            it->second.iconText = std::move(spec);
        }
        syncBoundVirtualTree(*h->state);
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

/// @brief Set one node's row text color and dim state.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
/// @param rgb Packed 0xRRGGBB text color; 0 restores the theme color.
/// @param dim Non-zero blends the row toward the background.
/// @return `1` when the node existed; otherwise `0`.
int8_t rt_virtual_tree_set_node_style(void *tree, rt_string id_s, int64_t rgb, int8_t dim) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), 0);
    try {
        auto it = h->state->nodes.find(toStd(id_s));
        if (it == h->state->nodes.end())
            return 0;
        it->second.color = static_cast<uint32_t>(rgb & 0xffffff);
        it->second.dim = dim != 0;
        syncBoundVirtualTree(*h->state);
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

/// @brief Consume the bound TreeView's latched virtual drop as stable identifiers.
/// @details Row indices are resolved against the model's current flattened order, so the
///          caller must consume the drop before mutating the model. Positions project as
///          `before`/`into`/`after`.
/// @param tree Managed VirtualTree handle.
/// @return New owned map with `source`, `target`, and `position` keys, or an empty map
///         when nothing is latched or a latched index no longer resolves.
void *rt_virtual_tree_take_drop_action(void *tree) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), nullptr);
    void *map = rt_map_new();
    if (!map)
        return nullptr;
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control = static_cast<vg_treeview_t *>(h->state->boundTree);
    if (!control)
        return map;
    size_t source = SIZE_MAX;
    size_t target = SIZE_MAX;
    vg_tree_drop_position_t position = VG_TREE_DROP_INTO;
    if (!vg_treeview_take_virtual_drop(control, &source, &target, &position))
        return map;
    if (!ensureVisibleTreeIndex(*h->state) || source >= h->state->visibleRows.size() ||
        target >= h->state->visibleRows.size())
        return map;
    try {
        const char *positionText = position == VG_TREE_DROP_BEFORE
                                       ? "before"
                                       : (position == VG_TREE_DROP_AFTER ? "after" : "into");
        mapSetStr(map, "source", h->state->visibleRows[source].id);
        mapSetStr(map, "target", h->state->visibleRows[target].id);
        mapSetStr(map, "position", positionText);
    } catch (const std::bad_alloc &) {
        return map;
    }
#endif
    return map;
}

/// @brief Scroll the bound TreeView so one node's row is visible.
/// @details Hidden rows (collapsed ancestors) and unbound models are ignored.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
void rt_virtual_tree_reveal_id(void *tree, rt_string id_s) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireTree(tree));
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control = static_cast<vg_treeview_t *>(h->state->boundTree);
    if (!control || !ensureVisibleTreeIndex(*h->state))
        return;
    try {
        auto row = h->state->visibleIndexById.find(toStd(id_s));
        if (row == h->state->visibleIndexById.end())
            return;
        vg_treeview_reveal_virtual_index(control, row->second);
    } catch (const std::bad_alloc &) {
        return;
    }
#else
    (void)id_s;
#endif
}

/// @brief Begin an inline edit over one node's visible row in the bound TreeView.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
/// @param text_s Initial editor contents.
/// @return `1` when the editor was placed; `0` for hidden rows or unbound models.
int8_t rt_virtual_tree_begin_edit(void *tree, rt_string id_s, rt_string text_s) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), 0);
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control = static_cast<vg_treeview_t *>(h->state->boundTree);
    if (!control || !ensureVisibleTreeIndex(*h->state))
        return 0;
    try {
        auto row = h->state->visibleIndexById.find(toStd(id_s));
        if (row == h->state->visibleIndexById.end())
            return 0;
        std::string text = toStd(text_s);
        return vg_treeview_begin_virtual_edit(control, row->second, text.c_str()) ? 1 : 0;
    } catch (const std::bad_alloc &) {
        return 0;
    }
#else
    (void)id_s;
    (void)text_s;
    return 0;
#endif
}

/// @brief Declare whether one node's children are fully populated.
/// @details Eagerly built models mark leaves loaded so rows without declared
///          children render as true leaves instead of lazy expandables.
/// @param tree Managed VirtualTree handle.
/// @param id_s Stable node identifier.
/// @param loaded Non-zero marks the node's child list as complete.
/// @return `1` when the node existed; otherwise `0`.
int8_t rt_virtual_tree_set_node_loaded(void *tree, rt_string id_s, int8_t loaded) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), 0);
    try {
        auto it = h->state->nodes.find(toStd(id_s));
        if (it == h->state->nodes.end())
            return 0;
        if (it->second.loaded != (loaded != 0)) {
            it->second.loaded = loaded != 0;
            syncBoundVirtualTree(*h->state);
        }
        return 1;
    } catch (const std::bad_alloc &) {
        return 0;
    }
}

/// @brief Resolve the stable id of the visible row under a window-space point.
/// @param tree Managed VirtualTree handle.
/// @param x Window-space horizontal coordinate.
/// @param y Window-space vertical coordinate.
/// @return New owned id string, empty when the point misses every row.
rt_string rt_virtual_tree_row_id_at(void *tree, int64_t x, int64_t y) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), nullptr);
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control = static_cast<vg_treeview_t *>(h->state->boundTree);
    size_t index = SIZE_MAX;
    if (!control || !vg_treeview_virtual_index_at(control, (float)x, (float)y, &index) ||
        !ensureVisibleTreeIndex(*h->state) || index >= h->state->visibleRows.size())
        return rt_str_empty();
    return makeString(h->state->visibleRows[index].id);
#else
    (void)x;
    (void)y;
    return rt_str_empty();
#endif
}

/// @brief Consume the bound TreeView's latched inline-edit commit as a stable id.
/// @details Returns an empty map when no virtual edit commit is pending. The map carries
///          `id` and `text` keys; the edge is consumed on read.
/// @param tree Managed VirtualTree handle.
/// @return New owned map, possibly empty.
void *rt_virtual_tree_take_edit_commit(void *tree) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireTree(tree), nullptr);
    void *map = rt_map_new();
    if (!map)
        return nullptr;
#ifdef ZANNA_ENABLE_GRAPHICS
    auto *control = static_cast<vg_treeview_t *>(h->state->boundTree);
    if (!control)
        return map;
    if (!vg_treeview_was_edit_committed(control))
        return map;
    size_t row = vg_treeview_get_edited_virtual_index(control);
    const char *text = vg_treeview_get_edit_text(control);
    if (!ensureVisibleTreeIndex(*h->state) || row >= h->state->visibleRows.size())
        return map;
    try {
        mapSetStr(map, "id", h->state->visibleRows[row].id);
        mapSetStr(map, "text", text ? text : "");
    } catch (const std::bad_alloc &) {
        return map;
    }
#endif
    return map;
}

/// @brief Create a managed command-state record for accessibility and UI state snapshots.
/// @param id Stable command identifier.
/// @param label Display label, also used as the initial accessible label.
/// @return New CommandState handle, or `NULL` on allocation failure.
void *rt_command_state_new(rt_string id, rt_string label) {
    auto *h = static_cast<CommandStateHandle *>(
        rt_obj_new_i64(RT_GUI_COMMAND_STATE_CLASS_ID, sizeof(CommandStateHandle)));
    if (!h)
        return nullptr;
    h->state = new (std::nothrow) CommandState();
    if (!h->state) {
        releaseObject(h);
        return nullptr;
    }
    try {
        h->state->id = toStd(id);
        h->state->label = toStd(label);
        h->state->accessibleLabel = h->state->label;
    } catch (const std::bad_alloc &) {
        delete h->state;
        h->state = nullptr;
        releaseObject(h);
        return nullptr;
    }
    rt_obj_set_finalizer(h, commandStateFinalizer);
    return h;
}

/// @brief Set a command state's enabled flag.
/// @param state Managed CommandState handle.
/// @param enabled Non-zero to enable the command.
void rt_command_state_set_enabled(void *state, int8_t enabled) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommandState(state));
    h->state->enabled = enabled != 0;
}

/// @brief Query a command state's enabled flag.
/// @param state Managed CommandState handle.
/// @return `1` when enabled; otherwise `0`.
int8_t rt_command_state_get_enabled(void *state) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommandState(state), 0);
    return h->state->enabled ? 1 : 0;
}

/// @brief Set a command state's checked flag.
/// @param state Managed CommandState handle.
/// @param checked Non-zero to mark the command checked.
void rt_command_state_set_checked(void *state, int8_t checked) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommandState(state));
    h->state->checked = checked != 0;
}

/// @brief Query a command state's checked flag.
/// @param state Managed CommandState handle.
/// @return `1` when checked; otherwise `0`.
int8_t rt_command_state_get_checked(void *state) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommandState(state), 0);
    return h->state->checked ? 1 : 0;
}

/// @brief Replace a command state's accessible label and description atomically.
/// @param state Managed CommandState handle.
/// @param label Accessible label.
/// @param description Accessible description.
void rt_command_state_set_accessible(void *state, rt_string label, rt_string description) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommandState(state));
    auto *s = h->state;
    try {
        s->accessibleLabel = toStd(label);
        s->accessibleDescription = toStd(description);
    } catch (const std::bad_alloc &) {
        return;
    }
}

/// @brief Snapshot all command-state fields into a managed map.
/// @param state Managed CommandState handle.
/// @return New state map, or `NULL` on allocation failure.
void *rt_command_state_snapshot(void *state) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommandState(state), nullptr);
    auto *s = h->state;
    void *map = rt_map_new();
    if (!map)
        return nullptr;
    mapSetStr(map, "id", s->id);
    mapSetStr(map, "label", s->label);
    mapSetStr(map, "accessibleLabel", s->accessibleLabel);
    mapSetStr(map, "accessibleDescription", s->accessibleDescription);
    rt_map_set_bool(map, rt_const_cstr("enabled"), s->enabled ? 1 : 0);
    rt_map_set_bool(map, rt_const_cstr("checked"), s->checked ? 1 : 0);
    return map;
}

/// @brief Compute the WCAG contrast ratio between two packed RGB colors.
/// @param fg_rgb Foreground `0xRRGGBB` color.
/// @param bg_rgb Background `0xRRGGBB` color.
/// @return Contrast ratio from 1.0 through 21.0.
double rt_accessibility_contrast_ratio(int64_t fg_rgb, int64_t bg_rgb) {
    double a = luminance(fg_rgb);
    double b = luminance(bg_rgb);
    if (a < b)
        std::swap(a, b);
    return (a + 0.05) / (b + 0.05);
}

/// @brief Test two colors against a requested minimum contrast ratio.
/// @param fg_rgb Foreground `0xRRGGBB` color.
/// @param bg_rgb Background `0xRRGGBB` color.
/// @param min_ratio Required ratio; invalid values use 4.5.
/// @return `1` when the computed ratio meets the threshold; otherwise `0`.
int8_t rt_accessibility_meets_contrast(int64_t fg_rgb, int64_t bg_rgb, double min_ratio) {
    if (!std::isfinite(min_ratio) || min_ratio <= 0.0)
        min_ratio = 4.5;
    return rt_accessibility_contrast_ratio(fg_rgb, bg_rgb) >= min_ratio ? 1 : 0;
}

/// @brief Return the built-in high-contrast GUI color-token palette.
/// @return New managed map of packed RGB tokens, or `NULL` on allocation failure.
void *rt_accessibility_high_contrast_tokens(void) {
    void *map = rt_map_new();
    if (!map)
        return nullptr;
    rt_map_set_int(map, rt_const_cstr("background"), 0x000000);
    rt_map_set_int(map, rt_const_cstr("foreground"), 0xffffff);
    rt_map_set_int(map, rt_const_cstr("accent"), 0x00d7ff);
    rt_map_set_int(map, rt_const_cstr("warning"), 0xffd75f);
    rt_map_set_int(map, rt_const_cstr("error"), 0xff5f5f);
    return map;
}

/// @brief Create a managed command with stable identity and display title.
/// @param id Stable command identifier.
/// @param title Runtime display title.
/// @return New Command handle, or `NULL` on allocation failure.
void *rt_command_new(rt_string id, rt_string title) {
    auto *h = static_cast<CommandHandle *>(
        rt_obj_new_i64(RT_GUI_COMMAND_CLASS_ID, sizeof(CommandHandle)));
    if (!h)
        return nullptr;
    h->state = new (std::nothrow) CommandData();
    if (!h->state) {
        releaseObject(h);
        return nullptr;
    }
    try {
        h->state->id = toStd(id);
        h->state->title = toStd(title);
    } catch (const std::bad_alloc &) {
        delete h->state;
        h->state = nullptr;
        releaseObject(h);
        return nullptr;
    }
    rt_obj_set_finalizer(h, commandFinalizer);
    return h;
}

/// @brief Return a command's stable identifier.
/// @param command Managed Command handle.
/// @return Newly referenced identifier string.
rt_string rt_command_get_id(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), nullptr);
    return makeString(h->state->id);
}

/// @brief Return a command's display title.
/// @param command Managed Command handle.
/// @return Newly referenced title string.
rt_string rt_command_get_title(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), nullptr);
    return makeString(h->state->title);
}

/// @brief Set and best-effort register a command keyboard shortcut.
/// @param command Managed Command handle.
/// @param keys Runtime shortcut chord specification.
void rt_command_set_shortcut(void *command, rt_string keys) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommand(command));
    try {
        h->state->shortcut = toStd(keys);
    } catch (const std::bad_alloc &) {
        return;
    }
    // Best-effort: register the chord with the global Shortcuts registry under the command id,
    // so Poll() can detect it via rt_shortcuts_was_triggered(id). A no-op until a GUI app exists.
    if (!h->state->id.empty() && !h->state->shortcut.empty()) {
        rt_string idStr = makeString(h->state->id);
        rt_string keysStr = makeString(h->state->shortcut);
        rt_string titleStr = makeString(h->state->title);
        if (idStr && keysStr)
            rt_shortcuts_register(idStr, keysStr, titleStr);
        if (idStr)
            rt_string_unref(idStr);
        if (keysStr)
            rt_string_unref(keysStr);
        if (titleStr)
            rt_string_unref(titleStr);
    }
}

/// @brief Return a command's configured shortcut.
/// @param command Managed Command handle.
/// @return Newly referenced shortcut string, possibly empty.
rt_string rt_command_get_shortcut(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), nullptr);
    return makeString(h->state->shortcut);
}

/// @brief Set command availability and project it to bound controls.
/// @param command Managed Command handle.
/// @param enabled Non-zero to enable invocation.
void rt_command_set_enabled(void *command, int8_t enabled) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommand(command));
    h->state->enabled = enabled != 0;
    pushCommandState(*h->state);
}

/// @brief Query whether a command is enabled.
/// @param command Managed Command handle.
/// @return `1` when enabled; otherwise `0`.
int8_t rt_command_is_enabled(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), 0);
    return h->state->enabled ? 1 : 0;
}

/// @brief Configure whether a command exposes checked state.
/// @param command Managed Command handle.
/// @param checkable Non-zero to make the command checkable.
void rt_command_set_checkable(void *command, int8_t checkable) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommand(command));
    h->state->checkable = checkable != 0;
    pushCommandState(*h->state);
}

/// @brief Query whether a command is checkable.
/// @param command Managed Command handle.
/// @return `1` when checkable; otherwise `0`.
int8_t rt_command_is_checkable(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), 0);
    return h->state->checkable ? 1 : 0;
}

/// @brief Set command checked state and project it to bound controls.
/// @param command Managed Command handle.
/// @param checked Non-zero to mark the command checked.
void rt_command_set_checked(void *command, int8_t checked) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommand(command));
    h->state->checked = checked != 0;
    pushCommandState(*h->state);
}

/// @brief Query whether a command is checked.
/// @param command Managed Command handle.
/// @return `1` when checked; otherwise `0`.
int8_t rt_command_is_checked(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), 0);
    return h->state->checked ? 1 : 0;
}

/// @brief Bind or unbind a non-owning MenuItem invocation source.
/// @param command Managed Command handle.
/// @param item MenuItem handle, or `NULL` to unbind.
void rt_command_bind_menu_item(void *command, void *item) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommand(command));
    h->state->menuItem = item; // raw handle; NULL unbinds
    pushCommandState(*h->state);
}

/// @brief Bind or unbind a non-owning ToolbarItem invocation source.
/// @param command Managed Command handle.
/// @param item ToolbarItem handle, or `NULL` to unbind.
void rt_command_bind_toolbar_item(void *command, void *item) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommand(command));
    h->state->toolbarItem = item; // raw handle; NULL unbinds
    pushCommandState(*h->state);
}

/// @brief Poll bound menu, toolbar, and shortcut sources for one command.
/// @param command Managed Command handle.
/// @return `1` when the enabled command was invoked; otherwise `0`.
int8_t rt_command_poll(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), 0);
    return pollCommandInto(*h->state, std::string()) ? 1 : 0;
}

/// @brief Return the invocation result cached by the latest poll.
/// @param command Managed Command handle.
/// @return `1` when the last poll observed invocation; otherwise `0`.
int8_t rt_command_was_invoked(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), 0);
    return h->state->invoked ? 1 : 0;
}

/// @brief Snapshot a command's identity, binding state, and latest invocation flag.
/// @param command Managed Command handle.
/// @return New managed map, or `NULL` on allocation failure.
void *rt_command_snapshot(void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommand(command), nullptr);
    auto *s = h->state;
    void *map = rt_map_new();
    if (!map)
        return nullptr;
    mapSetStr(map, "id", s->id);
    mapSetStr(map, "title", s->title);
    mapSetStr(map, "shortcut", s->shortcut);
    rt_map_set_bool(map, rt_const_cstr("enabled"), s->enabled ? 1 : 0);
    rt_map_set_bool(map, rt_const_cstr("checkable"), s->checkable ? 1 : 0);
    rt_map_set_bool(map, rt_const_cstr("checked"), s->checked ? 1 : 0);
    rt_map_set_bool(map, rt_const_cstr("invoked"), s->invoked ? 1 : 0);
    return map;
}

/// @brief Create an empty managed command registry.
/// @return New CommandRegistry handle, or `NULL` on allocation failure.
void *rt_command_registry_new(void) {
    auto *h = static_cast<CommandRegistryHandle *>(
        rt_obj_new_i64(RT_GUI_COMMAND_REGISTRY_CLASS_ID, sizeof(CommandRegistryHandle)));
    if (!h)
        return nullptr;
    h->state = new (std::nothrow) CommandRegistryData();
    if (!h->state) {
        releaseObject(h);
        return nullptr;
    }
    rt_obj_set_finalizer(h, commandRegistryFinalizer);
    return h;
}

/// @brief Add and retain one live command unless it is already registered.
/// @param registry Managed CommandRegistry handle.
/// @param command Live Command handle to co-own.
void rt_command_registry_add(void *registry, void *command) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommandRegistry(registry));
    if (!commandDataChecked(command))
        return; // not a live Command
    auto &cmds = h->state->commands;
    if (std::find(cmds.begin(), cmds.end(), command) != cmds.end())
        return; // already registered; avoid a double-retain
    try {
        cmds.push_back(command);
    } catch (const std::bad_alloc &) {
        return;
    }
    rt_obj_retain_known(command); // registry co-owns the command
}

/// @brief Return the number of retained registry commands.
/// @param registry Managed CommandRegistry handle.
/// @return Command count saturated to `INT64_MAX`.
int64_t rt_command_registry_count(void *registry) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommandRegistry(registry), 0);
    size_t n = h->state->commands.size();
    return n > static_cast<size_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(n);
}

/// @brief Find and retain a registered command by stable identifier.
/// @param registry Managed CommandRegistry handle.
/// @param id Command identifier to match.
/// @return Owned Command reference, or `NULL` when absent.
void *rt_command_registry_find(void *registry, rt_string id) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommandRegistry(registry), nullptr);
    std::string key;
    try {
        key = toStd(id);
    } catch (const std::bad_alloc &) {
        return nullptr;
    }
    for (void *cmd : h->state->commands) {
        CommandData *d = commandDataChecked(cmd);
        if (d && d->id == key) {
            rt_obj_retain_known(cmd); // return an owned reference
            return cmd;
        }
    }
    return nullptr;
}

/// @brief Return a registered command as an Option.
/// @details Wraps rt_command_registry_find(); the temporary retained command
///          reference is released after the Option retains a found command.
/// @param registry CommandRegistry object.
/// @param id Command id to find.
/// @return Zanna.Option.Some(Command) when found, otherwise Zanna.Option.None().
void *rt_command_registry_find_option(void *registry, rt_string id) {
    void *command = rt_command_registry_find(registry, id);
    if (!command)
        return rt_option_none();
    void *option = rt_option_some(command);
    releaseObject(command);
    return option;
}

/// @brief Bind or unbind a non-owning CommandPalette invocation source.
/// @param registry Managed CommandRegistry handle.
/// @param palette CommandPalette handle, or `NULL` to unbind.
void rt_command_registry_bind_palette(void *registry, void *palette) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommandRegistry(registry));
    h->state->palette = palette; // raw handle; NULL unbinds
}

/// @brief Poll every registered command and the bound palette once.
/// @param registry Managed CommandRegistry handle.
/// @return Newly referenced identifier of the first invoked command, or an empty string.
rt_string rt_command_registry_poll(void *registry) {
    RT_GUI_IDE_REQUIRE_OR_RETURN(h, requireCommandRegistry(registry), makeString(std::string()));
    // Read the palette's selection once (WasSelected is consumed on read).
    std::string paletteSel;
    if (h->state->palette && rt_commandpalette_was_command_selected(h->state->palette)) {
        rt_string s = rt_commandpalette_get_selected_command(h->state->palette);
        if (s) {
            paletteSel = toStd(s);
            rt_string_unref(s);
        }
    }
    std::string invokedId;
    for (void *cmd : h->state->commands) {
        CommandData *d = commandDataChecked(cmd);
        if (!d)
            continue;
        if (pollCommandInto(*d, paletteSel) && invokedId.empty())
            invokedId = d->id;
    }
    return makeString(invokedId);
}

/// @brief Release every command retained by a registry.
/// @param registry Managed CommandRegistry handle.
void rt_command_registry_clear(void *registry) {
    RT_GUI_IDE_REQUIRE_OR_RETURN_VOID(h, requireCommandRegistry(registry));
    for (void *cmd : h->state->commands) {
        if (cmd && rt_obj_release_known_check0(cmd))
            rt_obj_free(cmd);
    }
    h->state->commands.clear();
}

} // extern "C"
