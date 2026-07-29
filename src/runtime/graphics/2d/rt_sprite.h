//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/graphics/2d/rt_sprite.h
// Purpose: Public C ABI for reference-counted 2D sprites and named animation
// clip controllers. Sprites retain one or more Pixels frames and expose
// position, integer origin, percentage scale, rotation, depth, visibility,
// flipping, timed frame animation, Canvas drawing, and AABB queries.
//
// Key invariants:
//   - Position is the destination point for the sprite's scaled integer-pixel
//     origin; the default origin is the current frame's top-left corner.
//   - Scale uses integer percentages (100 = unchanged) and is clamped to at
//     least 1. Rotation is stored as an unnormalized signed degree value.
//   - Frame indices wrap modulo frame count when explicitly set.
//   - Drawing and hit testing skip invisible sprites. AABB queries account for
//     scale and origin but intentionally ignore rotation and flipping.
//
// Ownership/Lifetime:
//   - Constructors return caller-owned runtime reference-counted objects.
//   - A sprite retains every Pixels frame supplied to it and releases those
//     references in its finalizer.
//   - SpriteAnimator objects are also runtime-managed; destroy releases one
//     reference and only frees the object when its reference count reaches zero.
//
// Links: src/runtime/graphics/2d/rt_sprite.c (implementation)
//
//===----------------------------------------------------------------------===//
#pragma once

#include "rt_string.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @file
/// @brief Declares the 2D Sprite and SpriteAnimator runtime interfaces.

/// Runtime class tag used to validate opaque Sprite handles.
#define RT_SPRITE_CLASS_ID INT64_C(-0x520101)

//=========================================================================
// Sprite Creation
//=========================================================================

/// @brief Create a new sprite from a Pixels buffer.
/// @details The new sprite retains @p pixels as frame zero and initializes
///          position/origin/rotation/depth to zero, scale to 100%, visibility
///          to true, and the default animation delay to 100 ms.
/// @param pixels Valid Pixels object to retain as the first frame.
/// @return A caller-owned Sprite reference, or `NULL` after an invalid-object
///         trap or allocation failure.
void *rt_sprite_new(void *pixels);

/// @brief Create a sprite by decoding a signature-recognized image file.
/// @details BMP, PNG, and JPEG inputs produce one frame. GIF inputs transfer
///          all decoded frames and their individual delays into the sprite.
///          Detection is based on file magic rather than extension.
/// @param path Borrowed runtime string containing a UTF-8 file path.
/// @return A caller-owned Sprite reference, or `NULL` for an invalid path,
///         unsupported signature, decoding error, or allocation failure.
void *rt_sprite_from_file(void *path);

//=========================================================================
// Sprite Properties
//=========================================================================

/// @brief Get sprite X position.
/// @param sprite Valid Sprite handle.
/// @return Stored destination-anchor X coordinate.
int64_t rt_sprite_get_x(void *sprite);

/// @brief Set sprite X position.
/// @param sprite Valid Sprite handle.
/// @param x New destination-anchor X coordinate.
void rt_sprite_set_x(void *sprite, int64_t x);

/// @brief Get sprite Y position.
/// @param sprite Valid Sprite handle.
/// @return Stored destination-anchor Y coordinate.
int64_t rt_sprite_get_y(void *sprite);

/// @brief Set sprite Y position.
/// @param sprite Valid Sprite handle.
/// @param y New destination-anchor Y coordinate.
void rt_sprite_set_y(void *sprite, int64_t y);

/// @brief Get sprite width (of current frame).
/// @param sprite Valid Sprite handle.
/// @return Unscaled current-frame width in pixels, or `0` if no frame exists.
int64_t rt_sprite_get_width(void *sprite);

/// @brief Get sprite height (of current frame).
/// @param sprite Valid Sprite handle.
/// @return Unscaled current-frame height in pixels, or `0` if no frame exists.
int64_t rt_sprite_get_height(void *sprite);

/// @brief Get sprite horizontal scale (100 = 100%).
/// @param sprite Valid Sprite handle.
/// @return Stored horizontal percentage scale.
int64_t rt_sprite_get_scale_x(void *sprite);

/// @brief Set sprite horizontal scale (100 = 100%).
/// @param sprite Valid Sprite handle.
/// @param scale Horizontal percentage, clamped to a minimum of `1`.
void rt_sprite_set_scale_x(void *sprite, int64_t scale);

/// @brief Get sprite vertical scale (100 = 100%).
/// @param sprite Valid Sprite handle.
/// @return Stored vertical percentage scale.
int64_t rt_sprite_get_scale_y(void *sprite);

