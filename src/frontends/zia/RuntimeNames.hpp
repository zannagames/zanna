//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
///
/// @file RuntimeNames.hpp
/// @brief Zia-specific runtime name aliases and configuration constants.
///
/// @details This header imports the canonical runtime function names from the
/// auto-generated RuntimeNames.hpp (produced by rtgen from runtime.def) and
/// provides Zia-specific short aliases for backwards compatibility and
/// convenience. For example, `kListAdd` maps to `kCollectionsListPush`, and
/// `kBoxI64` maps to `kCoreBoxI64`.
///
/// The aliases exist because the Zia lowerer was written before the canonical
/// naming convention was finalized. Rather than updating hundreds of references
/// throughout the lowerer, these aliases bridge the gap. New code should prefer
/// the canonical names from `il::runtime::names` directly.
///
/// This file also defines Zia-specific compile-time configuration constants:
///   - Import limits (kMaxImportDepth, kMaxImportedFiles)
///   - Object layout constants (kObjectHeaderSize, kVtablePtrOffset, etc.)
///   - Internal runtime function names (rt_alloc, rt_obj_class_id)
///
/// @invariant All alias constants point to the same string literals as their
///            canonical counterparts in il::runtime::names.
/// @invariant Object layout constants must match the C runtime's struct layout
///            (see rt_heap.h, rt_obj_header.h).
///
/// Ownership/Lifetime: Header-only compile-time constants. No runtime state.
///
/// @see il/runtime/RuntimeNames.hpp — canonical generated names.
/// @see runtime.def — authoritative source for all runtime API definitions.
/// @see Lowerer.hpp — primary consumer of these name constants.
///
/// @note All runtime function names are defined in the generated file at
///       il/runtime/generated/RuntimeNames.hpp. This file only provides
///       aliases and Zia-specific constants.
///
//===----------------------------------------------------------------------===//

#pragma once

#include "il/runtime/RuntimeNames.hpp"

