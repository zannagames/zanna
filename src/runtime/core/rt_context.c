//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/core/rt_context.c
/// @file
/// @brief Implements runtime-context lifecycle, binding, locking, and fallback state.
///
// Purpose: Per-VM runtime context management. Each Zanna VM instance owns an
//   RtContext that holds all per-VM state: RNG seed, open file handles,
//   command-line arguments, module-level variables, and the OOP type registry.
//   Multiple independent VMs can coexist in a single process because all
//   mutable state is confined to the context rather than process-global vars.
//
// Key invariants:
//   - A context is bound to a thread via a thread-local pointer
//     (_Thread_local RtContext *g_rt_context). At most one context is active
//     per thread at any time; VMs must bind before executing and unbind after.
//   - bind_count is an atomic reference count incremented on bind/reservation
//     and decremented on unbind/cancellation. A last unbind to no replacement,
//     or cancellation of the last reservation, hands eligible state back to
//     the legacy context for native post-VM calls.
//   - A process-wide legacy context is initialized through an atomic state
//     machine and used as a fallback when g_rt_context is NULL. Shutdown may
//     clean it and publish an uninitialized state for safe later reuse.
//   - rt_context_init() transactionally constructs subsystem locks and empty
//     state. Cleanup claims the lifecycle before releasing that storage, so a
//     concurrent bind either owns a counted reservation or is rejected.
//   - RNG, module variables, files, arguments, and type metadata are serialized
//     independently when one context is inherited by multiple runtime threads.
//
// Ownership/Lifetime:
//   - The embedding application (VM or host) owns the RtContext struct storage.
//     The runtime does not allocate or free the struct itself.
//   - Internal heap allocations (file state, args, type registry) are freed
//     by rt_context_cleanup().
//
// Links: src/runtime/core/rt_context.h (public API),
//        src/runtime/core/rt_file.c (file handle state),
//        src/runtime/core/rt_args.c (command-line argument state),
//        src/runtime/core/rt_type_registry.c (OOP type registration),
//        src/runtime/core/rt_modvar.c (module variable storage)
//
//===----------------------------------------------------------------------===//

#include "rt_context.h"
#include "rt_context_internal.h"
#include "rt_internal.h"
#include "rt_platform.h"
#include "rt_string.h"
#include "rt_trap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if RT_PLATFORM_WINDOWS
// GetCurrentThreadId() is available via windows.h (included from rt_platform.h)
#else
#include <pthread.h>
#include <sched.h>
#endif

/* Cross-component forward declarations — weak defaults so programs that
   don't use file I/O or OOP don't need to link zanna_rt_io_fs / zanna_rt_oop.
   When those components ARE linked, their strong definitions override these. */
#if defined(_MSC_VER)
/// @brief Release optional file-subsystem state owned by a context.
/// @param ctx Context whose file state is being destroyed.
void rt_file_state_cleanup(RtContext *ctx);
/// @brief Release optional OOP type-registry state owned by a context.
/// @param ctx Context whose type registry is being destroyed.
void rt_type_registry_cleanup(RtContext *ctx);
/// @brief Transactionally initialize the optional OOP type registry.
/// @param ctx Context receiving the registry synchronization state.
/// @return Non-zero on success; zero after a recoverable native init failure.
int rt_type_registry_init(RtContext *ctx);
/// @brief Acquire exclusive access to optional type-registry state.
/// @param ctx Initialized context whose registry is being migrated.
void rt_type_registry_state_write_lock(RtContext *ctx);
/// @brief Release exclusive access to optional type-registry state.
/// @param ctx Context passed to the matching write-lock operation.
void rt_type_registry_state_write_unlock(RtContext *ctx);
#else
/// @brief Weak no-op file-state cleanup used when the I/O component is absent.
/// @param ctx Context being cleaned; ignored by this fallback.
__attribute__((weak)) void rt_file_state_cleanup(RtContext *ctx) {
    (void)ctx;
}

/// @brief Weak no-op type-registry cleanup used when the OOP component is absent.
/// @param ctx Context being cleaned; ignored by this fallback.
__attribute__((weak)) void rt_type_registry_cleanup(RtContext *ctx) {
    (void)ctx;
}

/// @brief Weak successful type-registry initializer for base-only runtimes.
/// @details The strong OOP implementation replaces this definition and creates
///          its native synchronization state transactionally.
/// @param ctx Context being initialized; ignored by this fallback.
/// @return Always one because no optional state exists.
__attribute__((weak)) int rt_type_registry_init(RtContext *ctx) {
    (void)ctx;
    return 1;
}

/// @brief Weak no-op registry write lock used when the OOP component is absent.
/// @param ctx Context being migrated; ignored by this fallback.
__attribute__((weak)) void rt_type_registry_state_write_lock(RtContext *ctx) {
    (void)ctx;
}

/// @brief Weak no-op registry write unlock used when the OOP component is absent.
/// @param ctx Context being migrated; ignored by this fallback.
__attribute__((weak)) void rt_type_registry_state_write_unlock(RtContext *ctx) {
    (void)ctx;
}
#endif
/// @brief Releases argument strings and storage owned by a context.
/// @param ctx Context whose argument state is being destroyed.
void rt_args_state_cleanup(RtContext *ctx);

/// @brief Thread-local pointer to the active runtime context.
///
/// Each thread can have at most one active VM context bound at a time.
/// The VM sets this pointer before executing Zanna code and clears it
/// afterward. When NULL, runtime functions fall back to the legacy context.
///
/// @note The pointer is borrowed. Binding counts and lifecycle synchronization
///       keep the caller-owned context storage live while it is published.
RT_THREAD_LOCAL RtContext *g_rt_context = NULL;

/// @brief Global legacy context for backward compatibility.
///
/// Used when no VM context is bound to the current thread. This enables
/// native code and tests to use runtime functions without explicit context
/// management. Lazily initialized on first access.
static RtContext g_legacy_ctx;

