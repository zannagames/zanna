//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/runtime/threads/rt_threads_primitives.cpp
// Purpose: Cross-platform synchronization primitives backing the
//          `Zanna.Threads.Gate`, `Zanna.Threads.Barrier`, and
//          `Zanna.Threads.RwLock` classes. Implemented in C++ to share one
//          portable `std::mutex` / `std::condition_variable` substrate across
//          Windows, macOS, and Linux without pulling in external dependencies.
//
// Key invariants:
//   - Gate acquisition is FIFO-fair across waiters; cancelled waiters are
//     removed from the queue without disturbing later arrivals.
//   - Barrier releases all parties simultaneously and resets per generation;
//     finalization wakes blocked participants so they trap rather than remain
//     stranded on detached state.
//   - RwLock provides writer-preference: queued writers block new readers so
//     a steady stream of readers cannot starve a waiting writer.
//   - All public entry points downcast their handle through a templated
//     `requireObject<T>` helper that validates the runtime class id before
//     touching internal state.
//
// Ownership/Lifetime:
//   - Gate / Barrier / RwLock objects are runtime-managed (refcounted) and
//     finalized through their `rt_obj` finalizer hook.
//   - Internal mutex / condvar / waiter-queue state is heap-allocated and
//     freed by the same finalizer.
//
// Links: src/runtime/threads/rt_threads.h (public API surface, class IDs),
//        src/runtime/threads/rt_monitor.h (alternative locking primitive),
//        src/runtime/threads/rt_safe_i64.c (related thread-safe atomic cell)
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Cross-platform implementations for Gate, Barrier, and RwLock.
/// @details Provides FIFO-fair gates, reusable barriers, and writer-preferred
///          reader-writer locks used by Zanna.Threads.* runtime APIs.

#include "rt_threads.h"

#include "rt.hpp"
#include "rt_object.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

// ============================================================================
// Zanna.Threads.Gate
// ============================================================================

/// @brief Per-thread waiter state for gate acquisition.
/// @details Each waiting thread owns a condition variable and a granted flag
///          that is set when a permit is reserved for it.
struct GateWaiter {
    std::condition_variable cv;
    bool granted = false;
    bool cancelled = false;
};

/// @brief Shared gate state implementing a FIFO semaphore.
/// @details Tracks permit count and a queue of waiters to ensure fairness.
struct GateState {
    /// @brief Construct gate state with the requested number of permits.
    /// @param initial_permits Initial available-permit count.
    explicit GateState(int64_t initial_permits) : permits(initial_permits) {}

    std::mutex mu;
    int64_t permits = 0;
    std::deque<GateWaiter *> waiters;
    bool closing = false;
    bool owner_detached = false;
};

/// @brief Runtime object wrapper storing gate state.
/// @details The RtGate object is managed by the runtime heap and owns a pointer
///          to the heap-allocated GateState.
struct RtGate {
    GateState *state = nullptr;
};

/// @brief Allocate raw storage and placement-construct primitive state.
/// @details Constructor exceptions are contained and converted to NULL after
///          releasing the raw allocation.
/// @tparam T State type to construct.
/// @tparam Args Constructor argument types.
/// @param args Arguments forwarded to `T`'s constructor.
/// @return Constructed state pointer, or nullptr on allocation or construction failure.
template <typename T, typename... Args> static T *allocateState(Args &&...args) {
    void *mem = std::malloc(sizeof(T));
    if (!mem)
        return nullptr;
    try {
        return ::new (mem) T(std::forward<Args>(args)...);
    } catch (...) {
        std::free(mem);
        return nullptr;
    }
}

/// @brief Destroy primitive state and release its malloc-backed storage.
/// @tparam T Concrete state type.
/// @param state State pointer to consume, or nullptr.
template <typename T> static void destroyState(T *state) {
    if (!state)
        return;
    state->~T();
    std::free(state);
}

/// @brief Test whether a detached gate state has no remaining stack waiters.
/// @details Called with GateState::mu held after finalization or waiter removal.
///          A detached state has no owning runtime object pointer left, so the
///          last waiter to leave is responsible for freeing the heap state.
/// @param state Gate state protected by the caller-held mutex.
/// @return True when @p state may be destroyed after releasing the mutex.
static bool gate_detached_and_idle_locked(const GateState &state) {
    return state.owner_detached && state.waiters.empty();
}

//===----------------------------------------------------------------------===//
// Shared Validation Helper
//===----------------------------------------------------------------------===//

/// @brief Release a retained runtime primitive object.
/// @details A NULL pointer is ignored; an object whose reference count reaches
///          zero is finalized and freed through the runtime heap.
/// @param obj Runtime object reference to release, or nullptr.
static void releaseRuntimeObject(void *obj) {
    if (obj && rt_obj_release_check0(obj))
        rt_obj_free(obj);
}