/// @brief Set sprite vertical scale (100 = 100%).
/// @param sprite Valid Sprite handle.
/// @param scale Vertical percentage, clamped to a minimum of `1`.
void rt_sprite_set_scale_y(void *sprite, int64_t scale);

/// @brief Get sprite rotation in degrees.
/// @param sprite Valid Sprite handle.
/// @return Stored unnormalized signed clockwise rotation.
int64_t rt_sprite_get_rotation(void *sprite);

/// @brief Set sprite rotation in degrees.
/// @param sprite Valid Sprite handle.
/// @param degrees Signed clockwise rotation; the value is not normalized.
void rt_sprite_set_rotation(void *sprite, int64_t degrees);

/// @brief Get sprite depth used by batch sorting.
/// @param sprite Valid Sprite handle.
/// @return Stored signed depth value.
int64_t rt_sprite_get_depth(void *sprite);

/// @brief Set sprite depth used by batch sorting.
/// @param sprite Valid Sprite handle.
/// @param depth New signed sorting depth.
void rt_sprite_set_depth(void *sprite, int64_t depth);

/// @brief Get sprite visibility.
/// @param sprite Valid Sprite handle.
/// @return `1` when visible, otherwise `0`.
int64_t rt_sprite_get_visible(void *sprite);

/// @brief Set sprite visibility.
/// @param sprite Valid Sprite handle.
/// @param visible Zero to hide; any nonzero value to show.
void rt_sprite_set_visible(void *sprite, int64_t visible);

/// @brief Get current animation frame index.
/// @param sprite Valid Sprite handle.
/// @return Stored zero-based current-frame index.
int64_t rt_sprite_get_frame(void *sprite);

/// @brief Set current animation frame index.
/// @details Positive and negative out-of-range values wrap modulo the frame
///          count, and the sprite's automatic animation timer is reset.
/// @param sprite Valid Sprite handle.
/// @param frame Requested signed frame index.
void rt_sprite_set_frame(void *sprite, int64_t frame);

/// @brief Get total number of frames.
/// @param sprite Valid Sprite handle.
/// @return Number of Pixels frames retained by the sprite.
int64_t rt_sprite_get_frame_count(void *sprite);

/// @brief Get sprite horizontal flip.
/// @param sprite Valid Sprite handle.
/// @return `1` when horizontally mirrored, otherwise `0`.
int64_t rt_sprite_get_flip_x(void *sprite);

/// @brief Set sprite horizontal flip.
/// @param sprite Valid Sprite handle.
/// @param flip Zero for normal orientation; any nonzero value to mirror.
void rt_sprite_set_flip_x(void *sprite, int64_t flip);

/// @brief Get sprite vertical flip.
/// @param sprite Valid Sprite handle.
/// @return `1` when vertically mirrored, otherwise `0`.
int64_t rt_sprite_get_flip_y(void *sprite);

/// @brief Set sprite vertical flip.
/// @param sprite Valid Sprite handle.
/// @param flip Zero for normal orientation; any nonzero value to mirror.
void rt_sprite_set_flip_y(void *sprite, int64_t flip);

//=========================================================================
// Sprite Methods
//=========================================================================

/// @brief Draw the sprite to a canvas.
/// @details Uses the sprite's stored position, scale, rotation, origin, flip
///          flags, and current frame. Null/invalid, invisible, or frameless
///          sprites and null canvases are silently skipped.
/// @param sprite Candidate Sprite handle.
/// @param canvas Destination Canvas.
void rt_sprite_draw(void *sprite, void *canvas);

/// @brief Internal helper used by SpriteBatch and scene traversal.
/// @details Draws without changing the stored position, scale, or rotation.
///          Transform preparation applies flip, scale, optional origin padding,
///          rotation, tint, and alpha in that order and may reuse a cached image.
/// @param sprite Candidate Sprite handle.
/// @param canvas Destination Canvas.
/// @param x Destination X coordinate for the transformed origin.
/// @param y Destination Y coordinate for the transformed origin.
/// @param scale_x Temporary horizontal percentage scale.
/// @param scale_y Temporary vertical percentage scale.
/// @param rotation Temporary clockwise rotation in degrees.
/// @param tint_color Tint color (0xAARRGGBB or 0x00RRGGBB), or -1 for no tint.
/// @param alpha Global alpha multiplier; 255 or greater preserves source alpha.
void rt_sprite_draw_transformed(void *sprite,
                                void *canvas,
                                int64_t x,
                                int64_t y,
                                int64_t scale_x,
                                int64_t scale_y,
                                int64_t rotation,
                                int64_t tint_color,
                                int64_t alpha);

