//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/vm/debug/Trace.cpp
// Purpose: Implement deterministic tracing for IL VM steps and consolidate the
//          presentation logic that surfaces interpreter activity to developers.
// Key invariants: Each executed instruction produces at most one flushed line,
//                 trace emission honours @ref TraceConfig::mode, and cached
//                 source mappings remain valid for the lifetime of the owning
//                 function.
// Ownership/Lifetime: Trace sinks borrow VM state and emit to externally owned
//                     streams; no persistent resources are allocated beyond a
//                     trace session.
// Links: docs/runtime-vm.md#debugger, docs/dev/vm.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements the IL virtual machine tracing facilities.
/// @details The helpers in this file translate interpreter activity into
///          human-readable diagnostics.  They maintain caches that map
///          instructions back to source lines, render operand values with
///          locale-stable formatting, and coordinate the various tracing modes
///          (IL, source, and disabled).  Keeping the implementation here keeps
///          the VM dispatch loop lightweight while concentrating all
///          presentation logic in a single location.

#include "zanna/vm/debug/Debug.hpp"

#include "il/core/BasicBlock.hpp"
#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Opcode.hpp"
#include "il/core/Value.hpp"
#include "support/source_manager.hpp"
#include "vm/VM.hpp"
#include <clocale>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <string>
#include <utility>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace il::vm {

/// @brief Determine whether tracing output should be emitted for the current configuration.
/// @details The interpreter consults this helper before performing any trace
///          bookkeeping.  When the mode is @c Off the VM can bypass trace logic
///          entirely, ensuring production builds avoid incidental overhead.  Any
///          other value signals that diagnostic output is desired and that the
///          trace sink should observe VM events.
/// @return True when tracing is active, false when it is disabled.
bool TraceConfig::enabled() const {
    return mode != Off;
}

namespace {
/// @brief RAII helper that temporarily forces the C locale for numeric formatting.
/// @details Tracing renders floating-point operands using @c LC_NUMERIC.  Host
///          locales that use commas for the decimal separator would otherwise
///          leak into the trace stream, producing nondeterministic output.  The
///          guard snapshots the current locale of both the global C API and the
///          @c std::ostream before substituting the classic locale.  Destruction
///          restores the original state so the rest of the program continues to
///          respect user locale preferences.
class LocaleGuard {
    std::ostream &os;    ///< Stream whose locale is temporarily replaced.
    std::locale oldLoc;  ///< Stream locale restored on scope exit.
    std::string oldC;    ///< Saved process-wide numeric locale name.

  public:
    /// @brief Enter the guard, snapshotting the current locale information.
    /// @details Captures the stream's locale object and the global numeric
    ///          locale (if available) before forcing both to the classic "C"
    ///          variant.  The constructor performs minimal work so it can wrap
    ///          hot interpreter paths without introducing noticeable overhead.
    /// @param s Stream that should use locale-stable numeric formatting.
    explicit LocaleGuard(std::ostream &s) : os(s), oldLoc(s.getloc()) {
        if (const char *c = std::setlocale(LC_NUMERIC, nullptr))
            oldC = c;
        os.imbue(std::locale::classic());
        std::setlocale(LC_NUMERIC, "C");
    }

    /// @brief Restore the previous locale settings captured by the constructor.
    /// @details Reinstates both the global C locale and the stream's locale so
    ///          downstream code observes the host's preferred formatting rules.
    ///          The guard is intentionally noexcept; even if locale restoration
    ///          fails the destructor suppresses exceptions to keep tracing code
    ///          robust.
    ~LocaleGuard() {
        if (!oldC.empty())
            std::setlocale(LC_NUMERIC, oldC.c_str());
        os.imbue(oldLoc);
    }
};

/// @brief Print a Value with stable numeric formatting.
/// @details Mirrors @ref il::core::toString but avoids heap allocations by
///          writing directly to @p os.  Temporaries print using the `%t` prefix,
///          booleans are spelled out, floating-point literals use
///          @c std::snprintf with explicit precision to preserve subnormal
///          values, and strings are quoted.  Signed zero is preserved by
///          inspecting @c std::signbit so traces remain faithful to execution.
/// @param os Stream receiving the textual representation.
/// @param v  Value to render.
void printValue(std::ostream &os, const il::core::Value &v) {
    switch (v.kind) {
        case il::core::Value::Kind::Temp:
            os << "%t" << std::dec << v.id;
            break;
        case il::core::Value::Kind::ConstInt:
            if (v.isBool)
                os << (v.i64 != 0 ? "true" : "false");
            else
                os << std::dec << v.i64;
            break;
        case il::core::Value::Kind::ConstFloat: {
            if (std::signbit(v.f64) && v.f64 == 0.0) {
                os << "-0.0";
                break;
            }
            if (v.f64 == 0.0) {
                os << "0.0";
                break;
            }
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.17g", v.f64);
            os << buf;
            break;
        }
        case il::core::Value::Kind::ConstStr:
            os << '"' << v.str << '"';
            break;
        case il::core::Value::Kind::GlobalAddr:
            os << '@' << v.str;
            break;
        case il::core::Value::Kind::NullPtr:
            os << "null";
            break;
    }
}
} // namespace