template <typename T>
/// @brief Validate and cast an opaque primitive object to its concrete wrapper.
/// @details Every Gate, Barrier, and RwLock entry point uses this helper. NULL
///          traps with @p what or a generated type diagnostic; a class mismatch
///          or detached internal state traps as an invalid object.
/// @tparam T Target runtime-wrapper type.
/// @param obj Opaque runtime object pointer.
/// @param classId Required runtime class ID, or zero to disable the class check.
/// @param typeName Type name used in generated diagnostics.
/// @param what Optional caller-specific NULL diagnostic.
/// @return Valid typed wrapper, or nullptr after a returning trap hook.
static T *requireObject(void *obj, int64_t classId, const char *typeName, const char *what) {
    if (!obj) {
        if (what) {
            rt_trap(what);
        } else {
            static thread_local char msg[128];
            std::snprintf(msg, sizeof(msg), "%s: null object", typeName ? typeName : "Object");
            rt_trap(msg);
        }
        return nullptr;
    }
    if (classId != 0 && !rt_obj_is_instance(obj, classId, sizeof(T))) {
        static thread_local char msg[128];
        std::snprintf(msg, sizeof(msg), "%s: invalid object", typeName ? typeName : "Object");
        rt_trap(msg);
        return nullptr;
    }
    auto *typed = static_cast<T *>(obj);
    if (!typed->state) {
        static thread_local char msg[128];
        std::snprintf(msg, sizeof(msg), "%s: invalid object", typeName ? typeName : "Object");
        rt_trap(msg);
        return nullptr;
    }
    return typed;
}

/// @brief Build a saturating steady-clock deadline.
/// @param ms Relative delay in milliseconds; non-positive values select now.
/// @return `now + ms` when representable, otherwise the maximum steady-clock time point.
static std::chrono::steady_clock::time_point steadyDeadlineFromNow(int64_t ms) {
    const auto now = std::chrono::steady_clock::now();
    if (ms <= 0)
        return now;
    const auto max_delta = std::chrono::steady_clock::time_point::max() - now;
    const auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(max_delta).count();
    if (max_ms <= 0 || ms >= max_ms)
        return std::chrono::steady_clock::time_point::max();
    return now + std::chrono::milliseconds(ms);
}

/// @brief Wait for a gate waiter without importing unavailable MSVC debug CRT timed-wait APIs.
/// @details Some MSVC 14.50 debug CRT installations ship an import library that references
///          `_Cnd_timedwait_for_unchecked` while the matching debug DLL does not export it.
///          On Windows, avoid `std::condition_variable::wait_until` so native demo binaries
///          that use Gate.TryEnterFor can still load under the installed debug runtime.
/// @param state Locked gate state whose closing flag is observed.
/// @param waiter Stack waiter whose granted or canceled state ends the wait.
/// @param lock Owning unique lock, temporarily released by the Windows polling path.
/// @param deadline Absolute steady-clock deadline.
/// @return True after a signal/state transition, or false when the deadline expires first.
static bool waitGateUntil(GateState &state,
                          GateWaiter &waiter,
                          std::unique_lock<std::mutex> &lock,
                          std::chrono::steady_clock::time_point deadline) {
#ifdef _WIN32
    while (!waiter.granted && !waiter.cancelled && !state.closing) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
            return false;
        const int64_t remaining_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const DWORD pause_ms =
            static_cast<DWORD>(remaining_ms <= 0 ? 1 : std::min<int64_t>(remaining_ms, 10));
        lock.unlock();
        ::Sleep(pause_ms);
        lock.lock();
    }
    return true;
#else
    return waiter.cv.wait_until(lock, deadline) != std::cv_status::timeout || waiter.granted;
#endif
}

/// @brief Validate a gate object pointer and return its typed wrapper.
/// @param gate Opaque gate pointer passed from the runtime.
/// @param what Error string to use when trapping on NULL.
/// @return Valid RtGate pointer, or nullptr if validation fails.
static RtGate *require_gate(void *gate, const char *what) {
    return requireObject<RtGate>(gate, RT_GATE_CLASS_ID, "Gate", what);
}

/// @brief Finalize a gate object and detach its shared state.
/// @details Marks the state closing, cancels every stack-owned waiter, and
///          clears the runtime wrapper. State is destroyed immediately only
///          when no waiter remains; otherwise the last departing waiter frees it.
/// @param obj Runtime object pointer to finalize.
static void gate_finalizer(void *obj) {
    auto *gate = static_cast<RtGate *>(obj);
    GateState *state = gate->state;
    if (!state)
        return;

    bool can_delete = false;
    {
        std::unique_lock<std::mutex> lock(state->mu);
        state->closing = true;
        state->owner_detached = true;
        for (GateWaiter *waiter : state->waiters) {
            waiter->cancelled = true;
            waiter->cv.notify_one();
        }
        can_delete = gate_detached_and_idle_locked(*state);
    }

    gate->state = nullptr;
    if (can_delete)
        destroyState(state);
}

// ============================================================================
// Zanna.Threads.Barrier
// ============================================================================

/// @brief Shared barrier state for coordinating fixed parties.
/// @details Tracks arrival count and generation so the barrier can be reused.
struct BarrierState {
    /// @brief Construct reusable barrier state for a fixed party count.
    /// @param parties_ Number of arrivals required per generation.
    explicit BarrierState(int64_t parties_) : parties(parties_) {}

    std::mutex mu;
    std::condition_variable cv;
    int64_t parties = 0;
    int64_t waiting = 0;
    int64_t generation = 0;
    bool closing = false;
    bool owner_detached = false;
};

