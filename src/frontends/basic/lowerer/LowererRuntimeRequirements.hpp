//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/frontends/basic/lowerer/LowererRuntimeRequirements.hpp
// Purpose: Declares Lowerer methods that request or declare individual BASIC
//          runtime helpers.
// Key invariants: Each method is safe to call repeatedly and records the
//                 corresponding helper in Lowerer's runtime requirements.
// Ownership/Lifetime: Mutates Lowerer-owned runtime tracking and module state.
// Integration: This fragment is included inside the Lowerer class body and
//              therefore must not add include guards, namespaces, or includes.
// Links: src/frontends/basic/Lowerer.hpp,
//        src/frontends/basic/LowerRuntime.cpp
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Declares idempotent Lowerer requests for BASIC runtime helpers.
/// @details This class-body fragment records only the runtime symbols needed by
///          emitted array, file, module-variable, string, timing, and trap
///          operations so module declarations remain demand-driven.

// ═══ Runtime Requirement Declarations ═══

/// @brief Require the generic runtime trap helper.
void requireTrap();
/// @brief Require allocation of legacy I32-element arrays.
void requireArrayI32New();
/// @brief Require resizing of legacy I32-element arrays.
void requireArrayI32Resize();
/// @brief Require legacy I32-array length queries.
void requireArrayI32Len();
/// @brief Require legacy I32-array element loads.
void requireArrayI32Get();
/// @brief Require legacy I32-array element stores.
void requireArrayI32Set();
/// @brief Require retaining legacy I32-array handles.
void requireArrayI32Retain();
/// @brief Require releasing legacy I32-array handles.
void requireArrayI32Release();
/// @brief Require the shared array out-of-bounds panic helper.
void requireArrayOobPanic();
// I64 array helpers (for LONG arrays)
/// @brief Require allocation of I64-element arrays.
void requireArrayI64New();
/// @brief Require resizing of I64-element arrays.
void requireArrayI64Resize();
/// @brief Require I64-array length queries.
void requireArrayI64Len();
/// @brief Require I64-array element loads.
void requireArrayI64Get();
/// @brief Require I64-array element stores.
void requireArrayI64Set();
/// @brief Require retaining I64-array handles.
void requireArrayI64Retain();
/// @brief Require releasing I64-array handles.
void requireArrayI64Release();
// F64 array helpers (for SINGLE/DOUBLE arrays)
/// @brief Require allocation of F64-element arrays.
void requireArrayF64New();
/// @brief Require resizing of F64-element arrays.
void requireArrayF64Resize();
/// @brief Require F64-array length queries.
void requireArrayF64Len();
/// @brief Require F64-array element loads.
void requireArrayF64Get();
/// @brief Require F64-array element stores.
void requireArrayF64Set();
/// @brief Require retaining F64-array handles.
void requireArrayF64Retain();
/// @brief Require releasing F64-array handles.
void requireArrayF64Release();

/// @brief Require allocation of string-element arrays.
void requireArrayStrAlloc();
/// @brief Require releasing string-array handles and owned elements.
void requireArrayStrRelease();
/// @brief Require retained string-array element loads.
void requireArrayStrGet();
/// @brief Require string-array element stores.
void requireArrayStrPut();
/// @brief Require string-array length queries.
void requireArrayStrLen();
// Object array helpers
/// @brief Require allocation of object-reference arrays.
void requireArrayObjNew();
/// @brief Require object-array length queries.
void requireArrayObjLen();
/// @brief Require object-array element loads.
void requireArrayObjGet();
/// @brief Require object-array element stores.
void requireArrayObjPut();
/// @brief Require resizing object-reference arrays.
void requireArrayObjResize();
/// @brief Require releasing object-array handles and elements.
void requireArrayObjRelease();
/// @brief Require file OPEN with error reporting and value-string paths.
void requireOpenErrVstr();
/// @brief Require file-channel CLOSE with error reporting.
void requireCloseErr();
/// @brief Require file-channel SEEK with error reporting.
void requireSeekChErr();
/// @brief Require channel writes with error reporting.
void requireWriteChErr();
/// @brief Require channel newline output with error reporting.
void requirePrintlnChErr();
/// @brief Require channel line input with error reporting.
void requireLineInputChErr();
// --- begin: require declarations ---
/// @brief Require end-of-file queries for a channel.
void requireEofCh();
/// @brief Require file-length queries for a channel.
void requireLofCh();
/// @brief Require current-position queries for a channel.
void requireLocCh();
// Module-level globals helpers
/// @brief Require address lookup for module-level I64 variables.
void requireModvarAddrI64();
/// @brief Require address lookup for module-level F64 variables.
void requireModvarAddrF64();
/// @brief Require address lookup for module-level I1 variables.
void requireModvarAddrI1();
/// @brief Require address lookup for module-level pointer variables.
void requireModvarAddrPtr();
/// @brief Require address lookup for module-level string variables.
void requireModvarAddrStr();
// --- end: require declarations ---
/// @brief Require conditional string retain support.
void requireStrRetainMaybe();
/// @brief Require conditional string release support.
void requireStrReleaseMaybe();
/// @brief Require millisecond sleep support.
void requireSleepMs();
/// @brief Require millisecond timer support.
void requireTimerMs();