/// @brief Set the sprite's origin point for rotation/scaling.
/// @param sprite Sprite object.
/// @param x Integer local X coordinate relative to the unscaled frame top-left.
/// @param y Integer local Y coordinate relative to the unscaled frame top-left.
void rt_sprite_set_origin(void *sprite, int64_t x, int64_t y);

/// @brief Add an animation frame from a Pixels buffer.
/// @param sprite Sprite object.
/// @param pixels Valid Pixels object retained as the new final frame.
void rt_sprite_add_frame(void *sprite, void *pixels);

/// @brief Set the default and every existing frame delay in milliseconds.
/// @param sprite Sprite object.
/// @param ms Delay between frames, clamped to at least 1 ms.
void rt_sprite_set_frame_delay(void *sprite, int64_t ms);

/// @brief Get one animation frame's delay in milliseconds.
/// @param sprite Sprite object.
/// @param frame Valid zero-based frame index.
/// @return Positive frame delay in milliseconds, or `0` after a range error.
int64_t rt_sprite_get_frame_delay_at(void *sprite, int64_t frame);

/// @brief Set one animation frame's delay in milliseconds.
/// @param sprite Sprite object.
/// @param frame Valid zero-based frame index.
/// @param ms Delay in milliseconds, clamped to at least 1.
void rt_sprite_set_frame_delay_at(void *sprite, int64_t frame, int64_t ms);

/// @brief Update animation (advances frame if delay has passed).
/// @details Uses the monotonic runtime timer and per-frame delays, skipping
///          complete elapsed cycles before bounded frame-by-frame catch-up.
/// @param sprite Sprite object.
void rt_sprite_update(void *sprite);

/// @brief Check if this sprite overlaps another sprite.
/// @details Tests scaled, origin-adjusted axis-aligned rectangles. Rotation and
///          flips are ignored; invisible or frameless sprites cannot overlap.
/// @param sprite First candidate Sprite.
/// @param other Second candidate Sprite.
/// @return `1` if the inclusive rectangles overlap, otherwise `0`.
int8_t rt_sprite_overlaps(void *sprite, void *other);

/// @brief Check if a point is inside the sprite's bounding box.
/// @details Tests the scaled, origin-adjusted AABB and intentionally ignores
///          rotation and flips. Boundary pixels count as inside.
/// @param sprite Sprite object.
/// @param x Point X coordinate.
/// @param y Point Y coordinate.
/// @return `1` if a visible, framed sprite contains the point, otherwise `0`.
int8_t rt_sprite_contains(void *sprite, int64_t x, int64_t y);

/// @brief Move sprite by delta amounts.
/// @param sprite Sprite object.
/// @param dx Signed X displacement, applied with saturation.
/// @param dy Signed Y displacement, applied with saturation.
void rt_sprite_move(void *sprite, int64_t dx, int64_t dy);

//=========================================================================
// Sprite Animator (named animation clip state machine)
//=========================================================================

/// Maximum number of case-sensitive named clips stored by one animator.
#define RT_ANIM_MAX_CLIPS 32

/// Runtime class tag used to validate `Zanna.Graphics.SpriteAnimator` handles.
#define RT_SPRITE_ANIMATOR_CLASS_ID 0x5650475350414e49LL

/// @brief A single named animation clip.
/// @details Describes a consecutive frame range and uniform timing policy.
///          Registration clamps negative starts to zero, nonpositive counts to
///          one, and nonpositive delays to 100 ms.
typedef struct rt_anim_clip {
    char name[64];          ///< Clip name (NUL-terminated)
    int64_t start_frame;    ///< First frame index in the sprite's frames[]
    int64_t frame_count;    ///< Number of frames in this clip
    int64_t frame_delay_ms; ///< Milliseconds between frames
    int loop;               ///< 1 = loop, 0 = play once and stop
} rt_anim_clip_t;

/// @brief Named animation state machine for sprites.
/// @details Tracks which clip is playing, the position within that clip,
///          and elapsed time. Call rt_sprite_animator_update() each frame
///          to advance the animation and synchronize the supplied sprite's
///          frame. The animator does not retain or permanently bind a sprite.
typedef struct rt_sprite_animator {
    rt_anim_clip_t clips[RT_ANIM_MAX_CLIPS]; ///< Registered clips
    int clip_count;                          ///< Number of registered clips
    int current_clip;                        ///< Index of playing clip (-1 = idle)
    int64_t clip_frame;                      ///< Frame index within current clip
    int64_t last_update_ms;                  ///< Timestamp of last frame advance
    int playing;                             ///< 1 if animation is running
} rt_sprite_animator_t;

