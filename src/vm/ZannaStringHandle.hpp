//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/ZannaStringHandle.hpp
// Purpose: RAII wrapper for runtime string handles to ensure proper cleanup.
// Key invariants: Handle owns one reference; copy increments, move transfers.
// Ownership/Lifetime: Calls rt_str_release_maybe on destruction if non-null.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Defines reference-counted RAII wrappers for runtime string handles.
/// @details ZannaStringHandle owns one retained handle, while
///          ScopedSlotStringGuard conditionally releases a slot-held reference
///          unless ownership is explicitly transferred.

#pragma once

#include "zanna/runtime/rt.h"

#include <utility>

namespace il::vm {

/// @brief RAII wrapper for runtime string handles (`rt_string`).
/// @details Manages the reference count of a runtime string handle, ensuring
///          that the string is properly released when the wrapper is destroyed.
///          Copy operations increment the reference count via `rt_str_retain_maybe`,
///          while move operations transfer ownership without changing the count.
/// @invariant The wrapped handle is either null or has at least one reference
///            owned by this wrapper instance.
class ZannaStringHandle {
  public:
    /// @brief Construct an empty handle (null string).
    ZannaStringHandle() noexcept : handle_(nullptr) {}

    /// @brief Construct from a raw handle, taking ownership.
    /// @param s Raw string handle to take ownership of. May be null.
    /// @note The caller must have already acquired a reference that this
    ///       wrapper now owns. Do not pass borrowed references.
    explicit ZannaStringHandle(rt_string s) noexcept : handle_(s) {}

    /// @brief Release the owned string handle on destruction.
    ~ZannaStringHandle() {
        if (handle_)
            rt_str_release_maybe(handle_);
    }

    /// @brief Copy constructor - increments reference count.
    /// @param other Handle to copy from.
    ZannaStringHandle(const ZannaStringHandle &other) noexcept : handle_(other.handle_) {
        if (handle_)
            rt_str_retain_maybe(handle_);
    }

    /// @brief Copy assignment - releases current, then copies and retains.
    /// @param other Handle to copy from.
    /// @return Reference to this handle.
    ZannaStringHandle &operator=(const ZannaStringHandle &other) noexcept {
        if (this != &other) {
            if (handle_)
                rt_str_release_maybe(handle_);
            handle_ = other.handle_;
            if (handle_)
                rt_str_retain_maybe(handle_);
        }
        return *this;
    }

    /// @brief Move constructor - transfers ownership.
    /// @param other Handle to move from.
    ZannaStringHandle(ZannaStringHandle &&other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    /// @brief Move assignment - releases current, then transfers ownership.
    /// @param other Handle to move from.
    /// @return Reference to this handle.
    ZannaStringHandle &operator=(ZannaStringHandle &&other) noexcept {
        if (this != &other) {
            if (handle_)
                rt_str_release_maybe(handle_);
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    /// @brief Get the raw handle for passing to runtime functions.
    /// @return The underlying rt_string handle.
    [[nodiscard]] rt_string get() const noexcept {
        return handle_;
    }

    /// @brief Implicit conversion to raw handle for convenience.
    /// @return The underlying rt_string handle.
    operator rt_string() const noexcept {
        return handle_;
    }

    /// @brief Check if the handle is non-null.
    /// @return True if the handle is valid (non-null).
    explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    /// @brief Release ownership and return the raw handle.
    /// @return The underlying rt_string handle.
    /// @note After calling this, the wrapper no longer owns the handle.
    [[nodiscard]] rt_string release() noexcept {
        rt_string result = handle_;
        handle_ = nullptr;
        return result;
    }

    /// @brief Reset to a new handle, releasing any current handle.
    /// @param s New handle to take ownership of. May be null.
    void reset(rt_string s = nullptr) noexcept {
        if (handle_)
            rt_str_release_maybe(handle_);
        handle_ = s;
    }

  private:
    rt_string handle_; ///< Owned runtime handle, or @c nullptr when empty.
};

/// @brief Scoped guard for conditionally releasing a runtime string reference.
/// @details Use this when a slot-like value may contain a string that needs
///          cleanup on scope exit, but ownership might be transferred first.
///          Call `dismiss()` to prevent the release when ownership is transferred.
/// @invariant Releases only when @c isString_ is true and the guard is not dismissed.
class ScopedSlotStringGuard {
  public:
    /// @brief Construct a guard for a raw handle that may represent a string value.
    /// @param str Reference to the handle to release conditionally.
    /// @param isString Whether @p str contains an owned string reference.
    ScopedSlotStringGuard(rt_string &str, bool isString) noexcept
        : str_(str), isString_(isString), dismissed_(false) {}

    /// @brief Release the string on destruction unless dismissed.
    ~ScopedSlotStringGuard() {
        if (isString_ && !dismissed_ && str_)
            rt_str_release_maybe(str_);
    }

    // Non-copyable, non-movable
    /// @brief Conditional string guards cannot be copied.
    ScopedSlotStringGuard(const ScopedSlotStringGuard &) = delete;
    /// @brief Conditional string guards cannot be copy-assigned.
    ScopedSlotStringGuard &operator=(const ScopedSlotStringGuard &) = delete;
    /// @brief Conditional string guards cannot be moved.
    ScopedSlotStringGuard(ScopedSlotStringGuard &&) = delete;
    /// @brief Conditional string guards cannot be move-assigned.
    ScopedSlotStringGuard &operator=(ScopedSlotStringGuard &&) = delete;

    /// @brief Prevent the guard from releasing the string.
    /// @details Call this when ownership is transferred elsewhere.
    void dismiss() noexcept {
        dismissed_ = true;
    }

  private:
    rt_string &str_; ///< Reference to the conditionally owned handle.
    bool isString_;  ///< Whether @c str_ represents a string reference.
    bool dismissed_; ///< Whether ownership escaped the guard.
};

} // namespace il::vm