/// @brief Test whether a detached barrier state has no waiting threads.
/// @details Called with BarrierState::mu held. When true, the runtime object has
///          already been finalized and every blocked arriver has left, so the
///          heap state can be destroyed outside the mutex scope.
/// @param state Barrier state protected by the caller-held mutex.
/// @return True when @p state may be destroyed after unlocking.
static bool barrier_detached_and_idle_locked(const BarrierState &state) {
    return state.owner_detached && state.waiting == 0;
}

/// @brief Runtime object wrapper storing barrier state.
/// @details The RtBarrier object is managed by the runtime heap and owns a
///          pointer to the heap-allocated BarrierState.
struct RtBarrier {
    BarrierState *state = nullptr;
};

/// @brief Validate a barrier object pointer and return its typed wrapper.
/// @param barrier Opaque barrier pointer passed from the runtime.
/// @param what Error string to use when trapping on NULL.
/// @return Valid RtBarrier pointer, or nullptr if validation fails.
static RtBarrier *require_barrier(void *barrier, const char *what) {
    return requireObject<RtBarrier>(barrier, RT_BARRIER_CLASS_ID, "Barrier", what);
}

/// @brief Finalize a barrier object and detach its shared state.
/// @details Marks the state closing, advances its generation, and wakes all
///          blocked arrivers. The last departing arriver destroys state when
///          immediate destruction is unsafe.
/// @param obj Runtime object pointer to finalize.
static void barrier_finalizer(void *obj) {
    auto *barrier = static_cast<RtBarrier *>(obj);
    BarrierState *state = barrier->state;
    if (!state)
        return;

    bool can_delete = false;
    {
        std::unique_lock<std::mutex> lock(state->mu);
        state->closing = true;
        state->owner_detached = true;
        ++state->generation;
        state->cv.notify_all();
        can_delete = barrier_detached_and_idle_locked(*state);
    }

    barrier->state = nullptr;
    if (can_delete)
        destroyState(state);
}

// ============================================================================
// Zanna.Threads.RwLock
// ============================================================================

/// @brief Writer wait node for the reader-writer lock.
/// @details Writers enqueue in FIFO order and are signaled individually.
struct RwLockWriterWaiter {
    std::condition_variable cv;
    bool cancelled = false;
};

/// @brief Shared reader-writer lock state with writer preference.
/// @details Tracks active readers, writer ownership, recursion depth, and a
///          FIFO queue of waiting writers.
struct RwLockState {
    std::mutex mu;
    std::condition_variable readers_cv;

    int64_t active_readers = 0;
    int64_t waiting_readers = 0;
    std::unordered_map<std::thread::id, int64_t> reader_counts;

    bool writer_active = false;
    std::thread::id writer_owner;
    int64_t write_recursion = 0;

    std::deque<RwLockWriterWaiter *> waiting_writers;
    bool closing = false;
    bool owner_detached = false;
};

/// @brief Test whether a detached reader-writer lock state has no users left.
/// @details Called with RwLockState::mu held. The finalizer detaches ownership
///          when readers, writers, or waiters still reference the state; the
///          last exiting waiter or holder uses this predicate to free it.
/// @param state Reader-writer lock state protected by the caller-held mutex.
/// @return True when @p state may be destroyed after unlocking.
static bool rwlock_detached_and_idle_locked(const RwLockState &state) {
    return state.owner_detached && state.active_readers == 0 && state.waiting_readers == 0 &&
           !state.writer_active && state.waiting_writers.empty();
}

/// @brief Runtime object wrapper storing reader-writer lock state.
/// @details The RtRwLock object is managed by the runtime heap and owns a
///          pointer to the heap-allocated RwLockState.
struct RtRwLock {
    RwLockState *state = nullptr;
};

/// @brief Validate a reader-writer lock pointer and return its typed wrapper.
/// @param lock Opaque lock pointer passed from the runtime.
/// @param what Error string to use when trapping on NULL.
/// @return Valid RtRwLock pointer, or nullptr if validation fails.
static RtRwLock *require_rwlock(void *lock, const char *what) {
    return requireObject<RtRwLock>(lock, RT_RWLOCK_CLASS_ID, "RwLock", what);
}

/// @brief Finalize a reader-writer lock and detach its shared state.
/// @details Marks the state closing, wakes readers, cancels queued writers,
///          and clears the wrapper. Active holders or waiters defer destruction
///          until the last participant exits.
/// @param obj Runtime object pointer to finalize.
static void rwlock_finalizer(void *obj) {
    auto *rw = static_cast<RtRwLock *>(obj);
    RwLockState *state = rw->state;
    if (!state)
        return;

    bool can_delete = false;
    {
        std::unique_lock<std::mutex> lock(state->mu);
        state->closing = true;
        state->owner_detached = true;
        state->readers_cv.notify_all();
        for (RwLockWriterWaiter *waiter : state->waiting_writers) {
            waiter->cancelled = true;
            waiter->cv.notify_one();
        }
        can_delete = rwlock_detached_and_idle_locked(*state);
    }

    rw->state = nullptr;
    if (can_delete)
        destroyState(state);
}

} // namespace

