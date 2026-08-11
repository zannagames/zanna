//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/game/rt_gameui.c
/// @file
/// @brief Implements core lightweight game UI widgets and shared helpers.
//
// Purpose: Lightweight in-game UI widgets that draw directly to a Canvas.
//   Provides Label, Bar, Panel, NineSlice, MenuList, and GameButton — the
//   building blocks for game HUDs, menus, and overlays without the overhead of
//   the desktop ZannaGUI widget system.
//
// Key invariants:
//   - All widgets use runtime-object reference counting.
//   - Draw functions are no-ops when canvas or widget is NULL.
//   - UIBar values are clamped: value in [0, max], max >= 1.
//   - UIMenuList selection wraps: MoveUp at 0 → last item, MoveDown at last → 0.
//   - UILabel falls back to built-in 8x8 font when font is NULL.
//   - Fixed-size text buffers are truncated on UTF-8 codepoint boundaries.
//
// Ownership/Lifetime:
//   - Widget-owned references are released by registered finalizers where
//     required; widgets with only inline state need no finalizer.
//   - UILabel and GameButton copy text into widget-owned buffers.
//   - UIMenuList stores item text in fixed widget-owned buffers.
//   - UILabel/UIMenuList retain assigned BitmapFont handles.
//   - UINineSlice retains its Pixels source.
//
// Links: rt_gameui.h (public API), rt_graphics.h (Canvas drawing),
//        rt_bitmapfont.h (custom font support), rt_pixels.h (NineSlice source)
//
//===----------------------------------------------------------------------===//

#include "rt_gameui.h"
#include "rt_bitmapfont.h"
#include "rt_gameui_internal.h"
#include "rt_graphics.h"
#include "rt_internal.h"
#include "rt_object.h"
#include "rt_pixels.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/// @brief Largest accepted width or height for bounded UI dimensions.
#define UI_MAX_DIM INT64_C(16384)

/// @brief Clamp a widget dimension to [1, UI_MAX_DIM] (rejects non-positive).
/// @param value Candidate dimension.
/// @return @p value clamped to the inclusive range `[1, 16384]`.
int64_t ui_clamp_dim(int64_t value) {
    if (value <= 0)
        return 1;
    return value > UI_MAX_DIM ? UI_MAX_DIM : value;
}

/// @brief Clamp an integer UI scale factor to [1, 16].
/// @param scale Candidate scale.
/// @return @p scale clamped to the inclusive range `[1, 16]`.
static int64_t ui_clamp_scale(int64_t scale) {
    if (scale < 1)
        return 1;
    if (scale > 16)
        return 16;
    return scale;
}

/// @brief Saturating int64 addition (clamps to INT64_MIN/MAX on overflow).
/// @param a First addend.
/// @param b Second addend.
/// @return The exact sum when representable, otherwise the nearest `int64_t`
///         endpoint.
int64_t ui_add_sat_i64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b)
        return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b)
        return INT64_MIN;
    return a + b;
}

/// @brief Multiply two signed integers with saturation.
/// @param a First factor.
/// @param b Second factor.
/// @return The exact product when representable, otherwise `INT64_MIN` or
///         `INT64_MAX` according to its sign.
/// @details Uses a 128-bit intermediate when available and a long-double
///          intermediate on other platforms.
int64_t ui_mul_sat_i64(int64_t a, int64_t b) {
    if (a == 0 || b == 0)
        return 0;
#if defined(__SIZEOF_INT128__)
    __extension__ __int128 r = (__int128)a * (__int128)b;
    if (r > INT64_MAX)
        return INT64_MAX;
    if (r < INT64_MIN)
        return INT64_MIN;
    return (int64_t)r;
#else
    long double r = (long double)a * (long double)b;
    if (r > (long double)INT64_MAX)
        return INT64_MAX;
    if (r < (long double)INT64_MIN)
        return INT64_MIN;
    return (int64_t)r;
#endif
}

/// @brief Convert a long double to int64, saturating to the int64 range
///        (non-finite -> 0; backs the bound off on short-mantissa platforms).
/// @param value Floating-point value to convert.
/// @return Zero for NaN or infinity, the appropriate `int64_t` endpoint near
///         or beyond its range, or @p value truncated toward zero.
/// @details Platforms whose `long double` has fewer than 64 mantissa bits use a
///          conservative 4096-unit boundary to avoid an out-of-range cast
///          after rounding.
int64_t ui_ld_to_i64_sat(long double value) {
    if (!isfinite(value))
        return 0;
#if LDBL_MANT_DIG < 64
    if (value >= (long double)(INT64_MAX - INT64_C(4096)))
        return INT64_MAX;
    if (value <= (long double)(INT64_MIN + INT64_C(4096)))
        return INT64_MIN;
#else
    if (value > (long double)INT64_MAX)
        return INT64_MAX;
    if (value < (long double)INT64_MIN)
        return INT64_MIN;
#endif
    return (int64_t)value;
}

/// @brief True if @p point lies in the half-open span [start, start+extent)
///        (overflow-safe via unsigned offset).
/// @param start Inclusive span origin.
/// @param extent Span length; nonpositive values describe an empty span.
/// @param point Coordinate to test.
/// @return `1` when the nonnegative unsigned offset is less than @p extent;
///         otherwise `0`.
int8_t ui_coord_inside(int64_t start, int64_t extent, int64_t point) {
    if (extent <= 0 || point < start)
        return 0;
    uint64_t offset = (uint64_t)point - (uint64_t)start;
    return offset < (uint64_t)extent;
}

/// @brief Offset of @p point from @p start, clamped to [0, extent]
///        (for mapping a click coordinate into a widget-local position).
/// @param start Coordinate origin.
/// @param extent Maximum returned offset; nonpositive values return zero.
/// @param point Coordinate to map.
/// @return Zero at or before @p start, @p extent at or beyond the far edge, or
///         the exact nonnegative offset between them.
int64_t ui_coord_offset_clamped(int64_t start, int64_t extent, int64_t point) {
    if (extent <= 0 || point <= start)
        return 0;
    uint64_t offset = (uint64_t)point - (uint64_t)start;
    if (offset >= (uint64_t)extent)
        return extent;
    return (int64_t)offset;
}

/// @brief True if @p obj is a BitmapFont instance.
/// @param obj Candidate runtime object.
/// @return `1` for a non-null object with RT_BITMAPFONT_CLASS_ID; otherwise
///         `0`.
static int8_t ui_is_bitmapfont(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_BITMAPFONT_CLASS_ID;
}

/// @brief True if @p obj is a Pixels instance.
/// @param obj Candidate runtime object.
/// @return `1` for a non-null object with RT_PIXELS_CLASS_ID; otherwise `0`.
static int8_t ui_is_pixels(void *obj) {
    return obj && rt_obj_class_id(obj) == RT_PIXELS_CLASS_ID;
}

/// @brief Validate an optional BitmapFont argument: NULL is allowed (returns
///        1); a wrong-type handle traps @p api.
/// @param font Candidate BitmapFont or `NULL` for default-font behavior.
/// @param api Trap message used for a non-null class mismatch.
/// @return `1` when @p font is null or valid; `0` after a mismatch trap.
int8_t ui_validate_bitmapfont(void *font, const char *api) {
    if (!font)
        return 1;
    if (!ui_is_bitmapfont(font)) {
        rt_trap(api);
        return 0;
    }
    return 1;
}

/// @brief Validate a required Pixels argument; traps @p api on a wrong-type
///        handle.
/// @param pixels Candidate Pixels handle.
/// @param api Trap message used for a non-null class mismatch.
/// @return `1` for a valid Pixels; `0` for null or after a mismatch trap.
static int8_t ui_validate_pixels(void *pixels, const char *api) {
    if (!pixels)
        return 0;
    if (!ui_is_pixels(pixels)) {
        rt_trap(api);
        return 0;
    }
    return 1;
}

/// @brief Validate a required Canvas argument; traps @p api on a non-Canvas
///        handle.
/// @param canvas Candidate 2D Canvas handle.
/// @param api Trap message used for a non-null type mismatch.
/// @return `1` for a valid 2D Canvas; `0` for null or after a mismatch trap.
int8_t ui_validate_canvas(void *canvas, const char *api) {
    if (!canvas)
        return 0;
    if (!rt_canvas_is_handle(canvas)) {
        rt_trap(api);
        return 0;
    }
    return 1;
}

/// @brief Resolve a Draw-call canvas (2D Canvas or Canvas3D) into a draw-ops table.
/// @param canvas Candidate 2D Canvas or registered Canvas3D handle.
/// @param api Trap message used when a non-null handle cannot be resolved.
/// @param ops Required output table populated on success.
/// @return `1` when draw operations were resolved; otherwise `0`.
/// @details ADR 0065: widgets are canvas-polymorphic — the same widget draws on the
///          2D Canvas and on Canvas3D via the ops table Canvas3D registers at init.
///          Null arguments return zero without trapping. Unknown non-null
///          handles trap with @p api exactly like `ui_validate_canvas`.
int8_t ui_resolve_draw_ops(void *canvas, const char *api, rt_gameui_draw_ops_t *ops) {
    if (!canvas || !ops)
        return 0;
    if (!rt_gameui_resolve_draw_ops(canvas, ops)) {
        rt_trap(api);
        return 0;
    }
    return 1;
}

/// @brief Length of @p s up to the first NUL or @p max_len bytes.
/// @param s Byte sequence to scan; `NULL` returns zero.
/// @param max_len Maximum readable byte count.
/// @return Visible prefix length, excluding the first NUL.
size_t ui_visible_len(const char *s, size_t max_len) {
    size_t len = 0;
    if (!s)
        return 0;
    while (len < max_len && s[len] != '\0')
        len++;
    return len;
}

/// @brief True if @p c is a UTF-8 continuation byte (10xxxxxx).
/// @param c Byte to classify.
/// @return Nonzero when the two high bits are `10`; otherwise zero.
static int ui_is_continuation(unsigned char c) {
    return (c & 0xC0u) == 0x80u;
}