/// @brief Initialization state for the legacy context.
///
/// State machine:
/// - -1: Initialization failed (waiters should trap instead of spinning)
/// - 0: Uninitialized (initial state)
/// - 1: Initializing (one thread is running rt_context_init)
/// - 2: Initialized (ready for use)
///
/// Uses atomic operations to ensure exactly-once initialization even under
/// concurrent access from multiple threads.
static int g_legacy_state = 0;

/// @brief Spinlock protecting state handoff between VM and legacy contexts.
///
/// Serializes the transfer of file handles, arguments, and type registrations
/// during context bind/unbind to prevent data races.
static int g_legacy_handoff_lock = 0;

/// @brief Acquire a simple spinlock with yield-on-contention.
///
/// Uses atomic test-and-set with acquire semantics. Yields after the first
/// failed attempt to reduce CPU waste under contention (CONC-010 fix).
///
/// @param lock Pointer to the lock variable (0 = unlocked, 1 = locked).
///
/// @note Only used for init/handoff paths where contention is rare.
static void rt_spin_lock(int *lock) {
    if (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {
        do {
#if RT_PLATFORM_WINDOWS
            SwitchToThread();
#else
            sched_yield();
#endif
        } while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE));
    }
}

/// @brief Release a spinlock.
///
/// Uses atomic clear with release semantics to ensure all writes within
/// the critical section are visible to subsequent lock acquirers.
///
/// @param lock Pointer to the lock variable to release.
static void rt_spin_unlock(int *lock) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

//===----------------------------------------------------------------------===//
// Per-Context Mutable-State Locks
//===----------------------------------------------------------------------===//

/// @brief Platform-private recursive mutex stored through `RtContext::state_locks`.
typedef struct rt_context_mutex {
#if RT_PLATFORM_WINDOWS
    CRITICAL_SECTION native;
#else
    pthread_mutex_t native;
#endif
} rt_context_mutex_t;

/// @brief One lock acquisition recorded for trap-safe lexical unwinding.
typedef struct rt_context_lock_frame {
    RtContext *ctx;
    rt_context_state_kind_t kind;
} rt_context_lock_frame_t;

/// Maximum nested context locks on one runtime thread.
#define RT_CONTEXT_LOCK_STACK_CAPACITY 32u

/// Thread-local acquisition stack released in reverse order before `longjmp`.
static RT_THREAD_LOCAL rt_context_lock_frame_t g_context_lock_stack[RT_CONTEXT_LOCK_STACK_CAPACITY];
static RT_THREAD_LOCAL uint32_t g_context_lock_depth = 0;

/// @brief Validate and return a context's platform mutex for @p kind.
/// @param ctx Initialized runtime context.
/// @param kind Subsystem lock selector.
/// @return Concrete mutex pointer, or NULL for invalid/uninitialized input.
static rt_context_mutex_t *rt_context_mutex_for_(RtContext *ctx, rt_context_state_kind_t kind) {
    if (!ctx || kind < RT_CONTEXT_STATE_RNG || kind > RT_CONTEXT_STATE_LIFECYCLE)
        return NULL;
    return (rt_context_mutex_t *)ctx->state_locks[(size_t)kind];
}

/// @brief Allocate and initialize one recursive platform mutex.
/// @details Recursive behavior permits layered helpers in the same subsystem
///          to share the context lock without self-deadlock. The function is
///          non-trapping so context initialization can unwind earlier slots
///          transactionally before reporting failure.
/// @return Owned mutex storage, or NULL on allocation/initialization failure.
static rt_context_mutex_t *rt_context_mutex_create_(void) {
    rt_context_mutex_t *mutex = (rt_context_mutex_t *)calloc(1, sizeof(*mutex));
    if (!mutex)
        return NULL;
#if RT_PLATFORM_WINDOWS
    if (!InitializeCriticalSectionAndSpinCount(&mutex->native, 64)) {
        free(mutex);
        return NULL;
    }
#else
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        free(mutex);
        return NULL;
    }
    int status = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    if (status == 0)
        status = pthread_mutex_init(&mutex->native, &attr);
    (void)pthread_mutexattr_destroy(&attr);
    if (status != 0) {
        free(mutex);
        return NULL;
    }
#endif
    return mutex;
}

/// @brief Destroy one context mutex and release its owned storage.
/// @param mutex Mutex returned by @ref rt_context_mutex_create_, or NULL.
static void rt_context_mutex_destroy_(rt_context_mutex_t *mutex) {
    if (!mutex)
        return;
#if RT_PLATFORM_WINDOWS
    DeleteCriticalSection(&mutex->native);
#else
    (void)pthread_mutex_destroy(&mutex->native);
#endif
    free(mutex);
}

/// @brief Acquire a concrete context mutex without touching the trap stack.
/// @param mutex Initialized mutex; NULL is an internal fatal error.
static void rt_context_mutex_lock_raw_(rt_context_mutex_t *mutex) {
    if (!mutex)
        rt_abort("Runtime context state lock is not initialized");
#if RT_PLATFORM_WINDOWS
    EnterCriticalSection(&mutex->native);
#else
    if (pthread_mutex_lock(&mutex->native) != 0)
        rt_abort("Failed to acquire runtime context state lock");
#endif
}

/// @brief Release a concrete context mutex without touching the trap stack.
/// @param mutex Initialized mutex; NULL is an internal fatal error.
static void rt_context_mutex_unlock_raw_(rt_context_mutex_t *mutex) {
    if (!mutex)
        rt_abort("Runtime context state lock is not initialized");
#if RT_PLATFORM_WINDOWS
    LeaveCriticalSection(&mutex->native);
#else
    if (pthread_mutex_unlock(&mutex->native) != 0)
        rt_abort("Failed to release runtime context state lock");
#endif
}

