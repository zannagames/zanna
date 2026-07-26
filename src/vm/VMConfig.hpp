//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: vm/VMConfig.hpp
// Purpose: Compile-time configuration flags and dispatch hook macros for the
//          Zanna VM subsystem, controlling dispatch strategy selection, pre/post
//          instruction hooks for profiling and polling, tail-call optimization,
//          and opcode execution counting.
// Key invariants: Hooks compile away to no-ops when not enabled.
//                 ZANNA_VM_TAILCALL defaults to 1 (tail-call optimization enabled).
//                 ZANNA_VM_OPCOUNTS defaults to 1 (opcode counting available).
//                 When opcode counting is enabled, DISPATCH_BEFORE is redefined to
//                 increment per-opcode counters (gated by runtime config flag).
// Ownership/Lifetime: Header-only; no owning objects or runtime state.
// Links: docs/il/il-guide.md#reference
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Compile-time configuration knobs for the VM subsystem.
/// @details Defines build-time feature toggles and dispatch hook macros used
///          by the interpreter. These macros are intentionally lightweight so
///          they are optimized away when not enabled.

#pragma once

/// @brief Indicates whether threaded dispatch is supported by this build.
/// @details Threaded dispatch requires GCC/Clang labels-as-values support in
///          addition to the `ZANNA_VM_THREADED` build flag.
#if defined(ZANNA_VM_THREADED) && (defined(__GNUC__) || defined(__clang__))
#define ZANNA_THREADING_SUPPORTED 1
#else
#define ZANNA_THREADING_SUPPORTED 0
#endif

// -----------------------------------------------------------------------------
// Dispatch hook macros (compiled away by default)
//
// These macros are invoked immediately before and after executing each
// instruction in the VM regardless of dispatch strategy (switch/labels/table).
// They default to empty do-while blocks so the optimizer removes them entirely
// when not overridden by the build or embedding application.
// -----------------------------------------------------------------------------
/// @brief Hook executed immediately before each instruction dispatch.
/// @details Override this macro to inject profiling or instrumentation. The
///          default definition is a no-op.
/// @param ST Active execution-state expression.
/// @param OPCODE Opcode expression for the instruction about to execute.
#ifndef ZANNA_VM_DISPATCH_BEFORE
#define ZANNA_VM_DISPATCH_BEFORE(ST, OPCODE)                                                       \
    do {                                                                                           \
    } while (0)
#endif

// NOTE: ZANNA_VM_DISPATCH_AFTER is optimized for the common case where polling
// is disabled (interruptEveryN == 0). The pollTick increment only occurs when
// polling is active, avoiding wasted cycles on every instruction dispatch.
/// @brief Hook executed immediately after each instruction dispatch.
/// @details The default implementation performs periodic polling when enabled
///          via the runtime config. Override to inject custom instrumentation.
/// @param ST Active execution-state expression updated by polling.
/// @param OPCODE Opcode expression for the instruction just executed; unused
///               by the default hook.
#ifndef ZANNA_VM_DISPATCH_AFTER
#define ZANNA_VM_DISPATCH_AFTER(ST, OPCODE)                                                        \
    do {                                                                                           \
        if ((ST).vm() && (ST).vm()->pollPendingInterrupt()) [[unlikely]] {                         \
            (ST).requestPause();                                                                   \
        }                                                                                          \
        const auto &cfg = (ST).config;                                                             \
        if (cfg.interruptEveryN) [[unlikely]] {                                                    \
            if ((++(ST).pollTick % cfg.interruptEveryN) == 0) {                                    \
                if (cfg.pollCallback && !(cfg.pollCallback((ST).vm()))) {                          \
                    (ST).requestPause();                                                           \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
    } while (0)
#endif

// -----------------------------------------------------------------------------
// Tail-call optimisation toggle
// -----------------------------------------------------------------------------
// Tail-call is enabled by default; can be overridden via -D or env handled at runtime.
/// @brief Compile-time toggle for tail-call optimization.
/// @details When set to 0, tail-call reuse of frames is disabled even if the
///          VM otherwise supports it.
#ifndef ZANNA_VM_TAILCALL
#define ZANNA_VM_TAILCALL 1
#endif

// -----------------------------------------------------------------------------
// Opcode execution counters (compile-time + runtime toggle)
// -----------------------------------------------------------------------------
/// @brief Compile-time toggle for opcode execution counters.
/// @details When enabled, `ZANNA_VM_DISPATCH_BEFORE` increments per-opcode
///          counters if the runtime config requests it. Disabled in release
///          builds by default to eliminate a conditional branch per instruction
///          from the dispatch hot path.
#ifndef ZANNA_VM_OPCOUNTS
#ifdef NDEBUG
#define ZANNA_VM_OPCOUNTS 0
#else
#define ZANNA_VM_OPCOUNTS 1
#endif
#endif

#if ZANNA_VM_OPCOUNTS
#undef ZANNA_VM_DISPATCH_BEFORE
/// @brief Count an opcode before dispatch when runtime counting is enabled.
/// @param ST Active execution-state expression containing the counter array.
/// @param OPCODE Opcode expression converted to the counter-array index.
#define ZANNA_VM_DISPATCH_BEFORE(ST, OPCODE)                                                       \
    do {                                                                                           \
        if ((ST).config.enableOpcodeCounts) {                                                      \
            const size_t _zannaOpcodeIndex = static_cast<size_t>(OPCODE);                          \
            if (_zannaOpcodeIndex < (ST).vm()->opCounts_.size())                                   \
                ++((ST).vm()->opCounts_[_zannaOpcodeIndex]);                                       \
        }                                                                                          \
    } while (0)
#endif