/// @brief Byte length (1–4) of the well-formed UTF-8 codepoint at @p pos.
/// @param s Byte sequence to inspect.
/// @param len Number of readable bytes in @p s.
/// @param pos Candidate lead-byte index.
/// @return Zero when @p s is null or @p pos is out of range; otherwise the
///         validated sequence length, with malformed input treated as one byte.
/// @details Validates continuation bytes and rejects overlong/surrogate/
///          out-of-range sequences (returns 1 for any malformed lead byte so
///          callers always make forward progress).
size_t ui_utf8_cp_len(const char *s, size_t len, size_t pos) {
    if (!s || pos >= len)
        return 0;
    unsigned char c = (unsigned char)s[pos];
    if (c < 0x80u)
        return 1;
    if (c >= 0xC2u && c <= 0xDFu) {
        if (pos + 1 < len && ui_is_continuation((unsigned char)s[pos + 1]))
            return 2;
        return 1;
    }
    if ((c & 0xF0u) == 0xE0u) {
        unsigned char c1 = pos + 1 < len ? (unsigned char)s[pos + 1] : 0;
        unsigned char c2 = pos + 2 < len ? (unsigned char)s[pos + 2] : 0;
        if (pos + 2 < len && ui_is_continuation(c1) && ui_is_continuation(c2) &&
            !(c == 0xE0u && c1 < 0xA0u) && !(c == 0xEDu && c1 >= 0xA0u))
            return 3;
        return 1;
    }
    if ((c & 0xF8u) == 0xF0u) {
        unsigned char c1 = pos + 1 < len ? (unsigned char)s[pos + 1] : 0;
        unsigned char c2 = pos + 2 < len ? (unsigned char)s[pos + 2] : 0;
        unsigned char c3 = pos + 3 < len ? (unsigned char)s[pos + 3] : 0;
        if (c >= 0xF0u && c <= 0xF4u && pos + 3 < len && ui_is_continuation(c1) &&
            ui_is_continuation(c2) && ui_is_continuation(c3) && !(c == 0xF0u && c1 < 0x90u) &&
            !(c == 0xF4u && c1 > 0x8Fu))
            return 4;
        return 1;
    }
    return 1;
}

/// @brief Largest byte length <= @p max_bytes that ends on a UTF-8 character
///        boundary (so truncation never splits a multibyte codepoint).
/// @param s Byte sequence to scan; must be non-null when @p len is nonzero.
/// @param len Number of readable bytes.
/// @param max_bytes Maximum prefix size.
/// @return The largest prefix not exceeding either limit and ending after a
///         validated or single-byte-malformed UTF-8 unit.
size_t ui_utf8_trunc_len(const char *s, size_t len, size_t max_bytes) {
    size_t pos = 0;
    size_t last = 0;
    while (pos < len && pos < max_bytes) {
        size_t cp_len = ui_utf8_cp_len(s, len, pos);
        if (pos + cp_len > len || pos + cp_len > max_bytes)
            break;
        pos += cp_len;
        last = pos;
    }
    return last;
}

/// @brief Byte length of the first @p max_codepoints UTF-8 characters of @p s.
/// @param s Byte sequence to scan; `NULL` returns zero.
/// @param len Number of readable bytes.
/// @param max_codepoints Maximum validated or malformed single-byte units.
/// @return Prefix size containing at most @p max_codepoints units.
size_t ui_utf8_trunc_codepoints(const char *s, size_t len, size_t max_codepoints) {
    size_t pos = 0;
    size_t count = 0;
    if (!s || max_codepoints == 0)
        return 0;
    while (pos < len && count < max_codepoints) {
        size_t cp_len = ui_utf8_cp_len(s, len, pos);
        if (pos + cp_len > len)
            break;
        pos += cp_len;
        count++;
    }
    return pos;
}

/// @brief Copy a runtime string into a fixed @p cap buffer, NUL-terminated and
///        truncated on a UTF-8 character boundary. Empty on NULL @p text.
/// @param dst Destination buffer.
/// @param cap Capacity of @p dst including the NUL terminator.
/// @param text Runtime string to copy; no reference is retained.
/// @details A null destination or zero capacity is a no-op. Otherwise the
///          destination is always terminated. Copying stops at an embedded NUL
///          and never splits a validated multibyte sequence.
void ui_copy_text(char *dst, size_t cap, rt_string text) {
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!text)
        return;

    const char *s = rt_string_cstr(text);
    if (!s)
        return;
    size_t len = ui_visible_len(s, (size_t)rt_str_len(text));
    size_t copy_len = ui_utf8_trunc_len(s, len, cap - 1);
    memcpy(dst, s, copy_len);
    dst[copy_len] = '\0';
}

/// @brief Drop one GC reference to @p obj and free it if the count hit zero.
/// @param obj Runtime object reference to release; `NULL` is a no-op.
void ui_release_obj(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

/// @brief Retain @p value, release the old occupant of @p *slot, and store
///        @p value (a GC-safe reference-replacing setter; no-op if unchanged).
/// @param slot Address of the owned reference slot; `NULL` is a no-op.
/// @param value New reference, or `NULL` to clear the slot.
/// @details Retains the replacement before releasing the old value.
void ui_replace_ref(void **slot, void *value) {
    if (!slot || *slot == value)
        return;
    if (value)
        rt_obj_retain_maybe(value);
    ui_release_obj(*slot);
    *slot = value;
}

/// @brief Draw a rounded-rect with alpha: opaque fast-path delegates to
///        rt_canvas_round_box; otherwise alpha-composites the rounded fill.
/// @param ops Resolved drawing operations and target canvas.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param w Width; nonpositive values are ignored.
/// @param h Height; nonpositive values are ignored.
/// @param radius Requested corner radius, clamped to half the smaller extent.
/// @param color Packed fill color.
/// @param alpha Opacity; nonpositive values are ignored and values at least 255
///        select the opaque fast path.
/// @details A translucent square falls back to one box-alpha call. A
///          translucent rounded box is rasterized as one alpha-blended
///          horizontal row per output pixel.
static void ui_round_box_alpha(const rt_gameui_draw_ops_t *ops,
                               int64_t x,
                               int64_t y,
                               int64_t w,
                               int64_t h,
                               int64_t radius,
                               int64_t color,
                               int64_t alpha) {
    if (!ops || w <= 0 || h <= 0 || alpha <= 0)
        return;
    if (alpha >= 255) {
        ops->round_box(ops->canvas, x, y, w, h, radius, color);
        return;
    }

    int64_t max_radius = (w < h ? w : h) / 2;
    if (radius < 0)
        radius = 0;
    if (radius > max_radius)
        radius = max_radius;
    if (radius <= 0) {
        ops->box_alpha(ops->canvas, x, y, w, h, color, alpha);
        return;
    }

    long double r = (long double)radius;
    long double rr = r * r;
    for (int64_t py = 0; py < h; py++) {
        int64_t inset = 0;
        if (py < radius || py >= h - radius) {
            long double dy = py < radius ? (r - 1.0L - (long double)py)
                                         : ((long double)py - (long double)(h - radius));
            long double inside = rr - dy * dy;
            if (inside < 0.0L)
                inside = 0.0L;
            long double extent = sqrtl(inside);
            long double raw_inset = r - extent - 1.0L;
            if (raw_inset > 0.0L) {
                if (raw_inset > (long double)radius)
                    inset = radius;
                else
                    inset = (int64_t)raw_inset;
            }
        }
        int64_t row_w = w - inset * 2;
        if (row_w > 0)
            ops->box_alpha(ops->canvas, x + inset, y + py, row_w, 1, color, alpha);
    }
}

//=============================================================================
// UILabel
//=============================================================================

/// @brief Label text-buffer capacity in bytes, including its NUL terminator.
#define LABEL_MAX_TEXT 512

/// @brief Private state stored in a runtime UILabel object.
typedef struct {
    void *vptr;
    int64_t x, y;
    char text[LABEL_MAX_TEXT];
    int64_t color;
    void *font;    ///< BitmapFont or NULL for default
    int64_t scale; ///< Integer scale (1 = normal)
    int8_t visible;
} rt_uilabel_impl;

/// @brief Validate and cast an opaque UILabel handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return The UILabel payload when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static rt_uilabel_impl *checked_label(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_UILABEL_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_uilabel_impl *)ptr;
}

/// @brief GC finalizer: release the label's referenced font.
/// @param obj UILabel payload being finalized; `NULL` is accepted.
/// @details Clears the owned font slot after releasing its reference.
static void uilabel_finalizer(void *obj) {
    rt_uilabel_impl *label = (rt_uilabel_impl *)obj;
    if (!label)
        return;
    ui_release_obj(label->font);
    label->font = NULL;
}

/// @brief Create a visible text label using the default canvas font.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param text Initial runtime string copied into the 512-byte label buffer.
/// @param color Initial packed text color.
/// @return A new UILabel reference, or `NULL` if allocation fails.
/// @details The input text is not retained, scale begins at one, and a
///          finalizer is registered for a subsequently assigned font.
void *rt_uilabel_new(int64_t x, int64_t y, rt_string text, int64_t color) {
    rt_uilabel_impl *label =
        (rt_uilabel_impl *)rt_obj_new_i64(RT_UILABEL_CLASS_ID, (int64_t)sizeof(rt_uilabel_impl));
    if (!label)
        return NULL;

    memset(label, 0, sizeof(rt_uilabel_impl));
    label->x = x;
    label->y = y;
    label->color = color;
    label->scale = 1;
    label->visible = 1;
    ui_copy_text(label->text, sizeof(label->text), text);
    rt_obj_set_finalizer(label, uilabel_finalizer);

    return label;
}

/// @brief Replace the label's displayed text, truncating to 511 bytes if needed.
/// @param ptr UILabel to mutate; `NULL` is a no-op.
/// @param text Runtime string to copy, or `NULL` to clear the label.
/// @details Truncation preserves validated UTF-8 sequence boundaries, embedded
///          NUL ends the visible copy, and no input reference is retained. A
///          non-null invalid label handle raises a runtime trap.
void rt_uilabel_set_text(void *ptr, rt_string text) {
    rt_uilabel_impl *label = checked_label(ptr, "UILabel.SetText: expected Zanna.Game.UI.HudLabel");
    if (!label)
        return;
    ui_copy_text(label->text, sizeof(label->text), text);
}

