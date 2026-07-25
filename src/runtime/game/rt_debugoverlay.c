//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_debugoverlay.c
/// @file
/// @brief Implements the FPS, frame-time, and integer-watch debug panel.
//
// Purpose: Debug overlay rendering FPS, delta time, and custom watched
//   variables as a semi-transparent panel in the top-right canvas corner.
//   Designed for use during game development; can be toggled with a single
//   key binding (e.g., F3).
//
// Key invariants:
//   - FPS is a rolling average over RT_DEBUG_FPS_HISTORY (16) frame deltas.
//   - Watch entries are stored in a flat array with linear scan (max 16).
//   - Drawing uses public rt_canvas_* APIs — no internal struct access.
//   - Disabled by default; Draw returns before issuing canvas calls when off.
//
// Ownership/Lifetime:
//   - The DebugOverlay uses the runtime object's reference count. Destroy
//     releases one reference and frees the object when it reaches zero.
//   - Watch name strings are not retained; accepted name bytes are copied into
//     fixed-length, NUL-terminated buffers.
//
// Links: src/runtime/game/rt_debugoverlay.h (public API)
//
//===----------------------------------------------------------------------===//

#include "rt_debugoverlay.h"
#include "rt_graphics.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_string.h"
#include "rt_trap.h"

#include <limits.h>
#include <string.h>

/// @brief Copy a NUL-terminated C string into a runtime string.
/// @param s Non-null, NUL-terminated source string.
/// @return A new runtime-string reference, or `NULL` if allocation fails.
static rt_string make_string(const char *s) {
    return rt_string_from_bytes(s, (int64_t)strlen(s));
}

/// @brief Maximum stored watch-name size, including the NUL terminator.
#define WATCH_NAME_MAX 32

/// @brief One fixed-capacity integer watch slot.
typedef struct {
    /// Copied, NUL-terminated display label.
    char name[WATCH_NAME_MAX];
    /// Most recently registered integer value.
    int64_t value;
    /// Nonzero while the slot participates.
    int8_t active;
} watch_entry_t;

/// @brief Private state stored behind an opaque DebugOverlay handle.
struct rt_debugoverlay_impl {
    /// Whether Draw emits the panel.
    int8_t enabled;
    int64_t frame_times[RT_DEBUG_FPS_HISTORY]; ///< Ring buffer of dt values (ms).
    int64_t frame_index;                       ///< Current ring buffer write position.
    int64_t frame_count;                       ///< Number of frames recorded (up to HISTORY).
    int64_t total_frames;                      ///< Total frames since creation.
    /// Fixed watch-slot table.
    watch_entry_t watches[RT_DEBUG_MAX_WATCHES];
    /// Number of active slots.
    int64_t watch_count;
};

/// @brief Validate an opaque DebugOverlay handle.
/// @param dbg Candidate handle; `NULL` is accepted.
/// @param api Trap message used when @p dbg has the wrong runtime class ID.
/// @return @p dbg when valid; otherwise `NULL`.
/// @details A non-null mismatched handle raises a runtime trap before this
///          function returns `NULL`.
static rt_debugoverlay checked_debugoverlay(rt_debugoverlay dbg, const char *api) {
    if (!dbg)
        return NULL;
    if (rt_obj_class_id(dbg) != RT_DEBUGOVERLAY_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return dbg;
}

/// @brief Create an empty, disabled DebugOverlay.
/// @return A newly allocated DebugOverlay reference, or `NULL` if allocation
///         fails.
/// @details Frame history and all watch slots are initialized to zero.
rt_debugoverlay rt_debugoverlay_new(void) {
    struct rt_debugoverlay_impl *dbg =
        rt_obj_new_i64(RT_DEBUGOVERLAY_CLASS_ID, sizeof(struct rt_debugoverlay_impl));
    if (!dbg)
        return NULL;

    dbg->enabled = 0;
    dbg->frame_index = 0;
    dbg->frame_count = 0;
    dbg->total_frames = 0;
    dbg->watch_count = 0;
    memset(dbg->frame_times, 0, sizeof(dbg->frame_times));
    memset(dbg->watches, 0, sizeof(dbg->watches));

    return dbg;
}

/// @brief Release one DebugOverlay reference.
/// @param dbg Handle to release; `NULL` is a no-op.
/// @details Frees the object when the reference count reaches zero. A non-null
///          handle of another runtime class raises a trap.
void rt_debugoverlay_destroy(rt_debugoverlay dbg) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Destroy: expected Zanna.Game.DebugOverlay");
    if (dbg && rt_obj_release_check0(dbg))
        rt_obj_free(dbg);
}