/// @copydoc rt_context_state_lock
void rt_context_state_lock(RtContext *ctx, rt_context_state_kind_t kind) {
    if (g_context_lock_depth >= RT_CONTEXT_LOCK_STACK_CAPACITY)
        rt_abort("Runtime context state lock nesting overflow");
    rt_context_mutex_t *mutex = rt_context_mutex_for_(ctx, kind);
    rt_context_mutex_lock_raw_(mutex);
    g_context_lock_stack[g_context_lock_depth].ctx = ctx;
    g_context_lock_stack[g_context_lock_depth].kind = kind;
    g_context_lock_depth++;
}

/// @copydoc rt_context_release_state
/// @note An empty per-thread lock stack makes this a no-op; otherwise locks
///       must be released in strict last-in, first-out order or the runtime aborts.
void rt_context_release_state(RtContext *ctx, rt_context_state_kind_t kind) {
    if (g_context_lock_depth == 0)
        return;
    rt_context_lock_frame_t *frame = &g_context_lock_stack[g_context_lock_depth - 1];
    if (frame->ctx != ctx || frame->kind != kind)
        rt_abort("Runtime context state locks released out of order");
    g_context_lock_depth--;
    frame->ctx = NULL;
    rt_context_mutex_unlock_raw_(rt_context_mutex_for_(ctx, kind));
}

/// @copydoc rt_context_state_abort_for_trap
/// @note Releases every tracked recursive acquisition in reverse order and
///       resets the current thread's lock depth to zero.
void rt_context_state_abort_for_trap(void) {
    while (g_context_lock_depth > 0) {
        rt_context_lock_frame_t frame = g_context_lock_stack[--g_context_lock_depth];
        g_context_lock_stack[g_context_lock_depth].ctx = NULL;
        rt_context_mutex_unlock_raw_(rt_context_mutex_for_(frame.ctx, frame.kind));
    }
}

//===----------------------------------------------------------------------===//
// Main-Thread Tracking
//===----------------------------------------------------------------------===//

#if RT_PLATFORM_WINDOWS
static DWORD g_main_thread_id_;
#else
static pthread_t g_main_thread_;
#endif
enum { RT_MAIN_THREAD_UNSET = 0, RT_MAIN_THREAD_READY = 1 };

static int g_main_thread_set_ = RT_MAIN_THREAD_UNSET;

/// @brief Spinlock protecting the platform-native main-thread identifier.
/// @details The identifier itself is not an atomic C type (`pthread_t` is
///          opaque on POSIX), so both readers and explicit overrides must
///          hold this lock. The accompanying atomic ready flag only controls
///          lazy capture; it does not by itself make identifier access safe.
static int g_main_thread_lock_ = 0;

/// @brief Install the calling thread as the main thread when none is recorded.
/// @details Serializes access to the platform-native identifier because
///          `pthread_t` is not guaranteed to support lock-free atomic access.
///          Explicit calls to @ref rt_set_main_thread may later override the
///          captured value under the same lock without racing concurrent
///          calls to @ref rt_is_main_thread.
/// @note A prior successful capture is left unchanged.
static void rt_capture_process_main_thread_(void) {
    if (__atomic_load_n(&g_main_thread_set_, __ATOMIC_ACQUIRE) == RT_MAIN_THREAD_READY)
        return;

    rt_spin_lock(&g_main_thread_lock_);
    if (__atomic_load_n(&g_main_thread_set_, __ATOMIC_RELAXED) == RT_MAIN_THREAD_READY) {
        rt_spin_unlock(&g_main_thread_lock_);
        return;
    }
#if RT_PLATFORM_WINDOWS
    g_main_thread_id_ = GetCurrentThreadId();
#else
    g_main_thread_ = pthread_self();
#endif
    __atomic_store_n(&g_main_thread_set_, RT_MAIN_THREAD_READY, __ATOMIC_RELEASE);
    rt_spin_unlock(&g_main_thread_lock_);
}

#if RT_PLATFORM_WINDOWS
/// @brief MSVC CRT-init shim that calls `rt_capture_process_main_thread_` at startup.
static void __cdecl rt_capture_process_main_thread_ctor(void) {
    rt_capture_process_main_thread_();
}

#pragma section(".CRT$XCU", read)
__declspec(allocate(".CRT$XCU")) void(__cdecl *rt_capture_process_main_thread_ctor_)(void) =
    rt_capture_process_main_thread_ctor;
#else
/// @brief GCC/Clang `__attribute__((constructor))` that runs main-thread capture before main().
__attribute__((constructor)) static void rt_capture_process_main_thread_ctor(void) {
    rt_capture_process_main_thread_();
}
#endif

/// @brief Manually re-install the calling thread as the main thread (overrides ctor capture).
/// @details Useful when the runtime is loaded as a shared library and
///          the constructor fired on the loader's thread rather than
///          the actual UI thread. Updates the native identifier while holding
///          the same lock used by readers, then publishes the ready flag.
/// @note Later calls intentionally replace the previously designated thread.
void rt_set_main_thread(void) {
    rt_spin_lock(&g_main_thread_lock_);
#if RT_PLATFORM_WINDOWS
    g_main_thread_id_ = GetCurrentThreadId();
#else
    g_main_thread_ = pthread_self();
#endif
    __atomic_store_n(&g_main_thread_set_, RT_MAIN_THREAD_READY, __ATOMIC_RELEASE);
    rt_spin_unlock(&g_main_thread_lock_);
}

/// @brief Return non-zero when the calling thread is the runtime's designated main thread.
/// @details Lazily captures the main thread on first call if neither
///          the constructor nor `rt_set_main_thread` has run.
///          On Windows uses thread-ID compare; on POSIX uses
///          `pthread_equal` because `pthread_t` is opaque and
///          comparison via `==` isn't portable.
/// @return One when the caller matches the designated main thread, otherwise zero.
int8_t rt_is_main_thread(void) {
    if (__atomic_load_n(&g_main_thread_set_, __ATOMIC_ACQUIRE) != RT_MAIN_THREAD_READY)
        rt_capture_process_main_thread_();

    rt_spin_lock(&g_main_thread_lock_);
#if RT_PLATFORM_WINDOWS
    int8_t result = GetCurrentThreadId() == g_main_thread_id_;
#else
    int8_t result = (int8_t)pthread_equal(pthread_self(), g_main_thread_);
#endif
    rt_spin_unlock(&g_main_thread_lock_);
    return result;
}

