//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file DebugAdapter.cpp
/// @brief Implements Zanna Studio's interactive VM-backed debug adapter.
///
/// Breakpoint, stop, stepping, evaluation, and variable-expansion events are emitted as sentinel-
/// prefixed compact JSON lines on stderr, leaving debuggee stdout and stderr intact. Commands arrive
/// as newline-delimited JSON on stdin and are queued by a background reader.
//
//===----------------------------------------------------------------------===//
#include "tools/zanna/DebugAdapter.hpp"

#include "il/core/Module.hpp"
#include "support/source_manager.hpp"
#include "tools/lsp-common/Json.hpp"
#include "tools/zanna/DebugExpr.hpp"
#include "zanna/vm/VM.hpp"
#include "zanna/vm/debug/DebugFrontend.hpp"

#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>

namespace il::tools::debug {
namespace {

using zanna::server::JsonValue;

/// @brief Sentinel marking a control-protocol line on stderr, keeping it distinct
///        from any raw stderr the debuggee writes.
constexpr const char *kSentinel = "@@VDBG@@ ";

/// @brief Trailing path component, for tolerant file matching (the adapter reports
///        canonical paths while the IDE sends its own spelling).
/// @param p Source path in either platform spelling.
/// @return Final path component, or the original string when it has no separator.
std::string baseNameOf(const std::string &p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

/// @brief Per-breakpoint condition / logpoint message. Plain breakpoints have no
///        entry; conditional/logpoints are keyed by basename:line.
struct BpMeta {
    std::string condition;
    std::string logMessage;
};

using BpMetaMap = std::unordered_map<std::string, BpMeta>;

/// @brief Build the tolerant basename-and-line key used for breakpoint metadata.
/// @param path IDE or VM source path.
/// @param line One-based source line.
/// @return Stable @c basename:line key.
std::string metaKey(const std::string &path, uint32_t line) {
    return baseNameOf(path) + ":" + std::to_string(line);
}

/// @brief Apply a setBreakpoints command's optional `meta` to @p map, replacing any
///        prior condition/logpoint for the lines it lists (so a cleared condition
///        drops out). Lines without a meta entry hold no entry (plain breakpoint).
/// @param map Mutable basename-and-line metadata map.
/// @param cmd Parsed @c setBreakpoints command.
void applyBpMeta(BpMetaMap &map, const JsonValue &cmd) {
    const std::string &path = cmd["path"].asString();
    if (const JsonValue *lines = cmd.get("lines")) {
        for (const auto &ln : lines->asArray())
            map.erase(metaKey(path, static_cast<uint32_t>(ln.asInt())));
    }
    if (const JsonValue *meta = cmd.get("meta")) {
        for (const auto &e : meta->asArray()) {
            const auto line = static_cast<uint32_t>(e["line"].asInt());
            BpMeta bm;
            if (const JsonValue *c = e.get("condition"))
                bm.condition = c->asString();
            if (const JsonValue *l = e.get("logMessage"))
                bm.logMessage = l->asString();
            if (!bm.condition.empty() || !bm.logMessage.empty())
                map[metaKey(path, line)] = std::move(bm);
        }
    }
}

/// @brief Build a logpoint output event (adapter -> IDE).
/// @param line Source line that triggered the logpoint.
/// @param message Interpolated logpoint text.
/// @return Compact event object ready for channel emission.
JsonValue logEvent(uint32_t line, const std::string &message) {
    return JsonValue::object({
        {"type", JsonValue("log")},
        {"line", JsonValue(static_cast<int64_t>(line))},
        {"message", JsonValue(message)},
    });
}

/// @brief Newline-delimited JSON control channel: events out on stderr (sentinel
///        prefixed), commands in on stdin. A background reader thread parses stdin
///        into a queue so commands can be consumed blockingly (while stopped) or
///        non-blockingly (a poll callback checking for Pause while running).
class DebugChannel {
  public:
    /// @brief Start the background stdin reader after synchronization state is initialized.
    /// @details The thread callback enters readerLoop() on this channel instance.
    DebugChannel() : reader_([this] { readerLoop(); }) {}

    /// @brief Detach the reader because debugger termination exits the process directly.
    ~DebugChannel() {
        reader_.detach();
    } // the process exits via _Exit; no join

    /// @brief Serialize one event atomically to the sentinel-prefixed control stream.
    /// @param event Event object to emit as compact JSON.
    void emit(const JsonValue &event) {
        std::lock_guard<std::mutex> lock(outMutex_);
        std::cerr << kSentinel << event.toCompactString() << "\n";
        std::cerr.flush();
    }

    /// @brief Block for the next command (used while the debuggee is stopped).
    /// @return Next parsed command, or @c std::nullopt after input EOF and queue drain.
    std::optional<JsonValue> readCommand() {
        std::unique_lock<std::mutex> lock(mutex_);
        /// @brief Test whether a queued command or end-of-input makes the wait ready.
        /// @return `true` when the queue is nonempty or EOF has been observed.
        cv_.wait(lock, [this] { return !queue_.empty() || eof_; });
        if (queue_.empty())
            return std::nullopt;
        JsonValue cmd = std::move(queue_.front());
        queue_.pop_front();
        return cmd;
    }

    /// @brief Pop the next queued command without blocking, or nullopt if none
    ///        (used by the poll callback while the debuggee is running).
    /// @return Next parsed command, or @c std::nullopt when the queue is empty.
    std::optional<JsonValue> tryReadCommand() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty())
            return std::nullopt;
        JsonValue cmd = std::move(queue_.front());
        queue_.pop_front();
        return cmd;
    }

