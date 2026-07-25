//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/lib/graphics/src/vgfx_wayland_text_input.h
// Purpose: Bridge Zanna's text-input contract to zwp_text_input_v3.
// Key invariants:
//   - Protocol updates are published only at done boundaries.
//   - Surrounding-text byte offsets are translated to public codepoint offsets.
// Ownership/Lifetime: The object owns its protocol proxy and copied surrounding text.
// Links: docs/adr/0139-native-wayland-backend-and-linux-runtime-selection.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include "vgfx_wayland_input.h"

/// @file
/// @brief Declares the Wayland text-input-v3 bridge for one graphics window.
/// @details The bridge publishes surrounding text and input-purpose metadata to the input
/// method, batches compositor events until `done`, and translates protocol byte offsets into
/// public composition-event Unicode codepoint offsets.

/// @brief Text-input-v3 protocol state and pending composition batch.
/// @details The object owns @ref proxy and all three character buffers. The connection, input
/// state, and window are borrowed and must outlive the bridge.
typedef struct vgfx_wayland_text_input {
    /// @brief Borrowed connection providing the text-input manager and dispatch table.
    vgfx_wayland_connection_t *connection;

    /// @brief Borrowed input state providing seat, surface, and modifier information.
    vgfx_wayland_input_t *input;

    /// @brief Borrowed window that receives composition events.
    struct vgfx_window *window;

    /// @brief Owned text-input-v3 context, or NULL when the protocol is unavailable.
    struct zwp_text_input_v3 *proxy;

    /// @brief Owned, possibly truncated UTF-8 surrounding-text snapshot.
    char *surrounding;

    /// @brief Owned preedit text staged until the next `done` event.
    char *pending_preedit;

    /// @brief Owned committed text staged until the next `done` event.
    char *pending_commit;

    /// @brief Cursor byte offset within @ref surrounding.
    int32_t cursor_byte;

    /// @brief Selection-anchor byte offset within @ref surrounding.
    int32_t anchor_byte;

    /// @brief Preedit selection start byte offset within @ref pending_preedit.
    int32_t preedit_begin_byte;

    /// @brief Preedit selection end byte offset within @ref pending_preedit.
    int32_t preedit_end_byte;

    /// @brief Bytes requested for deletion before @ref cursor_byte.
    uint32_t delete_before_bytes;

    /// @brief Bytes requested for deletion after @ref cursor_byte.
    uint32_t delete_after_bytes;

    /// @brief Number of client state commits sent to the text-input object.
    uint32_t commit_serial;

    /// @brief Most recently accepted public text-input state.
    vgfx_text_input_state_t state;

    /// @brief Nonzero when the application wants input-method processing enabled.
    int32_t desired_enabled;

    /// @brief Nonzero while the input method reports entry into this window's surface.
    int32_t entered;

    /// @brief Nonzero after an enable request and before disable or surface leave.
    int32_t protocol_enabled;

    /// @brief Nonzero while public composition start has not ended or been cancelled.
    int32_t composition_active;

    /// @brief Nonzero when a preedit update is staged for the next `done`.
    int32_t have_preedit;

    /// @brief Nonzero when committed text is staged for the next `done`.
    int32_t have_commit;

    /// @brief Nonzero when a surrounding-text deletion is staged for the next `done`.
    int32_t have_delete;
} vgfx_wayland_text_input_t;

/// @brief Create the optional text-input-v3 context for a window's seat.
/// @details Absence of the protocol manager is a successful no-op so basic keyboard input
/// remains available. If the manager exists, proxy creation and listener registration must
/// both succeed.
/// @param text_input Destination bridge state.
/// @param connection Borrowed open Wayland connection.
/// @param input Borrowed input state for the same seat and surface.
/// @param window Borrowed graphics window that receives composition events.
/// @return 1 when initialized, including the unsupported-protocol fallback; otherwise 0.
int vgfx_wayland_text_input_open(vgfx_wayland_text_input_t *text_input,
                                 vgfx_wayland_connection_t *connection,
                                 vgfx_wayland_input_t *input,
                                 struct vgfx_window *window);

/// @brief Destroy the protocol context, release copied text, and clear the bridge.
/// @param text_input Bridge state to close, or NULL.
void vgfx_wayland_text_input_close(vgfx_wayland_text_input_t *text_input);

/// @brief Set whether the application wants input-method processing enabled.
/// @details The protocol is actually enabled only while its context has entered this window's
/// surface. Disabling an active composition emits a cancellation event.
/// @param text_input Bridge state to update.
/// @param enabled Nonzero to request enabled text input; zero to disable it.
/// @return 1 when the preference is accepted, otherwise 0 for a null state.
int vgfx_wayland_text_input_set_enabled(vgfx_wayland_text_input_t *text_input, int32_t enabled);

/// @brief Copy and publish surrounding text, selection, purpose, and cursor geometry.
/// @details Cursor and anchor offsets are clamped to UTF-8 boundaries. Surrounding text longer
/// than the protocol budget is cropped around the cursor without splitting a codepoint.
/// @param text_input Bridge state to update.
/// @param state Public text-input state to copy.
/// @return 1 when the state and required text copy are accepted, otherwise 0.
int vgfx_wayland_text_input_set_state(vgfx_wayland_text_input_t *text_input,
                                      const vgfx_text_input_state_t *state);