/// @brief Trap with a thread-violation diagnostic if the caller isn't on the main thread.
/// @details Used at every entry point that touches GUI, input, or
///          other thread-affine subsystems. On a misuse, raises an
///          `InvalidOperation` trap with the source location so the
///          developer can find the offending call site immediately
///          rather than discovering a corrupted UI later.
/// @param file Source filename supplied by the assertion macro; null is shown
///             as `<unknown>`.
/// @param line Source line included in the trap metadata and message.
/// @note Returns normally only on the designated main thread or if a configured
///       trap hook handles the violation and returns.
void rt_assert_main_thread_(const char *file, int line) {
    if (!rt_is_main_thread()) {
        char buffer[256];
        snprintf(buffer,
                 sizeof(buffer),
                 "%s:%d: GUI/input state accessed from non-main thread",
                 file ? file : "<unknown>",
                 line);
        rt_trap_raise_kind(RT_TRAP_KIND_INVALID_OPERATION, Err_InvalidOperation, line, buffer);
        return;
    }
}

/// @brief Ensure the legacy context is initialized (thread-safe).
///
/// Implements a thread-safe double-checked locking pattern using atomics.
/// The first caller to observe state=0 transitions to state=1, initializes
/// the context, then transitions to state=2. Concurrent callers wait by
/// spinning until state reaches 2.
///
/// **Initialization sequence:**
/// ```
///          Thread A                Thread B
///             │                       │
///     load state=0                    │
///     CAS 0→1 succeeds                │
///             │               load state=1
///     rt_context_init()       spin waiting...
///             │                       │
///     store state=2                   │
///             │               load state=2
///     return                  return
/// ```
///
/// @return Non-zero when the context is ready; zero if initialization trapped
///         and the active trap hook returned control to this function.
///
/// @note Called automatically by rt_legacy_context() and rt_set_current_context().
static int rt_legacy_ensure_init(void) {
    for (;;) {
        int state = __atomic_load_n(&g_legacy_state, __ATOMIC_ACQUIRE);
        if (state == 2)
            return 1;
        if (state == -1) {
            rt_trap("Runtime context initialization failed");
            return 0;
        }

        int expected = 0;
        if (__atomic_compare_exchange_n(
                &g_legacy_state, &expected, 1, /*weak=*/0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            jmp_buf recovery;
            rt_trap_set_recovery(&recovery);
            if (setjmp(recovery) != 0) {
                rt_trap_clear_recovery();
                __atomic_store_n(&g_legacy_state, -1, __ATOMIC_RELEASE);
                rt_trap("Runtime context initialization failed");
                return 0;
            }
            rt_context_init(&g_legacy_ctx);
            rt_trap_clear_recovery();
            __atomic_store_n(&g_legacy_state, 2, __ATOMIC_RELEASE);
            return 1;
        }

        // Another thread is initializing or resetting; yield, then retry the
        // complete state machine because shutdown may publish state 0.
#if RT_PLATFORM_WINDOWS
        SwitchToThread();
#else
        sched_yield();
#endif
    }
}

/// @copydoc rt_context_acquire_state
RtContext *rt_context_acquire_state(rt_context_state_kind_t kind, int *is_legacy) {
    if (is_legacy)
        *is_legacy = 0;
    if (kind < RT_CONTEXT_STATE_RNG || kind > RT_CONTEXT_STATE_LIFECYCLE) {
        rt_trap("Runtime context state kind is invalid");
        return NULL;
    }

    RtContext *bound = g_rt_context;
    if (bound) {
        rt_context_state_lock(bound, kind);
        if (is_legacy)
            *is_legacy = bound == &g_legacy_ctx;
        return bound;
    }

    for (;;) {
        if (!rt_legacy_ensure_init())
            return NULL;
        rt_spin_lock(&g_legacy_handoff_lock);
        if (__atomic_load_n(&g_legacy_state, __ATOMIC_ACQUIRE) != 2) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            continue;
        }
        rt_context_state_lock(&g_legacy_ctx, kind);
        rt_spin_unlock(&g_legacy_handoff_lock);
        if (is_legacy)
            *is_legacy = 1;
        return &g_legacy_ctx;
    }
}