  private:
    /// @brief Parse nonempty stdin lines into the synchronized command queue until EOF.
    void readerLoop() {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty())
                continue;
            try {
                JsonValue cmd = JsonValue::parse(line);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    queue_.push_back(std::move(cmd));
                }
                cv_.notify_one();
            } catch (const std::exception &) {
                // Ignore an unparseable line.
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            eof_ = true;
        }
        cv_.notify_one();
    }

    std::mutex mutex_;
    std::mutex outMutex_;
    std::condition_variable cv_;
    std::deque<JsonValue> queue_;
    bool eof_ = false;
    // Construct last: readerLoop accesses every synchronization member above.
    std::thread reader_;
};

/// @brief Serialize a VM stop snapshot with stack frames and top-frame locals.
/// @param info Immutable stop information supplied by the VM.
/// @return Adapter @c stopped event, including structured-variable references.
JsonValue stoppedEvent(const il::vm::DebugStopInfo &info) {
    JsonValue::ArrayType frames;
    frames.reserve(info.frames.size());
    for (const auto &f : info.frames) {
        frames.push_back(JsonValue::object({
            {"function", JsonValue(f.function)},
            {"path", JsonValue(f.path)},
            {"line", JsonValue(static_cast<int64_t>(f.line))},
        }));
    }
    JsonValue::ArrayType locals;
    locals.reserve(info.locals.size());
    for (const auto &l : info.locals) {
        locals.push_back(JsonValue::object({
            {"name", JsonValue(l.name)},
            {"value", JsonValue(l.value)},
            {"type", JsonValue(l.type)},
            // Additive structured-variable fields (back-compatible; a consumer that
            // ignores them sees the historical flat locals). varRef>0 marks an
            // expandable composite; the IDE fetches children with a "variables" cmd.
            {"varRef", JsonValue(l.varRef)},
            {"childCount", JsonValue(l.childCount)},
        }));
    }
    return JsonValue::object({
        {"type", JsonValue("stopped")},
        {"reason", JsonValue(info.reason)},
        {"path", JsonValue(info.path)},
        {"line", JsonValue(static_cast<int64_t>(info.line))},
        {"column", JsonValue(static_cast<int64_t>(info.column))},
        {"frames", JsonValue::array(std::move(frames))},
        {"locals", JsonValue::array(std::move(locals))},
    });
}