/// @brief Reposition the label to screen coordinates (x, y).
/// @param ptr UILabel to mutate; `NULL` is a no-op.
/// @param x New left coordinate.
/// @param y New top coordinate.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uilabel_set_pos(void *ptr, int64_t x, int64_t y) {
    rt_uilabel_impl *label = checked_label(ptr, "UILabel.SetPos: expected Zanna.Game.UI.HudLabel");
    if (!label)
        return;
    label->x = x;
    label->y = y;
}

/// @brief Set the label's text color (RGBA packed integer).
/// @param ptr UILabel to mutate; `NULL` is a no-op.
/// @param color Packed value stored verbatim.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uilabel_set_color(void *ptr, int64_t color) {
    rt_uilabel_impl *label =
        checked_label(ptr, "UILabel.SetColor: expected Zanna.Game.UI.HudLabel");
    if (label)
        label->color = color;
}

/// @brief Assign a BitmapFont for rendering; NULL uses the built-in 8x8 font.
/// @param ptr UILabel to mutate; `NULL` is a no-op.
/// @param font Optional BitmapFont reference.
/// @details A valid replacement is retained before the old font is released.
///          A wrong font class or non-null invalid label raises a runtime trap.
void rt_uilabel_set_font(void *ptr, void *font) {
    rt_uilabel_impl *label = checked_label(ptr, "UILabel.SetFont: expected Zanna.Game.UI.HudLabel");
    if (!label)
        return;
    if (!ui_validate_bitmapfont(font, "UILabel.SetFont: expected BitmapFont"))
        return;
    ui_replace_ref(&label->font, font);
}

/// @brief Set the integer pixel scale for text rendering (minimum 1).
/// @param ptr UILabel to mutate; `NULL` is a no-op.
/// @param scale Requested scale, clamped to `[1, 16]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uilabel_set_scale(void *ptr, int64_t scale) {
    rt_uilabel_impl *label =
        checked_label(ptr, "UILabel.SetScale: expected Zanna.Game.UI.HudLabel");
    if (label)
        label->scale = ui_clamp_scale(scale);
}

/// @brief Show or hide the label; hidden labels are skipped during draw.
/// @param ptr UILabel to mutate; `NULL` is a no-op.
/// @param visible Zero hides the label; any nonzero value shows it.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uilabel_set_visible(void *ptr, int8_t visible) {
    rt_uilabel_impl *label =
        checked_label(ptr, "UILabel.SetVisible: expected Zanna.Game.UI.HudLabel");
    if (label)
        label->visible = visible ? 1 : 0;
}

/// @brief Return the label's current X position in screen coordinates.
/// @param ptr UILabel to query.
/// @return Stored X coordinate, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uilabel_get_x(void *ptr) {
    rt_uilabel_impl *label = checked_label(ptr, "UILabel.X: expected Zanna.Game.UI.HudLabel");
    return label ? label->x : 0;
}

/// @brief Return the label's current Y position in screen coordinates.
/// @param ptr UILabel to query.
/// @return Stored Y coordinate, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uilabel_get_y(void *ptr) {
    rt_uilabel_impl *label = checked_label(ptr, "UILabel.Y: expected Zanna.Game.UI.HudLabel");
    return label ? label->y : 0;
}

/// @brief Render the label onto the canvas using its font, scale, and color.
/// @param ptr UILabel to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Skips rendering if the label is hidden or has empty text. Uses
///          the assigned BitmapFont when set, otherwise falls back to the
///          built-in 8x8 pixel font. Null arguments are no-ops; invalid non-null
///          label or canvas handles raise runtime traps.
void rt_uilabel_draw(void *ptr, void *canvas) {
    rt_gameui_draw_ops_t ops;
    if (!ptr || !canvas)
        return;
    rt_uilabel_impl *label = checked_label(ptr, "UILabel.Draw: expected Zanna.Game.UI.HudLabel");
    if (!label || !ui_resolve_draw_ops(canvas, "UILabel.Draw: expected Canvas or Canvas3D", &ops))
        return;
    if (!label->visible || label->text[0] == '\0')
        return;

    rt_string rtext = rt_const_cstr(label->text);

    if (label->font) {
        if (label->scale > 1)
            ops.text_font_scaled(
                canvas, label->x, label->y, rtext, label->font, label->scale, label->color);
        else
            ops.text_font(canvas, label->x, label->y, rtext, label->font, label->color);
    } else {
        if (label->scale > 1)
            ops.text_scaled(canvas, label->x, label->y, rtext, label->scale, label->color);
        else
            ops.text(canvas, label->x, label->y, rtext, label->color);
    }
}

//=============================================================================
// UIBar
//=============================================================================

/// @brief Fill from the left edge toward the right.
#define BAR_DIR_LEFT_TO_RIGHT 0
/// @brief Fill from the right edge toward the left.
#define BAR_DIR_RIGHT_TO_LEFT 1
/// @brief Fill from the bottom edge toward the top.
#define BAR_DIR_BOTTOM_TO_TOP 2
/// @brief Fill from the top edge toward the bottom.
#define BAR_DIR_TOP_TO_BOTTOM 3

/// @brief Private state stored in a runtime UIBar object.
typedef struct {
    void *vptr;
    int64_t x, y, w, h;
    int64_t value, max_value;
    int64_t fg_color, bg_color;
    int64_t border_color; ///< 0 = no border
    int64_t direction;
    int8_t visible;
} rt_uibar_impl;

/// @brief Validate and cast an opaque UIBar handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return The UIBar payload when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static rt_uibar_impl *checked_bar(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_UIBAR_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_uibar_impl *)ptr;
}

/// @brief Create a visible left-to-right progress bar.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @param fg_color Initial filled-region color.
/// @param bg_color Initial empty-region color.
/// @return A new UIBar reference, or `NULL` if allocation fails.
/// @details The initial value is zero of 100 and the border is disabled.
void *rt_uibar_new(int64_t x, int64_t y, int64_t w, int64_t h, int64_t fg_color, int64_t bg_color) {
    rt_uibar_impl *bar =
        (rt_uibar_impl *)rt_obj_new_i64(RT_UIBAR_CLASS_ID, (int64_t)sizeof(rt_uibar_impl));
    if (!bar)
        return NULL;

    bar->vptr = NULL;
    bar->x = x;
    bar->y = y;
    bar->w = ui_clamp_dim(w);
    bar->h = ui_clamp_dim(h);
    bar->value = 0;
    bar->max_value = 100;
    bar->fg_color = fg_color;
    bar->bg_color = bg_color;
    bar->border_color = 0;
    bar->direction = BAR_DIR_LEFT_TO_RIGHT;
    bar->visible = 1;

    return bar;
}

/// @brief Set the bar's current and maximum values, clamping to [0, max].
/// @param ptr UIBar to mutate; `NULL` is a no-op.
/// @param value Requested current value.
/// @param max_value Requested denominator; values below one become one.
/// @details max_value is forced to at least 1 to prevent division by zero
///          during fill-ratio computation. Current value then clamps to the
///          inclusive range `[0, max_value]`. A non-null invalid handle raises
///          a runtime trap.
void rt_uibar_set_value(void *ptr, int64_t value, int64_t max_value) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.SetValue: expected Zanna.Game.UI.Bar");
    if (!bar)
        return;
    if (max_value < 1)
        max_value = 1;
    if (value < 0)
        value = 0;
    if (value > max_value)
        value = max_value;
    bar->value = value;
    bar->max_value = max_value;
}

/// @brief Reposition the bar to screen coordinates (x, y).
/// @param ptr UIBar to mutate; `NULL` is a no-op.
/// @param x New left coordinate.
/// @param y New top coordinate.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_pos(void *ptr, int64_t x, int64_t y) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.SetPos: expected Zanna.Game.UI.Bar");
    if (!bar)
        return;
    bar->x = x;
    bar->y = y;
}

/// @brief Set the bar's width and height in pixels (minimum 1 each).
/// @param ptr UIBar to mutate; `NULL` is a no-op.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_size(void *ptr, int64_t w, int64_t h) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.SetSize: expected Zanna.Game.UI.Bar");
    if (!bar)
        return;
    bar->w = ui_clamp_dim(w);
    bar->h = ui_clamp_dim(h);
}

/// @brief Set the foreground (filled portion) and background colors of the bar.
/// @param ptr UIBar to mutate; `NULL` is a no-op.
/// @param fg Packed foreground value stored verbatim.
/// @param bg Packed background value stored verbatim.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_colors(void *ptr, int64_t fg, int64_t bg) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.SetColors: expected Zanna.Game.UI.Bar");
    if (!bar)
        return;
    bar->fg_color = fg;
    bar->bg_color = bg;
}

/// @brief Set the bar's border color; 0 disables the border outline.
/// @param ptr UIBar to mutate; `NULL` is a no-op.
/// @param color Packed border value, or zero to disable.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_border(void *ptr, int64_t color) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.SetBorder: expected Zanna.Game.UI.Bar");
    if (bar)
        bar->border_color = color;
}

/// @brief Set the bar's fill direction (0=L→R, 1=R→L, 2=B→T, 3=T→B).
/// @param ptr UIBar to mutate; `NULL` is a no-op.
/// @param dir Direction constant; values outside `[0, 3]` select left-to-right.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_direction(void *ptr, int64_t dir) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.SetDirection: expected Zanna.Game.UI.Bar");
    if (!bar)
        return;
    if (dir < 0 || dir > 3)
        dir = BAR_DIR_LEFT_TO_RIGHT;
    bar->direction = dir;
}

/// @brief Show or hide the bar; hidden bars are skipped during draw.
/// @param ptr UIBar to mutate; `NULL` is a no-op.
/// @param visible Zero hides the bar; any nonzero value shows it.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uibar_set_visible(void *ptr, int8_t visible) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.SetVisible: expected Zanna.Game.UI.Bar");
    if (bar)
        bar->visible = visible ? 1 : 0;
}

/// @brief Return the bar's current fill value.
/// @param ptr UIBar to query.
/// @return Current value, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uibar_get_value(void *ptr) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.Value: expected Zanna.Game.UI.Bar");
    return bar ? bar->value : 0;
}