/// @brief Enable panel rendering.
/// @param dbg Overlay to enable; `NULL` is a no-op.
/// @details A non-null handle of another runtime class raises a trap.
void rt_debugoverlay_enable(rt_debugoverlay dbg) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Enable: expected Zanna.Game.DebugOverlay");
    if (!dbg)
        return;
    dbg->enabled = 1;
}

/// @brief Disable panel rendering.
/// @param dbg Overlay to disable; `NULL` is a no-op.
/// @details Updating frame history and watches remains possible while disabled.
///          A non-null handle of another runtime class raises a trap.
void rt_debugoverlay_disable(rt_debugoverlay dbg) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Disable: expected Zanna.Game.DebugOverlay");
    if (!dbg)
        return;
    dbg->enabled = 0;
}

/// @brief Toggle panel rendering between enabled and disabled.
/// @param dbg Overlay to toggle; `NULL` is a no-op.
/// @details A non-null handle of another runtime class raises a trap.
void rt_debugoverlay_toggle(rt_debugoverlay dbg) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Toggle: expected Zanna.Game.DebugOverlay");
    if (!dbg)
        return;
    dbg->enabled = dbg->enabled ? 0 : 1;
}

/// @brief Check whether the debug overlay is currently visible.
/// @param dbg Overlay to query.
/// @return `1` when enabled; otherwise `0`.
/// @details A non-null handle of another runtime class raises a trap.
int8_t rt_debugoverlay_is_enabled(rt_debugoverlay dbg) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.IsEnabled: expected Zanna.Game.DebugOverlay");
    return dbg ? dbg->enabled : 0;
}

/// @brief Add one frame delta to the rolling FPS history.
/// @param dbg Overlay whose history is updated; `NULL` is a no-op.
/// @param dt_ms Frame duration in milliseconds; negative values are recorded
///        as zero.
/// @details Writes the next ring-buffer slot, grows the sample count to
///          RT_DEBUG_FPS_HISTORY, and saturates the lifetime frame counter at
///          `INT64_MAX`. A non-null handle of another runtime class raises a
///          trap.
void rt_debugoverlay_update(rt_debugoverlay dbg, int64_t dt_ms) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Update: expected Zanna.Game.DebugOverlay");
    if (!dbg)
        return;
    if (dt_ms < 0)
        dt_ms = 0;

    dbg->frame_times[dbg->frame_index] = dt_ms;
    dbg->frame_index = (dbg->frame_index + 1) % RT_DEBUG_FPS_HISTORY;
    if (dbg->frame_count < RT_DEBUG_FPS_HISTORY)
        dbg->frame_count++;
    if (dbg->total_frames < INT64_MAX)
        dbg->total_frames++;
}

/// @brief Return a watch name's C string only when it fits and has no NUL.
/// @param name Runtime string to validate; no reference is retained.
/// @details Names that do not fit the fixed buffer are rejected rather than
///          truncated, so `Watch` and `Unwatch` address entries by the same full
///          name and cannot collide through a shared truncated prefix
///          (VDOC-259). An embedded NUL is also rejected.
/// @return A borrowed C-string view when valid, or `NULL` for a null, empty,
///         overlong, inaccessible, or embedded-NUL name.
static const char *watch_name_cstr(rt_string name) {
    if (!name)
        return NULL;
    const char *cname = rt_string_cstr(name);
    if (!cname)
        return NULL;
    int64_t len = rt_str_len(name);
    if (len <= 0 || (size_t)len >= WATCH_NAME_MAX || strlen(cname) != (size_t)len)
        return NULL;
    return cname;
}