/// @brief Build a `variables` response: one level of children of @p varRef, paged
///        by @p start / @p count. Children carry their own varRef>0 when they are
///        themselves expandable, so the IDE can drill down. Refs are only valid at
///        the current stop; a stale ref (after resume) yields an empty child list.
/// @param info Current VM stop and variable-expansion provider.
/// @param varRef Stop-scoped composite reference.
/// @param start Zero-based child offset, clamped to zero.
/// @param count Maximum child count; nonpositive values select the default page size.
/// @return Adapter @c variables response.
JsonValue variablesEvent(const il::vm::DebugStopInfo &info,
                         int64_t varRef,
                         int64_t start,
                         int64_t count) {
    JsonValue::ArrayType vars;
    if (info.vars && varRef > 0) {
        if (start < 0)
            start = 0;
        if (count <= 0)
            count = 100; // default page size when the IDE omits a bound
        for (const auto &c : info.vars->expand(varRef, start, count)) {
            vars.push_back(JsonValue::object({
                {"name", JsonValue(c.name)},
                {"value", JsonValue(c.value)},
                {"type", JsonValue(c.type)},
                {"varRef", JsonValue(c.varRef)},
                {"childCount", JsonValue(c.childCount)},
            }));
        }
    }
    return JsonValue::object({
        {"type", JsonValue("variables")},
        {"varRef", JsonValue(varRef)},
        {"start", JsonValue(start)},
        {"vars", JsonValue::array(std::move(vars))},
    });
}

/// @brief Build the final adapter termination event.
/// @param reason Stable exit, crash, or terminated reason token.
/// @param exitCode Debuggee or adapter exit code.
/// @return Adapter @c terminated event.
JsonValue terminatedEvent(const char *reason, int exitCode) {
    return JsonValue::object({
        {"type", JsonValue("terminated")},
        {"reason", JsonValue(reason)},
        {"exitCode", JsonValue(static_cast<int64_t>(exitCode))},
    });
}

/// @brief Build a side-effect-free local resolver for debug expressions.
/// @details The DebugExpr evaluator is intentionally decoupled from VM storage.
///          It asks for value/type strings by local name, so this adapter bridge
///          supplies those pairs from the top-frame stop snapshot without
///          mutating the debuggee or touching runtime memory.
/// @param info VM stop information containing the locals visible to the IDE.
/// @return Resolver callback suitable for condition, logpoint, and watch evaluation.
zanna::dbgexpr::Resolver localResolver(const il::vm::DebugStopInfo &info) {
    /// @brief Resolve one local name to debugger value and type text.
    /// @param name Local variable name to find.
    /// @param[out] val Receives the formatted value.
    /// @param[out] ty Receives the formatted type.
    /// @return `true` when a matching local is present.
    return [&info](const std::string &name, std::string &val, std::string &ty) -> bool {
        for (const auto &l : info.locals) {
            if (l.name == name) {
                val = l.value;
                ty = l.type;
                return true;
            }
        }
        return false;
    };
}

/// @brief Return the debugger display type for an evaluated expression result.
/// @param value Pure evaluator result.
/// @return Compact type string used by the IDE Variables/Evaluate surfaces.
std::string debugValueType(const zanna::dbgexpr::Value &value) {
    switch (value.k) {
        case zanna::dbgexpr::Value::K::Int:
            return "i64";
        case zanna::dbgexpr::Value::K::Flt:
            return "f64";
        case zanna::dbgexpr::Value::K::Bool:
            return "i1";
        case zanna::dbgexpr::Value::K::Str:
            return "str";
        default:
            return "";
    }
}

/// @brief Resolve a watch/evaluate expression against the current stop.
/// @details Evaluation is pure and uses only the current stop's local snapshot.
///          Expressions use the same grammar as conditional breakpoints and
///          logpoints: literals, locals, arithmetic, comparisons, boolean ops,
///          and parentheses. Parse/type errors return ok=false so the IDE can
///          show an unavailable expression instead of a stale value.
/// @param info Current VM stop containing visible locals.
/// @param expr Expression text received from the IDE.
/// @return Adapter @c evaluated event with a scalar value or @c ok set to false.
JsonValue evaluatedEvent(const il::vm::DebugStopInfo &info, const std::string &expr) {
    auto resolve = localResolver(info);
    zanna::dbgexpr::Value value = zanna::dbgexpr::Eval(expr, resolve).run();
    if (!value.isErr())
        return JsonValue::object({
            {"type", JsonValue("evaluated")},
            {"expr", JsonValue(expr)},
            {"value", JsonValue(value.str())},
            {"valueType", JsonValue(debugValueType(value))},
            {"ok", JsonValue(true)},
            // Watch results are pure scalars today, so always leaves. Emitting the
            // structured fields keeps their shape aligned with locals for the IDE.
            {"varRef", JsonValue(static_cast<int64_t>(0))},
            {"childCount", JsonValue(static_cast<int64_t>(0))},
        });
    return JsonValue::object({
        {"type", JsonValue("evaluated")},
        {"expr", JsonValue(expr)},
        {"value", JsonValue("")},
        {"valueType", JsonValue("")},
        {"ok", JsonValue(false)},
        {"varRef", JsonValue(static_cast<int64_t>(0))},
        {"childCount", JsonValue(static_cast<int64_t>(0))},
    });
}

