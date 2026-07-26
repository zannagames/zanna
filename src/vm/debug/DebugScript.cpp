//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/debug/DebugScript.cpp
// Purpose: Implement the queue-based script loader that drives the interactive
//          VM debugger.
// Links: docs/runtime-vm.md#debugger
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Parses debugger command scripts into queued actions.
/// @details Keeps I/O and parsing logic out of the header so the debugger can
///          include lightweight declarations while the implementation handles
///          error messaging and command expansion.
#include "zanna/vm/debug/Debug.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <sstream>

namespace il::vm {
/// @brief Construct a script by loading actions from a command file.
/// @details Reads the file line-by-line, interpreting recognised commands into
///          queued actions.  "continue" lines enqueue a Continue action,
///          "step" and "step N" emit Step actions with appropriate counts, and
///          "step-over" / "step-out" request frame-depth-aware stepping.
///          Unknown lines emit `[DEBUG]` messages to stderr but do not abort
///          parsing, allowing iterative script development.
/// @param path File-system path to the debugger command script.
DebugScript::DebugScript(const std::string &path) {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "[DEBUG] unable to open " << path << "\n";
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        /// @brief Test whether one byte is classified as whitespace.
        /// @param ch Byte to inspect.
        /// @return Nonzero when `ch` is whitespace in the active C locale.
        auto isAsciiSpace = [](unsigned char ch) { return std::isspace(ch); };
        auto beginIt = std::find_if_not(line.begin(), line.end(), isAsciiSpace);
        auto endIt = std::find_if_not(line.rbegin(), line.rend(), isAsciiSpace).base();
        if (beginIt >= endIt) {
            line.clear();
        } else {
            line.assign(beginIt, endIt);
        }
        if (line.empty())
            continue;
        if (line == "continue") {
            actions.push({DebugActionKind::Continue, 0});
        } else if (line == "step") {
            actions.push({DebugActionKind::Step, 1});
        } else if (line.rfind("step ", 0) == 0) {
            std::istringstream iss(line.substr(5));
            uint64_t n = 0;
            if (iss >> n)
                actions.push({DebugActionKind::Step, n});
            else
                std::cerr << "[DEBUG] ignored: " << line << "\n";
        } else if (line == "step-over") {
            actions.push({DebugActionKind::StepOver, 0});
        } else if (line == "step-out") {
            actions.push({DebugActionKind::StepOut, 0});
        } else {
            std::cerr << "[DEBUG] ignored: " << line << "\n";
        }
    }
}

/// @brief Queue a step action for @p count instructions.
/// @details Allows tooling to append scripted actions after construction without
///          re-reading a file.  Actions are enqueued in FIFO order so appended
///          steps execute after any previously loaded commands.
/// @param count Number of instruction boundaries to advance before pausing.
void DebugScript::addStep(uint64_t count) {
    actions.push({DebugActionKind::Step, count});
}

/// @brief Retrieve the next queued action.
/// @details Pops the next action from the queue, defaulting to a Continue action
///          when the queue is empty.  Returning a Continue action in the empty
///          case lets the debugger resume execution naturally without checking
///          for null results.
/// @return Oldest queued action, or a Continue action when the queue is empty.
DebugAction DebugScript::nextAction() {
    if (actions.empty())
        return {DebugActionKind::Continue, 0};
    auto act = actions.front();
    actions.pop();
    return act;
}

/// @brief Determine whether all actions have been consumed.
/// @details Exposes the queue emptiness predicate so driver code can decide when
///          to re-read scripts or fall back to interactive mode.
/// @return @c true when no scripted actions remain.
bool DebugScript::empty() const {
    return actions.empty();
}

} // namespace il::vm