/// @brief Initialize a runtime context with default values.
///
/// Sets up a fresh context with:
/// - Deterministic RNG seed for reproducible random number sequences
/// - Empty file handle table
/// - Empty argument list
/// - Empty module variable storage
/// - Empty type registry
/// - Zero bind count (no threads attached)
///
/// **Usage example:**
/// ```c
/// RtContext ctx;
/// rt_context_init(&ctx);
///
/// // Use the context
/// rt_set_current_context(&ctx);
/// // ... execute Zanna code ...
/// rt_set_current_context(NULL);
///
/// // Cleanup when done
/// rt_context_cleanup(&ctx);
/// ```
///
/// @param ctx Pointer to the context to initialize. Must not be NULL.
///
/// @pre @p ctx is zeroed/uninitialized or has completed rt_context_cleanup();
///      reinitializing a live context would discard ownership metadata.
/// @note Allocates platform-private subsystem locks. Initialization is
///       transactional: failure releases every lock constructed so far and
///       leaves the lifecycle uninitialized.
/// @note The deterministic seed ensures tests produce repeatable results.
///
/// @see rt_context_cleanup For releasing resources when done
void rt_context_init(RtContext *ctx) {
    if (!ctx)
        return;

    __atomic_store_n(&ctx->lifecycle_state, RT_CONTEXT_LIFECYCLE_UNINITIALIZED, __ATOMIC_RELAXED);
    __atomic_store_n(&ctx->bind_count, 0, __ATOMIC_RELAXED);
    for (size_t i = 0; i < RT_CONTEXT_STATE_LOCK_COUNT; ++i)
        ctx->state_locks[i] = NULL;

    // Initialize RNG with deterministic seed (same as old global default)
    ctx->rng_state = 0xDEADBEEFCAFEBABEULL;

    // Initialize empty modvar table
    ctx->modvar_entries = NULL;
    ctx->modvar_count = 0;
    ctx->modvar_capacity = 0;
    ctx->modvar_index_slots = NULL;
    ctx->modvar_index_capacity = 0;

    // Initialize file state
    ctx->file_state.entries = NULL;
    ctx->file_state.count = 0;
    ctx->file_state.capacity = 0;

    // Initialize argument store
    ctx->args_state.items = NULL;
    ctx->args_state.size = 0;
    ctx->args_state.cap = 0;

    // Initialize type registry state
    ctx->type_registry.classes = NULL;
    ctx->type_registry.classes_len = 0;
    ctx->type_registry.classes_cap = 0;
    ctx->type_registry.ifaces = NULL;
    ctx->type_registry.ifaces_len = 0;
    ctx->type_registry.ifaces_cap = 0;
    ctx->type_registry.bindings = NULL;
    ctx->type_registry.bindings_len = 0;
    ctx->type_registry.bindings_cap = 0;
    ctx->type_registry.rw_lock = NULL;
    ctx->type_registry.sealed = 0;

    for (size_t i = 0; i < RT_CONTEXT_STATE_LOCK_COUNT; ++i) {
        ctx->state_locks[i] = rt_context_mutex_create_();
        if (!ctx->state_locks[i]) {
            for (size_t j = 0; j < i; ++j) {
                rt_context_mutex_destroy_((rt_context_mutex_t *)ctx->state_locks[j]);
                ctx->state_locks[j] = NULL;
            }
            rt_trap("Runtime context state-lock initialization failed");
            return;
        }
    }

    if (!rt_type_registry_init(ctx)) {
        for (size_t i = 0; i < RT_CONTEXT_STATE_LOCK_COUNT; ++i) {
            rt_context_mutex_destroy_((rt_context_mutex_t *)ctx->state_locks[i]);
            ctx->state_locks[i] = NULL;
        }
        rt_trap("Runtime context type-registry lock initialization failed");
        return;
    }

    __atomic_store_n(&ctx->lifecycle_state, RT_CONTEXT_LIFECYCLE_READY, __ATOMIC_RELEASE);
}

/// @brief Cleanup a runtime context and free owned resources.
///
/// Releases all resources associated with a context:
/// - Closes open file handles
/// - Releases command-line argument strings
/// - Frees module variable storage and releases any string values
/// - Frees registered class/interface metadata
///
/// After cleanup, the context can be reused by calling rt_context_init() again.
///
/// **Cleanup order:**
/// 1. File state (closes any open files)
/// 2. Argument state (releases string references)
/// 3. Module variables (releases string slot values, frees names and storage)
/// 4. Type registry (frees owned class info structures)
///
/// @param ctx Pointer to the context to clean up. Ignored if NULL.
///
/// @note Safe to call on a context that was initialized and already cleaned.
///       Arbitrary uninitialized storage is not a valid context.
/// @note If the current thread is the context's sole binding, cleanup first
///       unbinds it. A context bound by any other thread is rejected.
/// @note Cleanup claims `CLEANING` while binding publication is excluded. A
///       concurrent bind therefore either increments the count first (causing
///       cleanup to return a trap) or observes a non-ready context and traps.
///
/// @see rt_context_init For initializing a context
void rt_context_cleanup(RtContext *ctx) {
    if (!ctx)
        return;

    size_t bindings = __atomic_load_n(&ctx->bind_count, __ATOMIC_ACQUIRE);
    if (bindings == 1 && g_rt_context == ctx) {
        rt_set_current_context(NULL);
        bindings = __atomic_load_n(&ctx->bind_count, __ATOMIC_ACQUIRE);
    }
    rt_spin_lock(&g_legacy_handoff_lock);
    const int lifecycle = __atomic_load_n(&ctx->lifecycle_state, __ATOMIC_ACQUIRE);
    if (lifecycle == RT_CONTEXT_LIFECYCLE_UNINITIALIZED) {
        rt_spin_unlock(&g_legacy_handoff_lock);
        return;
    }
    if (lifecycle != RT_CONTEXT_LIFECYCLE_READY) {
        rt_spin_unlock(&g_legacy_handoff_lock);
        rt_trap("Runtime context cleanup is already in progress");
        return;
    }

    bindings = __atomic_load_n(&ctx->bind_count, __ATOMIC_ACQUIRE);
    if (bindings != 0) {
        rt_spin_unlock(&g_legacy_handoff_lock);
        rt_trap("Cannot clean up a runtime context while it is bound");
        return;
    }
    __atomic_store_n(&ctx->lifecycle_state, RT_CONTEXT_LIFECYCLE_CLEANING, __ATOMIC_RELEASE);
    rt_spin_unlock(&g_legacy_handoff_lock);

    rt_file_state_cleanup(ctx);
    rt_args_state_cleanup(ctx);

    for (size_t i = 0; i < ctx->modvar_count; ++i) {
        RtModvarEntry *e = &ctx->modvar_entries[i];
        if (e->kind == RT_MODVAR_KIND_STR && e->addr) {
            rt_string *slot = (rt_string *)e->addr;
            if (slot && *slot) {
                rt_string_unref(*slot);
                *slot = NULL;
            }
        }
        free(e->name);
        free(e->addr);
        e->name = NULL;
        e->addr = NULL;
        e->size = 0;
        e->kind = 0;
    }
    free(ctx->modvar_entries);
    ctx->modvar_entries = NULL;
    ctx->modvar_count = 0;
    ctx->modvar_capacity = 0;
    free(ctx->modvar_index_slots);
    ctx->modvar_index_slots = NULL;
    ctx->modvar_index_capacity = 0;

    rt_type_registry_cleanup(ctx);

    for (size_t i = 0; i < RT_CONTEXT_STATE_LOCK_COUNT; ++i) {
        rt_context_mutex_destroy_((rt_context_mutex_t *)ctx->state_locks[i]);
        ctx->state_locks[i] = NULL;
    }
    __atomic_store_n(&ctx->lifecycle_state, RT_CONTEXT_LIFECYCLE_UNINITIALIZED, __ATOMIC_RELEASE);
}