/// @brief Return the bar's maximum value (the denominator for fill ratio).
/// @param ptr UIBar to query.
/// @return Positive maximum value, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uibar_get_max(void *ptr) {
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.Max: expected Zanna.Game.UI.Bar");
    return bar ? bar->max_value : 0;
}

/// @brief Map @p value in [0, max_value] proportionally onto a pixel @p extent
///        (e.g. progress-bar fill width), clamped to [0, extent].
/// @param value Current value.
/// @param extent Available pixel width or height.
/// @param max_value Positive denominator.
/// @return Zero for nonpositive arguments; otherwise the ceiling of the
///         proportional fill clamped to `[1, extent]`.
static int64_t ui_scaled_fill(int64_t value, int64_t extent, int64_t max_value) {
    if (value <= 0 || extent <= 0 || max_value <= 0)
        return 0;
    long double fill = ((long double)value * (long double)extent) / (long double)max_value;
    if (fill <= 0.0L)
        return 0;
    if (fill >= (long double)extent)
        return extent;
    int64_t pixels = (int64_t)ceill(fill);
    if (pixels < 1)
        pixels = 1;
    return pixels > extent ? extent : pixels;
}

/// @brief Draw a UIBar background, directional fill, and optional border.
/// @param ptr UIBar to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Null arguments and hidden bars are no-ops. Positive values use a
///          ceiling-rounded fill, so any nonzero fraction occupies at least one
///          pixel. Invalid non-null widget or canvas handles raise runtime
///          traps.
void rt_uibar_draw(void *ptr, void *canvas) {
    rt_gameui_draw_ops_t ops;
    if (!ptr || !canvas)
        return;
    rt_uibar_impl *bar = checked_bar(ptr, "UIBar.Draw: expected Zanna.Game.UI.Bar");
    if (!bar || !ui_resolve_draw_ops(canvas, "UIBar.Draw: expected Canvas or Canvas3D", &ops))
        return;
    if (!bar->visible)
        return;

    // Background
    ops.box(canvas, bar->x, bar->y, bar->w, bar->h, bar->bg_color);

    // Filled portion
    if (bar->value > 0 && bar->max_value > 0) {
        int64_t fill;
        switch (bar->direction) {
            case BAR_DIR_RIGHT_TO_LEFT:
                fill = ui_scaled_fill(bar->value, bar->w, bar->max_value);
                ops.box(canvas, bar->x + bar->w - fill, bar->y, fill, bar->h, bar->fg_color);
                break;
            case BAR_DIR_TOP_TO_BOTTOM:
                fill = ui_scaled_fill(bar->value, bar->h, bar->max_value);
                ops.box(canvas, bar->x, bar->y, bar->w, fill, bar->fg_color);
                break;
            case BAR_DIR_BOTTOM_TO_TOP:
                fill = ui_scaled_fill(bar->value, bar->h, bar->max_value);
                ops.box(canvas, bar->x, bar->y + bar->h - fill, bar->w, fill, bar->fg_color);
                break;
            default: // LEFT_TO_RIGHT
                fill = ui_scaled_fill(bar->value, bar->w, bar->max_value);
                ops.box(canvas, bar->x, bar->y, fill, bar->h, bar->fg_color);
                break;
        }
    }

    // Border
    if (bar->border_color != 0)
        ops.frame(canvas, bar->x, bar->y, bar->w, bar->h, bar->border_color);
}

//=============================================================================
// UIPanel
//=============================================================================

/// @brief Private state stored in a runtime UIPanel object.
typedef struct {
    void *vptr;
    int64_t x, y, w, h;
    int64_t bg_color;
    int64_t alpha;        ///< 0-255
    int64_t border_color; ///< 0 = no border
    int64_t border_thickness;
    int64_t corner_radius; ///< 0 = sharp corners
    int8_t visible;
} rt_uipanel_impl;

/// @brief Validate and cast an opaque UIPanel handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return The UIPanel payload when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static rt_uipanel_impl *checked_panel(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_UIPANEL_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_uipanel_impl *)ptr;
}

/// @brief Create a visible filled panel with sharp corners and no border.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @param bg_color Initial packed fill color.
/// @param alpha Initial opacity clamped to `[0, 255]`.
/// @return A new UIPanel reference, or `NULL` if allocation fails.
void *rt_uipanel_new(int64_t x, int64_t y, int64_t w, int64_t h, int64_t bg_color, int64_t alpha) {
    rt_uipanel_impl *panel =
        (rt_uipanel_impl *)rt_obj_new_i64(RT_UIPANEL_CLASS_ID, (int64_t)sizeof(rt_uipanel_impl));
    if (!panel)
        return NULL;

    panel->vptr = NULL;
    panel->x = x;
    panel->y = y;
    panel->w = ui_clamp_dim(w);
    panel->h = ui_clamp_dim(h);
    panel->bg_color = bg_color;
    panel->alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
    panel->border_color = 0;
    panel->border_thickness = 1;
    panel->corner_radius = 0;
    panel->visible = 1;

    return panel;
}

/// @brief Reposition the panel to screen coordinates (x, y).
/// @param ptr UIPanel to mutate; `NULL` is a no-op.
/// @param x New left coordinate.
/// @param y New top coordinate.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_pos(void *ptr, int64_t x, int64_t y) {
    rt_uipanel_impl *panel = checked_panel(ptr, "UIPanel.SetPos: expected Zanna.Game.UI.Panel");
    if (!panel)
        return;
    panel->x = x;
    panel->y = y;
}

/// @brief Set the panel's width and height in pixels (minimum 1 each).
/// @param ptr UIPanel to mutate; `NULL` is a no-op.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_size(void *ptr, int64_t w, int64_t h) {
    rt_uipanel_impl *panel = checked_panel(ptr, "UIPanel.SetSize: expected Zanna.Game.UI.Panel");
    if (!panel)
        return;
    panel->w = ui_clamp_dim(w);
    panel->h = ui_clamp_dim(h);
}

/// @brief Set the panel's background color and alpha (opacity 0-255).
/// @param ptr UIPanel to mutate; `NULL` is a no-op.
/// @param bg_color Packed fill value stored verbatim.
/// @param alpha Opacity clamped to `[0, 255]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_color(void *ptr, int64_t bg_color, int64_t alpha) {
    rt_uipanel_impl *panel = checked_panel(ptr, "UIPanel.SetColor: expected Zanna.Game.UI.Panel");
    if (!panel)
        return;
    panel->bg_color = bg_color;
    panel->alpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
}

/// @brief Set the panel's border color and thickness. Color 0 or thickness <= 0 disables the
/// border.
/// @param ptr UIPanel to mutate; `NULL` is a no-op.
/// @param color Packed border color, with zero disabling the border.
/// @param thickness Requested pixel thickness. Positive enabled values clamp
///        to at most 16384 and are further limited to half the smaller panel
///        dimension while drawing.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_border(void *ptr, int64_t color, int64_t thickness) {
    rt_uipanel_impl *panel = checked_panel(ptr, "UIPanel.SetBorder: expected Zanna.Game.UI.Panel");
    if (!panel)
        return;
    if (color == 0 || thickness <= 0) {
        panel->border_color = 0;
        panel->border_thickness = 0;
        return;
    }
    panel->border_color = color;
    panel->border_thickness = thickness > UI_MAX_DIM ? UI_MAX_DIM : thickness;
}

/// @brief Set the corner radius for rounded-rectangle drawing (0 = sharp).
/// @param ptr UIPanel to mutate; `NULL` is a no-op.
/// @param radius Requested radius; negative values become zero. Drawing clamps
///        it to half the current smaller dimension.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_corner_radius(void *ptr, int64_t radius) {
    rt_uipanel_impl *panel =
        checked_panel(ptr, "UIPanel.SetCornerRadius: expected Zanna.Game.UI.Panel");
    if (panel)
        panel->corner_radius = radius < 0 ? 0 : radius;
}

/// @brief Show or hide the panel; hidden panels are skipped during draw.
/// @param ptr UIPanel to mutate; `NULL` is a no-op.
/// @param visible Zero hides the panel; any nonzero value shows it.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uipanel_set_visible(void *ptr, int8_t visible) {
    rt_uipanel_impl *panel = checked_panel(ptr, "UIPanel.SetVisible: expected Zanna.Game.UI.Panel");
    if (panel)
        panel->visible = visible ? 1 : 0;
}

/// @brief Render the panel onto the canvas as a filled rectangle.
/// @param ptr UIPanel to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Draws a rounded or sharp rectangle depending on corner_radius,
///          then overlays inward border frames if enabled. Zero alpha suppresses
///          only the background, not the border. Null arguments and hidden
///          panels are no-ops; invalid non-null handles raise runtime traps.
void rt_uipanel_draw(void *ptr, void *canvas) {
    rt_gameui_draw_ops_t ops;
    if (!ptr || !canvas)
        return;
    rt_uipanel_impl *panel = checked_panel(ptr, "UIPanel.Draw: expected Zanna.Game.UI.Panel");
    if (!panel || !ui_resolve_draw_ops(canvas, "UIPanel.Draw: expected Canvas or Canvas3D", &ops))
        return;
    if (!panel->visible)
        return;

    // Background with alpha
    if (panel->alpha >= 255) {
        if (panel->corner_radius > 0)
            ops.round_box(canvas,
                                panel->x,
                                panel->y,
                                panel->w,
                                panel->h,
                                panel->corner_radius,
                                panel->bg_color);
        else
            ops.box(canvas, panel->x, panel->y, panel->w, panel->h, panel->bg_color);
    } else if (panel->alpha > 0) {
        if (panel->corner_radius > 0)
            ui_round_box_alpha(&ops,
                               panel->x,
                               panel->y,
                               panel->w,
                               panel->h,
                               panel->corner_radius,
                               panel->bg_color,
                               panel->alpha);
        else
            ops.box_alpha(
                canvas, panel->x, panel->y, panel->w, panel->h, panel->bg_color, panel->alpha);
    }

    // Border
    if (panel->border_color != 0 && panel->border_thickness > 0) {
        int64_t max_t = (panel->w < panel->h ? panel->w : panel->h) / 2;
        if (max_t < 1)
            max_t = 1;
        int64_t thickness = panel->border_thickness > max_t ? max_t : panel->border_thickness;
        for (int64_t t = 0; t < thickness; t++) {
            int64_t iw = panel->w - t * 2;
            int64_t ih = panel->h - t * 2;
            if (iw <= 0 || ih <= 0)
                break;
            int64_t radius = panel->corner_radius - t;
            if (radius < 0)
                radius = 0;
            int64_t max_radius = (iw < ih ? iw : ih) / 2;
            if (radius > max_radius)
                radius = max_radius;
            if (radius > 0)
                ops.round_frame(
                    canvas, panel->x + t, panel->y + t, iw, ih, radius, panel->border_color);
            else
                ops.frame(canvas, panel->x + t, panel->y + t, iw, ih, panel->border_color);
        }
    }
}