/// @brief Allocate an empty, stopped animator.
/// @return A caller-owned runtime-managed animator reference, or `NULL` when
///         allocation fails. Release it with `rt_sprite_animator_destroy()`.
rt_sprite_animator_t *rt_sprite_animator_new(void);

/// @brief Release one reference to an animator.
/// @details The object is freed only if the runtime reference count reaches zero.
///          Null or invalid handles are ignored.
/// @param animator Animator reference to release.
void rt_sprite_animator_destroy(rt_sprite_animator_t *animator);

/// @brief Register a named animation clip.
/// @details Names are matched case-sensitively, and registering an existing name
///          replaces that clip in place.
/// @param animator Animator to add the clip to.
/// @param name Nonempty NUL-terminated clip name of at most 63 bytes.
/// @param start_frame First frame index, clamped to zero.
/// @param frame_count Number of frames, clamped to at least one.
/// @param frame_delay_ms Milliseconds between frames; nonpositive means 100.
/// @param loop Zero to play once; nonzero to loop.
/// @return `1` on success, or `0` for invalid input, an empty/overlong name, or a
///         full table when no existing clip can be replaced.
int rt_sprite_animator_add_clip(rt_sprite_animator_t *animator,
                                const char *name,
                                int64_t start_frame,
                                int64_t frame_count,
                                int64_t frame_delay_ms,
                                int loop);

/// @brief Start playing a named clip.
/// @param animator Animator.
/// @param name     Clip name (must match a registered clip).
/// @return `1` if the case-sensitive name is found and restarted from its first
///         frame; otherwise `0` without disturbing current playback.
int8_t rt_sprite_animator_play(rt_sprite_animator_t *animator, const char *name);

/// @brief Stop the current animation (leaves sprite on its current frame).
/// @param animator Candidate Animator; invalid handles are ignored.
void rt_sprite_animator_stop(rt_sprite_animator_t *animator);

/// @brief Advance the animation and update the sprite's current frame.
/// @details Must be called once per frame. Automatically advances clip_frame
///          based on elapsed time and sets the sprite's frame accordingly.
/// @param animator Animator to advance.
/// @param sprite   Sprite whose frame will be updated.
void rt_sprite_animator_update(rt_sprite_animator_t *animator, void *sprite);

/// @brief Return 1 if the animator is currently playing a clip, 0 otherwise.
/// @param animator Candidate Animator.
/// @return `1` while playback is active, otherwise `0`.
int8_t rt_sprite_animator_is_playing(rt_sprite_animator_t *animator);

/// @brief Return the name of the currently playing clip, or NULL if idle.
/// @param animator Candidate Animator.
/// @return Borrowed pointer to the internal clip name while playing, otherwise
///         `NULL`. The pointer is invalidated when the animator is destroyed or
///         the corresponding clip is replaced.
const char *rt_sprite_animator_get_current(rt_sprite_animator_t *animator);

/// @brief Add a named clip using a runtime string.
/// @param animator Candidate opaque Animator handle.
/// @param name Borrowed runtime string containing the clip name.
/// @param start_frame First frame index, clamped to zero.
/// @param frame_count Number of frames, clamped to at least one.
/// @param frame_delay_ms Milliseconds between frames; nonpositive means 100.
/// @param loop Zero to play once; nonzero to loop.
/// @return `1` when registration succeeds, otherwise `0`.
int8_t rt_sprite_animator_add_clip_str(void *animator,
                                       rt_string name,
                                       int64_t start_frame,
                                       int64_t frame_count,
                                       int64_t frame_delay_ms,
                                       int64_t loop);

/// @brief Start playing a named clip using a runtime string.
/// @param animator Candidate opaque Animator handle.
/// @param name Borrowed runtime string containing a case-sensitive clip name.
/// @return `1` when the clip is found and started, otherwise `0`.
int8_t rt_sprite_animator_play_str(void *animator, rt_string name);

/// @brief Return the current clip name as a runtime string.
/// @param animator Candidate opaque Animator handle.
/// @return Runtime string containing the active name, or the empty runtime
///         string when stopped or invalid.
rt_string rt_sprite_animator_get_current_str(void *animator);

#ifdef __cplusplus
}
#endif