/// @brief Find an active watch slot by its complete stored name.
/// @param dbg Valid DebugOverlay to search.
/// @param name Non-null, NUL-terminated name to compare.
/// @return The slot index, or `-1` when no active slot matches.
static int64_t find_watch(rt_debugoverlay dbg, const char *name) {
    for (int64_t i = 0; i < RT_DEBUG_MAX_WATCHES; i++) {
        if (dbg->watches[i].active && strcmp(dbg->watches[i].name, name) == 0)
            return i;
    }
    return -1;
}

/// @brief Register or update a named integer watch.
/// @param dbg Overlay to mutate; `NULL` is a no-op.
/// @param name Nonempty runtime string label shorter than WATCH_NAME_MAX bytes
///        and containing no embedded NUL.
/// @param value Integer to display beside the label.
/// @details An existing exact-name match is updated in place. Otherwise the
///          first inactive slot receives a byte copy of the name. Invalid
///          names and registrations beyond RT_DEBUG_MAX_WATCHES are silently
///          ignored. The input string is never retained. A non-null overlay
///          handle of another runtime class raises a trap.
void rt_debugoverlay_watch(rt_debugoverlay dbg, rt_string name, int64_t value) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Watch: expected Zanna.Game.DebugOverlay");
    if (!dbg || !name)
        return;

    // Reject a name that does not fit the fixed buffer (or contains an embedded
    // NUL) instead of truncating it, so lookup and storage use the same full name
    // (VDOC-259).
    const char *cname = watch_name_cstr(name);
    if (!cname)
        return;

    // Check if already exists — update value
    int64_t idx = find_watch(dbg, cname);
    if (idx >= 0) {
        dbg->watches[idx].value = value;
        return;
    }

    // Find an empty slot
    for (int64_t i = 0; i < RT_DEBUG_MAX_WATCHES; i++) {
        if (!dbg->watches[i].active) {
            size_t len = strlen(cname); // guaranteed < WATCH_NAME_MAX by watch_name_cstr
            memcpy(dbg->watches[i].name, cname, len);
            dbg->watches[i].name[len] = '\0';
            dbg->watches[i].value = value;
            dbg->watches[i].active = 1;
            dbg->watch_count++;
            return;
        }
    }
    // Silently ignore if all slots are full.
}

/// @brief Remove an integer watch by exact name.
/// @param dbg Overlay to mutate.
/// @param name Runtime string containing the complete registered label.
/// @return `1` when an active slot was removed; otherwise `0`.
/// @details Invalid names and null arguments return zero. Removal clears the
///          slot's active flag and first name byte, decrements the active
///          count, and does not retain or release @p name. A non-null overlay
///          handle of another runtime class raises a trap.
int8_t rt_debugoverlay_unwatch(rt_debugoverlay dbg, rt_string name) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Unwatch: expected Zanna.Game.DebugOverlay");
    if (!dbg || !name)
        return 0;

    // Use the same validation as Watch so registration and removal address
    // exactly the same complete-name domain (VDOC-259).
    const char *cname = watch_name_cstr(name);
    if (!cname)
        return 0;

    int64_t idx = find_watch(dbg, cname);
    if (idx < 0)
        return 0;

    dbg->watches[idx].active = 0;
    dbg->watches[idx].name[0] = '\0';
    dbg->watch_count--;
    return 1;
}

/// @brief Remove all registered watches.
/// @param dbg Overlay to clear; `NULL` is a no-op.
/// @details Marks every slot inactive, clears each stored name, and resets the
///          active count without changing frame history or enabled state. A
///          non-null handle of another runtime class raises a trap.
void rt_debugoverlay_clear(rt_debugoverlay dbg) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Clear: expected Zanna.Game.DebugOverlay");
    if (!dbg)
        return;
    for (int64_t i = 0; i < RT_DEBUG_MAX_WATCHES; i++) {
        dbg->watches[i].active = 0;
        dbg->watches[i].name[0] = '\0';
    }
    dbg->watch_count = 0;
}

