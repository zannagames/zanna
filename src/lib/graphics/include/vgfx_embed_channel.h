//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_embed_channel.h
// Purpose: Named shared-memory frame/input channel between a Studio host
//          and one embedded game process (ADR 0225).
// Key invariants:
//   - The HOST creates the channel sized for a fixed maximum frame; the
//     game attaches read/write and never resizes the mapping.
//   - Frames use a two-slot seqlock: the producer publishes into slot
//     (seq & 1) and the consumer copies latest-wins, retrying on torn
//     reads; neither side ever blocks the other.
//   - Input travels game-ward through a fixed-record ring; overflow drops
//     the oldest events (the game resynchronizes from fresh state).
//   - All cross-process fields are C11 atomics in a flat POD header.
// Ownership/Lifetime:
//   - Each side owns only its handle; the OS object lives until both
//     sides close (POSIX shm is unlinked by the host on close).
// Links: src/lib/graphics/src/vgfx_embed_channel.c,
//        docs/adr/0225-play-loop-and-embedded-play.md
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Environment variable naming the channel a game should attach.
#define VGFX_EMBED_CHANNEL_ENV "ZANNA_EMBED_CHANNEL"

/// @brief Embed input event kinds (host → game).
enum {
    VGFX_EMBED_EVENT_NONE = 0,
    VGFX_EMBED_EVENT_MOUSE_MOVE = 1,   ///< a=x, b=y
    VGFX_EMBED_EVENT_MOUSE_DOWN = 2,   ///< a=x, b=y, c=button
    VGFX_EMBED_EVENT_MOUSE_UP = 3,     ///< a=x, b=y, c=button
    VGFX_EMBED_EVENT_WHEEL = 4,        ///< a=x, b=y, c=deltaY*120
    VGFX_EMBED_EVENT_KEY_DOWN = 5,     ///< a=keycode, b=mods
    VGFX_EMBED_EVENT_KEY_UP = 6,       ///< a=keycode, b=mods
    VGFX_EMBED_EVENT_TEXT = 7,         ///< a=unicode codepoint
    VGFX_EMBED_EVENT_RESIZE = 8,       ///< a=width, b=height
    VGFX_EMBED_EVENT_CLOSE = 9,        ///< host asked the game to quit
};

/// @brief One fixed-size input record.
typedef struct {
    int32_t kind;
    int32_t a;
    int32_t b;
    int32_t c;
} vgfx_embed_event_t;

/// @brief Opaque channel handle (one per process side).
typedef struct vgfx_embed_channel vgfx_embed_channel_t;

/// @brief Create a channel as the HOST, sized for frames up to max_w×max_h.
/// @param name Portable channel name (letters, digits, '-', '_').
/// @param max_w Maximum frame width the ring can carry (1..8192).
/// @param max_h Maximum frame height the ring can carry (1..8192).
/// @param out Receives the handle on success.
/// @return 1 on success, 0 on failure (unsupported platform, bad args, OS error).
int vgfx_embed_channel_create(const char *name, int32_t max_w, int32_t max_h,
                              vgfx_embed_channel_t **out);

/// @brief Attach to an existing channel as the GAME producer.
/// @param name Channel name from VGFX_EMBED_CHANNEL_ENV.
/// @param out Receives the handle on success.
/// @return 1 on success, 0 on failure.
int vgfx_embed_channel_attach(const char *name, vgfx_embed_channel_t **out);

/// @brief Close one side's handle; the host also unlinks the OS object.
void vgfx_embed_channel_close(vgfx_embed_channel_t *channel);

/// @brief GAME: publish one RGBA8 frame (latest-wins, never blocks).
/// @return 1 when the frame was published, 0 on size overflow or bad args.
int vgfx_embed_channel_publish_frame(vgfx_embed_channel_t *channel, const uint8_t *rgba,
                                     int32_t width, int32_t height);

/// @brief HOST: copy the newest unseen frame into @p dst (capacity max_w*max_h*4).
/// @param dst Destination buffer.
/// @param width Receives the frame width.
/// @param height Receives the frame height.
/// @return 1 when a new frame was copied, 0 when nothing new arrived.
int vgfx_embed_channel_acquire_frame(vgfx_embed_channel_t *channel, uint8_t *dst,
                                     int32_t *width, int32_t *height);

/// @brief HOST: queue one input event for the game (drops oldest on overflow).
/// @return 1 on success, 0 on bad args.
int vgfx_embed_channel_push_event(vgfx_embed_channel_t *channel, const vgfx_embed_event_t *event);

/// @brief GAME: drain one queued input event.
/// @return 1 when an event was written to @p event, 0 when the ring is empty.
int vgfx_embed_channel_poll_event(vgfx_embed_channel_t *channel, vgfx_embed_event_t *event);

/// @brief HOST: request a game framebuffer size (delivered as a RESIZE event).
int vgfx_embed_channel_set_size(vgfx_embed_channel_t *channel, int32_t width, int32_t height);

/// @brief GAME: mark the producer exited so the host can report it truthfully.
void vgfx_embed_channel_mark_exited(vgfx_embed_channel_t *channel);

/// @brief Whether the producer marked the channel exited.
int vgfx_embed_channel_producer_exited(const vgfx_embed_channel_t *channel);

/// @brief Whether a producer has attached and published at least one frame.
int vgfx_embed_channel_producer_attached(const vgfx_embed_channel_t *channel);

/// @brief Peek the newest published frame's dimensions without copying.
/// @return 1 when a frame has been published, otherwise 0.
int vgfx_embed_channel_frame_size(const vgfx_embed_channel_t *channel, int32_t *width,
                                  int32_t *height);

/// @brief Channel frame capacity as configured by the host.
void vgfx_embed_channel_capacity(const vgfx_embed_channel_t *channel, int32_t *max_w,
                                 int32_t *max_h);

#ifdef __cplusplus
}
#endif