/// @brief Test whether a context may publish or consume a binding.
/// @details Lifecycle state uses atomic publication because initialization and
///          cleanup occur outside the caller's TLS. Binding callers perform
///          this test while holding `g_legacy_handoff_lock`, which makes the
///          ready check and bind-count transition one cleanup-excluding action.
/// @param ctx Candidate caller-owned context.
/// @return Non-zero only while @p ctx is initialized and ready.
static int rt_context_is_ready_(const RtContext *ctx) {
    return ctx &&
           __atomic_load_n(&ctx->lifecycle_state, __ATOMIC_ACQUIRE) == RT_CONTEXT_LIFECYCLE_READY;
}

/// @brief Try to reserve one thread binding on a runtime context.
/// @details Uses a compare-exchange loop instead of fetch-add so `SIZE_MAX`
///          cannot wrap to zero. The caller decides how to report failure and
///          can therefore release any lifecycle lock before invoking a trap
///          hook that may return or perform a non-local jump.
/// @param ctx Initialized context whose binding count is incremented.
/// @param previous Receives the count observed immediately before increment.
/// @return Non-zero on success; zero when the counter is already `SIZE_MAX`.
static int rt_context_try_add_binding(RtContext *ctx, size_t *previous) {
    size_t observed = __atomic_load_n(&ctx->bind_count, __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == SIZE_MAX)
            return 0;
        size_t desired = observed + 1;
        if (__atomic_compare_exchange_n(&ctx->bind_count,
                                        &observed,
                                        desired,
                                        /*weak=*/1,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (previous)
                *previous = observed;
            return 1;
        }
    }
}

/// @brief Try to release one thread binding from a runtime context.
/// @details Uses a compare-exchange loop so a corrupted zero count cannot
///          underflow to `SIZE_MAX`. As with @ref rt_context_try_add_binding,
///          failure is reported to the caller before any trap is dispatched.
/// @param ctx Initialized context whose binding count is decremented.
/// @param previous Receives the count observed immediately before decrement.
/// @return Non-zero on success; zero when the counter is already zero.
static int rt_context_try_remove_binding(RtContext *ctx, size_t *previous) {
    size_t observed = __atomic_load_n(&ctx->bind_count, __ATOMIC_ACQUIRE);
    for (;;) {
        if (observed == 0)
            return 0;
        size_t desired = observed - 1;
        if (__atomic_compare_exchange_n(&ctx->bind_count,
                                        &observed,
                                        desired,
                                        /*weak=*/1,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE)) {
            if (previous)
                *previous = observed;
            return 1;
        }
    }
}

/// @brief Acquire one subsystem lock on two contexts in address order.
/// @details Migration already holds the process handoff lock, but stable address
///          ordering also makes this helper safe against future two-context
///          operations. Raw acquisition intentionally avoids the trap-unwind
///          stack because the migration section performs no trapping work.
/// @param a First context.
/// @param b Second distinct context.
/// @param kind Subsystem lock to acquire on both.
static void rt_context_lock_pair_raw_(RtContext *a, RtContext *b, rt_context_state_kind_t kind) {
    int a_first = (uintptr_t)(void *)a < (uintptr_t)(void *)b;
    RtContext *first = a_first ? a : b;
    RtContext *second = a_first ? b : a;
    rt_context_mutex_lock_raw_(rt_context_mutex_for_(first, kind));
    rt_context_mutex_lock_raw_(rt_context_mutex_for_(second, kind));
}

/// @brief Release a pair acquired by @ref rt_context_lock_pair_raw_.
/// @param a First context originally supplied.
/// @param b Second distinct context originally supplied.
/// @param kind Subsystem lock held on both.
static void rt_context_unlock_pair_raw_(RtContext *a, RtContext *b, rt_context_state_kind_t kind) {
    int a_first = (uintptr_t)(void *)a < (uintptr_t)(void *)b;
    RtContext *first = a_first ? a : b;
    RtContext *second = a_first ? b : a;
    rt_context_mutex_unlock_raw_(rt_context_mutex_for_(second, kind));
    rt_context_mutex_unlock_raw_(rt_context_mutex_for_(first, kind));
}

