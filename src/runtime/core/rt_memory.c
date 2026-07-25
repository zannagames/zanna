//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_memory.c
// Purpose: Heap allocation shim for the Zanna runtime C ABI. Validates
//   requested sizes, enforces non-negative limits, and guarantees that callers
//   receive zero-initialised buffers even for zero-byte requests. Mirrors the
//   VM's allocation semantics so that diagnostics and trap conditions remain
//   consistent between interpreted (VM) and native (AOT) execution paths.
//
// Key invariants:
//   - The default rt_alloc path returns a zero-initialised buffer. A test hook
//     may override allocation behavior and is responsible for its own contract.
//   - Requesting a negative or overflow-inducing size fires rt_trap() rather
//     than returning NULL, keeping error handling uniform with other runtime
//     limit violations.
//   - rt_free(ptr) is a thin wrapper around free(). Passing NULL is safe (no-op,
//     matching standard C free() semantics).
//   - Runtime call sites that require deterministic allocation interposition
//     use this shim; specialized subsystems may use their own allocators.
//
// Ownership/Lifetime:
//   - The optional hook is process-global startup state and is unsynchronized.
//   - Callers own returned memory and must free compatible storage via rt_free()
//     or transfer it to the owning higher-level runtime object.
//
// Links: src/runtime/rt_internal.h (internal API),
//        src/runtime/core/rt_trap.h (rt_trap for invalid sizes)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Runtime heap allocation shim shared by BASIC intrinsics.
/// @details Provides @ref rt_alloc, a defensive wrapper around @c calloc that
///          rejects negative or oversized requests and reports errors via
///          @ref rt_trap. The default path returns zeroed storage of at least
///          one byte; an explicitly installed test hook may override that path.

#include "rt_internal.h"
#include <stdlib.h>

/// @brief Validate a byte count and allocate zero-initialized default storage.
/// @details Zero-byte requests allocate one byte so successful calls always
///   return storage that may be passed back to @ref rt_free.
/// @param bytes Signed byte count to validate.
/// @return Newly allocated zeroed storage, or NULL after raising a trap for a
///   negative/oversized request or allocation failure.
static void *rt_alloc_impl(int64_t bytes) {
    if (bytes < 0)
        return rt_trap("negative allocation"), NULL;
    if ((uint64_t)bytes > SIZE_MAX) {
        rt_trap("allocation too large");
        return NULL;
    }
    size_t request = (size_t)bytes;
    if (request == 0)
        request = 1;
    void *p = calloc(1, request);
    if (!p) {
        rt_trap("out of memory");
        return NULL;
    }
    return p;
}

/// @brief Cookie required before dispatch through the writable hook pointer.
static const uint64_t k_rt_alloc_hook_cookie = 0xA110CA7E5EEDBEEFULL;
/// @brief Armed cookie value, written during single-threaded startup/test setup.
static uint64_t g_rt_alloc_hook_armed = 0;
/// @brief Optional process-global allocation interceptor installed for tests.
static rt_alloc_hook_fn g_rt_alloc_hook = NULL;

/// @brief Install a hook that can override @ref rt_alloc for testing.
/// @details The hook receives the requested byte count along with a pointer to
///          the default implementation.  Passing @c NULL restores the default
///          behaviour.  Intended for unit tests that need to simulate allocator
///          failures without exhausting system memory. Installation is
///          unsynchronized and must occur before concurrent allocation begins.
/// @param hook Replacement function or @c NULL to disable overrides.
/// @warning Any non-NULL storage returned by a hook must be compatible with
///   @ref rt_free; callers cannot enforce zero initialization when the hook
///   chooses not to delegate to the supplied default allocator.
void rt_set_alloc_hook(rt_alloc_hook_fn hook) {
    g_rt_alloc_hook = hook;
    g_rt_alloc_hook_armed = hook ? k_rt_alloc_hook_cookie : 0;
}

/// @brief Allocate zero-initialised storage for runtime subsystems.
/// @details Delegates to the optional test hook when installed, otherwise calls
///          the default implementation described above. The hook receives the
///          unvalidated signed request and decides whether to call the default.
/// @param bytes Number of bytes requested by the caller.
/// @return On the default path, caller-owned zeroed storage; on a hooked path,
///   the hook's result. Default failures trap and return NULL if dispatch returns.
void *rt_alloc(int64_t bytes) {
    // The allocation hook exists for tests only. On native Windows builds we
    // have seen stray writable-data corruption present a non-null function
    // pointer here; require an explicit arm cookie from rt_set_alloc_hook()
    // before dispatching through the hook.
    if (g_rt_alloc_hook_armed == k_rt_alloc_hook_cookie && g_rt_alloc_hook)
        return g_rt_alloc_hook(bytes, rt_alloc_impl);
    return rt_alloc_impl(bytes);
}

/// @brief Free storage returned by @ref rt_alloc.
/// @details This is intentionally small today, but keeps allocation call sites
///          paired with the runtime allocator shim for future instrumentation.
///          NULL is accepted with standard `free` semantics.
/// @param ptr Owned allocation returned by the default allocator or a
///   free-compatible test hook.
void rt_free(void *ptr) {
    free(ptr);
}
