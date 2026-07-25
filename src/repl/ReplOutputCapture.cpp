//===----------------------------------------------------------------------===//
/// @file
/// @brief Implements scoped capture of runtime output for REPL evaluation.
/// @details Construction replaces the process runtime's C-ABI output hook and
///          saves the prior hook in opaque state. Destruction restores that
///          exact function/context pair. Captured byte ranges are appended to
///          C++ string storage without taking ownership of runtime buffers.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplOutputCapture.cpp
// Purpose: Implementation of scoped runtime stdout capture for REPL eval.
// Key invariants:
//   - The prior runtime output hook is restored exactly once.
//   - The runtime callback never owns the byte range it receives.
// Ownership/Lifetime:
//   - HookState is owned by ScopedReplOutputCapture and deleted in the destructor.
// Links: src/repl/ReplOutputCapture.hpp, src/runtime/core/rt_output.h
//
//===----------------------------------------------------------------------===//

#include "ReplOutputCapture.hpp"

#include "runtime/core/rt_output.h"

namespace zanna::repl {

/// @brief Saved runtime output hook for one scoped capture.
/// @details The C runtime API returns a plain hook record. Keeping it in an
///          out-of-line state object avoids exposing runtime headers through
///          the public C++ REPL header.
struct ScopedReplOutputCapture::HookState {
    rt_output_capture_hook previous{};
};

/// @brief Bridge the runtime C output hook into one capture object.
/// @details Exceptions are swallowed to preserve the `noexcept` C-ABI boundary;
///          bytes may therefore be dropped if string growth fails.
/// @param data Borrowed output byte range.
/// @param len Number of bytes at @p data.
/// @param ctx Capture object supplied during hook installation; may be `NULL`.
void ScopedReplOutputCapture::captureCallback(const char *data, size_t len, void *ctx) noexcept {
    if (!ctx)
        return;
    try {
        static_cast<ScopedReplOutputCapture *>(ctx)->append(data, len);
    } catch (...) {
        // The runtime output hook has a C ABI. Preserve that boundary by
        // dropping bytes on allocation failure instead of propagating.
    }
}

/// @brief Install a runtime output hook for this capture scope.
/// @details The previously active hook is retained for exact restoration.
ScopedReplOutputCapture::ScopedReplOutputCapture() : state_(new HookState) {
    state_->previous = rt_output_set_capture_hook(captureCallback, this);
}

/// @brief Restore the previous runtime hook and release opaque state.
ScopedReplOutputCapture::~ScopedReplOutputCapture() noexcept {
    if (state_) {
        rt_output_set_capture_hook(state_->previous.fn, state_->previous.ctx);
        delete state_;
        state_ = nullptr;
    }
}

/// @brief Append a borrowed runtime output byte range.
/// @param data Bytes to copy; `NULL` is ignored.
/// @param len Number of bytes to append; zero is ignored.
void ScopedReplOutputCapture::append(const char *data, size_t len) {
    if (!data || len == 0)
        return;
    output_.append(data, len);
}

} // namespace zanna::repl