/// @brief Move legacy-compatible subsystem state when the destination is empty.
/// @details Transfers file channels, command-line arguments, and the complete
///          type registry independently. Each transfer clears the source so
///          ownership remains unique. Callers must hold
///          `g_legacy_handoff_lock`, and one argument must be the static legacy
///          context; passing the same context for both sides is a safe no-op.
/// @param destination Context receiving any subsystem whose primary storage is empty.
/// @param source Context relinquishing the transferred subsystem state.
static void rt_context_move_legacy_state(RtContext *destination, RtContext *source) {
    if (!destination || !source || destination == source)
        return;

    rt_context_lock_pair_raw_(destination, source, RT_CONTEXT_STATE_FILE);
    rt_context_lock_pair_raw_(destination, source, RT_CONTEXT_STATE_ARGS);
    rt_type_registry_state_write_lock(destination);
    rt_type_registry_state_write_lock(source);

    if (destination->file_state.entries == NULL && source->file_state.entries != NULL) {
        destination->file_state = source->file_state;
        source->file_state.entries = NULL;
        source->file_state.count = 0;
        source->file_state.capacity = 0;
    }

    if (destination->args_state.items == NULL && source->args_state.items != NULL) {
        destination->args_state = source->args_state;
        source->args_state.items = NULL;
        source->args_state.size = 0;
        source->args_state.cap = 0;
    }

    int destination_registry_empty = destination->type_registry.classes == NULL &&
                                     destination->type_registry.ifaces == NULL &&
                                     destination->type_registry.bindings == NULL;
    int source_registry_nonempty = source->type_registry.classes != NULL ||
                                   source->type_registry.ifaces != NULL ||
                                   source->type_registry.bindings != NULL;
    if (destination_registry_empty && source_registry_nonempty) {
        destination->type_registry.classes = source->type_registry.classes;
        destination->type_registry.classes_len = source->type_registry.classes_len;
        destination->type_registry.classes_cap = source->type_registry.classes_cap;
        destination->type_registry.ifaces = source->type_registry.ifaces;
        destination->type_registry.ifaces_len = source->type_registry.ifaces_len;
        destination->type_registry.ifaces_cap = source->type_registry.ifaces_cap;
        destination->type_registry.bindings = source->type_registry.bindings;
        destination->type_registry.bindings_len = source->type_registry.bindings_len;
        destination->type_registry.bindings_cap = source->type_registry.bindings_cap;
        destination->type_registry.sealed = source->type_registry.sealed;
        source->type_registry.classes = NULL;
        source->type_registry.classes_len = 0;
        source->type_registry.classes_cap = 0;
        source->type_registry.ifaces = NULL;
        source->type_registry.ifaces_len = 0;
        source->type_registry.ifaces_cap = 0;
        source->type_registry.bindings = NULL;
        source->type_registry.bindings_len = 0;
        source->type_registry.bindings_cap = 0;
        source->type_registry.sealed = 0;
    }

    rt_type_registry_state_write_unlock(source);
    rt_type_registry_state_write_unlock(destination);
    rt_context_unlock_pair_raw_(destination, source, RT_CONTEXT_STATE_ARGS);
    rt_context_unlock_pair_raw_(destination, source, RT_CONTEXT_STATE_FILE);
}

/// @copydoc rt_context_reserve_thread_binding
int rt_context_reserve_thread_binding(RtContext *ctx) {
    if (!ctx) {
        rt_trap("Thread context reservation requires a context");
        return 0;
    }

    for (;;) {
        if (!rt_legacy_ensure_init())
            return 0;
        rt_spin_lock(&g_legacy_handoff_lock);
        if (__atomic_load_n(&g_legacy_state, __ATOMIC_ACQUIRE) != 2) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            continue;
        }
        if (!rt_context_is_ready_(ctx)) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Cannot reserve an uninitialized runtime context");
            return 0;
        }

        size_t previous = 0;
        if (!rt_context_try_add_binding(ctx, &previous)) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Runtime context binding count overflow");
            return 0;
        }
        if (previous == 0)
            rt_context_move_legacy_state(ctx, &g_legacy_ctx);
        rt_spin_unlock(&g_legacy_handoff_lock);
        return 1;
    }
}

/// @copydoc rt_context_adopt_reserved_thread_binding
int rt_context_adopt_reserved_thread_binding(RtContext *ctx) {
    if (!ctx) {
        rt_trap("Thread context adoption requires a context");
        return 0;
    }
    if (g_rt_context != NULL) {
        rt_trap("Thread context adoption requires an unbound thread");
        return 0;
    }

    for (;;) {
        if (!rt_legacy_ensure_init())
            return 0;
        rt_spin_lock(&g_legacy_handoff_lock);
        if (__atomic_load_n(&g_legacy_state, __ATOMIC_ACQUIRE) != 2) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            continue;
        }
        if (!rt_context_is_ready_(ctx)) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Cannot adopt an uninitialized runtime context");
            return 0;
        }
        if (__atomic_load_n(&ctx->bind_count, __ATOMIC_ACQUIRE) == 0) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Thread context reservation is no longer live");
            return 0;
        }

        g_rt_context = ctx;
        rt_spin_unlock(&g_legacy_handoff_lock);
        return 1;
    }
}

/// @copydoc rt_context_cancel_reserved_thread_binding
int rt_context_cancel_reserved_thread_binding(RtContext *ctx) {
    if (!ctx) {
        rt_trap("Thread context cancellation requires a context");
        return 0;
    }

    for (;;) {
        if (!rt_legacy_ensure_init())
            return 0;
        rt_spin_lock(&g_legacy_handoff_lock);
        if (__atomic_load_n(&g_legacy_state, __ATOMIC_ACQUIRE) != 2) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            continue;
        }
        if (!rt_context_is_ready_(ctx)) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Cannot cancel a binding on an uninitialized runtime context");
            return 0;
        }

        size_t previous = 0;
        if (!rt_context_try_remove_binding(ctx, &previous)) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Runtime context binding count underflow");
            return 0;
        }
        if (previous == 1)
            rt_context_move_legacy_state(&g_legacy_ctx, ctx);
        rt_spin_unlock(&g_legacy_handoff_lock);
        return 1;
    }
}

