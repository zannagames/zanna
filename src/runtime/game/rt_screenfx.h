//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/game/rt_screenfx.h
/// @file
/// @brief Declares the ScreenFX C ABI for camera shake, color overlays, and
///        Canvas-rendered transitions.
// Purpose: Screen effects manager for camera shake, color flash, and screen fades. It reserves
// RT_SCREENFX_MAX_EFFECTS (8) slots initially and grows when more concurrent effects are needed.
//
// Key invariants:
//   - Durations and update deltas are integer milliseconds. Shake intensity and
//     returned offsets are integer pixel displacements.
//   - Flash/fade colors are packed as 0xRRGGBBAA; transition colors use the
//     Canvas 0x00RRGGBB convention.
//   - rt_screenfx_update must be called once per frame with the elapsed delta time.
//   - Non-positive update deltas are ignored; elapsed time saturates instead of overflowing.
//   - Initial concurrent-effect reservation is RT_SCREENFX_MAX_EFFECTS (8); storage grows on
//   demand.
//
// Ownership/Lifetime:
//   - GC-managed via rt_obj_new_i64; explicit destroy releases the caller's reference.
//
// Links: src/runtime/game/rt_screenfx.c (implementation), src/runtime/graphics/rt_camera.h,
// src/runtime/game/rt_particle.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Initial number of reserved concurrent effect slots.
#define RT_SCREENFX_MAX_EFFECTS 8

/// @brief Runtime class identifier used to validate ScreenFX handles.
#define RT_SCREENFX_CLASS_ID INT64_C(-0x510212)

/// @brief Stable effect kinds stored in ScreenFX slots.
typedef enum {
    RT_SCREENFX_NONE = 0,
    RT_SCREENFX_SHAKE = 1,
    RT_SCREENFX_FLASH = 2,
    RT_SCREENFX_FADE_IN = 3,
    RT_SCREENFX_FADE_OUT = 4,
    RT_SCREENFX_WIPE = 5,
    RT_SCREENFX_CIRCLE_IN = 6,
    RT_SCREENFX_CIRCLE_OUT = 7,
    RT_SCREENFX_DISSOLVE = 8,
    RT_SCREENFX_PIXELATE = 9,
} rt_screenfx_type_t;

/// @brief Wipe grows from the left edge toward the right.
#define RT_DIR_LEFT 0
/// @brief Wipe grows from the right edge toward the left.
#define RT_DIR_RIGHT 1
/// @brief Wipe grows from the top edge toward the bottom.
#define RT_DIR_UP 2
/// @brief Wipe grows from the bottom edge toward the top.
#define RT_DIR_DOWN 3

/// @brief Opaque handle to a ScreenFX manager.
typedef struct rt_screenfx_impl *rt_screenfx;

/// @name Effect-type constants
/// Stable numeric identifiers for `IsTypeActive`/`CancelType`, mirroring the
/// private @c rt_screenfx_type_t enum so callers never copy raw integers.
/// @{
/// @brief Return the camera-shake effect identifier.
/// @return @ref RT_SCREENFX_SHAKE.
int64_t rt_screenfx_type_shake(void);      ///< 1
/// @brief Return the color-flash effect identifier.
/// @return @ref RT_SCREENFX_FLASH.
int64_t rt_screenfx_type_flash(void);      ///< 2
/// @brief Return the fade-from-color effect identifier.
/// @return @ref RT_SCREENFX_FADE_IN.
int64_t rt_screenfx_type_fade_in(void);    ///< 3
/// @brief Return the fade-to-color effect identifier.
/// @return @ref RT_SCREENFX_FADE_OUT.
int64_t rt_screenfx_type_fade_out(void);   ///< 4
/// @brief Return the directional-wipe effect identifier.
/// @return @ref RT_SCREENFX_WIPE.
int64_t rt_screenfx_type_wipe(void);       ///< 5
/// @brief Return the closing-circle effect identifier.
/// @return @ref RT_SCREENFX_CIRCLE_IN.
int64_t rt_screenfx_type_circle_in(void);  ///< 6
/// @brief Return the opening-circle effect identifier.
/// @return @ref RT_SCREENFX_CIRCLE_OUT.
int64_t rt_screenfx_type_circle_out(void); ///< 7
/// @brief Return the ordered-dissolve effect identifier.
/// @return @ref RT_SCREENFX_DISSOLVE.
int64_t rt_screenfx_type_dissolve(void);   ///< 8
/// @brief Return the pixelate-grid effect identifier.
/// @return @ref RT_SCREENFX_PIXELATE.
int64_t rt_screenfx_type_pixelate(void);   ///< 9
/// @}