/// @brief Return the most recently computed frames-per-second value.
/// @param dbg Overlay whose rolling history is queried.
/// @return Integer FPS computed as `1000 * sample_count / sum(dt_ms)`, or zero
///         when no samples exist, their saturated sum is nonpositive, or the
///         handle is null/invalid.
/// @details The delta sum saturates at `INT64_MAX`. Integer division truncates
///          fractional FPS. A non-null invalid handle raises a runtime trap.
int64_t rt_debugoverlay_get_fps(rt_debugoverlay dbg) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.FPS: expected Zanna.Game.DebugOverlay");
    if (!dbg || dbg->frame_count == 0)
        return 0;

    int64_t sum = 0;
    for (int64_t i = 0; i < dbg->frame_count; i++)
        sum = sum > INT64_MAX - dbg->frame_times[i] ? INT64_MAX : sum + dbg->frame_times[i];

    if (sum <= 0)
        return 0;

    // FPS = 1000 * frame_count / sum_of_dt_ms
    return (1000 * dbg->frame_count) / sum;
}

// --- Drawing ---

/// @brief Format a signed integer at the end of a caller-provided buffer.
/// @param val Integer to format, including full support for `INT64_MIN`.
/// @param buf Writable output buffer.
/// @param bufsize Size of @p buf in bytes.
/// @return A pointer within @p buf to the formatted suffix, or @p buf when
///         @p bufsize is zero.
/// @details For a nonzero size, the last byte is always NUL. If the buffer is
///          too small, leading digits and possibly the minus sign are omitted.
static char *i64_to_str(int64_t val, char *buf, size_t bufsize) {
    if (bufsize == 0)
        return buf;

    int negative = 0;
    uint64_t uval;
    if (val < 0) {
        negative = 1;
        uval = (uint64_t)(-(val + 1)) + 1;
    } else {
        uval = (uint64_t)val;
    }

    buf[bufsize - 1] = '\0';
    size_t pos = bufsize - 1;

    if (uval == 0) {
        if (pos > 0)
            buf[--pos] = '0';
    } else {
        while (uval > 0 && pos > 0) {
            buf[--pos] = '0' + (char)(uval % 10);
            uval /= 10;
        }
    }

    if (negative && pos > 0)
        buf[--pos] = '-';

    return &buf[pos];
}

