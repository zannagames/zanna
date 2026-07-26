//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/VM_DebugUtils.cpp
// Purpose: Provide VM-side helpers for opcode mnemonics and trap diagnostics.
// Key invariants: Diagnostic caches mirror the most recent execution context so
//                 debugger output remains coherent across pause/resume cycles.
// Ownership/Lifetime: Functions mutate VM-owned tracking structures in place
//                     without allocating persistent external state.
// Links: docs/runtime-vm.md#diagnostics
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief VM debugging utilities for opcode and trap reporting.
/// @details Provides convenience helpers for translating opcodes into readable
///          mnemonics, exposing trap messages, and synthesising frame summaries
///          when the VM encounters errors.  These functions are deliberately kept
///          out-of-line to keep the main VM implementation focused on execution
///          semantics.

#include "il/core/Function.hpp"
#include "il/core/OpcodeInfo.hpp"
#include "support/source_manager.hpp"
#include "vm/VM.hpp"
#include <algorithm>
#include <iostream>
#include <string>

namespace il::vm {
namespace {
using il::core::getOpcodeInfo;
using il::core::kNumOpcodes;

/// @brief Copy a valid source location into a diagnostic frame.
/// @details Preserves the structured location, converts line and column to the
///          diagnostic representation, and resolves a file path when a source
///          manager is available.
/// @param [out] frame Frame descriptor receiving location fields.
/// @param loc Source location to copy; invalid locations leave @p frame unchanged.
/// @param sm Optional source manager used to resolve the file identifier.
void fillFrameLocation(FrameInfo &frame,
                       il::support::SourceLoc loc,
                       const il::support::SourceManager *sm) {
    if (!loc.isValid())
        return;
    frame.loc = loc;
    if (loc.hasLine())
        frame.line = static_cast<int32_t>(loc.line);
    if (loc.hasColumn())
        frame.column = static_cast<int32_t>(loc.column);
    if (sm && loc.hasFile())
        frame.file = std::string(sm->getPath(loc.file_id));
}
} // namespace

/// @brief Translate an opcode to a printable mnemonic.
/// @details Consults the opcode metadata table and returns the canonical name
///          when available, falling back to a numeric placeholder when metadata
///          is missing.  Keeps debugger output stable even for unrecognised
///          opcodes.
/// @param op Opcode enumerator to translate.
/// @return String mnemonic or numeric placeholder.
std::string opcodeMnemonic(il::core::Opcode op) {
    const size_t index = static_cast<size_t>(op);
    if (index < kNumOpcodes) {
        const auto &info = getOpcodeInfo(op);
        if (info.name && info.name[0] != '\0')
            return info.name;
    }
    return std::string("opcode#") + std::to_string(static_cast<int>(op));
}

/// @brief Retrieve the most recent trap message recorded by the VM.
/// @details Returns an optional containing the cached trap message when one is
///          available; otherwise @c std::nullopt so callers can distinguish
///          between "no trap" and "empty string" cases.
/// @return Optional string describing the last trap.
std::optional<std::string> VM::lastTrapMessage() const {
    if (lastTrap.message.empty())
        return std::nullopt;
    return lastTrap.message;
}

/// @brief Clear stale trap state before a new execution.
/// @details Resets lastTrap, trapToken, and runtimeContext message so
///          subsequent executions start with a clean slate.
void VM::clearTrapState() {
    lastTrap.error = {};
    lastTrap.frame = {};
    lastTrap.message.clear();
    trapToken.error = {};
    trapToken.message.clear();
    trapToken.valid = false;
    runtimeContext.message.clear();
}

/// @brief Construct a diagnostic frame snapshot for a VM error.
/// @details Aggregates function name, block label, instruction index, and source
///          location by consulting current execution context, runtime context,
///          and cached trap state.  The helper prefers freshly available data but
///          falls back to previously recorded information when necessary, ensuring
///          that debugger output always contains best-effort metadata.
/// @param error Error descriptor reported by the VM core.
/// @return Populated frame summary describing the failing execution point.
FrameInfo VM::buildFrameInfo(const VmError &error) const {
    FrameInfo frame{};

    // Reconstruct context from execStack (always current) with currentContext fallback.
    const auto ctx = currentTrapContext();

    // Function name: prefer live context, then runtime context, then cached
    if (ctx.function)
        frame.function = ctx.function->name;
    else if (!runtimeContext.function.empty())
        frame.function = runtimeContext.function;
    else if (!lastTrap.frame.function.empty())
        frame.function = lastTrap.frame.function;

    // Block label: prefer live context, then runtime context, then cached
    if (ctx.block)
        frame.block = ctx.block->label;
    else if (!runtimeContext.block.empty())
        frame.block = runtimeContext.block;
    else if (!lastTrap.frame.block.empty())
        frame.block = lastTrap.frame.block;

    // Instruction pointer
    frame.ip = error.ip;
    if (frame.ip == 0 && ctx.hasInstruction)
        frame.ip = static_cast<uint64_t>(ctx.instructionIndex);
    else if (frame.ip == 0 && lastTrap.frame.ip != 0)
        frame.ip = lastTrap.frame.ip;

    const auto *sm = debug.getSourceManager();

    // Source location
    if (ctx.loc.isValid()) {
        fillFrameLocation(frame, ctx.loc, sm);
    } else if (runtimeContext.loc.isValid()) {
        fillFrameLocation(frame, runtimeContext.loc, sm);
    } else if (lastTrap.frame.loc.isValid()) {
        frame.loc = lastTrap.frame.loc;
        frame.file = lastTrap.frame.file;
        frame.line = lastTrap.frame.line;
        frame.column = lastTrap.frame.column;
    }

    if (error.line >= 0) {
        frame.line = error.line;
    } else if (frame.line < 0 && lastTrap.frame.line >= 0) {
        frame.line = lastTrap.frame.line;
        frame.column = lastTrap.frame.column;
    }

    // Check if any handler is installed
    /// @brief Test whether one execution state has an active exception handler.
    /// @param st Execution state pointer, which may be null.
    /// @return `true` when `st` owns a nonempty exception-handler stack.
    frame.handlerInstalled =
        std::any_of(execStack.begin(), execStack.end(), [](const ExecState *st) {
            return st && !st->fr.ehStack.empty();
        });
    return frame;
}

/// @brief Cache details about the latest trap and return its message.
/// @details Stores the provided error and frame information, recomputes the user
///          facing message via @ref vm_format_error, and appends any pending
///          runtime-context message.  The combined message is cached for future
///          retrieval via @ref lastTrapMessage.
/// @param error Error descriptor raised by the VM.
/// @param frame Frame information produced by @ref buildFrameInfo.
/// @return The formatted trap message stored in the VM state.
std::string VM::recordTrap(const VmError &error, const FrameInfo &frame) {
    lastTrap.error = error;
    lastTrap.frame = frame;
    lastTrap.message = vm_format_error(error, frame);
    if (!runtimeContext.message.empty()) {
        lastTrap.message += ": ";
        lastTrap.message += runtimeContext.message;
        runtimeContext.message.clear();
    }
    return lastTrap.message;
}

/// @brief Build a most-recent-first snapshot of active VM frames.
/// @details Walks the execution stack without mutating it, recording function,
///          block, instruction, source, and handler information for each valid
///          execution state.
/// @return Diagnostic frames ordered from the active callee to the oldest caller.
std::vector<FrameInfo> VM::buildBacktrace() const {
    std::vector<FrameInfo> frames;
    frames.reserve(execStack.size());

    // Walk from top (most recent) to bottom (oldest)
    for (auto it = execStack.rbegin(); it != execStack.rend(); ++it) {
        const auto *es = *it;
        if (!es)
            continue;

        FrameInfo frame{};

        if (es->fr.func)
            frame.function = es->fr.func->name;

        if (es->bb) {
            frame.block = es->bb->label;
            frame.ip = es->ip;

            if (es->ip < es->bb->instructions.size()) {
                const auto &instr = es->bb->instructions[es->ip];
                fillFrameLocation(frame, instr.loc, debug.getSourceManager());
            }
        }

        frame.handlerInstalled = !es->fr.ehStack.empty();
        frames.push_back(std::move(frame));
    }

    return frames;
}

/// @brief Print a human-readable VM backtrace to standard error.
/// @param frames Most-recent-first frame descriptors to render; an empty vector
///               produces no output.
void VM::printBacktrace(const std::vector<FrameInfo> &frames) {
    if (frames.empty())
        return;

    std::cerr << "Zanna backtrace (most recent call first):\n";
    for (size_t i = 0; i < frames.size(); ++i) {
        const auto &f = frames[i];
        std::cerr << "  #" << i << "  @" << (f.function.empty() ? "<unknown>" : f.function);
        if (!f.block.empty())
            std::cerr << "  " << f.block << "#" << f.ip;
        if (!f.file.empty()) {
            std::cerr << "  " << f.file;
            if (f.line >= 0) {
                std::cerr << ':' << f.line;
                if (f.column >= 0)
                    std::cerr << ':' << f.column;
            }
        } else if (f.line >= 0) {
            std::cerr << "  line " << f.line;
            if (f.column >= 0)
                std::cerr << ':' << f.column;
        }
        std::cerr << '\n';
    }
}

} // namespace il::vm
