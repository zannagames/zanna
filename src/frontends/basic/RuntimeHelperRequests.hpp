//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/RuntimeHelperRequests.hpp
// Purpose: Runtime helper request interface for BASIC lowering.
//
// This header defines the interface for requesting runtime helpers during
// lowering. It extracts the runtime requirement tracking from Lowerer to
// reduce header size and improve modularity.
//
// The RuntimeHelperRequester interface allows lowering code to declare
// dependencies on runtime functions without directly including Lowerer.hpp.
//
//===----------------------------------------------------------------------===//
/// @file RuntimeHelperRequests.hpp
/// @brief Declares the runtime-feature request abstraction used by BASIC lowering.
/// @details Implementations record helper dependencies; these methods do not
///          themselves prescribe when declarations are emitted or calls are made.

#pragma once

#include "il/runtime/RuntimeSignatures.hpp"

namespace il::frontends::basic {

/// @brief Interface for requesting runtime helpers during lowering.
///
/// This interface abstracts the runtime requirement tracking mechanism,
/// allowing lowering helpers to declare their runtime dependencies without
/// tight coupling to the Lowerer class.
class RuntimeHelperRequester {
  public:
    /// @brief Destroy an implementation through the request interface.
    virtual ~RuntimeHelperRequester() = default;

    /// Runtime feature identifier accepted by the generic request API.
    using RuntimeFeature = il::runtime::RuntimeFeature;

    /// @brief Request a runtime helper by feature enum.
    /// @param feature Feature whose declaration must be made available.
    virtual void requestHelper(RuntimeFeature feature) = 0;

    /// @brief Check if a runtime helper is needed.
    /// @param feature Feature to query.
    /// @return `true` when the implementation has recorded @p feature.
    [[nodiscard]] virtual bool isHelperNeeded(RuntimeFeature feature) const = 0;

    // =========================================================================
    // Convenience request methods for common runtime helpers
    // =========================================================================

    /// @brief Request the trap helper.
    virtual void requireTrap() = 0;

    /// @brief Request the I32-array allocation helper.
    virtual void requireArrayI32New() = 0;
    /// @brief Request the I32-array resize helper.
    virtual void requireArrayI32Resize() = 0;
    /// @brief Request the I32-array length helper.
    virtual void requireArrayI32Len() = 0;
    /// @brief Request the I32-array element-load helper.
    virtual void requireArrayI32Get() = 0;
    /// @brief Request the I32-array element-store helper.
    virtual void requireArrayI32Set() = 0;
    /// @brief Request the I32-array retain helper.
    virtual void requireArrayI32Retain() = 0;
    /// @brief Request the I32-array release helper.
    virtual void requireArrayI32Release() = 0;
    /// @brief Request the shared array-bounds panic helper.
    virtual void requireArrayOobPanic() = 0;

    /// @brief Request the I64-array allocation helper.
    virtual void requireArrayI64New() = 0;
    /// @brief Request the I64-array resize helper.
    virtual void requireArrayI64Resize() = 0;
    /// @brief Request the I64-array length helper.
    virtual void requireArrayI64Len() = 0;
    /// @brief Request the I64-array element-load helper.
    virtual void requireArrayI64Get() = 0;
    /// @brief Request the I64-array element-store helper.
    virtual void requireArrayI64Set() = 0;
    /// @brief Request the I64-array retain helper.
    virtual void requireArrayI64Retain() = 0;
    /// @brief Request the I64-array release helper.
    virtual void requireArrayI64Release() = 0;

    /// @brief Request the F64-array allocation helper.
    virtual void requireArrayF64New() = 0;
    /// @brief Request the F64-array resize helper.
    virtual void requireArrayF64Resize() = 0;
    /// @brief Request the F64-array length helper.
    virtual void requireArrayF64Len() = 0;
    /// @brief Request the F64-array element-load helper.
    virtual void requireArrayF64Get() = 0;
    /// @brief Request the F64-array element-store helper.
    virtual void requireArrayF64Set() = 0;
    /// @brief Request the F64-array retain helper.
    virtual void requireArrayF64Retain() = 0;
    /// @brief Request the F64-array release helper.
    virtual void requireArrayF64Release() = 0;

    /// @brief Request the string-array allocation helper.
    virtual void requireArrayStrAlloc() = 0;
    /// @brief Request the string-array release helper.
    virtual void requireArrayStrRelease() = 0;
    /// @brief Request the string-array element-load helper.
    virtual void requireArrayStrGet() = 0;
    /// @brief Request the string-array element-store helper.
    virtual void requireArrayStrPut() = 0;
    /// @brief Request the string-array length helper.
    virtual void requireArrayStrLen() = 0;

    /// @brief Request the object-array allocation helper.
    virtual void requireArrayObjNew() = 0;
    /// @brief Request the object-array length helper.
    virtual void requireArrayObjLen() = 0;
    /// @brief Request the object-array element-load helper.
    virtual void requireArrayObjGet() = 0;
    /// @brief Request the object-array element-store helper.
    virtual void requireArrayObjPut() = 0;
    /// @brief Request the object-array resize helper.
    virtual void requireArrayObjResize() = 0;
    /// @brief Request the object-array release helper.
    virtual void requireArrayObjRelease() = 0;

    /// @brief Request the VStr-path OPEN-with-error-code helper.
    virtual void requireOpenErrVstr() = 0;
    /// @brief Request the channel CLOSE-with-error-code helper.
    virtual void requireCloseErr() = 0;
    /// @brief Request the channel SEEK-with-error-code helper.
    virtual void requireSeekChErr() = 0;
    /// @brief Request the channel WRITE-with-error-code helper.
    virtual void requireWriteChErr() = 0;
    /// @brief Request the channel PRINTLN-with-error-code helper.
    virtual void requirePrintlnChErr() = 0;
    /// @brief Request the channel LINE INPUT-with-error-code helper.
    virtual void requireLineInputChErr() = 0;
    /// @brief Request the channel EOF query helper.
    virtual void requireEofCh() = 0;
    /// @brief Request the channel length query helper.
    virtual void requireLofCh() = 0;
    /// @brief Request the channel position query helper.
    virtual void requireLocCh() = 0;

    /// @brief Request the I64 module-variable address helper.
    virtual void requireModvarAddrI64() = 0;
    /// @brief Request the F64 module-variable address helper.
    virtual void requireModvarAddrF64() = 0;
    /// @brief Request the I1 module-variable address helper.
    virtual void requireModvarAddrI1() = 0;
    /// @brief Request the pointer module-variable address helper.
    virtual void requireModvarAddrPtr() = 0;
    /// @brief Request the string module-variable address helper.
    virtual void requireModvarAddrStr() = 0;

    /// @brief Request the nullable string retain helper.
    virtual void requireStrRetainMaybe() = 0;
    /// @brief Request the nullable string release helper.
    virtual void requireStrReleaseMaybe() = 0;

    /// @brief Request the millisecond sleep helper.
    virtual void requireSleepMs() = 0;
    /// @brief Request the millisecond timer helper.
    virtual void requireTimerMs() = 0;
};

} // namespace il::frontends::basic
