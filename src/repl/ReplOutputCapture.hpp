//===----------------------------------------------------------------------===//
/// @file
/// @brief Declares an RAII bridge from runtime output hooks to an owned byte buffer.
/// @details The public header avoids exposing the runtime C hook record through
///          an opaque private state type. A capture has unique ownership because
///          nested hook replacement/restoration is tied to construction order.
///
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/repl/ReplOutputCapture.hpp
// Purpose: Scoped capture of runtime stdout produced by REPL evaluation.
// Key invariants:
//   - Capture is restored when the object is destroyed.
//   - Captured bytes are stored verbatim without UTF-8 interpretation.
// Ownership/Lifetime:
//   - Owns only the accumulated std::string buffer.
//   - Temporarily borrows the runtime output hook while alive.
// Links: src/runtime/core/rt_output.h
//
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

namespace zanna::repl {

/// @brief RAII guard that captures Zanna runtime stdout into memory.
/// @details The VM's terminal output routines flow through the runtime
///          @c rt_output layer. This guard installs a temporary capture hook so
///          REPL evaluation can collect program output without redirecting the
///          process-wide stdout file descriptor. That avoids pipe-buffer
///          deadlocks and ensures hook restoration happens on every exit path.
class ScopedReplOutputCapture {
  public:
    /// @brief Begin capturing runtime stdout.
    /// @details Saves the previously installed runtime output hook and replaces
    ///          it with a hook that appends raw bytes to this object's internal
    ///          buffer. The saved hook is restored by the destructor.
    ScopedReplOutputCapture();

    /// @brief Restore the runtime stdout hook active before construction.
    /// @details Destruction is noexcept so callers can safely use this guard
    ///          across compile/run paths that may return early on errors.
    ~ScopedReplOutputCapture() noexcept;

    /// @brief Disable copying because each guard owns one hook-restoration scope.
    ScopedReplOutputCapture(const ScopedReplOutputCapture &) = delete;
    /// @brief Disable copy assignment because hook-restoration scope is unique.
    /// @return This declaration is deleted and cannot be invoked.
    ScopedReplOutputCapture &operator=(const ScopedReplOutputCapture &) = delete;

    /// @brief Return all bytes captured so far.
    /// @details The returned string may contain arbitrary bytes, including
    ///          embedded NUL characters. The REPL currently prints it as text.
    /// @return Borrowed reference valid for the lifetime of this guard.
    const std::string &output() const noexcept {
        return output_;
    }

  private:
    /// @brief Opaque storage for the previously installed runtime hook.
    struct HookState;

    /// @brief Runtime C callback that forwards captured bytes into @p ctx.
    /// @details The runtime invokes this function through a plain function
    ///          pointer, so it must not throw. Invalid or null context pointers
    ///          are ignored.
    /// @param data Pointer to output bytes.
    /// @param len Number of bytes available at @p data.
    /// @param ctx Opaque pointer expected to reference this guard.
    static void captureCallback(const char *data, size_t len, void *ctx) noexcept;

    /// @brief Append a byte range received from the runtime output layer.
    /// @param data Pointer to output bytes.
    /// @param len Number of bytes in @p data.
    void append(const char *data, size_t len);

    std::string output_;
    HookState *state_{nullptr};
};

} // namespace zanna::repl