//=============================================================================
// UINineSlice
//=============================================================================

/// @brief Private source, inset, tint-cache state for a UINineSlice.
typedef struct {
    void *vptr;
    void *pixels;        ///< Source Pixels handle (retained)
    void *tinted_pixels; ///< Cached tinted source, if tint != 0
    uint64_t tinted_source_generation;
    int64_t tinted_color;
    int64_t left;   ///< Left margin (corner width)
    int64_t top;    ///< Top margin (corner height)
    int64_t right;  ///< Right margin
    int64_t bottom; ///< Bottom margin
    int64_t tint;   ///< Tint color (0 = no tint)
} rt_uinineslice_impl;

/// @brief Validate and cast an opaque UINineSlice handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return The UINineSlice payload when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static rt_uinineslice_impl *checked_nineslice(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_UININESLICE_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_uinineslice_impl *)ptr;
}

/// @brief GC finalizer: release the nine-slice's referenced source image.
/// @param obj UINineSlice payload being finalized; `NULL` is accepted.
/// @details Releases both the retained source and any cached tinted Pixels,
///          then clears their slots.
static void uinineslice_finalizer(void *obj) {
    rt_uinineslice_impl *ns = (rt_uinineslice_impl *)obj;
    if (!ns)
        return;
    ui_release_obj(ns->pixels);
    ui_release_obj(ns->tinted_pixels);
    ns->pixels = NULL;
    ns->tinted_pixels = NULL;
}

/// @brief Create a nine-slice descriptor for a retained Pixels source.
/// @param pixels Required Pixels handle.
/// @param left Requested left source inset.
/// @param top Requested top source inset.
/// @param right Requested right source inset.
/// @param bottom Requested bottom source inset.
/// @return A new UINineSlice reference, or `NULL` for null Pixels or allocation
///         failure.
/// @details A wrong source class raises a runtime trap. Negative insets become
///          zero. If opposing horizontal or vertical insets exceed their source
///          dimension, that pair is replaced by a complete half-and-remainder
///          split. The source is retained and tint begins disabled.
void *rt_uinineslice_new(void *pixels, int64_t left, int64_t top, int64_t right, int64_t bottom) {
    if (!ui_validate_pixels(pixels, "UINineSlice: expected Pixels"))
        return NULL;

    rt_uinineslice_impl *ns = (rt_uinineslice_impl *)rt_obj_new_i64(
        RT_UININESLICE_CLASS_ID, (int64_t)sizeof(rt_uinineslice_impl));
    if (!ns)
        return NULL;

    ns->vptr = NULL;
    int64_t pw = rt_pixels_width(pixels);
    int64_t ph = rt_pixels_height(pixels);

    // Clamp margins to source dimensions
    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right < 0)
        right = 0;
    if (bottom < 0)
        bottom = 0;
    if (left > pw || right > pw || left > pw - right) {
        left = pw / 2;
        right = pw - left;
    }
    if (top > ph || bottom > ph || top > ph - bottom) {
        top = ph / 2;
        bottom = ph - top;
    }

    rt_obj_retain_maybe(pixels);
    ns->pixels = pixels;
    ns->tinted_pixels = NULL;
    ns->tinted_source_generation = 0;
    ns->tinted_color = 0;
    ns->left = left;
    ns->top = top;
    ns->right = right;
    ns->bottom = bottom;
    ns->tint = 0;
    rt_obj_set_finalizer(ns, uinineslice_finalizer);

    return ns;
}

/// @brief Set a color tint applied over the nine-slice texture when drawn.
/// @param ptr UINineSlice to mutate; `NULL` is a no-op.
/// @param color Tint passed to rt_pixels_tint(), or zero to use the source.
/// @details Reapplying the current tint is a no-op. A changed tint releases and
///          invalidates the cached tinted image. A non-null invalid widget
///          handle raises a runtime trap.
void rt_uinineslice_set_tint(void *ptr, int64_t color) {
    rt_uinineslice_impl *ns =
        checked_nineslice(ptr, "UINineSlice.SetTint: expected Zanna.Game.UI.NineSlice");
    if (!ns)
        return;
    if (ns->tint == color)
        return;
    ns->tint = color;
    ui_release_obj(ns->tinted_pixels);
    ns->tinted_pixels = NULL;
    ns->tinted_source_generation = 0;
    ns->tinted_color = 0;
}

/// @brief Resolve the Pixels image a nine-slice should draw from (its
///        explicit source, a current tinted cache, or the source as fallback).
/// @param ns Nine-slice payload, or `NULL`.
/// @return Borrowed cached/source Pixels, or `NULL` when unavailable.
/// @details A nonzero tint rebuilds the owned cache when the source generation
///          or tint value changes. Tint-allocation failure falls back to the
///          original retained source.
static void *uinineslice_draw_source(rt_uinineslice_impl *ns) {
    if (!ns || !ns->pixels || ns->tint == 0)
        return ns ? ns->pixels : NULL;

    uint64_t generation = rt_pixels_generation(ns->pixels);
    if (!ns->tinted_pixels || ns->tinted_source_generation != generation ||
        ns->tinted_color != ns->tint) {
        ui_release_obj(ns->tinted_pixels);
        ns->tinted_pixels = rt_pixels_tint(ns->pixels, ns->tint);
        ns->tinted_source_generation = generation;
        ns->tinted_color = ns->tint;
    }
    return ns->tinted_pixels ? ns->tinted_pixels : ns->pixels;
}

/// @brief Render the nine-slice texture onto the canvas at the given rect.
/// @param ptr UINineSlice to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @param x Destination left coordinate.
/// @param y Destination top coordinate.
/// @param w Destination width; nonpositive values are ignored and positive
///        values clamp to 16384.
/// @param h Destination height, normalized like @p w.
/// @details Splits the source texture into 9 patches using the configured insets
///          and tiles the source center and edge strips to fill the destination.
///          Corners retain their natural size unless clipped by a destination
///          smaller than the combined insets. Null arguments are no-ops;
///          invalid non-null widget or canvas handles raise runtime traps.
void rt_uinineslice_draw(void *ptr, void *canvas, int64_t x, int64_t y, int64_t w, int64_t h) {
    rt_gameui_draw_ops_t ops;
    if (!ptr || !canvas)
        return;
    rt_uinineslice_impl *ns =
        checked_nineslice(ptr, "UINineSlice.Draw: expected Zanna.Game.UI.NineSlice");
    if (!ns || !ui_resolve_draw_ops(canvas, "UINineSlice.Draw: expected Canvas or Canvas3D", &ops))
        return;
    if (!ns->pixels || w <= 0 || h <= 0)
        return;

    w = ui_clamp_dim(w);
    h = ui_clamp_dim(h);
    void *source_pixels = uinineslice_draw_source(ns);
    if (!source_pixels)
        return;

    int64_t pw = rt_pixels_width(source_pixels);
    int64_t ph = rt_pixels_height(source_pixels);
    int64_t src_l = ns->left, src_t = ns->top, src_r = ns->right, src_b = ns->bottom;
    int64_t L = src_l < w ? src_l : w;
    int64_t R = src_r < w - L ? src_r : w - L;
    int64_t T = src_t < h ? src_t : h;
    int64_t B = src_b < h - T ? src_b : h - T;

    // Source center region
    int64_t src_cx = src_l;
    int64_t src_cy = src_t;
    int64_t src_cw = pw - src_l - src_r;
    int64_t src_ch = ph - src_t - src_b;

    // Destination center region
    int64_t dst_cw = w - L - R;
    int64_t dst_ch = h - T - B;

    // Draw 4 corners (unscaled — blit directly)
    if (T > 0 && L > 0) // top-left
        ops.blit_region(canvas, x, y, source_pixels, 0, 0, L, T);
    if (T > 0 && R > 0) // top-right
        ops.blit_region(canvas, x + w - R, y, source_pixels, pw - R, 0, R, T);
    if (B > 0 && L > 0) // bottom-left
        ops.blit_region(canvas, x, y + h - B, source_pixels, 0, ph - B, L, B);
    if (B > 0 && R > 0) // bottom-right
        ops.blit_region(canvas, x + w - R, y + h - B, source_pixels, pw - R, ph - B, R, B);

    // Draw 4 edges (stretched by tiling the 1-pixel-wide/tall source strip)
    // Top edge
    if (T > 0 && dst_cw > 0 && src_cw > 0) {
        for (int64_t dx = 0; dx < dst_cw; dx += src_cw) {
            int64_t bw = src_cw;
            if (dx + bw > dst_cw)
                bw = dst_cw - dx;
            ops.blit_region(canvas, x + L + dx, y, source_pixels, src_cx, 0, bw, T);
        }
    }
    // Bottom edge
    if (B > 0 && dst_cw > 0 && src_cw > 0) {
        for (int64_t dx = 0; dx < dst_cw; dx += src_cw) {
            int64_t bw = src_cw;
            if (dx + bw > dst_cw)
                bw = dst_cw - dx;
            ops.blit_region(
                canvas, x + L + dx, y + h - B, source_pixels, src_cx, ph - B, bw, B);
        }
    }
    // Left edge
    if (L > 0 && dst_ch > 0 && src_ch > 0) {
        for (int64_t dy = 0; dy < dst_ch; dy += src_ch) {
            int64_t bh = src_ch;
            if (dy + bh > dst_ch)
                bh = dst_ch - dy;
            ops.blit_region(canvas, x, y + T + dy, source_pixels, 0, src_cy, L, bh);
        }
    }
    // Right edge
    if (R > 0 && dst_ch > 0 && src_ch > 0) {
        for (int64_t dy = 0; dy < dst_ch; dy += src_ch) {
            int64_t bh = src_ch;
            if (dy + bh > dst_ch)
                bh = dst_ch - dy;
            ops.blit_region(
                canvas, x + w - R, y + T + dy, source_pixels, pw - R, src_cy, R, bh);
        }
    }

    // Center (tiled)
    if (dst_cw > 0 && dst_ch > 0 && src_cw > 0 && src_ch > 0) {
        for (int64_t dy = 0; dy < dst_ch; dy += src_ch) {
            int64_t bh = src_ch;
            if (dy + bh > dst_ch)
                bh = dst_ch - dy;
            for (int64_t dx = 0; dx < dst_cw; dx += src_cw) {
                int64_t bw = src_cw;
                if (dx + bw > dst_cw)
                    bw = dst_cw - dx;
                ops.blit_region(
                    canvas, x + L + dx, y + T + dy, source_pixels, src_cx, src_cy, bw, bh);
            }
        }
    }
}

