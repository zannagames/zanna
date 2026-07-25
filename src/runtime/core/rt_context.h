//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
// File: src/runtime/core/rt_context.h
/// @file
/// @brief Declares caller-owned per-VM runtime contexts and binding lifecycle.
///
// Purpose: Per-VM runtime context that isolates all global mutable state across concurrent VM
// instances, including module variables, file channels, argument stores, and type registry.
//
// Key invariants:
//   - Thread-local binding ensures each thread accesses its active VM context.
//   - A VM thread calls rt_set_current_context before invoking stateful runtime functions.
//   - Mutable context subsystems are independently serialized for inherited multi-thread use.
//   - Binding reservations and cleanup are one lifecycle transaction: cleaned contexts reject
//     new bindings until they are explicitly initialized again.
//   - RtContext owns all sub-state structures; releasing the context releases all children.
//
// Ownership/Lifetime:
//   - The VM owns the RtContext object and is responsible for its lifetime.
//   - A TLS binding or pre-start child reservation prevents runtime cleanup, but the runtime does
//     not allocate or free the caller-owned RtContext structure itself.
//
// Links: src/runtime/core/rt_context.c (implementation), src/runtime/core/rt_string.h
//
//===----------------------------------------------------------------------===//
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rt_string.h"

#ifdef __cplusplus
extern "C" {
#endif

/// @brief Module-level variable entry for per-VM storage.
typedef struct RtModvarEntry {
    char *name;    ///< Owned copy of variable name.
    int kind;      ///< Storage kind (I64, F64, I1, PTR, STR).
    void *addr;    ///< Allocated storage block.
    size_t size;   ///< Size in bytes.
    uint64_t hash; ///< Cached hash of (name, kind) for indexed lookup.
} RtModvarEntry;

/// @brief Storage kinds used to size and clean module-variable slots.
enum {
    /// Signed 64-bit integer slot.
    RT_MODVAR_KIND_I64 = 0,
    /// IEEE-754 double slot.
    RT_MODVAR_KIND_F64 = 1,
    /// Boolean byte slot.
    RT_MODVAR_KIND_I1 = 2,
    /// Generic runtime pointer slot.
    RT_MODVAR_KIND_PTR = 3,
    /// Retained runtime string-handle slot.
    RT_MODVAR_KIND_STR = 4,
    /// Opaque fixed-size byte block.
    RT_MODVAR_KIND_BLOCK = 5,
};

/// @brief Opaque file-channel entry defined by the optional I/O component.
struct RtFileChannelEntry;

/// @brief Per-context dynamically allocated file-channel table.
typedef struct RtFileState {
    struct RtFileChannelEntry *entries;
    size_t count;
    size_t capacity;
} RtFileState;

/// @brief Per-context retained command-line argument array.
typedef struct RtArgsState {
    rt_string *items;
    size_t size;
    size_t cap;
} RtArgsState;

/// @brief Per-context OOP class/interface/binding registry storage.
///
/// Concrete element types and the platform read/write lock remain private to
/// the optional OOP runtime component.
typedef struct RtTypeRegistryState {
    void *classes;
    size_t classes_len, classes_cap;
    void *ifaces;
    size_t ifaces_len, ifaces_cap;
    void *bindings;
    size_t bindings_len, bindings_cap;
    void *rw_lock; ///< Opaque platform rwlock (SRWLOCK / pthread_rwlock_t)
    int sealed;    ///< 1 after init; enables lock-free reads via atomic check
} RtTypeRegistryState;

/// Number of opaque subsystem mutex slots embedded in @ref RtContext.
/// @details The concrete lock types remain platform-private. Slots separately
///          serialize RNG, module-variable metadata, file channels, arguments,
///          and context-lifecycle handoff so unrelated subsystems do not share
///          one hot lock.
#define RT_CONTEXT_STATE_LOCK_COUNT 5

/// @brief Lifecycle states published through @ref RtContext::lifecycle_state.
/// @details State transitions are synchronized with context binding. A context
///          begins uninitialized, becomes ready only after every native lock is
///          constructed, and enters cleaning before owned subsystem storage is
///          released. Only a ready context may acquire a new binding.
typedef enum RtContextLifecycleState {
    RT_CONTEXT_LIFECYCLE_UNINITIALIZED = 0,
    RT_CONTEXT_LIFECYCLE_READY = 1,
    RT_CONTEXT_LIFECYCLE_CLEANING = 2
} RtContextLifecycleState;

/// @brief Per-VM runtime context isolating global state.
/// @details Moves runtime global variables into per-VM storage so multiple
///          VM instances can coexist without interfering. Each VM owns one
///          RtContext and binds it to the current thread before execution.
typedef struct RtContext {
    // Random number generator state (from rt_random.c)
    uint64_t rng_state;

    // Module-level variable table (from rt_modvar.c)
    RtModvarEntry *modvar_entries;
    size_t modvar_count;
    size_t modvar_capacity;
    size_t *modvar_index_slots;
    size_t modvar_index_capacity;

    // File channel table (rt_file.c)
    RtFileState file_state;

    // Command-line argument store (rt_args.c)
    RtArgsState args_state;

    // Type registry (rt_type_registry.c)
    RtTypeRegistryState type_registry;

    /// Opaque recursive mutexes for mutable context subsystems.
    /// @details Initialized and destroyed with the context. Runtime code must
    ///          access these only through `rt_context_internal.h`; exposing the
    ///          storage array here keeps @ref RtContext caller-allocatable while
    ///          preserving platform-independent public headers.
    void *state_locks[RT_CONTEXT_STATE_LOCK_COUNT];

    /// Atomic lifecycle publication state from @ref RtContextLifecycleState.
    /// @details Binding and child-reservation paths accept only `READY`.
    ///          Cleanup changes `READY` to `CLEANING` while the global handoff
    ///          lock excludes a new binding, then publishes `UNINITIALIZED`
    ///          after every owned lock and subsystem allocation is released.
    int lifecycle_state;

    /// Number of threads currently bound through @ref rt_set_current_context.
    /// @details Accessed with atomic operations. The runtime rejects overflow,
    ///          underflow, and cleanup while another thread remains bound.
    size_t bind_count;

    // Future expansions:
    // - VM-only state
} RtContext;

/// @brief Initialize a runtime context with default values.
/// @details Sets the deterministic RNG seed, initializes empty subsystem
///          state, and transactionally constructs every native lock. The
///          lifecycle becomes `READY` only after complete success.
/// @param ctx Context to initialize.
/// @note Native allocation or lock initialization failure traps and leaves the
///       context `UNINITIALIZED` with no owned lock storage.
void rt_context_init(RtContext *ctx);

/// @brief Cleanup a runtime context and free owned resources.
/// @details Frees file, argument, module-variable, and type-registry state. If
///          the caller is the sole bound thread, the function first unbinds;
///          cleanup traps and returns when any other binding remains.
/// @param ctx Initialized context to clean up, or NULL for a no-op.
/// @note Repeated cleanup is supported, but arbitrary uninitialized storage is
///       not a valid context.
void rt_context_cleanup(RtContext *ctx);

/// @brief Bind a runtime context to the current thread.
/// @details Sets the thread-local context pointer so subsequent runtime calls
///          access this VM's state. Counter changes and first/last-binding
///          legacy-state migration form one transaction; the prior binding is
///          preserved if the operation traps. A cleaned or incompletely
///          initialized context is rejected before TLS publication.
/// @param ctx Initialized context to bind, or NULL to unbind.
void rt_set_current_context(RtContext *ctx);

/// @brief Retrieve the current thread's runtime context.
/// @details Returns the context bound via rt_set_current_context.
/// @return Borrowed active context, or NULL if none bound.
RtContext *rt_get_current_context(void);

/// @brief Access the process-wide legacy runtime context.
/// @details Initializes on first use. Used to preserve single-VM behaviour when
///          no VM-bound context is active. A successful shutdown resets the
///          lazy state, so a later call returns a freshly initialized context.
/// @return Shared legacy context, or NULL only if initialization trapped and
///         the configured trap handler returned.
RtContext *rt_legacy_context(void);

/// @brief Clean up the legacy runtime context at process shutdown.
/// @details Closes any open BASIC file channels, releases argument storage,
///          and frees the type registry held by the static legacy context. A
///          sole binding owned by the caller is released automatically. The
///          operation is deferred when another thread remains bound and is a
///          no-op if the legacy context was never initialized.
/// @note Call only from a quiescent shutdown phase; borrowed pointers returned
///       by @ref rt_legacy_context are not lifetime reservations.
void rt_legacy_context_shutdown(void);

#ifdef __cplusplus
}
#endif