/// @name Wipe-direction constants
/// Stable values for the @c direction argument of `Wipe`.
/// @{
/// @brief Return the left-origin wipe direction.
/// @return @ref RT_DIR_LEFT.
int64_t rt_screenfx_dir_left(void);  ///< 0
/// @brief Return the right-origin wipe direction.
/// @return @ref RT_DIR_RIGHT.
int64_t rt_screenfx_dir_right(void); ///< 1
/// @brief Return the top-origin wipe direction.
/// @return @ref RT_DIR_UP.
int64_t rt_screenfx_dir_up(void);    ///< 2
/// @brief Return the bottom-origin wipe direction.
/// @return @ref RT_DIR_DOWN.
int64_t rt_screenfx_dir_down(void);  ///< 3
/// @}

/// @brief Pack an overlay color for Flash/FadeIn/FadeOut in the ScreenFX
///   @c 0xRRGGBBAA byte order (alpha in the least-significant byte).
///
/// ScreenFX overlay colors do NOT use the canonical @c Zanna.Graphics.Color
/// byte order (@c 0xAARRGGBB) — passing a @c Color.Rgba() value directly to
/// Flash/FadeIn/FadeOut reads the wrong alpha channel. Use this constructor so
/// the encoding is explicit and correct at the call site.
/// @param r Red channel, clamped to [0, 255].
/// @param g Green channel, clamped to [0, 255].
/// @param b Blue channel, clamped to [0, 255].
/// @param a Alpha channel, clamped to [0, 255].
/// @return The color packed as 0xRRGGBBAA.
int64_t rt_screenfx_rgba(int64_t r, int64_t g, int64_t b, int64_t a);

/// @brief Pack a transition color for Wipe/CircleIn/CircleOut/Dissolve/Pixelate
///   in the Canvas @c 0x00RRGGBB byte order (no alpha).
/// @param r Red channel, clamped to [0, 255].
/// @param g Green channel, clamped to [0, 255].
/// @param b Blue channel, clamped to [0, 255].
/// @return The color packed as 0x00RRGGBB.
int64_t rt_screenfx_rgb(int64_t r, int64_t g, int64_t b);

/// @brief Allocates and initializes a new ScreenFX effects manager.
/// @details Reserves @ref RT_SCREENFX_MAX_EFFECTS zeroed slots and seeds the
///          per-instance shake generator from the active runtime RNG.
/// @return Owned ScreenFX handle with no active effects, or `NULL` if
///         allocation fails.
rt_screenfx rt_screenfx_new(void);

/// @brief Destroys a ScreenFX manager and releases its memory.
/// @param fx The effects manager to destroy. Passing NULL is a no-op.
void rt_screenfx_destroy(rt_screenfx fx);

/// @brief Advances all active screen effects by the given time delta.
///
/// Decays shake intensity, advances flash/fade timers, and removes expired
/// effects. An effect's terminal frame remains observable until the following
/// positive update reclaims its slot.
/// @param fx The effects manager to update.
/// @param dt Elapsed time since the last update in integer milliseconds;
///   nonpositive values are ignored.
void rt_screenfx_update(rt_screenfx fx, int64_t dt);

/// @brief Starts a camera shake effect with configurable intensity and
///   decay.
///
/// Produces random per-frame offsets that the renderer should add to the
/// camera position. The offsets decay over the duration.
/// @param fx The effects manager.
/// @param intensity Maximum per-axis displacement in integer pixels;
///   negative values clamp to zero.
/// @param duration Duration of the shake in milliseconds.
/// @param decay Decay selector: values <= 0 keep constant intensity, values
///   from 1 through 1499 use linear falloff, and values >= 1500 use quadratic
///   falloff.
void rt_screenfx_shake(rt_screenfx fx, int64_t intensity, int64_t duration, int64_t decay);