//=============================================================================
// UIMenuList
//=============================================================================

/// @brief Menu-item buffer capacity in bytes, including its NUL terminator.
#define MENULIST_MAX_TEXT 128

/// @brief Private state stored in a runtime UIMenuList object.
typedef struct {
    void *vptr;
    int64_t x, y;
    int64_t item_height;
    char items[RT_UIMENULIST_MAX_ITEMS][MENULIST_MAX_TEXT];
    int64_t count;
    int64_t selected;
    int64_t text_color;
    int64_t selected_color;
    int64_t highlight_bg;
    void *font; ///< BitmapFont or NULL
    int8_t visible;
} rt_uimenulist_impl;

/// @brief Validate and cast an opaque UIMenuList handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return The UIMenuList payload when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static rt_uimenulist_impl *checked_menulist(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_UIMENULIST_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_uimenulist_impl *)ptr;
}

/// @brief GC finalizer: release the menu list's optional BitmapFont.
/// @param obj UIMenuList payload being finalized; `NULL` is accepted.
/// @details Item labels are inline byte buffers and require no release.
static void uimenulist_finalizer(void *obj) {
    rt_uimenulist_impl *menu = (rt_uimenulist_impl *)obj;
    if (!menu)
        return;
    ui_release_obj(menu->font);
    menu->font = NULL;
}

/// @brief Create an empty, visible vertical menu list.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param item_height Row height; nonpositive values select 16 and positive
///        values clamp to `[1, 16384]`.
/// @return A new UIMenuList reference, or `NULL` if allocation fails.
/// @details Selection begins at zero with default white/yellow/dark-gray
///          colors and no custom font.
void *rt_uimenulist_new(int64_t x, int64_t y, int64_t item_height) {
    rt_uimenulist_impl *menu = (rt_uimenulist_impl *)rt_obj_new_i64(
        RT_UIMENULIST_CLASS_ID, (int64_t)sizeof(rt_uimenulist_impl));
    if (!menu)
        return NULL;

    memset(menu, 0, sizeof(rt_uimenulist_impl));
    menu->x = x;
    menu->y = y;
    menu->item_height = item_height > 0 ? ui_clamp_dim(item_height) : 16;
    menu->selected = 0;
    menu->text_color = 0xFFFFFF;     // White
    menu->selected_color = 0xFFFF00; // Yellow
    menu->highlight_bg = 0x333333;   // Dark gray
    menu->visible = 1;
    rt_obj_set_finalizer(menu, uimenulist_finalizer);

    return menu;
}

/// @brief Append a text item to the menu list (max 64 items, 127 bytes each).
/// @param ptr UIMenuList to mutate; `NULL` is a no-op.
/// @param text Required runtime string copied into the next inline buffer.
/// @details Null text is ignored. Copying stops at embedded NUL and preserves
///          validated UTF-8 boundaries. The input is not retained. Adding past
///          RT_UIMENULIST_MAX_ITEMS raises an item-limit trap; a non-null invalid
///          menu handle raises a class trap.
void rt_uimenulist_add_item(void *ptr, rt_string text) {
    if (!ptr || !text)
        return;
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.AddItem: expected Zanna.Game.UI.MenuList");
    if (!menu)
        return;
    if (menu->count >= RT_UIMENULIST_MAX_ITEMS) {
        rt_trap("UIMenuList.AddItem: item limit exceeded (max 64)");
        return;
    }

    ui_copy_text(menu->items[menu->count], sizeof(menu->items[menu->count]), text);
    menu->count++;
}

/// @brief Remove all entries from the uimenulist.
/// @param ptr UIMenuList to clear; `NULL` is a no-op.
/// @details Resets count and selection without changing styling, visibility,
///          font, position, or row height. A non-null invalid handle raises a
///          runtime trap.
void rt_uimenulist_clear(void *ptr) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.Clear: expected Zanna.Game.UI.MenuList");
    if (!menu)
        return;
    menu->count = 0;
    menu->selected = 0;
}

/// @brief Set the selected item index, clamped to [0, count-1].
/// @param ptr UIMenuList to mutate; `NULL` is a no-op.
/// @param index Requested selection.
/// @details An empty menu stores zero. A non-null invalid handle raises a
///          runtime trap.
void rt_uimenulist_set_selected(void *ptr, int64_t index) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.SetSelected: expected Zanna.Game.UI.MenuList");
    if (!menu)
        return;
    if (menu->count == 0) {
        menu->selected = 0;
        return;
    }
    if (index < 0)
        index = 0;
    if (index >= menu->count)
        index = menu->count - 1;
    menu->selected = index;
}

/// @brief Return the zero-based index of the currently highlighted menu item.
/// @param ptr UIMenuList to query.
/// @return Stored selected index, or zero for a null or invalid handle.
/// @details Empty menus also report zero. A non-null invalid handle raises a
///          runtime trap.
int64_t rt_uimenulist_get_selected(void *ptr) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.Selected: expected Zanna.Game.UI.MenuList");
    return menu ? menu->selected : 0;
}

/// @brief Move the selection cursor up by one; wraps from first to last item.
/// @param ptr UIMenuList to mutate; `NULL` is a no-op.
/// @details Empty menus are unchanged. A non-null invalid handle raises a
///          runtime trap.
void rt_uimenulist_move_up(void *ptr) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.MoveUp: expected Zanna.Game.UI.MenuList");
    if (!menu)
        return;
    if (menu->count == 0)
        return;
    if (menu->selected <= 0)
        menu->selected = menu->count - 1; // Wrap to bottom
    else
        menu->selected--;
}

/// @brief Move the selection cursor down by one; wraps from last to first item.
/// @param ptr UIMenuList to mutate; `NULL` is a no-op.
/// @details Empty menus are unchanged. A non-null invalid handle raises a
///          runtime trap.
void rt_uimenulist_move_down(void *ptr) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.MoveDown: expected Zanna.Game.UI.MenuList");
    if (!menu)
        return;
    if (menu->count == 0)
        return;
    if (menu->selected >= menu->count - 1)
        menu->selected = 0; // Wrap to top
    else
        menu->selected++;
}

/// @brief Set text/selection colors. `text_color` is the unselected entry color, `selected_color`
/// is the highlighted entry, `highlight_bg` is the row background drawn behind the selection.
/// @param ptr UIMenuList to mutate; `NULL` is a no-op.
/// @param text_color Packed unselected-label color.
/// @param selected_color Packed selected-label color.
/// @param highlight_bg Packed selected-row background color.
/// @details Values are stored verbatim. A non-null invalid handle raises a
///          runtime trap.
void rt_uimenulist_set_colors(void *ptr,
                              int64_t text_color,
                              int64_t selected_color,
                              int64_t highlight_bg) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.SetColors: expected Zanna.Game.UI.MenuList");
    if (!menu)
        return;
    menu->text_color = text_color;
    menu->selected_color = selected_color;
    menu->highlight_bg = highlight_bg;
}

/// @brief Assign a BitmapFont for menu item text; NULL uses the default font.
/// @param ptr UIMenuList to mutate; `NULL` is a no-op.
/// @param font Optional BitmapFont reference.
/// @details A valid replacement is retained before the prior font is released.
///          A wrong font class or non-null invalid menu raises a runtime trap.
void rt_uimenulist_set_font(void *ptr, void *font) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.SetFont: expected Zanna.Game.UI.MenuList");
    if (!menu)
        return;
    if (!ui_validate_bitmapfont(font, "UIMenuList.SetFont: expected BitmapFont"))
        return;
    ui_replace_ref(&menu->font, font);
}

/// @brief Show or hide the menu list; hidden menus are skipped during draw.
/// @param ptr UIMenuList to mutate; `NULL` is a no-op.
/// @param visible Zero hides the menu; any nonzero value shows it.
/// @details A non-null invalid handle raises a runtime trap.
void rt_uimenulist_set_visible(void *ptr, int8_t visible) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.SetVisible: expected Zanna.Game.UI.MenuList");
    if (menu)
        menu->visible = visible ? 1 : 0;
}

/// @brief Return the count of elements in the uimenulist.
/// @param ptr UIMenuList to query.
/// @return Number of stored items, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_uimenulist_get_count(void *ptr) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.Count: expected Zanna.Game.UI.MenuList");
    return menu ? menu->count : 0;
}