/// @brief DebugFrontend that serializes each stop and blocks for the IDE's next
///        command. The VM thread is paused for the duration of onStop.
/// @details Implements source-LINE stepping on top of the VM's instruction-level
///          step machinery: while a step keeps landing on the same source line it
///          auto-issues another step without notifying the IDE, surfacing a stop
///          only when the line changes (or a breakpoint intervenes).
class AdapterFrontend : public il::vm::DebugFrontend {
  public:
    /// @brief Bind the VM frontend to its control channel and initial breakpoint metadata.
    /// @param chan Debugger control channel that outlives this frontend.
    /// @param meta Initial conditional-breakpoint and logpoint metadata.
    AdapterFrontend(DebugChannel &chan, BpMetaMap meta) : chan_(chan), bpMeta_(std::move(meta)) {}

    /// @brief Surface a VM stop, apply breakpoint policy, and await the next execution action.
    /// @param info Current immutable stop snapshot.
    /// @return VM action selected by stepping policy or the IDE.
    il::vm::DebugAction onStop(const il::vm::DebugStopInfo &info) override {
        // Run-to-cursor: keep stepping over until the target line is reached. A
        // real breakpoint or exception cancels it; the step bound guards against a
        // target that is never hit (then it behaves like Continue-to-end).
        if (runToActive_) {
            const bool hitTarget =
                info.line == runToLine_ && baseNameOf(info.path) == baseNameOf(runToPath_);
            const bool interrupted = info.reason == "breakpoint" || info.reason == "exception";
            if (!hitTarget && !interrupted && info.line != 0 && runToSteps_ < kMaxRunToSteps) {
                ++runToSteps_;
                return {il::vm::DebugActionKind::StepOver, 0};
            }
            runToActive_ = false;
            runToSteps_ = 0;
            mode_ = StepMode::None;
            autoSteps_ = 0;
            // fall through to surface the stop at the current location
        }

        // Continue an in-progress line step while still on the origin line. Step-out
        // always surfaces its first pause (it lands in the caller). A breakpoint or
        // an unhandled trap always surfaces, even mid-step.
        if (mode_ != StepMode::None && info.reason != "breakpoint" && info.reason != "exception") {
            const bool sameLine = info.line == originLine_ && info.path == originPath_;
            if (mode_ != StepMode::Out && sameLine && info.line != 0 &&
                autoSteps_ < kMaxAutoSteps) {
                ++autoSteps_;
                return stepAction(mode_);
            }
        }
        mode_ = StepMode::None;
        autoSteps_ = 0;

        // Conditional breakpoints / logpoints (breakpoint stops only — stepping onto
        // such a line always stops). A logpoint emits a message and resumes; a
        // conditional stop is suppressed when the condition is falsey.
        if (info.reason == "breakpoint") {
            auto it = bpMeta_.find(metaKey(info.path, info.line));
            if (it != bpMeta_.end()) {
                auto resolve = localResolver(info);
                if (!it->second.logMessage.empty()) {
                    chan_.emit(logEvent(
                        info.line, zanna::dbgexpr::interpolate(it->second.logMessage, resolve)));
                    return {il::vm::DebugActionKind::Continue, 0};
                }
                if (!it->second.condition.empty() &&
                    !zanna::dbgexpr::conditionHolds(it->second.condition, resolve)) {
                    return {il::vm::DebugActionKind::Continue, 0};
                }
            }
        }

        chan_.emit(stoppedEvent(info));
        for (;;) {
            auto cmd = chan_.readCommand();
            if (!cmd)
                std::_Exit(0); // IDE/pipe gone: stop debugging immediately.
            const std::string &type = (*cmd)["type"].asString();
            if (type == "continue")
                return {il::vm::DebugActionKind::Continue, 0};
            if (type == "stepOver")
                return beginStep(StepMode::Over, info);
            if (type == "stepIn")
                return beginStep(StepMode::In, info);
            if (type == "stepOut")
                return beginStep(StepMode::Out, info);
            if (type == "runToLine") {
                runToActive_ = true;
                runToPath_ = (*cmd)["path"].asString();
                runToLine_ = static_cast<uint32_t>((*cmd)["line"].asInt());
                runToSteps_ = 0;
                return {il::vm::DebugActionKind::StepOver, 0};
            }
            if (type == "setBreakpoints") {
                // Mid-session edit of conditions/logpoints. The breakpoint line set
                // itself is fixed at launch; adding/removing lines while running is
                // future dynamic-breakpoint work.
                applyBpMeta(bpMeta_, *cmd);
                continue;
            }
            if (type == "evaluate") {
                // Watch/hover query: report the value and stay stopped.
                chan_.emit(evaluatedEvent(info, (*cmd)["expr"].asString()));
                continue;
            }
            if (type == "variables") {
                // Lazy one-level expansion of a composite local; stay stopped. The
                // VM thread is paused here, so reading child values is safe.
                const int64_t ref = (*cmd)["varRef"].asInt();
                int64_t start = 0;
                int64_t count = 0;
                if (const JsonValue *s = cmd->get("start"))
                    start = s->asInt();
                if (const JsonValue *c = cmd->get("count"))
                    count = c->asInt();
                chan_.emit(variablesEvent(info, ref, start, count));
                continue;
            }
            if (type == "terminate") {
                chan_.emit(terminatedEvent("terminated", 0));
                std::_Exit(0);
            }
            // "pause" while already stopped, or any unknown command: keep waiting.
        }
    }