/// @brief Construct a TraceSink with the given configuration.
/// @details Stores the configuration by value and ensures the standard error
///          stream uses binary mode on Windows so newline translation does not
///          corrupt emitted IL snippets.  File caches and instruction maps are
///          populated lazily, allowing the trace sink to remain cheap when
///          tracing is disabled.
/// @param cfg Trace configuration controlling emission behaviour and source lookup.
TraceSink::TraceSink(TraceConfig cfg) : cfg(cfg) {
#ifdef _WIN32
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    // Set the C locale for numeric formatting once at session start rather than
    // per-instruction. This avoids two costly setlocale() calls per traced step.
    if (this->cfg.enabled()) {
        savedCLocale_ =
            std::setlocale(LC_NUMERIC, nullptr) ? std::setlocale(LC_NUMERIC, nullptr) : "";
        std::setlocale(LC_NUMERIC, "C");
        savedStreamLocale_ = std::cerr.getloc();
        std::cerr.imbue(std::locale::classic());
        localeSet_ = true;
    }
}

/// @brief Restore locale state changed for an enabled trace session.
TraceSink::~TraceSink() {
    if (localeSet_) {
        if (!savedCLocale_.empty())
            std::setlocale(LC_NUMERIC, savedCLocale_.c_str());
        std::cerr.imbue(savedStreamLocale_);
    }
}

/// @brief Precompute instruction source locations for a prepared frame.
/// @details When a new frame begins executing the trace sink builds an index
///          from instruction pointers to their owning blocks and instruction
///          offsets.  The cache is computed only once per function to minimise
///          tracing overhead during tight interpreter loops and is reused for
///          subsequent activations of the same function.
/// @param fr Frame whose function's instructions should be indexed.
void TraceSink::onFramePrepared(const Frame &fr) {
    if (!fr.func)
        return;
    auto [it, inserted] = instrLocations.try_emplace(fr.func);
    if (!inserted)
        return;
    auto &map = it->second;
    // Reserve buckets to avoid rehashing while indexing instructions.
    std::size_t totalInstrs = 0;
    for (const auto &block : fr.func->blocks)
        totalInstrs += block.instructions.size();
    if (totalInstrs)
        map.reserve(totalInstrs);
    for (const auto &block : fr.func->blocks) {
        for (size_t idx = 0; idx < block.instructions.size(); ++idx)
            map.emplace(&block.instructions[idx], InstrLocation{&block, idx});
    }
}

/// @brief Retrieve cached file contents, loading from disk if necessary.
/// @details Uses @ref TraceConfig::sm to resolve source file identifiers into
///          absolute paths and then caches the line-by-line contents so repeated
///          trace entries avoid redundant I/O. When no source manager, file
///          identifier, or resolved path is available the function returns
///          null. A resolved but unreadable file is cached with an empty line
///          list so repeated trace entries do not retry I/O. The optional path
///          hint avoids another source-manager path lookup at call sites.
/// @param file_id   Source manager identifier for the file.
/// @param path_hint Optional filesystem path to use when the source manager path is empty.
/// @return Cached file entry, or @c nullptr when the file cannot be resolved.
const TraceSink::FileCacheEntry *TraceSink::getOrLoadFile(uint32_t file_id, std::string path_hint) {
    if (!cfg.sm || file_id == 0)
        return nullptr;
    auto it = fileCache.find(file_id);
    if (it != fileCache.end())
        return &it->second;

    FileCacheEntry entry;
    if (!path_hint.empty())
        entry.path = std::move(path_hint);
    else
        entry.path = std::string(cfg.sm->getPath(file_id));

    if (entry.path.empty())
        return nullptr;

    std::filesystem::path fsPath;
#if defined(__cpp_char8_t)
    const auto *raw = reinterpret_cast<const char8_t *>(entry.path.data());
    const std::u8string u8Path(raw, raw + entry.path.size());
    fsPath = std::filesystem::path(u8Path);
#else
    fsPath = std::filesystem::path(entry.path);
#endif
    std::ifstream f(fsPath);
    if (f) {
        std::string line;
        while (std::getline(f, line))
            entry.lines.push_back(line);
    }

    auto [pos, inserted] = fileCache.emplace(file_id, std::move(entry));
    (void)inserted;
    return &pos->second;
}