extern "C" {
// ============================================================================
// Zanna.Threads.Gate
// ============================================================================

/// @brief Create a new gate (counting semaphore) with initial permits.
/// @details Permits must be non-negative; otherwise the runtime traps.
///          The returned object owns an internal GateState.
/// @param permits Initial permit count.
/// @return Opaque gate object, or NULL on allocation failure.
void *rt_gate_new(int64_t permits) {
    if (permits < 0) {
        rt_trap("Gate.New: permits cannot be negative");
        return nullptr;
    }

    auto *gate = static_cast<RtGate *>(rt_obj_new_i64(RT_GATE_CLASS_ID, (int64_t)sizeof(RtGate)));
    if (!gate) {
        rt_trap("Gate.New: alloc failed");
        return nullptr; // Unreachable, but silences compiler warnings
    }

    auto *state = allocateState<GateState>(permits);
    if (!state) {
        if (rt_obj_release_check0(gate))
            rt_obj_free(gate);
        rt_trap("Gate.New: alloc failed");
        return nullptr;
    }

    gate->state = state;
    rt_obj_set_finalizer(gate, &gate_finalizer);
    return gate;
}

/// @brief Enter the gate, blocking until a permit is available.
/// @details If permits are available and no waiters exist, this function
///          consumes a permit and returns immediately. Otherwise it queues
///          the caller and waits until a permit is granted in FIFO order.
/// @param gate Opaque gate object pointer.
void rt_gate_enter(void *gate) {
    RtGate *g = require_gate(gate, "Gate.Enter: null object");
    if (!g)
        return;
    rt_obj_retain_maybe(gate);

    const char *trap_msg = nullptr;
    GateState *state_to_destroy = nullptr;
    {
        GateState &state = *g->state;
        std::unique_lock<std::mutex> lock(state.mu);

        if (state.closing) {
            trap_msg = "Gate.Enter: object finalized";
        } else if (state.waiters.empty() && state.permits > 0) {
            --state.permits;
        } else {
            GateWaiter waiter;
            state.waiters.push_back(&waiter);
            while (!waiter.granted && !waiter.cancelled && !state.closing) {
                waiter.cv.wait(lock);
            }
            if (waiter.cancelled || state.closing) {
                auto it = std::find(state.waiters.begin(), state.waiters.end(), &waiter);
                if (it != state.waiters.end())
                    state.waiters.erase(it);
                if (gate_detached_and_idle_locked(state))
                    state_to_destroy = &state;
                trap_msg = "Gate.Enter: object finalized while waiting";
            }
        }
    }

    if (state_to_destroy)
        destroyState(state_to_destroy);
    releaseRuntimeObject(gate);
    if (trap_msg)
        rt_trap(trap_msg);
}

/// @brief Attempt to enter the gate without blocking.
/// @details Succeeds only if there are no waiters and at least one permit.
/// @param gate Opaque gate object pointer.
/// @return 1 if a permit was acquired, 0 otherwise.
int8_t rt_gate_try_enter(void *gate) {
    RtGate *g = require_gate(gate, "Gate.TryEnter: null object");
    if (!g)
        return 0;
    rt_obj_retain_maybe(gate);

    const char *trap_msg = nullptr;
    int8_t result = 0;
    {
        GateState &state = *g->state;
        std::unique_lock<std::mutex> lock(state.mu);
        if (state.closing) {
            trap_msg = "Gate.TryEnter: object finalized";
        } else if (state.waiters.empty() && state.permits > 0) {
            --state.permits;
            result = 1;
        }
    }

    releaseRuntimeObject(gate);
    if (trap_msg)
        rt_trap(trap_msg);
    return result;
}

/// @brief Attempt to enter the gate with a timeout.
/// @details Waits up to @p ms milliseconds for a permit to be granted. A
///          timeout removes the waiter from the queue before returning.
/// @param gate Opaque gate object pointer.
/// @param ms Timeout in milliseconds (negative treated as zero).
/// @return 1 if a permit was acquired, 0 on timeout.
int8_t rt_gate_try_enter_for(void *gate, int64_t ms) {
    RtGate *g = require_gate(gate, "Gate.TryEnterFor: null object");
    if (!g)
        return 0;
    rt_obj_retain_maybe(gate);

    if (ms < 0)
        ms = 0;

    const char *trap_msg = nullptr;
    int8_t result = 0;
    GateState *state_to_destroy = nullptr;
    {
        GateState &state = *g->state;
        std::unique_lock<std::mutex> lock(state.mu);

        if (state.closing) {
            trap_msg = "Gate.TryEnterFor: object finalized";
        } else if (state.waiters.empty() && state.permits > 0) {
            --state.permits;
            result = 1;
        } else if (ms != 0) {
            GateWaiter waiter;
            state.waiters.push_back(&waiter);
            bool timed_out = false;

            const auto deadline = steadyDeadlineFromNow(ms);

            while (!waiter.granted && !waiter.cancelled && !state.closing) {
                if (!waitGateUntil(state, waiter, lock, deadline) && !waiter.granted) {
                    auto it = std::find(state.waiters.begin(), state.waiters.end(), &waiter);
                    if (it != state.waiters.end())
                        state.waiters.erase(it);
                    if (gate_detached_and_idle_locked(state))
                        state_to_destroy = &state;
                    timed_out = true;
                    break;
                }
            }
            if (timed_out) {
                result = 0;
            } else if (waiter.cancelled || state.closing) {
                auto it = std::find(state.waiters.begin(), state.waiters.end(), &waiter);
                if (it != state.waiters.end())
                    state.waiters.erase(it);
                if (gate_detached_and_idle_locked(state))
                    state_to_destroy = &state;
                trap_msg = "Gate.TryEnterFor: object finalized while waiting";
            } else {
                result = 1;
            }
        }
    }

    if (state_to_destroy)
        destroyState(state_to_destroy);
    releaseRuntimeObject(gate);
    if (trap_msg)
        rt_trap(trap_msg);
    return result;
}

/// @brief Release a single permit back to the gate.
/// @details Equivalent to @ref rt_gate_leave_many with a count of one.
/// @param gate Opaque gate object pointer.
void rt_gate_leave(void *gate) {
    rt_gate_leave_many(gate, 1);
}

/// @brief Release multiple permits back to the gate.
/// @details Increments the permit count and wakes queued waiters in FIFO
///          order, reserving a permit for each woken thread.
/// @param gate Opaque gate object pointer.
/// @param count Number of permits to release (must be non-negative).
void rt_gate_leave_many(void *gate, int64_t count) {
    RtGate *g = require_gate(gate, "Gate.Leave: null object");
    if (!g)
        return;
    rt_obj_retain_maybe(gate);

    if (count < 0) {
        releaseRuntimeObject(gate);
        rt_trap("Gate.Leave: count cannot be negative");
        return;
    }

    const char *trap_msg = nullptr;
    {
        GateState &state = *g->state;
        std::unique_lock<std::mutex> lock(state.mu);

        if (state.closing) {
            trap_msg = "Gate.Leave: object finalized";
        } else if (count > std::numeric_limits<int64_t>::max() - state.permits) {
            trap_msg = "Gate.Leave: permit count overflow";
        } else {
            state.permits += count;
            while (state.permits > 0 && !state.waiters.empty()) {
                GateWaiter *waiter = state.waiters.front();
                state.waiters.pop_front();
                --state.permits; // Reserve the permit for the woken waiter.
                waiter->granted = true;
                waiter->cv.notify_one();
            }
        }
    }

    releaseRuntimeObject(gate);
    if (trap_msg)
        rt_trap(trap_msg);
}

/// @brief Query the current permit count.
/// @details Returned under the gate's lock to ensure a consistent snapshot.
/// @param gate Opaque gate object pointer.
/// @return Current number of available permits.
int64_t rt_gate_get_permits(void *gate) {
    RtGate *g = require_gate(gate, "Gate.get_Permits: null object");
    if (!g)
        return 0;
    rt_obj_retain_maybe(gate);

    const char *trap_msg = nullptr;
    int64_t permits = 0;
    {
        GateState &state = *g->state;
        std::unique_lock<std::mutex> lock(state.mu);
        if (state.closing)
            trap_msg = "Gate.get_Permits: object finalized";
        else
            permits = state.permits;
    }

    releaseRuntimeObject(gate);
    if (trap_msg)
        rt_trap(trap_msg);
    return permits;
}

// ============================================================================
// Zanna.Threads.Barrier
// ============================================================================

/// @brief Create a new reusable barrier.
/// @details The barrier releases when @p parties threads have arrived. The
///          party count must be at least one.
/// @param parties Number of participating threads.
/// @return Opaque barrier object, or NULL on allocation failure.
void *rt_barrier_new(int64_t parties) {
    if (parties < 1) {
        rt_trap("Barrier.New: parties must be >= 1");
        return nullptr;
    }

    auto *barrier =
        static_cast<RtBarrier *>(rt_obj_new_i64(RT_BARRIER_CLASS_ID, (int64_t)sizeof(RtBarrier)));
    if (!barrier) {
        rt_trap("Barrier.New: alloc failed");
        return nullptr; // Unreachable, but silences compiler warnings
    }

    auto *state = allocateState<BarrierState>(parties);
    if (!state) {
        if (rt_obj_release_check0(barrier))
            rt_obj_free(barrier);
        rt_trap("Barrier.New: alloc failed");
        return nullptr;
    }

    barrier->state = state;
    rt_obj_set_finalizer(barrier, &barrier_finalizer);
    return barrier;
}

/// @brief Arrive at the barrier and wait for all parties.
/// @details Returns the arrival index for this generation (0-based). The
///          last arriving thread releases all waiters and advances the
///          generation so the barrier can be reused.
/// @param barrier Opaque barrier object pointer.
/// @return Arrival index in the current generation.
int64_t rt_barrier_arrive(void *barrier) {
    RtBarrier *b = require_barrier(barrier, "Barrier.Arrive: null object");
    if (!b)
        return 0;
    rt_obj_retain_maybe(barrier);

    const char *trap_msg = nullptr;
    int64_t index = 0;
    BarrierState *state_to_destroy = nullptr;
    {
        BarrierState &state = *b->state;
        std::unique_lock<std::mutex> lock(state.mu);

        if (state.closing) {
            trap_msg = "Barrier.Arrive: object finalized";
        } else {
            index = state.waiting;
            const int64_t gen = state.generation;
            ++state.waiting;

            if (state.waiting == state.parties) {
                state.waiting = 0;
                ++state.generation;
                state.cv.notify_all();
            } else {
                while (state.generation == gen && !state.closing) {
                    state.cv.wait(lock);
                }
                if (state.closing) {
                    if (state.waiting > 0)
                        --state.waiting;
                    if (barrier_detached_and_idle_locked(state))
                        state_to_destroy = &state;
                    trap_msg = "Barrier.Arrive: object finalized while waiting";
                }
            }
        }
    }

    if (state_to_destroy)
        destroyState(state_to_destroy);
    releaseRuntimeObject(barrier);
    if (trap_msg)
        rt_trap(trap_msg);
    return index;
}

/// @brief Reset the barrier to a new generation.
/// @details Traps if any threads are currently waiting at the barrier to
///          avoid leaving them stranded.
/// @param barrier Opaque barrier object pointer.
void rt_barrier_reset(void *barrier) {
    RtBarrier *b = require_barrier(barrier, "Barrier.Reset: null object");
    if (!b)
        return;
    rt_obj_retain_maybe(barrier);

    const char *trap_msg = nullptr;
    {
        BarrierState &state = *b->state;
        std::unique_lock<std::mutex> lock(state.mu);
        if (state.closing)
            trap_msg = "Barrier.Reset: object finalized";
        else if (state.waiting != 0)
            trap_msg = "Barrier.Reset: threads are waiting";
        else
            ++state.generation;
    }

    releaseRuntimeObject(barrier);
    if (trap_msg)
        rt_trap(trap_msg);
}

/// @brief Get the configured party count for the barrier.
/// @param barrier Opaque barrier object pointer.
/// @return Number of parties required to release the barrier.
int64_t rt_barrier_get_parties(void *barrier) {
    RtBarrier *b = require_barrier(barrier, "Barrier.get_Parties: null object");
    if (!b)
        return 0;
    rt_obj_retain_maybe(barrier);

    const char *trap_msg = nullptr;
    int64_t parties = 0;
    {
        BarrierState &state = *b->state;
        std::unique_lock<std::mutex> lock(state.mu);
        if (state.closing)
            trap_msg = "Barrier.get_Parties: object finalized";
        else
            parties = state.parties;
    }

    releaseRuntimeObject(barrier);
    if (trap_msg)
        rt_trap(trap_msg);
    return parties;
}

/// @brief Get the number of parties currently waiting.
/// @param barrier Opaque barrier object pointer.
/// @return Count of threads waiting in the current generation.
int64_t rt_barrier_get_waiting(void *barrier) {
    RtBarrier *b = require_barrier(barrier, "Barrier.get_Waiting: null object");
    if (!b)
        return 0;
    rt_obj_retain_maybe(barrier);

    const char *trap_msg = nullptr;
    int64_t waiting = 0;
    {
        BarrierState &state = *b->state;
        std::unique_lock<std::mutex> lock(state.mu);
        if (state.closing)
            trap_msg = "Barrier.get_Waiting: object finalized";
        else
            waiting = state.waiting;
    }

    releaseRuntimeObject(barrier);
    if (trap_msg)
        rt_trap(trap_msg);
    return waiting;
}

// ============================================================================
// Zanna.Threads.RwLock
// ============================================================================

/// @brief Create a new reader-writer lock instance.
/// @details The lock is writer-preferred to prevent writer starvation.
/// @return Opaque reader-writer lock object, or NULL on failure.
void *rt_rwlock_new(void) {
    auto *lock =
        static_cast<RtRwLock *>(rt_obj_new_i64(RT_RWLOCK_CLASS_ID, (int64_t)sizeof(RtRwLock)));
    if (!lock) {
        rt_trap("RwLock.New: alloc failed");
        return nullptr; // Unreachable, but silences compiler warnings
    }

    auto *state = allocateState<RwLockState>();
    if (!state) {
        if (rt_obj_release_check0(lock))
            rt_obj_free(lock);
        rt_trap("RwLock.New: alloc failed");
        return nullptr;
    }

    lock->state = state;
    rt_obj_set_finalizer(lock, &rwlock_finalizer);
    return lock;
}

/// @brief Acquire the lock in shared (reader) mode.
/// @details Blocks while a writer is active or waiting to preserve writer
///          preference. Multiple readers may enter concurrently.
/// @param lock Opaque reader-writer lock object pointer.
void rt_rwlock_read_enter(void *lock) {
    RtRwLock *rw = require_rwlock(lock, "RwLock.ReadEnter: null object");
    if (!rw)
        return;
    rt_obj_retain_maybe(lock);

    const char *trap_msg = nullptr;
    int8_t acquired = 0;
    RwLockState *state_to_destroy = nullptr;
    {
        RwLockState &state = *rw->state;
        const std::thread::id tid = std::this_thread::get_id();
        std::unique_lock<std::mutex> lk(state.mu);
        if (state.closing) {
            trap_msg = "RwLock.ReadEnter: object finalized";
        } else if (state.writer_active && state.writer_owner == tid) {
            ++state.active_readers;
            ++state.reader_counts[tid];
            acquired = 1;
        } else {
            ++state.waiting_readers;
            while ((state.writer_active || !state.waiting_writers.empty()) && !state.closing) {
                state.readers_cv.wait(lk);
            }
            --state.waiting_readers;
            if (state.closing) {
                if (rwlock_detached_and_idle_locked(state))
                    state_to_destroy = &state;
                trap_msg = "RwLock.ReadEnter: object finalized while waiting";
            } else {
                ++state.active_readers;
                ++state.reader_counts[tid];
                acquired = 1;
            }
        }
    }

    if (state_to_destroy)
        destroyState(state_to_destroy);
    if (!acquired)
        releaseRuntimeObject(lock);
    if (trap_msg)
        rt_trap(trap_msg);
}

/// @brief Release a previously acquired read lock.
/// @details Traps if no matching read lock is held.
/// @param lock Opaque reader-writer lock object pointer.
void rt_rwlock_read_exit(void *lock) {
    RtRwLock *rw = require_rwlock(lock, "RwLock.ReadExit: null object");
    if (!rw)
        return;

    const char *trap_msg = nullptr;
    int8_t released = 0;
    RwLockState *state_to_destroy = nullptr;
    {
        RwLockState &state = *rw->state;
        const std::thread::id tid = std::this_thread::get_id();
        std::unique_lock<std::mutex> lk(state.mu);
        auto it = state.reader_counts.find(tid);
        if (it == state.reader_counts.end() || it->second <= 0) {
            trap_msg = "RwLock.ReadExit: exit without matching enter";
        } else {
            --it->second;
            if (it->second == 0)
                state.reader_counts.erase(it);
            --state.active_readers;
            if (state.active_readers == 0 && !state.writer_active &&
                !state.waiting_writers.empty()) {
                state.waiting_writers.front()->cv.notify_one();
            }
            released = 1;
            if (rwlock_detached_and_idle_locked(state))
                state_to_destroy = &state;
        }
    }

    if (state_to_destroy)
        destroyState(state_to_destroy);
    if (trap_msg)
        rt_trap(trap_msg);
    if (released)
        releaseRuntimeObject(lock);
}

/// @brief Acquire the lock in exclusive (writer) mode.
/// @details Blocks until no readers or writers are active. If the calling
///          thread already owns the write lock, the recursion count is
///          incremented and the function returns immediately.
/// @param lock Opaque reader-writer lock object pointer.
void rt_rwlock_write_enter(void *lock) {
    RtRwLock *rw = require_rwlock(lock, "RwLock.WriteEnter: null object");
    if (!rw)
        return;
    rt_obj_retain_maybe(lock);

    const char *trap_msg = nullptr;
    int8_t acquired = 0;
    RwLockState *state_to_destroy = nullptr;
    {
        RwLockState &state = *rw->state;
        const std::thread::id tid = std::this_thread::get_id();
        std::unique_lock<std::mutex> lk(state.mu);

        if (state.closing) {
            trap_msg = "RwLock.WriteEnter: object finalized";
        } else if (state.writer_active && state.writer_owner == tid) {
            ++state.write_recursion;
            acquired = 1;
        } else {
            auto reader_it = state.reader_counts.find(tid);
            if (reader_it != state.reader_counts.end() && reader_it->second > 0) {
                trap_msg = "RwLock.WriteEnter: cannot upgrade read lock";
            } else {
                RwLockWriterWaiter waiter;
                state.waiting_writers.push_back(&waiter);

                while (true) {
                    if (state.closing || waiter.cancelled) {
                        auto it = std::find(
                            state.waiting_writers.begin(), state.waiting_writers.end(), &waiter);
                        if (it != state.waiting_writers.end())
                            state.waiting_writers.erase(it);
                        if (rwlock_detached_and_idle_locked(state))
                            state_to_destroy = &state;
                        trap_msg = "RwLock.WriteEnter: object finalized while waiting";
                        break;
                    }
                    const bool is_front = state.waiting_writers.front() == &waiter;
                    if (is_front && !state.writer_active && state.active_readers == 0) {
                        state.waiting_writers.pop_front();
                        state.writer_active = true;
                        state.writer_owner = tid;
                        state.write_recursion = 1;
                        acquired = 1;
                        break;
                    }
                    waiter.cv.wait(lk);
                }
            }
        }
    }

    if (state_to_destroy)
        destroyState(state_to_destroy);
    if (!acquired)
        releaseRuntimeObject(lock);
    if (trap_msg)
        rt_trap(trap_msg);
}

/// @brief Release a previously acquired write lock.
/// @details Traps if the caller is not the owner or if no write lock is
///          held. Writer recursion is decremented and the lock is released
///          when the recursion count reaches zero.
/// @param lock Opaque reader-writer lock object pointer.
void rt_rwlock_write_exit(void *lock) {
    RtRwLock *rw = require_rwlock(lock, "RwLock.WriteExit: null object");
    if (!rw)
        return;

    const char *trap_msg = nullptr;
    int8_t released = 0;
    RwLockState *state_to_destroy = nullptr;
    {
        RwLockState &state = *rw->state;
        const std::thread::id tid = std::this_thread::get_id();
        std::unique_lock<std::mutex> lk(state.mu);

        if (!state.writer_active) {
            trap_msg = "RwLock.WriteExit: exit without matching enter";
        } else if (state.writer_owner != tid) {
            trap_msg = "RwLock.WriteExit: not owner";
        } else {
            --state.write_recursion;
            released = 1;
            if (state.write_recursion > 0) {
                // A recursive write-enter retains once per successful enter.
            } else {
                state.writer_active = false;
                state.writer_owner = std::thread::id();

                if (!state.waiting_writers.empty()) {
                    state.waiting_writers.front()->cv.notify_one();
                } else {
                    state.readers_cv.notify_all();
                }
            }
            if (rwlock_detached_and_idle_locked(state))
                state_to_destroy = &state;
        }
    }

    if (state_to_destroy)
        destroyState(state_to_destroy);
    if (trap_msg)
        rt_trap(trap_msg);
    if (released)
        releaseRuntimeObject(lock);
}

/// @brief Attempt to acquire a read lock without blocking.
/// @details Succeeds only if no writer is active and no writers are waiting.
/// @param lock Opaque reader-writer lock object pointer.
/// @return 1 if acquired, 0 otherwise.
int8_t rt_rwlock_try_read_enter(void *lock) {
    RtRwLock *rw = require_rwlock(lock, "RwLock.TryReadEnter: null object");
    if (!rw)
        return 0;
    rt_obj_retain_maybe(lock);

    const char *trap_msg = nullptr;
    int8_t result = 0;
    {
        RwLockState &state = *rw->state;
        const std::thread::id tid = std::this_thread::get_id();
        std::unique_lock<std::mutex> lk(state.mu);
        if (state.closing) {
            trap_msg = "RwLock.TryReadEnter: object finalized";
        } else if (state.writer_active && state.writer_owner == tid) {
            ++state.active_readers;
            ++state.reader_counts[tid];
            result = 1;
        } else if (!state.writer_active && state.waiting_writers.empty()) {
            ++state.active_readers;
            ++state.reader_counts[tid];
            result = 1;
        }
    }

    if (!result)
        releaseRuntimeObject(lock);
    if (trap_msg)
        rt_trap(trap_msg);
    return result;
}

/// @brief Attempt to acquire a write lock without blocking.
/// @details Succeeds only if no readers or writers are active. If the
///          caller already owns the write lock, recursion is incremented
///          and the call succeeds.
/// @param lock Opaque reader-writer lock object pointer.
/// @return 1 if acquired, 0 otherwise.
int8_t rt_rwlock_try_write_enter(void *lock) {
    RtRwLock *rw = require_rwlock(lock, "RwLock.TryWriteEnter: null object");
    if (!rw)
        return 0;
    rt_obj_retain_maybe(lock);

    const char *trap_msg = nullptr;
    int8_t result = 0;
    {
        RwLockState &state = *rw->state;
        const std::thread::id tid = std::this_thread::get_id();
        std::unique_lock<std::mutex> lk(state.mu);

        if (state.closing) {
            trap_msg = "RwLock.TryWriteEnter: object finalized";
        } else if (state.writer_active && state.writer_owner == tid) {
            ++state.write_recursion;
            result = 1;
        } else {
            auto reader_it = state.reader_counts.find(tid);
            if (reader_it == state.reader_counts.end() || reader_it->second <= 0) {
                if (!state.writer_active && state.active_readers == 0 &&
                    state.waiting_writers.empty()) {
                    state.writer_active = true;
                    state.writer_owner = tid;
                    state.write_recursion = 1;
                    result = 1;
                }
            }
        }
    }

    if (!result)
        releaseRuntimeObject(lock);
    if (trap_msg)
        rt_trap(trap_msg);
    return result;
}

/// @brief Query the number of active readers.
/// @details Returned under the lock for a consistent snapshot.
/// @param lock Opaque reader-writer lock object pointer.
/// @return Number of active reader holders.
int64_t rt_rwlock_get_readers(void *lock) {
    RtRwLock *rw = require_rwlock(lock, "RwLock.get_Readers: null object");
    if (!rw)
        return 0;
    rt_obj_retain_maybe(lock);

    const char *trap_msg = nullptr;
    int64_t readers = 0;
    {
        RwLockState &state = *rw->state;
        std::unique_lock<std::mutex> lk(state.mu);
        if (state.closing)
            trap_msg = "RwLock.get_Readers: object finalized";
        else
            readers = state.active_readers;
    }

    releaseRuntimeObject(lock);
    if (trap_msg)
        rt_trap(trap_msg);
    return readers;
}

/// @brief Check whether a writer currently holds the lock.
/// @details Returned under the lock for a consistent snapshot.
/// @param lock Opaque reader-writer lock object pointer.
/// @return 1 if a writer is active, 0 otherwise.
int8_t rt_rwlock_get_is_write_locked(void *lock) {
    RtRwLock *rw = require_rwlock(lock, "RwLock.get_IsWriteLocked: null object");
    if (!rw)
        return 0;
    rt_obj_retain_maybe(lock);

    const char *trap_msg = nullptr;
    int8_t locked = 0;
    {
        RwLockState &state = *rw->state;
        std::unique_lock<std::mutex> lk(state.mu);
        if (state.closing)
            trap_msg = "RwLock.get_IsWriteLocked: object finalized";
        else
            locked = state.writer_active ? 1 : 0;
    }

    releaseRuntimeObject(lock);
    if (trap_msg)
        rt_trap(trap_msg);
    return locked;
}

} // extern "C"