/// @brief Draw all visible menu rows and the current selection highlight.
/// @param ptr UIMenuList to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @details Null arguments, hidden menus, and empty menus are no-ops. The
///          selected highlight begins four pixels left of the label and spans
///          measured text width plus 16 pixels; labels render two pixels below
///          each row origin. Invalid non-null handles raise runtime traps.
void rt_uimenulist_draw(void *ptr, void *canvas) {
    rt_gameui_draw_ops_t ops;
    if (!ptr || !canvas)
        return;
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.Draw: expected Zanna.Game.UI.MenuList");
    if (!menu || !ui_resolve_draw_ops(canvas, "UIMenuList.Draw: expected Canvas or Canvas3D", &ops))
        return;
    if (!menu->visible || menu->count == 0)
        return;

    for (int64_t i = 0; i < menu->count; i++) {
        int64_t iy = menu->y + i * menu->item_height;
        int8_t is_selected = (i == menu->selected);

        // Highlight background for selected item
        if (is_selected) {
            // Use the full canvas width? No — use a reasonable highlight width.
            // Measure text to get width, or use a fixed highlight width.
            int64_t hw = 200; // Default highlight width
            rt_string rtext = rt_const_cstr(menu->items[i]);
            if (menu->font)
                hw = rt_bitmapfont_text_width(menu->font, rtext) + 16;
            else
                hw = rt_canvas_text_width(rtext) + 16;
            ops.box(canvas, menu->x - 4, iy, hw, menu->item_height, menu->highlight_bg);
        }

        int64_t color = is_selected ? menu->selected_color : menu->text_color;
        rt_string rtext = rt_const_cstr(menu->items[i]);

        if (menu->font)
            ops.text_font(canvas, menu->x, iy + 2, rtext, menu->font, color);
        else
            ops.text(canvas, menu->x, iy + 2, rtext, color);
    }
}

/// @brief Handle input for menu navigation. Returns selected index on confirm, -1 otherwise.
/// @param ptr UIMenuList to update.
/// @param up Nonzero requests one wrapped upward move.
/// @param down Nonzero requests one wrapped downward move.
/// @param confirm Nonzero returns the resulting selection.
/// @return Selected index when @p confirm is nonzero on a visible, nonempty
///         menu; otherwise `-1`.
/// @details Simultaneous up and down cancel navigation. A non-null invalid menu
///          handle raises a runtime trap.
int64_t rt_uimenulist_handle_input(void *ptr, int8_t up, int8_t down, int8_t confirm) {
    rt_uimenulist_impl *menu =
        checked_menulist(ptr, "UIMenuList.HandleInput: expected Zanna.Game.UI.MenuList");
    if (!menu)
        return -1;
    if (!menu->visible || menu->count == 0)
        return -1;

    if (up && !down) {
        menu->selected--;
        if (menu->selected < 0)
            menu->selected = menu->count - 1; // wrap
    }
    if (down && !up) {
        menu->selected++;
        if (menu->selected >= menu->count)
            menu->selected = 0; // wrap
    }
    if (confirm)
        return menu->selected;
    return -1;
}

//=============================================================================
// GameButton — standalone styled button for custom layouts
//=============================================================================

/// @brief Private inline state stored in a runtime GameButton object.
typedef struct {
    void *vptr;
    int64_t x, y, width, height;
    char text[64];
    int64_t text_scale;
    int64_t color_normal;
    int64_t color_selected;
    int64_t text_color;
    int64_t text_color_selected;
    int64_t border_color;
    int64_t border_width;
    int8_t visible;
} rt_gamebutton_impl;

/// @brief Validate and cast an opaque GameButton handle.
/// @param ptr Candidate handle; `NULL` is accepted.
/// @param api Trap message used for a non-null class mismatch.
/// @return The GameButton payload when valid; otherwise `NULL`.
/// @details A mismatched handle raises a runtime trap.
static rt_gamebutton_impl *checked_gamebutton(void *ptr, const char *api) {
    if (!ptr)
        return NULL;
    if (rt_obj_class_id(ptr) != RT_GAMEBUTTON_CLASS_ID) {
        rt_trap(api);
        return NULL;
    }
    return (rt_gamebutton_impl *)ptr;
}

/// @brief Construct a styled game button at (x, y) with size (w, h) and label text. Defaults:
/// dark gray normal, blue selected, light gray text, 1 px border, scale 1, visible. Text is
/// truncated to 63 bytes.
/// @param x Initial left coordinate.
/// @param y Initial top coordinate.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @param text Optional runtime string copied into the inline label buffer.
/// @return A new GameButton reference, or `NULL` if allocation fails.
/// @details The input text is not retained and UTF-8 truncation avoids splitting
///          a validated multibyte sequence.
void *rt_gamebutton_new(int64_t x, int64_t y, int64_t w, int64_t h, void *text) {
    rt_gamebutton_impl *btn = (rt_gamebutton_impl *)rt_obj_new_i64(
        RT_GAMEBUTTON_CLASS_ID, (int64_t)sizeof(rt_gamebutton_impl));
    if (!btn)
        return NULL;
    memset(btn, 0, sizeof(rt_gamebutton_impl));
    btn->x = x;
    btn->y = y;
    btn->width = ui_clamp_dim(w);
    btn->height = ui_clamp_dim(h);
    btn->text_scale = 1;
    btn->color_normal = 0x333333;
    btn->color_selected = 0x4444AA;
    btn->text_color = 0xCCCCCC;
    btn->text_color_selected = 0xFFFFFF;
    btn->border_color = 0x666666;
    btn->border_width = 1;
    btn->visible = 1;
    ui_copy_text(btn->text, sizeof(btn->text), (rt_string)text);
    return btn;
}

/// @brief Replace the button's label text (truncated to 63 bytes).
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param text Optional runtime string to copy; `NULL` clears the label.
/// @details The input is not retained. A non-null invalid button handle raises
///          a runtime trap.
void rt_gamebutton_set_text(void *ptr, void *text) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetText: expected Zanna.Game.UI.GameButton");
    if (!btn)
        return;
    ui_copy_text(btn->text, sizeof(btn->text), (rt_string)text);
}

/// @brief Set the box-fill colors for unselected (`normal`) and selected (highlighted) states.
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param normal Packed normal-state fill color.
/// @param selected Packed selected-state fill color.
/// @details Values are stored verbatim. A non-null invalid handle raises a
///          runtime trap.
void rt_gamebutton_set_colors(void *ptr, int64_t normal, int64_t selected) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetColors: expected Zanna.Game.UI.GameButton");
    if (!btn)
        return;
    btn->color_normal = normal;
    btn->color_selected = selected;
}

/// @brief Set the text colors for unselected and selected states.
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param normal Packed normal-state text color.
/// @param selected Packed selected-state text color.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_text_colors(void *ptr, int64_t normal, int64_t selected) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetTextColors: expected Zanna.Game.UI.GameButton");
    if (!btn)
        return;
    btn->text_color = normal;
    btn->text_color_selected = selected;
}

/// @brief Set border outline width (px) and color. Width 0 disables the border.
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param width Requested thickness; nonpositive values become zero. Drawing
///        further limits it to half the smaller button dimension.
/// @param color Packed border color; zero also suppresses drawing.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_border(void *ptr, int64_t width, int64_t color) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetBorder: expected Zanna.Game.UI.GameButton");
    if (!btn)
        return;
    btn->border_width = width > 0 ? width : 0;
    btn->border_color = color;
}

/// @brief Read the button's screen X position.
/// @param ptr GameButton to query.
/// @return Stored X coordinate, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_gamebutton_get_x(void *ptr) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.X: expected Zanna.Game.UI.GameButton");
    return btn ? btn->x : 0;
}

/// @brief Read the button's screen Y position.
/// @param ptr GameButton to query.
/// @return Stored Y coordinate, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_gamebutton_get_y(void *ptr) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.Y: expected Zanna.Game.UI.GameButton");
    return btn ? btn->y : 0;
}

/// @brief Read the button width in pixels.
/// @param ptr GameButton to query.
/// @return Positive stored width, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_gamebutton_get_width(void *ptr) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.Width: expected Zanna.Game.UI.GameButton");
    return btn ? btn->width : 0;
}

/// @brief Read the button height in pixels.
/// @param ptr GameButton to query.
/// @return Positive stored height, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_gamebutton_get_height(void *ptr) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.Height: expected Zanna.Game.UI.GameButton");
    return btn ? btn->height : 0;
}

/// @brief Set the button's screen X position.
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param v New X coordinate stored verbatim.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_x(void *ptr, int64_t v) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetX: expected Zanna.Game.UI.GameButton");
    if (btn)
        btn->x = v;
}

/// @brief Set the button's screen Y position.
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param v New Y coordinate stored verbatim.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_y(void *ptr, int64_t v) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetY: expected Zanna.Game.UI.GameButton");
    if (btn)
        btn->y = v;
}

/// @brief Resize the button, clamping dimensions to the UI maximum.
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param w Width clamped to `[1, 16384]`.
/// @param h Height clamped to `[1, 16384]`.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_size(void *ptr, int64_t w, int64_t h) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetSize: expected Zanna.Game.UI.GameButton");
    if (!btn)
        return;
    btn->width = ui_clamp_dim(w);
    btn->height = ui_clamp_dim(h);
}

/// @brief Toggle visibility.
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param visible Zero hides the button; any nonzero value shows it.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_visible(void *ptr, int8_t visible) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetVisible: expected Zanna.Game.UI.GameButton");
    if (btn)
        btn->visible = visible ? 1 : 0;
}

/// @brief Return 1 if the button is visible.
/// @param ptr GameButton to query.
/// @return Normalized visible flag, or zero for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int8_t rt_gamebutton_get_visible(void *ptr) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.Visible: expected Zanna.Game.UI.GameButton");
    return btn ? btn->visible : 0;
}

/// @brief Set integer text scale, clamped to [1, 16].
/// @param ptr GameButton to mutate; `NULL` is a no-op.
/// @param scale Requested integer multiplier.
/// @details A non-null invalid handle raises a runtime trap.
void rt_gamebutton_set_text_scale(void *ptr, int64_t scale) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.SetTextScale: expected Zanna.Game.UI.GameButton");
    if (btn)
        btn->text_scale = ui_clamp_scale(scale);
}

/// @brief Return the current integer text scale.
/// @param ptr GameButton to query.
/// @return Stored scale, or one for a null or invalid handle.
/// @details A non-null invalid handle raises a runtime trap.
int64_t rt_gamebutton_get_text_scale(void *ptr) {
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.TextScale: expected Zanna.Game.UI.GameButton");
    return btn ? btn->text_scale : 1;
}