namespace il::frontends::zia::runtime {

/// @brief Import all canonical runtime names into the zia::runtime namespace.
using namespace il::runtime::names;

//=============================================================================
/// @name String Aliases
/// @brief Short names for Zanna.String runtime functions.
/// @{
//=============================================================================

// kStringContains comes directly from the canonical catalog (Zanna.String.Contains).
/// @brief Get the length of a string. Maps to Zanna.String.get_Length.
inline constexpr const char *kStringLength = kStringGetLength;
/// @brief Convert an i64 integer to its string representation.
inline constexpr const char *kStringFromInt = kCoreConvertToStringInt;
/// @brief Convert an f64 float to its string representation.
inline constexpr const char *kStringFromNum = kCoreConvertToStringDouble;
/// @}

//=============================================================================
/// @name Core.Object Aliases
/// @brief Short names for Zanna.Core.Object runtime functions.
/// @{
//=============================================================================

/// @brief Convert any object to its string representation.
inline constexpr const char *kObjectToString = kCoreObjectToString;
/// @}

//=============================================================================
/// @name Boxing Aliases
/// @brief Short names for Zanna.Core.Box boxing/unboxing functions.
/// @details Boxing wraps primitive values (i64, f64, bool, str) into heap-
///          allocated Box objects for polymorphic storage in collections.
/// @{
//=============================================================================

/// @brief Box an i64 value into a heap-allocated Box object.
inline constexpr const char *kBoxI64 = kCoreBoxI64;
/// @brief Box an f64 value into a heap-allocated Box object.
inline constexpr const char *kBoxF64 = kCoreBoxF64;
/// @brief Box a boolean (i1) value into a heap-allocated Box object.
inline constexpr const char *kBoxI1 = kCoreBoxI1;
/// @brief Box a string pointer into a heap-allocated Box object.
inline constexpr const char *kBoxStr = kCoreBoxStr;
/// @brief Allocate a runtime-managed box for inline value-type storage.
inline constexpr const char *kBoxValueType = kRuntimeUnsafeValueType;
/// @brief Register an owned field inside a boxed value-type object.
inline constexpr const char *kBoxValueTypeAddField = kRuntimeUnsafeValueTypeAddField;
/// @brief Unbox a Box object to extract the i64 value.
inline constexpr const char *kUnboxI64 = kCoreBoxToI64;
/// @brief Unbox a Box object to extract the f64 value.
inline constexpr const char *kUnboxF64 = kCoreBoxToF64;
/// @brief Unbox a Box object to extract the boolean (i1) value.
inline constexpr const char *kUnboxI1 = kCoreBoxToI1;
/// @brief Unbox a Box object to extract the string pointer.
inline constexpr const char *kUnboxStr = kCoreBoxToStr;
/// @}

//=============================================================================
/// @name Core.Convert Aliases
/// @brief Short names for Zanna.Core.Convert type conversion functions.
/// @{
//=============================================================================

/// @brief Convert a value to f64 (double-precision float).
inline constexpr const char *kConvertToDouble = kCoreConvertToDouble;
/// @brief Convert a value to i64 (64-bit integer).
inline constexpr const char *kConvertToInt = kCoreConvertToInt64;
/// @}

//=============================================================================
/// @name Core.Parse Aliases
/// @brief Short names for Zanna.Core.Parse string-to-number parsing functions.
/// @{
//=============================================================================

/// @brief Parse a string to an f64 value.
inline constexpr const char *kParseDouble = kCoreParseTryDouble;
/// @brief Parse a string to an i64 value.
inline constexpr const char *kParseInt64 = kCoreParseTryInt;
/// @}

//=============================================================================
/// @name List Aliases
/// @brief Short names for Zanna.Collections.List functions.
/// @details List is a dynamic growable array with O(1) amortized append.
/// @{
//=============================================================================

/// @brief Create a new empty List. Returns a heap-allocated list handle.
inline constexpr const char *kListNew = kCollectionsListNew;
/// @brief Append an element to the end of the list. O(1) amortized.
inline constexpr const char *kListAdd = kCollectionsListPush;
/// @brief Get the element at a given index. O(1).
inline constexpr const char *kListGet = kCollectionsListGet;
/// @brief Set the element at a given index. O(1).
inline constexpr const char *kListSet = kCollectionsListSet;
/// @brief Get the number of elements in the list. O(1).
inline constexpr const char *kListCount = kCollectionsListGetCount;
/// @brief Remove all elements from the list.
inline constexpr const char *kListClear = kCollectionsListClear;
/// @brief Remove the element at a given index, shifting subsequent elements.
inline constexpr const char *kListRemoveAt = kCollectionsListRemoveAt;
/// @brief Check if the list contains a given value. O(n) linear scan.
inline constexpr const char *kListContains = kCollectionsListHas;
/// @brief Remove the first occurrence of a value. O(n).
inline constexpr const char *kListRemove = kCollectionsListRemove;
/// @brief Insert an element at a given index, shifting subsequent elements.
inline constexpr const char *kListInsert = kCollectionsListInsert;
/// @brief Find the index of the first occurrence of a value, or -1 if absent.
inline constexpr const char *kListFind = kCollectionsListFind;
/// @brief Remove and return the last element.
inline constexpr const char *kListPop = kCollectionsListPop;
/// @}

//=============================================================================
/// @name Set Aliases
/// @brief Short names for Zanna.Collections.Set functions.
/// @details Set is a hash-based unordered collection of unique values.
/// @{
//=============================================================================

/// @brief Create a new empty Set. Returns a heap-allocated set handle.
inline constexpr const char *kSetNew = kCollectionsSetNew;
/// @brief Get the number of elements in the set. O(1).
inline constexpr const char *kSetCount = kCollectionsSetGetCount;
/// @brief Check if the set contains a given value. O(1) average.
inline constexpr const char *kSetHas = kCollectionsSetHas;
/// @brief Add a value to the set. No effect if already present. O(1) average.
inline constexpr const char *kSetPut = kCollectionsSetAdd;
/// @brief Remove a value from the set. O(1) average.
inline constexpr const char *kSetDrop = kCollectionsSetRemove;
/// @brief Remove all elements from the set.
inline constexpr const char *kSetClear = kCollectionsSetClear;
/// @}

//=============================================================================
/// @name Map Aliases
/// @brief Short names for Zanna.Collections.Map functions.
/// @details Map is a hash-based key-value dictionary with O(1) average lookups.
/// @{
//=============================================================================

/// @brief Create a new empty Map. Returns a heap-allocated map handle.
inline constexpr const char *kMapNew = kCollectionsMapNew;
/// @brief Set a key-value pair, overwriting any existing value for the key.
inline constexpr const char *kMapSet = kCollectionsMapSet;
/// @brief Get the value for a given key. Traps if key is absent.
inline constexpr const char *kMapGet = kCollectionsMapGet;
/// @brief Get an optional string value for a given key; missing keys return null.
inline constexpr const char *kMapGetOptStr = kCollectionsMapGetOptStr;
/// @brief Get the value for a given key, or a default if absent.
inline constexpr const char *kMapGetOr = kCollectionsMapGetOr;
/// @brief Check if the map contains a given key. O(1) average.
inline constexpr const char *kMapContainsKey = kCollectionsMapHas;
/// @brief Get the number of key-value pairs in the map. O(1).
inline constexpr const char *kMapCount = kCollectionsMapGetCount;
/// @brief Remove a key-value pair by key.
inline constexpr const char *kMapRemove = kCollectionsMapRemove;
/// @brief Set a key-value pair only if the key is not already present.
inline constexpr const char *kMapSetIfMissing = kCollectionsMapSetIfMissing;
/// @brief Remove all key-value pairs from the map.
inline constexpr const char *kMapClear = kCollectionsMapClear;
/// @brief Get a Seq of all keys in the map.
inline constexpr const char *kMapKeys = kCollectionsMapKeys;
/// @brief Get a Seq of all values in the map.
inline constexpr const char *kMapValues = kCollectionsMapValues;
/// @}

//=============================================================================
/// @name IntMap Aliases
/// @brief Short names for Zanna.Collections.IntMap functions.
/// @details IntMap is the integer-keyed runtime backing store used when Zia lowers
///          `Map[Integer, T]`. Values are stored as boxed objects just like string-keyed
///          Map values, while keys are passed as widened i64 integers.
/// @{
//=============================================================================

/// @brief Create a new empty integer-keyed map.
inline constexpr const char *kIntMapNew = kCollectionsIntMapNew;
/// @brief Set an integer key-value pair, overwriting any existing value for the key.
inline constexpr const char *kIntMapSet = kCollectionsIntMapSet;
/// @brief Get the boxed value for an integer key, or null when absent.
inline constexpr const char *kIntMapGet = kCollectionsIntMapGet;
/// @brief Get the boxed value for an integer key, or a boxed default when absent.
inline constexpr const char *kIntMapGetOr = kCollectionsIntMapGetOr;
/// @brief Check whether an integer key is present.
inline constexpr const char *kIntMapContainsKey = kCollectionsIntMapHas;
/// @brief Get the number of integer key-value pairs.
inline constexpr const char *kIntMapCount = kCollectionsIntMapGetCount;
/// @brief Remove an integer key-value pair.
inline constexpr const char *kIntMapRemove = kCollectionsIntMapRemove;
/// @brief Remove all integer key-value pairs.
inline constexpr const char *kIntMapClear = kCollectionsIntMapClear;
/// @brief Get a Seq of boxed integer keys.
inline constexpr const char *kIntMapKeys = kCollectionsIntMapKeys;
/// @brief Get a Seq of boxed values.
inline constexpr const char *kIntMapValues = kCollectionsIntMapValues;
/// @}

//=============================================================================
/// @name Seq Aliases
/// @brief Short names for Zanna.Collections.Seq (immutable sequence) functions.
/// @{
//=============================================================================

/// @brief Get the number of elements in the Seq. O(1).
inline constexpr const char *kSeqLen = kCollectionsSeqGetCount;
/// @brief Get the element at a given index in the Seq. O(1). Returns obj (Ptr).
inline constexpr const char *kSeqGet = kCollectionsSeqGet;
/// @brief Get a string element at a given index in a seq<str> Seq. Returns Str directly.
/// @details seq<str> sequences store raw rt_string pointers (not boxed). This function
///          casts the void* element to rt_string without boxing/unboxing overhead.
inline constexpr const char *kSeqGetStr = kCollectionsSeqGetStr;
/// @}

//=============================================================================
/// @name Math & System Aliases
/// @brief Short names for miscellaneous runtime functions.
/// @{
//=============================================================================

/// @brief Generate a random number. Maps to Zanna.Math.Random.NextDouble.
inline constexpr const char *kMathRandom = kMathRandomNextDouble;
/// @brief Sleep for a given number of milliseconds. Maps to Zanna.Time.Clock.Sleep.
inline constexpr const char *kSystemSleep = kTimeClockSleep;
/// @}

//=============================================================================
/// @name Thread Aliases
/// @brief Short names for Zanna.Threads.Thread functions.
/// @{
//=============================================================================

/// @brief Start a new thread executing a given function. Returns thread handle.
inline constexpr const char *kThreadSpawn = kThreadsThreadStart;
/// @brief Wait for a thread to complete execution.
inline constexpr const char *kThreadJoin = kThreadsThreadJoin;
/// @brief Suspend the current thread for a given number of milliseconds.
inline constexpr const char *kThreadSleep = kThreadsThreadSleep;
/// @}

//=============================================================================
/// @name Async/Future Aliases
/// @brief Short names for Zanna.Threads.Async and Zanna.Threads.Future functions.
/// @{
//=============================================================================

/// @brief Spawn an async task. Returns a Future handle.
inline constexpr const char *kAsyncRun = kThreadsAsyncRun;
/// @brief Block until a Future completes and return its value.
inline constexpr const char *kFutureGet = kThreadsFutureGet;
/// @}

//=============================================================================
// Zia-Specific Configuration Constants
//=============================================================================
/// @name Configuration Constants
/// @brief Compile-time constants for compiler behavior and object layout.
/// @{

/// @brief Maximum depth for import recursion to prevent stack overflow.
inline constexpr size_t kMaxImportDepth = 50;

/// @brief Maximum number of imported files to prevent runaway compilation.
/// @details Raised to 512 for modular dogfood applications (ADR 0217).
inline constexpr size_t kMaxImportedFiles = 512;

/// @brief Native machine word size used by Zia IL pointer/i64 stack slots.
/// @details Zia currently lowers references, i64 values, and f64 values into 8-byte IL slots.
///          Use this constant instead of spelling raw `8` at allocation sites.
inline constexpr size_t kMachineWordSize = 8;

/// @brief Object header size for class types in bytes.
/// All class instances begin with an 8-byte header containing runtime info.
inline constexpr size_t kObjectHeaderSize = kMachineWordSize;

/// @brief Offset of the vtable pointer within class objects.
inline constexpr size_t kVtablePtrOffset = kMachineWordSize;

/// @brief Size of the vtable pointer in bytes.
inline constexpr size_t kVtablePtrSize = kMachineWordSize;

/// @brief Offset where class fields begin (after header and vtable ptr).
inline constexpr size_t kClassFieldsOffset = kObjectHeaderSize + kVtablePtrSize;

/// @}

//=============================================================================
// Internal Runtime Functions
//=============================================================================
/// @name Internal Runtime Functions
/// @brief Low-level runtime functions not in the Zanna.* namespace.
/// @{

/// @brief Allocate zero-initialized raw runtime storage.
/// @details Signature: rt_alloc(i64 bytes) -> ptr
inline constexpr const char *kRtAlloc = "rt_alloc";

/// @brief Get the class ID from a runtime object's header.
/// @details Signature: rt_obj_class_id(ptr) -> i64
inline constexpr const char *kRtObjClassId = "rt_obj_class_id";
inline constexpr const char *kRtObjSetClassDtorHook = "rt_obj_set_class_dtor_hook";
inline constexpr const char *kZiaDtorDispatch = "__zia_dtor_dispatch";
/// @brief ADR 0315: per-class strong-slot registration (entry prologue).
inline constexpr const char *kRtObjClassLayoutBegin = "rt_obj_class_layout_begin";
inline constexpr const char *kRtObjClassLayoutAddSlot = "rt_obj_class_layout_add_slot";
inline constexpr const char *kZiaLayoutInit = "__zia_layout_init";

/// @}

//=============================================================================
/// @name Memory Management Aliases
/// @brief Short names for Zanna.Memory retain/release functions.
/// @{
//=============================================================================

/// @brief Increment reference count of a heap object.
inline constexpr const char *kHeapRetain = kRuntimeUnsafeRetain;
/// @brief Decrement reference count; free when it reaches zero.
inline constexpr const char *kHeapRelease = kRuntimeUnsafeRelease;
/// @brief Decrement reference count for a string and return the remaining count.
inline constexpr const char *kHeapReleaseStr = kRuntimeUnsafeReleaseStr;
/// @brief Release a string if it's heap-allocated (no-op for static/interned).
/// @details Uses the C function name directly because it's already registered
///          in RuntimeSignatures.cpp (not via runtime.def's mangled name).
inline constexpr const char *kStrReleaseMaybe = "rt_str_release_maybe";
/// @brief Retain a string if it's heap-allocated (no-op for static/interned).
/// @details Converts a borrowed string reference (e.g. from an class field Load)
///          into an owned reference, preventing use-after-free when the loaded
///          string is consumed by concatenation or passed to functions.
inline constexpr const char *kStrRetainMaybe = "rt_str_retain_maybe";

/// @}

} // namespace il::frontends::zia::runtime