/// @brief Starts a full-screen color flash effect.
///
/// The overlay appears instantly at the given color and fades out over the
/// duration. Useful for damage indicators, pickups, or impact feedback.
///
/// @note Color format: packed @b 0xRRGGBBAA (alpha in the least-significant
///   byte). This differs from the @c 0x00RRGGBB format used by Canvas
///   drawing methods. Example: a full-opacity white flash is @c 0xFFFFFFFF;
///   a half-opacity red flash is @c 0xFF000080.
/// @param fx The effects manager.
/// @param color Flash color in packed 0xRRGGBBAA format.
/// @param duration Duration of the flash in milliseconds.
void rt_screenfx_flash(rt_screenfx fx, int64_t color, int64_t duration);

/// @brief Starts a fade-in effect (from an opaque color overlay to fully
///   transparent).
///
/// Commonly used for scene transitions where the screen starts covered
/// and gradually reveals the scene.
///
/// @note Color format: packed @b 0xRRGGBBAA — alpha in the least-significant
///   byte. Example: fade in from opaque black: @c 0x000000FF.
/// @param fx The effects manager.
/// @param color Starting overlay color in packed 0xRRGGBBAA format.
/// @param duration Duration of the fade in milliseconds.
void rt_screenfx_fade_in(rt_screenfx fx, int64_t color, int64_t duration);

/// @brief Starts a fade-out effect (from fully transparent to an opaque
///   color overlay).
///
/// Commonly used for scene transitions where the screen gradually becomes
/// covered before switching scenes.
///
/// @note Color format: packed @b 0xRRGGBBAA — alpha in the least-significant
///   byte. Example: fade to opaque black: @c 0x000000FF.
/// @param fx The effects manager.
/// @param color Target overlay color in packed 0xRRGGBBAA format.
/// @param duration Duration of the fade in milliseconds.
void rt_screenfx_fade_out(rt_screenfx fx, int64_t color, int64_t duration);

/// @brief Immediately cancels all active effects.
///
/// Resets shake offsets to zero and overlay alpha to transparent.
/// @param fx The effects manager.
void rt_screenfx_cancel_all(rt_screenfx fx);

/// @brief Cancels all active effects of a specific type.
/// @param fx The effects manager.
/// @param type The effect type to cancel, as an rt_screenfx_type_t value
///   (e.g., RT_SCREENFX_SHAKE, RT_SCREENFX_FLASH).
void rt_screenfx_cancel_type(rt_screenfx fx, int64_t type);

/// @brief Queries whether any screen effect is currently running.
/// @param fx The effects manager to query.
/// @return 1 if at least one effect is active, 0 if all effects have
///   expired or been cancelled.
int8_t rt_screenfx_is_active(rt_screenfx fx);

/// @brief Queries whether a specific effect type is currently running.
/// @param fx The effects manager to query.
/// @param type The effect type to check, as an rt_screenfx_type_t value.
/// @return 1 if at least one effect of this type is active, 0 otherwise.
int8_t rt_screenfx_is_type_active(rt_screenfx fx, int64_t type);

/// @brief Retrieves the current horizontal camera shake offset.
///
/// The renderer should add this offset to the camera X position each frame
/// to produce the shake effect.
/// @param fx The effects manager to query.
/// @return X displacement in fixed-point pixels (1000 = 1 pixel). Zero
///   when no shake is active.
int64_t rt_screenfx_get_shake_x(rt_screenfx fx);

/// @brief Retrieves the current vertical camera shake offset.
///
/// The renderer should add this offset to the camera Y position each frame
/// to produce the shake effect.
/// @param fx The effects manager to query.
/// @return Y displacement in fixed-point pixels (1000 = 1 pixel). Zero
///   when no shake is active.
int64_t rt_screenfx_get_shake_y(rt_screenfx fx);

/// @brief Retrieves the current overlay color for flash and fade effects.
///
/// The renderer should draw a filled rectangle with this color over the
/// entire screen at the alpha returned by rt_screenfx_get_overlay_alpha().
/// @param fx The effects manager to query.
/// @return The overlay RGB channels in the upper 24 bits (`0xRRGGBB00`);
///   retrieve the current alpha separately. Returns 0 when no flash or fade
///   effect has contributed during the latest positive update.
int64_t rt_screenfx_get_overlay_color(rt_screenfx fx);

/// @brief Retrieves the current overlay alpha intensity for flash and fade
///   effects.
/// @param fx The effects manager to query.
/// @return Alpha value in [0, 255], where 0 is fully transparent and 255
///   is fully opaque. Returns 0 when no flash or fade effect is active.
int64_t rt_screenfx_get_overlay_alpha(rt_screenfx fx);