/// @brief Render the button to `canvas`. Picks colors based on `is_selected`, draws box +
/// optional border + centered label. No-op if button is hidden.
/// @param ptr GameButton to render.
/// @param canvas 2D Canvas or registered Canvas3D target.
/// @param is_selected Nonzero selects highlighted fill and text colors.
/// @details Text is clipped to the number of default eight-pixel cells that
///          fit at the current scale, then centered. If no cell fits, the
///          already-drawn box and border remain without text. Null arguments
///          and hidden buttons are no-ops; invalid non-null handles trap.
void rt_gamebutton_draw(void *ptr, void *canvas, int8_t is_selected) {
    rt_gameui_draw_ops_t ops;
    if (!ptr || !canvas)
        return;
    rt_gamebutton_impl *btn =
        checked_gamebutton(ptr, "GameButton.Draw: expected Zanna.Game.UI.GameButton");
    if (!btn || !ui_resolve_draw_ops(canvas, "GameButton.Draw: expected Canvas or Canvas3D", &ops))
        return;
    if (!btn->visible)
        return;

    int64_t bg = is_selected ? btn->color_selected : btn->color_normal;
    int64_t tc = is_selected ? btn->text_color_selected : btn->text_color;

    // Background
    ops.box(canvas, btn->x, btn->y, btn->width, btn->height, bg);

    // Border
    if (btn->border_width > 0 && btn->border_color != 0) {
        int64_t max_t = (btn->width < btn->height ? btn->width : btn->height) / 2;
        if (max_t < 1)
            max_t = 1;
        int64_t border = btn->border_width > max_t ? max_t : btn->border_width;
        for (int64_t t = 0; t < border; t++) {
            int64_t bw = btn->width - t * 2;
            int64_t bh = btn->height - t * 2;
            if (bw <= 0 || bh <= 0)
                break;
            ops.frame(canvas, btn->x + t, btn->y + t, bw, bh, btn->border_color);
        }
    }

    // Centered text
    if (btn->text[0] != '\0') {
        int64_t scale = ui_clamp_scale(btn->text_scale);
        int64_t cell_w = 8 * scale;
        int64_t max_codepoints = cell_w > 0 ? btn->width / cell_w : 0;
        if (max_codepoints <= 0)
            return;
        char clipped[sizeof(btn->text)];
        size_t text_len = ui_visible_len(btn->text, sizeof(btn->text));
        size_t clipped_len = ui_utf8_trunc_codepoints(btn->text, text_len, (size_t)max_codepoints);
        memcpy(clipped, btn->text, clipped_len);
        clipped[clipped_len] = '\0';
        rt_string rtext = rt_const_cstr(clipped);
        int64_t text_w = rt_canvas_text_scaled_width(rtext, scale);
        int64_t text_h = rt_canvas_text_height() * scale;
        int64_t text_x = btn->x + (btn->width - text_w) / 2;
        int64_t text_y = btn->y + (btn->height - text_h) / 2;
        if (scale > 1)
            ops.text_scaled(canvas, text_x, text_y, rtext, scale, tc);
        else
            ops.text(canvas, text_x, text_y, rtext, tc);
    }
}

//=============================================================================
// Runtime-additions shared constants
//=============================================================================


/// @brief Number of UTF-8 codepoints in the first @p bytes of @p text.
/// @param text Byte sequence to scan.
/// @param bytes Number of readable bytes; nonpositive values return zero.
/// @return Count of validated codepoints plus malformed single-byte units.
int64_t ui_codepoint_count_bytes(const char *text, int64_t bytes) {
    if (!text || bytes <= 0)
        return 0;
    size_t pos = 0;
    int64_t count = 0;
    while (pos < (size_t)bytes) {
        size_t len = ui_utf8_cp_len(text, (size_t)bytes, pos);
        if (pos + len > (size_t)bytes)
            break;
        pos += len;
        count++;
    }
    return count;
}

/// @brief Byte offset of the @p cp_index-th codepoint (clamped to @p bytes).
/// @param text Byte sequence to scan.
/// @param bytes Number of readable bytes.
/// @param cp_index Zero-based codepoint boundary count.
/// @return Zero for null/nonpositive inputs; otherwise the boundary after at
///         most @p cp_index units, capped at @p bytes.
int64_t ui_byte_for_codepoint(const char *text, int64_t bytes, int64_t cp_index) {
    if (!text || bytes <= 0 || cp_index <= 0)
        return 0;
    size_t pos = 0;
    int64_t count = 0;
    while (pos < (size_t)bytes && count < cp_index) {
        size_t len = ui_utf8_cp_len(text, (size_t)bytes, pos);
        if (pos + len > (size_t)bytes)
            break;
        pos += len;
        count++;
    }
    return (int64_t)pos;
}

/// @brief Codepoint index containing/preceding byte offset @p byte_index
///        (inverse of ui_byte_for_codepoint).
/// @param text Byte sequence to scan.
/// @param bytes Number of readable bytes.
/// @param byte_index Byte boundary to map, clamped to @p bytes.
/// @return Number of complete UTF-8 units ending at or before the boundary.
int64_t ui_codepoint_for_byte(const char *text, int64_t bytes, int64_t byte_index) {
    if (!text || bytes <= 0 || byte_index <= 0)
        return 0;
    if (byte_index > bytes)
        byte_index = bytes;
    size_t pos = 0;
    int64_t count = 0;
    while (pos < (size_t)byte_index) {
        size_t len = ui_utf8_cp_len(text, (size_t)bytes, pos);
        if (pos + len > (size_t)bytes || pos + len > (size_t)byte_index)
            break;
        pos += len;
        count++;
    }
    return count;
}

/// @brief Byte offset of the codepoint immediately before @p byte_index
///        (for left-arrow / backspace caret movement).
/// @param text Byte sequence to scan.
/// @param bytes Number of readable bytes.
/// @param byte_index Current byte position, clamped to @p bytes.
/// @return Start of the unit containing or immediately preceding the position,
///         or zero for null/nonpositive inputs.
int64_t ui_prev_codepoint_byte(const char *text, int64_t bytes, int64_t byte_index) {
    if (!text || bytes <= 0 || byte_index <= 0)
        return 0;
    if (byte_index > bytes)
        byte_index = bytes;
    int64_t prev = 0;
    int64_t pos = 0;
    while (pos < byte_index) {
        prev = pos;
        size_t len = ui_utf8_cp_len(text, (size_t)bytes, (size_t)pos);
        if (pos + (int64_t)len >= byte_index)
            break;
        pos += (int64_t)len;
    }
    return prev;
}

/// @brief Byte offset of the codepoint immediately after @p byte_index
///        (for right-arrow / delete caret movement).
/// @param text Byte sequence to scan.
/// @param bytes Number of readable bytes.
/// @param byte_index Current unit start; negative values become zero.
/// @return Next validated/malformed-unit boundary capped at @p bytes, or the
///         nonnegative byte length when already at or beyond the end.
int64_t ui_next_codepoint_byte(const char *text, int64_t bytes, int64_t byte_index) {
    if (!text || bytes <= 0 || byte_index >= bytes)
        return bytes > 0 ? bytes : 0;
    if (byte_index < 0)
        byte_index = 0;
    size_t len = ui_utf8_cp_len(text, (size_t)bytes, (size_t)byte_index);
    int64_t next = byte_index + (int64_t)len;
    return next > bytes ? bytes : next;
}

/// @brief Rendered pixel width of the first @p bytes of @p text in @p font at
///        @p scale (used to position the caret/selection).
/// @param text Byte sequence to measure.
/// @param bytes Prefix size, clamped to 511; nonpositive values return zero.
/// @param font Optional BitmapFont; `NULL` selects canvas-default metrics.
/// @param scale Integer scale clamped to `[1, 16]`.
/// @return Measured prefix width multiplied by the normalized scale.
/// @details The copied prefix is NUL-terminated, so embedded NUL shortens the
///          measured text.
int64_t ui_text_prefix_width(const char *text, int64_t bytes, void *font, int64_t scale) {
    if (!text || bytes <= 0)
        return 0;
    if (bytes > 511)
        bytes = 511;
    char tmp[512];
    memcpy(tmp, text, (size_t)bytes);
    tmp[bytes] = '\0';
    rt_string s = rt_const_cstr(tmp);
    int64_t width = font ? rt_bitmapfont_text_width(font, s) : rt_canvas_text_width(s);
    return width * ui_clamp_scale(scale);
}

/// @brief Draw a text run at (x, y) using an optional bitmap font and scale,
///        falling back to the canvas default font when none is set.
/// @param ops Resolved operations and target canvas.
/// @param x Left coordinate.
/// @param y Top coordinate.
/// @param text Nonempty NUL-terminated text.
/// @param font Optional BitmapFont passed to font-aware operations.
/// @param scale Integer multiplier clamped to `[1, 16]`.
/// @param color Packed text color.
/// @details Null operations, null text, and empty text are no-ops.
void ui_draw_text_basic(const rt_gameui_draw_ops_t *ops,
                        int64_t x,
                        int64_t y,
                        const char *text,
                        void *font,
                        int64_t scale,
                        int64_t color) {
    if (!ops || !text || text[0] == '\0')
        return;
    rt_string s = rt_const_cstr(text);
    scale = ui_clamp_scale(scale);
    if (font) {
        if (scale > 1)
            ops->text_font_scaled(ops->canvas, x, y, s, font, scale, color);
        else
            ops->text_font(ops->canvas, x, y, s, font, color);
    } else if (scale > 1) {
        ops->text_scaled(ops->canvas, x, y, s, scale, color);
    } else {
        ops->text(ops->canvas, x, y, s, color);
    }
}

/// @brief True if point lies within the axis-aligned widget rect (used for
///        hit-testing clicks/hovers).
/// @param x Rectangle left coordinate.
/// @param y Rectangle top coordinate.
/// @param w Rectangle width.
/// @param h Rectangle height.
/// @param px Point X coordinate.
/// @param py Point Y coordinate.
/// @return `1` when the point lies in both half-open coordinate spans;
///         otherwise `0`.
int8_t ui_point_inside(int64_t x, int64_t y, int64_t w, int64_t h, int64_t px, int64_t py) {
    return ui_coord_inside(x, w, px) && ui_coord_inside(y, h, py);
}