/// @brief Bind a runtime context to the current thread.
///
/// Associates a context with the calling thread, enabling all runtime
/// functions to use that context's state. This is the primary mechanism
/// by which VMs execute Zanna code with isolated state.
///
/// **Binding lifecycle:**
/// ```
/// Thread                              Context
///   │                                    │
///   │── set_current_context(ctx) ───────▶│  bind_count++
///   │                                    │
///   │          [execute code]            │
///   │                                    │
///   │── set_current_context(NULL) ──────▶│  bind_count--
///   │                                    │
/// ```
///
/// **State transfer on first bind:**
/// When a context is bound for the first time (bind_count 0→1), any state
/// accumulated in the legacy context is transferred to the new context:
/// - File handles (if destination empty)
/// - Command-line arguments (if destination empty)
/// - Type registrations (if destination empty)
///
/// **State transfer on last unbind:**
/// When the last thread unbinds (bind_count 1→0 with NULL destination),
/// state is transferred back to the legacy context so code running after
/// VM exit continues to work.
///
/// @param ctx Context to bind, or NULL to unbind the current context.
///
/// @note Thread-safe: updates are serialized by atomic counters and spinlock.
/// @note Binding the same context that's already bound is a no-op.
/// @note Calling with NULL when no context is bound is a no-op.
///
/// @see rt_get_current_context For reading the current context
/// @see rt_legacy_context For the fallback when no context is bound
void rt_set_current_context(RtContext *ctx) {
    RtContext *old = g_rt_context;
    if (old == ctx)
        return;

    for (;;) {
        if (!rt_legacy_ensure_init())
            return;

        /* Shutdown changes READY to INITIALIZING before taking this lock.
           Rechecking under the lock closes the window between lazy-init and
           the binding transaction without ever waiting while holding it. */
        rt_spin_lock(&g_legacy_handoff_lock);
        if (__atomic_load_n(&g_legacy_state, __ATOMIC_ACQUIRE) != 2) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            continue;
        }

        size_t old_previous = 0;
        size_t new_previous = 0;
        if (ctx && !rt_context_is_ready_(ctx)) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Cannot bind an uninitialized runtime context");
            return;
        }
        if (ctx && !rt_context_try_add_binding(ctx, &new_previous)) {
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Runtime context binding count overflow");
            return;
        }

        if (old && !rt_context_try_remove_binding(old, &old_previous)) {
            if (ctx && !rt_context_try_remove_binding(ctx, NULL))
                rt_abort("Runtime context binding rollback failed");
            rt_spin_unlock(&g_legacy_handoff_lock);
            rt_trap("Runtime context binding count underflow");
            return;
        }

        if (old && old_previous == 1 && ctx == NULL) {
            // Last thread unbound: move state back to legacy so calls after VM exit keep working.
            rt_context_move_legacy_state(&g_legacy_ctx, old);
        }

        if (ctx && new_previous == 0) {
            // First bind: adopt legacy state to preserve pre-context behaviour.
            rt_context_move_legacy_state(ctx, &g_legacy_ctx);
        }

        /* Publish TLS only after counter updates and any first-bind state
           migration are complete. Runtime calls on this thread cannot observe
           a partially adopted context. */
        g_rt_context = ctx;
        rt_spin_unlock(&g_legacy_handoff_lock);
        return;
    }
}

/// @brief Retrieve the current thread's runtime context.
///
/// Returns the context bound to the calling thread via rt_set_current_context(),
/// or NULL if no context is currently bound. Runtime functions typically call
/// this first, then fall back to rt_legacy_context() if the result is NULL.
///
/// **Common usage pattern:**
/// ```c
/// static RtContext *get_effective_context(void) {
///     RtContext *ctx = rt_get_current_context();
///     if (!ctx)
///         ctx = rt_legacy_context();
///     return ctx;
/// }
/// ```
///
/// @return The currently bound context, or NULL if none is bound.
///
/// @note Thread-safe: reads thread-local storage.
/// @note O(1) time complexity.
///
/// @see rt_set_current_context For binding a context
/// @see rt_legacy_context For the fallback context
RtContext *rt_get_current_context(void) {
    return g_rt_context;
}

/// @brief Get the global legacy context for backward compatibility.
///
/// Returns the shared fallback context used when no VM context is bound.
/// The legacy context is lazily initialized on first access. An explicit
/// quiescent shutdown can clean it and reset lazy initialization for later reuse.
///
/// **When to use:**
/// - Native code not running under a VM
/// - Runtime functions called after VM exit
/// - Test code that doesn't need context isolation
///
/// **Example:**
/// ```c
/// // In runtime function:
/// RtContext *ctx = rt_get_current_context();
/// if (!ctx)
///     ctx = rt_legacy_context();  // Use fallback
/// // Now ctx is guaranteed non-NULL
/// ```
///
/// @return Borrowed pointer to the global legacy context, or `NULL` if
///         initialization trapped and the trap handler returned.
///
/// @note Thread-safe: uses atomic lazy initialization.
/// @note The returned context is shared across all threads not using a VM context.
///
/// @see rt_get_current_context For getting the thread's bound context
RtContext *rt_legacy_context(void) {
    if (!rt_legacy_ensure_init())
        return NULL;
    return &g_legacy_ctx;
}

/// @brief Clean up the legacy runtime context at process shutdown.
/// @details Calls rt_context_cleanup on the static g_legacy_ctx to close
///          any open BASIC file channels, release argument storage, and
///          free the type registry.  No-op if the legacy context was never
///          initialized.
///
///          Called from the rt_global_shutdown atexit handler in rt_heap.c,
///          AFTER GC finalizers have run and BEFORE string intern teardown.
void rt_legacy_context_shutdown(void) {
    /* A test or embedder may explicitly bind the legacy context. Release the
       calling thread's sole binding before claiming the reset state so the
       ordinary unbind path never waits for a shutdown owned by itself. */
    if (g_rt_context == &g_legacy_ctx &&
        __atomic_load_n(&g_legacy_ctx.bind_count, __ATOMIC_ACQUIRE) == 1)
        rt_set_current_context(NULL);

    int expected = 2;
    if (!__atomic_compare_exchange_n(&g_legacy_state,
                                     &expected,
                                     1,
                                     /*weak=*/0,
                                     __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return;

    rt_spin_lock(&g_legacy_handoff_lock);
    if (__atomic_load_n(&g_legacy_ctx.bind_count, __ATOMIC_ACQUIRE) != 0) {
        __atomic_store_n(&g_legacy_state, 2, __ATOMIC_RELEASE);
        rt_spin_unlock(&g_legacy_handoff_lock);
        return;
    }
    rt_spin_unlock(&g_legacy_handoff_lock);

    /* State 1 prevents new binding transactions while cleanup owns the static
       storage. Publishing 0 makes subsequent access perform a fresh init. */
    rt_context_cleanup(&g_legacy_ctx);
    __atomic_store_n(&g_legacy_state, 0, __ATOMIC_RELEASE);
}