//=========================================================================
// Transition Effects (Wipe, Circle, Dissolve, Pixelate)
//=========================================================================

/// @brief Start a directional wipe transition.
/// @details The colored rectangle grows from the selected edge; out-of-range
///          directions default to @ref RT_DIR_LEFT.
/// @param fx The effects manager.
/// @param direction RT_DIR_LEFT/RIGHT/UP/DOWN.
/// @param color Overlay color (0xRRGGBB).
/// @param duration Positive duration in milliseconds.
void rt_screenfx_wipe(rt_screenfx fx, int64_t direction, int64_t color, int64_t duration);

/// @brief Start a circle-closing transition from a center point.
/// @details A scanline-rendered clear circular opening shrinks until the Canvas
///          is fully covered.
/// @param fx The effects manager.
/// @param cx Horizontal aperture center in Canvas coordinates.
/// @param cy Vertical aperture center in Canvas coordinates.
/// @param color Overlay color in 0x00RRGGBB format.
/// @param duration Positive duration in milliseconds.
void rt_screenfx_circle_in(rt_screenfx fx, int64_t cx, int64_t cy, int64_t color, int64_t duration);

/// @brief Start a circle-opening transition from a center point.
/// @details A scanline-rendered clear circular opening grows until the Canvas
///          is fully revealed.
/// @param fx The effects manager.
/// @param cx Horizontal aperture center in Canvas coordinates.
/// @param cy Vertical aperture center in Canvas coordinates.
/// @param color Overlay color in 0x00RRGGBB format.
/// @param duration Positive duration in milliseconds.
void rt_screenfx_circle_out(
    rt_screenfx fx, int64_t cx, int64_t cy, int64_t color, int64_t duration);

/// @brief Start a dissolve transition (random pixel coverage via Bayer dithering).
/// @details Coverage follows a deterministic 4-by-4 ordered threshold pattern.
/// @param fx The effects manager.
/// @param color Overlay color in 0x00RRGGBB format.
/// @param duration Positive duration in milliseconds.
void rt_screenfx_dissolve(rt_screenfx fx, int64_t color, int64_t duration);

/// @brief Start a pixelation transition (increasing block size).
/// @details Draws a progressively darker grid approximation because the
///          runtime does not read Canvas pixels back for block averaging.
/// @param fx The effects manager.
/// @param max_block_size Maximum block size in pixels at full effect.
/// @param duration Positive duration in milliseconds.
void rt_screenfx_pixelate(rt_screenfx fx, int64_t max_block_size, int64_t duration);

/// @brief Check if all effects have finished (none active).
/// @param fx The effects manager; `NULL` is treated as finished.
/// @return `1` when no slots are active; otherwise `0`.
int8_t rt_screenfx_is_finished(rt_screenfx fx);

/// @brief Get the progress of the first active transition (0-1000 per mille).
/// The frame a transition reaches its duration it is held one extra update at
/// its terminal state, so the value 1000 (fully covered) is observable before
/// the slot is reclaimed on the following update. Returns 0 when no transition
/// is active.
/// @param fx The effects manager.
/// @return First transition slot's progress in the inclusive range 0..1000.
int64_t rt_screenfx_get_transition_progress(rt_screenfx fx);

/// @brief Render all active transition overlays to a Canvas.
/// @details Also draws the cached flash/fade overlay computed by the latest
///          positive update; shake offsets remain caller-applied.
/// @param fx The effects manager.
/// @param canvas Canvas handle.
/// @param screen_w Screen width in pixels.
/// @param screen_h Screen height in pixels.
void rt_screenfx_draw(rt_screenfx fx, void *canvas, int64_t screen_w, int64_t screen_h);

/// @brief Horizontal half-chord of a disc of @p radius at vertical offset @p dy
///   from the center — the half-width of the circular opening on that scanline,
///   or 0 when the row is entirely outside the disc. Internal helper backing the
///   CircleIn/CircleOut mask; exposed for tests, not a registered runtime API.
/// @param radius Disc radius.
/// @param dy Signed vertical distance from the center.
/// @return Non-negative half-chord length.
int64_t rt_screenfx_circle_half_chord(int64_t radius, int64_t dy);

#ifdef __cplusplus
}
#endif
