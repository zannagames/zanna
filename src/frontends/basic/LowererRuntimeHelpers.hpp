//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/LowererRuntimeHelpers.hpp
// Purpose: Runtime helper tracking for BASIC lowering.
// Key invariants: Helper requests are idempotent; tracking state resets
//                 between program lowering invocations.
// Ownership/Lifetime: Included transitively via Lowerer.hpp.
// Links: docs/runtime-vm.md#runtime-abi
//
//===----------------------------------------------------------------------===//

/**
 * @file LowererRuntimeHelpers.hpp
 * @brief Defines dense identifiers and requirement storage for manual helpers.
 *
 * ManualRuntimeHelper ordinals index a fixed boolean array. Count is a sentinel
 * and must not be passed to the indexing helper for array access.
 */

#pragma once

#include <array>
#include <cstddef>

namespace il::frontends::basic {

/// @brief Manual runtime helpers that require explicit tracking.
/// @details These are helpers not covered by the RuntimeFeature enum but
///          still need declaration in the IL module when used.
enum class ManualRuntimeHelper : std::size_t {
    /// Generic string trap routine.
    Trap = 0,
    /// Allocate an `i32` array.
    ArrayI32New,
    /// Resize an `i32` array.
    ArrayI32Resize,
    /// Query an `i32` array length.
    ArrayI32Len,
    /// Load an `i32` array element.
    ArrayI32Get,
    /// Store an `i32` array element.
    ArrayI32Set,
    /// Retain an `i32` array handle.
    ArrayI32Retain,
    /// Release an `i32` array handle.
    ArrayI32Release,
    /// Allocate an `i64` array.
    ArrayI64New,
    /// Resize an `i64` array.
    ArrayI64Resize,
    /// Query an `i64` array length.
    ArrayI64Len,
    /// Load an `i64` array element.
    ArrayI64Get,
    /// Store an `i64` array element.
    ArrayI64Set,
    /// Retain an `i64` array handle.
    ArrayI64Retain,
    /// Release an `i64` array handle.
    ArrayI64Release,
    /// Allocate an `f64` array.
    ArrayF64New,
    /// Resize an `f64` array.
    ArrayF64Resize,
    /// Query an `f64` array length.
    ArrayF64Len,
    /// Load an `f64` array element.
    ArrayF64Get,
    /// Store an `f64` array element.
    ArrayF64Set,
    /// Retain an `f64` array handle.
    ArrayF64Retain,
    /// Release an `f64` array handle.
    ArrayF64Release,
    /// Allocate a string array.
    ArrayStrAlloc,
    /// Release a string-array handle.
    ArrayStrRelease,
    /// Load a string-array element.
    ArrayStrGet,
    /// Store a string-array element.
    ArrayStrPut,
    /// Query a string-array length.
    ArrayStrLen,
    /// Allocate an object-reference array.
    ArrayObjNew,
    /// Query an object-array length.
    ArrayObjLen,
    /// Load an object-array element.
    ArrayObjGet,
    /// Store an object-array element.
    ArrayObjPut,
    /// Resize an object array.
    ArrayObjResize,
    /// Release an object-array handle.
    ArrayObjRelease,
    /// Report an array bounds failure.
    ArrayOobPanic,
    /// Open a file channel and return structured error data.
    OpenErrVstr,
    /// Close a file channel and return an error code.
    CloseErr,
    /// Seek within a file channel and return an error code.
    SeekChErr,
    /// Write a channel record and return an error code.
    WriteChErr,
    /// Write a newline-terminated channel record and return an error code.
    PrintlnChErr,
    /// Read a complete channel line with error reporting.
    LineInputChErr,
    /// Query channel EOF state.
    EofCh,
    /// Query channel length.
    LofCh,
    /// Query channel position.
    LocCh,
    /// Retain a possibly null string handle.
    StrRetainMaybe,
    /// Release a possibly null string handle.
    StrReleaseMaybe,
    /// Sleep for a millisecond duration.
    SleepMs,
    /// Query the runtime millisecond timer.
    TimerMs,
    /// Resolve address of an `i64` module variable.
    ModvarAddrI64,
    /// Resolve address of an `f64` module variable.
    ModvarAddrF64,
    /// Resolve address of an `i1` module variable.
    ModvarAddrI1,
    /// Resolve address of a pointer module variable.
    ModvarAddrPtr,
    /// Resolve address of a string module variable.
    ModvarAddrStr,
    /// One-past-the-last helper sentinel.
    Count
};

/// @brief Total number of manual runtime helpers.
inline constexpr std::size_t kManualRuntimeHelperCount =
    static_cast<std::size_t>(ManualRuntimeHelper::Count);

/// @brief Convert a ManualRuntimeHelper to its array index.
/// @param helper Valid helper enumerator.
/// @return Underlying zero-based ordinal.
/// @pre @p helper is not ManualRuntimeHelper::Count when used for array access.
inline constexpr std::size_t manualRuntimeHelperIndex(ManualRuntimeHelper helper) noexcept {
    return static_cast<std::size_t>(helper);
}

/// @brief Tracking array type for manual runtime helper requirements.
using ManualHelperRequirements = std::array<bool, kManualRuntimeHelperCount>;

} // namespace il::frontends::basic
