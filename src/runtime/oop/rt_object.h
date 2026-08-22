//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/oop/rt_object.h
// Purpose: Reference-counted object allocation, retain/release, and System.Object surface providing
// the foundational object model for all Zanna heap objects.
//
// Key invariants:
//   - Refcounts never underflow; retain and release calls must be balanced.
//   - Objects start at refcount 1; zero-count objects are finalized and freed
//     by rt_obj_free or the public Memory release path unless resurrected.
//   - Finalizer callbacks are invoked from rt_obj_free before releasing heap storage.
//   - rt_obj_resurrect restores a zero count to one; a pool that resurrects an
//     object must explicitly reinstall its finalizer for the next release cycle.
//
// Ownership/Lifetime:
//   - Objects start with refcount 1; caller owns the initial reference.
//   - Retains add ownership; rt_memory_release decrements and finalizes/frees
//     at zero, while low-level deferred-release callers explicitly invoke
//     rt_obj_free after observing the zero transition.
//
// Links: src/runtime/oop/rt_object.c (implementation), src/runtime/core/rt_heap.h
//
//===----------------------------------------------------------------------===//

/**
 * @file rt_object.h
 * @brief Declares managed object allocation, identity, and lifetime services.
 * @details This foundational API creates class-tagged payloads, performs safe
 *          instance validation, exposes permissive and compiler-proven retain
 *          or release paths, manages finalizers and resurrection, and supplies
 *          the native System.Object behavior shared by runtime heap values.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Finalizer callback invoked from @ref rt_obj_free before releasing heap storage.
/// @param obj Zero-reference object payload being finalized. The callback may
///        call @ref rt_obj_resurrect to keep the allocation alive.
typedef void (*rt_obj_finalizer_t)(void *obj);

struct rt_string_impl; // fwd decl is provided in rt_string.h; include where needed

/// @brief Allocate a new runtime-managed object with the given class identifier and size.
/// @details Creates a zero-filled `RT_HEAP_OBJECT` payload with reference count
///          one and records @p class_id in its heap metadata. Negative or
///          unrepresentable sizes trap.
/// @param class_id Runtime class identifier tag for the object to create.
/// @param byte_size Total size in bytes to allocate for the object payload.
/// @return Caller-owned object payload, or @c NULL after reporting an allocation
///         failure through the runtime trap path.
void *rt_obj_new_i64(int64_t class_id, int64_t byte_size);

/// @brief Get the class ID of a runtime-managed object.
/// @param p Pointer to a runtime-managed object (may be NULL).
/// @return The class ID, or 0 if p is NULL or not a valid object.
int64_t rt_obj_class_id(void *p);

/// @brief Check that @p p is a live object instance with the requested class and payload size.
/// @details This is for runtime-owned opaque handle validation before casting a payload to an
///          implementation struct. It rejects NULL, non-heap pointers, non-object heap payloads,
///          wrong class IDs, and allocations smaller than @p min_payload_bytes.
/// @param p Candidate object payload pointer.
/// @param class_id Expected runtime class identifier.
/// @param min_payload_bytes Minimum payload size required by the target implementation struct.
/// @return 1 when the object matches, otherwise 0.
int8_t rt_obj_is_instance(void *p, int64_t class_id, size_t min_payload_bytes);

/// @brief Narrow a runtime handle to an exact runtime class, trapping on mismatch.
/// @details Backs the Zia `value as RuntimeClass` cast. Runtime classes compare by
///          exact class id because there is no hierarchy walk, so an unrelated
///          handle would otherwise survive the cast and fail later inside an
///          unrelated accessor. Null narrows to null so nullable runtime handles
///          keep their `== null` guards.
/// @param p Candidate object payload pointer, or @c NULL.
/// @param class_id Exact runtime class identifier required by the cast.
/// @return @p p when it is null or an instance of @p class_id.
/// @warning Traps when @p p is a live handle of any other class.
void *rt_cast_runtime_class(void *p, int64_t class_id);

/// @brief Increment the reference count for a runtime-managed object if the pointer is
/// non-null.
/// @details Recognizes runtime string handles and heap payloads. Null, raw, and
///          stale pointers are silently ignored by this permissive internal helper.
/// @param p Managed value to retain; @c NULL is ignored.
void rt_obj_retain_maybe(void *p);

/// @brief Increment the reference count for a compiler-proven runtime heap object.
/// @param p Pointer to a runtime-managed object; NULL pointers are ignored.
/// @warning This skips string/raw-pointer discrimination and is only valid for
///          values proven to originate from object allocation helpers.
void rt_obj_retain_known(void *p);

/// @brief Public Zanna.Memory retain wrapper.
/// @details Validates that @p p is a live runtime heap/string handle before
///          retaining. Invalid non-null pointers trap instead of relying on
///          debug-only heap assertions. Objects and arrays are accepted; an
///          internal string payload pointer or unsupported heap kind traps.
/// @param p Managed string handle, object payload, or array payload to retain;
///        @c NULL is ignored.
void rt_memory_retain(void *p);

/// @brief String-typed Zanna.Memory retain wrapper.
/// @details Validates the public string handle before incrementing its reference
///          count; a non-NULL invalid handle traps.
/// @param s Managed string handle to retain; @c NULL is ignored.
void rt_memory_retain_str(struct rt_string_impl *s);

/// @brief Decrement the reference count and report whether the object should be destroyed.
/// @details Heap payloads use deferred release so a true result requires a
///          subsequent @ref rt_obj_free call. String handles are released
///          immediately and always return zero; null, raw, and stale pointers
///          are ignored.
/// @param p Managed value to release; may be @c NULL.
/// @return @c 1 when a heap payload reaches zero and requires destruction;
///         otherwise @c 0.
int32_t rt_obj_release_check0(void *p);

/// @brief Release a compiler-proven runtime heap object and report last-user state.
/// @param p Pointer to a runtime-managed object.
/// @return 1 when the reference count reaches zero, otherwise 0.
/// @warning This skips string/raw-pointer discrimination and is only valid for
///          values proven to originate from object allocation helpers.
int32_t rt_obj_release_known_check0(void *p);

/// @brief Public Zanna.Memory release wrapper.
/// @details Releases strings, arrays, and objects through their managed
///          lifetime paths. Last-reference arrays release their owned managed
///          elements, and object finalizers run before last-reference objects
///          are reclaimed. A finalizer may resurrect an object, in which case
///          the new nonzero count is returned. Invalid, stale, and unsupported
///          non-null values trap.
/// @param p Managed string handle, object payload, or array payload to release;
///        @c NULL returns zero.
/// @return Remaining reference count, or 0 when the value was destroyed.
int64_t rt_memory_release(void *p);

/// @brief String-typed Zanna.Memory release wrapper.
/// @details Validates and decrements a string handle without requiring a
///          `void *` cast at the language/runtime boundary.
/// @param s Managed string handle to release; @c NULL returns zero.
/// @return Post-release count, `INT64_MAX` for an immortal string, or zero for
///         null or final release.
int64_t rt_memory_release_str(struct rt_string_impl *s);

/// @brief Finalize and reclaim a zero-reference heap payload.
/// @details Objects run their installed finalizer and arrays release owned
///          managed elements before deallocation. A resurrecting finalizer keeps
///          an object live. String handles are released normally as a compatibility
///          special case; invalid pointers are ignored, while nonzero-reference
///          or unsupported heap payloads trap.
/// @param p Zero-reference object/array payload or string handle; @c NULL is ignored.
void rt_obj_free(void *p);

/// @brief Install a finalizer callback for a runtime-managed object.
/// @details Replaces the object's current finalizer slot. The slot is cleared
///          before callback invocation, so an installation runs at most once.
///          A callback that resurrects the object must install a finalizer again
///          if another release cycle should invoke it. Null, invalid, and
///          non-object payloads are ignored.
/// @param p Object payload pointer returned by @ref rt_obj_new_i64; ignored when NULL.
/// @param fn Finalizer callback or NULL to clear.
void rt_obj_set_finalizer(void *p, rt_obj_finalizer_t fn);

/// @brief Resurrect an object inside its finalizer to recycle it into a pool.
/// @details Sets the reference count from 0 to 1 atomically.  Must only be
///          called from within a finalizer installed via @ref rt_obj_set_finalizer.
///          After resurrection @ref rt_heap_free_zero_ref will observe a
///          non-zero count and skip deallocation, keeping the allocation alive.
///          The caller is responsible for re-installing the finalizer before
///          returning the object to callers so the next release cycle can
///          recycle the object again.
/// @param p Object payload pointer whose refcount is currently zero.
void rt_obj_resurrect(void *p);

// --- System.Object runtime surface ---

/// @brief Value equality check between @p self and @p other.
/// @details Runtime strings compare by byte content and tagged boxes compare by
///          tag and value. All other values, including null, compare by pointer
///          identity; this function does not dispatch an overridable method.
/// @param self First value to compare; may be @c NULL.
/// @param other Second value to compare; may be @c NULL.
/// @return @c 1 when the values are equal under the runtime rules, otherwise @c 0.
int64_t rt_obj_equals(void *self, void *other);

/// @brief Compute a hash code for @p self.
/// @details Hashes complete string bytes and tagged-box values consistently
///          with @ref rt_obj_equals. Other non-null objects use a mixed pointer
///          hash, and null hashes to zero.
/// @param self Value to hash; may be @c NULL.
/// @return Signed 64-bit representation of the content, value, or identity hash.
int64_t rt_obj_get_hash_code(void *self);

/// @brief Convert @p self to a runtime string.
/// @details Retains and returns string inputs, formats tagged primitive boxes,
///          and uses built-in or registered qualified class names for objects.
///          Null becomes `"<null>"`; unavailable or invalid metadata falls back
///          to `"Object"`.
/// @param self Value to describe; may be @c NULL.
/// @return Caller-owned runtime string containing the display representation.
struct rt_string_impl *rt_obj_to_string(void *self);

/// @brief Identity equality check for two object references.
/// @details Returns 1 when pointers are identical, 0 otherwise.
/// @param a First reference; may be @c NULL.
/// @param b Second reference; may be @c NULL.
/// @return @c 1 when @p a and @p b are the same pointer, otherwise @c 0.
int64_t rt_obj_reference_equals(void *a, void *b);

// --- Object Introspection ---

/// @brief Get the fully-qualified type name of an object.
/// @details Recognizes strings, built-in runtime classes, and registered class
///          metadata. Null returns `"<null>"`; invalid, non-object, or unnamed
///          payloads fall back to `"Object"`.
/// @param self Value whose runtime type name is requested; may be @c NULL.
/// @return Caller-owned runtime string containing the resolved type name.
struct rt_string_impl *rt_obj_type_name(void *self);

/// @brief Get the numeric type ID of an object.
/// @details Returns the fixed string class identifier for a runtime string and
///          the heap metadata class identifier for an object payload.
/// @param self Value whose runtime type identifier is requested; may be @c NULL.
/// @return Runtime class identifier, or zero for null, invalid, or non-object payloads.
int64_t rt_obj_type_id(void *self);

/// @brief Check if an object reference is null.
/// @param self Reference to test.
/// @return 1 when @p self is NULL, 0 otherwise.
int64_t rt_obj_is_null(void *self);

// --- Weak Reference Support ---
/// @brief Store a weak reference without incrementing reference count.
/// @param addr Address of the field to store to.
/// @param value Managed target pointer to store (may be NULL).
/// @details Runtime-managed objects, arrays, and strings are wrapped in a
///          zeroing weak handle; non-runtime raw pointers are stored as-is for
///          compatibility. Replacing an existing managed weak slot resets its
///          handle without retaining the new target.
void rt_weak_store(void **addr, void *value);

/// @brief Load a weak reference and retain the live target.
/// @param addr Address of the field to load from.
/// @return The retained live target pointer, or NULL after the target has been freed.
///         Runtime-managed weak handles return owned references; legacy raw
///         pointer slots are returned borrowed.
void *rt_weak_load(void **addr);

#ifdef __cplusplus
}
#endif