  private:
    enum class StepMode { None, Over, In, Out };

    /// @brief Record the source origin and begin one line-oriented step.
    /// @param mode Step-in, step-over, or step-out policy.
    /// @param info Current stop providing the origin line and path.
    /// @return Corresponding VM instruction-level action.
    il::vm::DebugAction beginStep(StepMode mode, const il::vm::DebugStopInfo &info) {
        mode_ = mode;
        originLine_ = info.line;
        originPath_ = info.path;
        autoSteps_ = 0;
        return stepAction(mode);
    }

    /// @brief Convert adapter line-step state to the VM's next debug action.
    /// @param mode Current adapter step mode.
    /// @return VM step, step-over, step-out, or continue action.
    static il::vm::DebugAction stepAction(StepMode mode) {
        switch (mode) {
            case StepMode::In:
                return {il::vm::DebugActionKind::Step, 1};
            case StepMode::Over:
                return {il::vm::DebugActionKind::StepOver, 0};
            case StepMode::Out:
                return {il::vm::DebugActionKind::StepOut, 0};
            default:
                return {il::vm::DebugActionKind::Continue, 0};
        }
    }

    /// @brief Safety bound on per-line auto-steps so a degenerate line cannot spin.
    static constexpr int kMaxAutoSteps = 200000;
    /// @brief Safety bound on run-to-cursor steps (it may cross many lines).
    static constexpr int kMaxRunToSteps = 5000000;

    DebugChannel &chan_;
    StepMode mode_ = StepMode::None;
    uint32_t originLine_ = 0;
    std::string originPath_;
    int autoSteps_ = 0;

    // Run-to-cursor: step over until reaching (runToPath_, runToLine_), or until an
    // existing breakpoint/exception intervenes, or the step bound is exhausted.
    bool runToActive_ = false;
    uint32_t runToLine_ = 0;
    std::string runToPath_;
    int runToSteps_ = 0;

    // Conditional-breakpoint / logpoint metadata, keyed by basename:line.
    BpMetaMap bpMeta_;
};

} // namespace