/// @brief Draw the enabled overlay in the canvas's top-right corner.
/// @param dbg Overlay containing frame history and watch values.
/// @param canvas_ptr Opaque canvas handle passed to the public canvas API.
/// @details Returns without drawing for a null overlay, null canvas, or
///          disabled overlay. The panel includes color-coded integer FPS, the
///          newest delta in milliseconds, and active watches in slot order.
///          Display labels longer than 28 bytes are shortened at a UTF-8
///          continuation-byte boundary. Temporary runtime strings created for
///          text calls are released immediately. A non-null overlay handle of
///          another runtime class raises a trap.
void rt_debugoverlay_draw(rt_debugoverlay dbg, void *canvas_ptr) {
    dbg = checked_debugoverlay(dbg, "DebugOverlay.Draw: expected Zanna.Game.DebugOverlay");
    if (!dbg || !canvas_ptr || !dbg->enabled)
        return;

    // Layout constants
    const int64_t SCALE = 1;
    const int64_t LINE_H = 12;
    const int64_t PAD = 6;
    const int64_t COL_BG = 0x000000;
    const int64_t COL_LABEL = 0x888888;
    const int64_t COL_VALUE = 0x44FF44;
    const int64_t COL_FPS_GOOD = 0x44FF44;
    const int64_t COL_FPS_WARN = 0xFFDD44;
    const int64_t COL_FPS_BAD = 0xFF4444;
    const int64_t ALPHA = 180;

    // Count lines: FPS + DT + blank + watches
    int64_t num_lines = 2; // FPS + DT
    int64_t num_watches = 0;
    for (int64_t i = 0; i < RT_DEBUG_MAX_WATCHES; i++) {
        if (dbg->watches[i].active)
            num_watches++;
    }
    if (num_watches > 0)
        num_lines += 1 + num_watches; // blank line + watches

    // Panel dimensions
    int64_t panel_w = 160;
    int64_t panel_h = PAD * 2 + num_lines * LINE_H;
    int64_t canvas_w = rt_canvas_width(canvas_ptr);
    int64_t panel_x = canvas_w - panel_w - 4;
    int64_t panel_y = 4;

    // Background
    rt_canvas_box_alpha(canvas_ptr, panel_x, panel_y, panel_w, panel_h, COL_BG, ALPHA);

    int64_t tx = panel_x + PAD;
    int64_t ty = panel_y + PAD;
    char numbuf[24];

    // FPS line
    int64_t fps = rt_debugoverlay_get_fps(dbg);
    int64_t fps_col = COL_FPS_GOOD;
    if (fps < 30)
        fps_col = COL_FPS_BAD;
    else if (fps < 55)
        fps_col = COL_FPS_WARN;

    {
        char line[48];
        const char *fpsstr = i64_to_str(fps, numbuf, sizeof(numbuf));
        size_t flen = strlen(fpsstr);
        memcpy(line, "FPS: ", 5);
        if (flen > 40)
            flen = 40;
        memcpy(line + 5, fpsstr, flen);
        line[5 + flen] = '\0';
        rt_string s = make_string(line);
        rt_canvas_text_scaled(canvas_ptr, tx, ty, s, SCALE, fps_col);
        rt_string_unref(s);
        ty += LINE_H;
    }

    // DT line
    {
        char line[48];
        int64_t dt = 0;
        if (dbg->frame_count > 0) {
            int64_t prev = (dbg->frame_index - 1 + RT_DEBUG_FPS_HISTORY) % RT_DEBUG_FPS_HISTORY;
            dt = dbg->frame_times[prev];
        }
        const char *dtstr = i64_to_str(dt, numbuf, sizeof(numbuf));
        size_t dlen = strlen(dtstr);
        memcpy(line, "DT:  ", 5);
        if (dlen > 38)
            dlen = 38;
        memcpy(line + 5, dtstr, dlen);
        size_t pos = 5 + dlen;
        memcpy(line + pos, " ms", 3);
        line[pos + 3] = '\0';
        rt_string s = make_string(line);
        rt_canvas_text_scaled(canvas_ptr, tx, ty, s, SCALE, COL_LABEL);
        rt_string_unref(s);
        ty += LINE_H;
    }

    // Watch variables
    if (num_watches > 0) {
        ty += LINE_H; // blank separator

        for (int64_t i = 0; i < RT_DEBUG_MAX_WATCHES; i++) {
            if (!dbg->watches[i].active)
                continue;

            // Build "name: value" string
            char line[80];
            const char *wname = dbg->watches[i].name;
            size_t nlen = strlen(wname);
            if (nlen > 28) {
                nlen = 28;
                // Back off to a UTF-8 character boundary so the displayed name is
                // never a split multi-byte sequence (VDOC-259).
                while (nlen > 0 && ((unsigned char)wname[nlen] & 0xC0) == 0x80)
                    nlen--;
            }
            memcpy(line, wname, nlen);
            line[nlen] = ':';
            line[nlen + 1] = ' ';

            const char *valstr = i64_to_str(dbg->watches[i].value, numbuf, sizeof(numbuf));
            size_t vlen = strlen(valstr);
            if (vlen > 40)
                vlen = 40;
            memcpy(line + nlen + 2, valstr, vlen);
            line[nlen + 2 + vlen] = '\0';

            rt_string s = make_string(line);
            rt_canvas_text_scaled(canvas_ptr, tx, ty, s, SCALE, COL_VALUE);
            rt_string_unref(s);
            ty += LINE_H;
        }
    }
}