/// @brief Emit a trace line for a single executed instruction.
/// @details Guarded by @ref TraceConfig::enabled.  Resolves the currently
///          executing block and instruction index using the cached maps and then
///          prints a deterministic record containing the opcode, operand values,
///          and, when configured, source information.  LocaleGuard ensures
///          numeric operands always render using the C locale regardless of the
///          host environment.  The formatting varies by trace mode: IL traces
///          emit mnemonic/operand tuples while SRC traces echo filename, line,
///          and (when available) the source text for the active instruction.
/// @param in Instruction being executed.
/// @param fr Execution frame that provides function and block context.
void TraceSink::onStep(const il::core::Instr &in, const Frame &fr) {
    if (!cfg.enabled())
        return;
    // Locale is set once at session start (constructor), no per-step guard needed.
    std::cerr << std::noboolalpha << std::dec;
    const auto *fn = fr.func;
    if (!fn)
        return;
    const il::core::BasicBlock *blk = nullptr;
    size_t ip = 0;
    auto fnIt = instrLocations.find(fn);
    if (fnIt != instrLocations.end()) {
        auto locIt = fnIt->second.find(&in);
        if (locIt != fnIt->second.end()) {
            blk = locIt->second.block;
            ip = locIt->second.ip;
        }
    }
    if (!blk)
        return;
    if (cfg.mode == TraceConfig::IL) {
        std::cerr << "[IL] fn=@" << fn->name << " blk=" << blk->label << " ip=#" << ip
                  << " op=" << il::core::toString(in.op);
        if (!in.operands.empty()) {
            std::cerr << ' ';
            for (size_t i = 0; i < in.operands.size(); ++i) {
                if (i)
                    std::cerr << ", ";
                printValue(std::cerr, in.operands[i]);
            }
        }
        if (in.result)
            std::cerr << " -> %t" << std::dec << *in.result;
        std::cerr << '\n' << std::flush;
        return;
    }
    if (cfg.mode == TraceConfig::SRC) {
        std::string locStr = "<unknown>";
        std::string srcLine;
        if (cfg.sm && in.loc.hasFile()) {
            std::string path(cfg.sm->getPath(in.loc.file_id));
            std::filesystem::path p(path);
            locStr = p.filename().string();
            if (in.loc.hasLine()) {
                locStr += ':' + std::to_string(in.loc.line);
                if (in.loc.hasColumn())
                    locStr += ':' + std::to_string(in.loc.column);
            }
            if (!path.empty()) {
                const auto *entry = getOrLoadFile(in.loc.file_id, std::move(path));
                if (entry && in.loc.hasLine() &&
                    static_cast<size_t>(in.loc.line) <= entry->lines.size()) {
                    const std::string &line = entry->lines[in.loc.line - 1];
                    if (!line.empty()) {
                        if (in.loc.hasColumn() && in.loc.column - 1 < line.size())
                            srcLine = line.substr(in.loc.column - 1);
                        else
                            srcLine = line;
                        while (!srcLine.empty() &&
                               (srcLine.back() == '\n' || srcLine.back() == '\r'))
                            srcLine.pop_back();
                    }
                }
            }
        }
        std::cerr << "[SRC] " << locStr << "  (fn=@" << fn->name << " blk=" << blk->label << " ip=#"
                  << ip << ')';
        if (!srcLine.empty())
            std::cerr << "  " << srcLine;
        std::cerr << '\n' << std::flush;
    }
}

/// @brief Emit a trace line for a tail-call optimisation event.
/// @details When tracing is enabled, prints a compact line describing the
///          function tail-called from/to. Emission is mode-agnostic and always
///          prints a single line to keep ordering relative to steps.
/// @param from Caller function being replaced.
/// @param to   Callee function entered via tail-call.
void TraceSink::onTailCall(const il::core::Function *from, const il::core::Function *to) {
    if (!cfg.enabled())
        return;
    std::cerr << "[IL] tailcall " << (from ? from->name : std::string("<unknown>")) << " -> "
              << (to ? to->name : std::string("<unknown>")) << '\n'
              << std::flush;
}

} // namespace il::vm