/// @brief Run a verified IL module under interactive newline-JSON debugger control.
/// @param module Caller-owned verified module executed by the VM.
/// @param programArgs Arguments exposed to the debuggee.
/// @param maxSteps Optional VM step budget; zero means unlimited.
/// @param sm Caller-owned source manager used for source locations.
/// @param debugLayouts Class-layout metadata moved into the VM debug configuration.
/// @return Debuggee exit code, normalized to one when the VM result exceeds @c int range or traps.
int runDebugAdapter(il::core::Module &module,
                    const std::vector<std::string> &programArgs,
                    uint64_t maxSteps,
                    il::support::SourceManager &sm,
                    il::vm::DebugClassLayoutTable debugLayouts) {
    DebugChannel chan;
    chan.emit(JsonValue::object({
        {"type", JsonValue("initialized")},
        {"reason", JsonValue("launch")},
    }));

    il::vm::RunConfig runCfg;
    runCfg.trace.sm = &sm;
    runCfg.maxSteps = maxSteps;
    runCfg.programArgs = programArgs;
    runCfg.debug.setSourceManager(&sm);
    runCfg.debug.setClassLayouts(std::move(debugLayouts));

    // Handshake: accept breakpoints, then wait for an explicit launch so the IDE
    // can install breakpoints before the program runs.
    BpMetaMap initialMeta;
    bool launched = false;
    while (!launched) {
        auto cmd = chan.readCommand();
        if (!cmd)
            return 0; // IDE gone before launch.
        const std::string &type = (*cmd)["type"].asString();
        if (type == "setBreakpoints") {
            const std::string &path = (*cmd)["path"].asString();
            for (const auto &ln : (*cmd)["lines"].asArray()) {
                const int64_t line = ln.asInt();
                if (line > 0)
                    runCfg.debug.addBreakSrcLine(path, static_cast<uint32_t>(line));
            }
            applyBpMeta(initialMeta, *cmd);
        } else if (type == "launch") {
            launched = true;
        } else if (type == "terminate") {
            chan.emit(terminatedEvent("terminated", 0));
            return 0;
        }
    }

    AdapterFrontend frontend(chan, std::move(initialMeta));
    runCfg.frontend = &frontend;

    // While the program runs freely (between stops), poll for an interactive Pause
    // or a terminate. requestDebugPause makes the frontend stop at the next
    // instruction without ending the run, so execution can resume.
    runCfg.interruptEveryN = 20000;
    /// @brief Drain pause and terminate commands while the VM runs freely.
    /// @param vm Active virtual machine that can receive a cooperative pause request.
    /// @return Always `true` to continue execution after polling.
    runCfg.pollCallback = [&chan](il::vm::VM &vm) -> bool {
        while (auto cmd = chan.tryReadCommand()) {
            const std::string &type = (*cmd)["type"].asString();
            if (type == "pause") {
                il::vm::requestDebugPause(vm);
                // Stop draining so the pause stop materializes before any queued
                // follow-up command (e.g. terminate) is handled at the stop.
                break;
            }
            if (type == "terminate") {
                chan.emit(terminatedEvent("terminated", 0));
                std::_Exit(0);
            }
            // Other commands have no meaning while running; ignore them.
        }
        return true; // keep running
    };

    il::vm::Runner runner(module, std::move(runCfg));
    const int64_t runResult = runner.run();

    int rc = 0;
    const auto intMin = static_cast<int64_t>(std::numeric_limits<int>::min());
    const auto intMax = static_cast<int64_t>(std::numeric_limits<int>::max());
    if (runResult < intMin || runResult > intMax)
        rc = 1;
    else
        rc = static_cast<int>(runResult);

    const auto trapMessage = runner.lastTrapMessage();
    const char *reason = "exit";
    if (trapMessage && !trapMessage->empty()) {
        reason = "crash";
        std::cerr << *trapMessage;
        if (trapMessage->back() != '\n')
            std::cerr << '\n';
        if (rc == 0)
            rc = 1;
    }

    chan.emit(terminatedEvent(reason, rc));
    return rc;
}

} // namespace il::tools::debug
