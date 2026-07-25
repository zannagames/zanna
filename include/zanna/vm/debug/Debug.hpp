//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: include/zanna/vm/debug/Debug.hpp
// Purpose: Expose lightweight debugger and tracing configuration types for the
//          IL virtual machine without requiring full VM internals.
// Key invariants: Public types remain header-only data containers and helpers so
//                 tools can configure debugging without pulling interpreter
//                 implementation details.
// Ownership/Lifetime: Debug controller owns its internal caches by value; trace
//                     sinks do not own frame memory and operate on caller-managed
//                     VM state.
// Links: docs/internals/codemap/vm-runtime.md, src/vm/debug/
//
//===----------------------------------------------------------------------===//

/**
 * @file include/zanna/vm/debug/Debug.hpp
 * @brief Declares VM tracing, breakpoint, watchpoint, and debug-script support.
 *
 * @details
 * The types in this header form the lightweight debugger state shared by the VM
 * runner and interactive tooling. `TraceSink` renders instruction activity,
 * `DebugCtrl` manages breakpoints and watches, and `DebugScript` queues
 * automation commands.
 *
 * Debug state owns copied names, paths, tags, layouts, and queued events.
 * Source managers, functions, frames, instructions, and watched addresses are
 * borrowed and must remain valid for the operations that inspect them.
 */

#pragma once

#include "il/core/Type.hpp"
#include "support/source_location.hpp"
#include "support/string_interner.hpp"
#include "support/symbol.hpp"
#include "zanna/vm/debug/DebugClassLayout.hpp"

#include <cstddef>
#include <cstdint>
#include <locale>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace il::core
{
struct BasicBlock;
struct Function;
struct Instr;
} // namespace il::core

namespace il::support
{
class SourceManager;
} // namespace il::support

namespace il::vm
{
struct Frame;

/// @brief Discrete asynchronous events surfaced by VM debugging.
enum class DebugEvent
{
    None = 0,    ///< No debugger event is pending.
    TailCall,    ///< A tail-call reused the current frame (`from` to `to`).
    MemWatchHit, ///< A memory write intersected a watched range.
};

/**
 * @brief Identifies the functions participating in a tail-call frame reuse.
 * @ownership Both pointers are borrowed from the executing module and may be
 *            null when function metadata is unavailable.
 */
struct TailCallInfo
{
    const il::core::Function *from = nullptr; ///< Function whose frame was reused.
    const il::core::Function *to = nullptr;   ///< Function entered by the tail call.
};

/**
 * @brief Owning event record for a write intersecting a watched memory range.
 *
 * `addr` and `size` describe the write, not the complete registered watch. The
 * address is informational and is not dereferenced by the event.
 */
struct MemWatchHit
{
    const void *addr = nullptr; ///< Address of the write.
    std::size_t size = 0;       ///< Number of bytes written.
    std::string tag;            ///< User-provided tag for the watch range.
};

/**
 * @brief Selects interpreter trace presentation and source resolution.
 * @ownership The source-manager pointer is borrowed and must outlive any
 *            `TraceSink` that uses this configuration.
 */
struct TraceConfig
{
    /// @brief Mutually exclusive trace output modes.
    enum Mode
    {
        Off, ///< Tracing disabled.
        IL,  ///< Trace IL instructions.
        SRC  ///< Trace source locations.
    } mode{Off};

    /// @brief Optional borrowed source manager used by source tracing.
    const il::support::SourceManager *sm = nullptr;

    /**
     * @brief Determine whether trace bookkeeping and output are enabled.
     * @return `true` for `IL` or `SRC`; `false` for `Off`.
     */
    [[nodiscard]] bool enabled() const;
};

/**
 * @brief Formats deterministic instruction and tail-call traces on stderr.
 *
 * The sink caches instruction locations by module pointers and source file
 * contents by file identifier. Consequently, referenced module objects and the
 * configured source manager must outlive the sink. An enabled sink temporarily
 * changes the process C numeric locale and `std::cerr` locale, restoring both
 * when destroyed; concurrent locale mutation is therefore unsupported.
 */
class TraceSink
{
  public:
    /**
     * @brief Construct a trace session from copied configuration.
     * @param cfg Mode and borrowed source-manager pointer.
     *
     * On Windows, stderr is placed in binary mode. When tracing is enabled, the
     * C numeric locale and `std::cerr` use the classic locale until destruction.
     */
    explicit TraceSink(TraceConfig cfg = {});

    /**
     * @brief Restore the C numeric and stderr locales captured at construction.
     */
    ~TraceSink();

    /**
     * @brief Cache instruction-to-block locations for a prepared frame.
     * @param fr Borrowed frame whose function is indexed once. A frame without
     *           a function is ignored.
     *
     * @pre The function, its blocks, and its instruction storage remain stable
     *      for the trace-sink lifetime.
     */
    void onFramePrepared(const Frame &fr);

    /**
     * @brief Emit the configured trace representation of one instruction.
     * @param in Borrowed instruction being executed.
     * @param fr Borrowed frame providing function context.
     *
     * No output is produced when tracing is disabled, the frame has no function,
     * or `onFramePrepared()` has not indexed the instruction.
     */
    void onStep(const il::core::Instr &in, const Frame &fr);

    /**
     * @brief Emit one tail-call event when tracing is enabled.
     * @param from Borrowed caller function, or null to print an unknown name.
     * @param to Borrowed callee function, or null to print an unknown name.
     */
    void onTailCall(const il::core::Function *from, const il::core::Function *to);

    /**
     * @brief Query the copied trace configuration.
     * @return `true` when output should be emitted.
     */
    [[nodiscard]] bool isEnabled() const noexcept
    {
        return cfg.enabled();
    }

  private:
    struct InstrLocation
    {
        const il::core::BasicBlock *block = nullptr; ///< Owning basic block.
        size_t ip = 0;                               ///< Instruction position within the block.
    };

    struct FileCacheEntry
    {
        std::string path;               ///< Canonical file path.
        std::vector<std::string> lines; ///< Cached source text split into lines.
    };

    /**
     * @brief Find or populate cached source lines for a file identifier.
     * @param fileId Nonzero source-manager file identifier.
     * @param pathHint Optional owned path used instead of source-manager lookup.
     * @return Pointer into `fileCache`, or null when the source manager or file
     *         identifier is unavailable.
     *
     * @note A non-null pointer remains valid until the trace sink is destroyed.
     */
    const FileCacheEntry *getOrLoadFile(uint32_t fileId, std::string pathHint = {});

    TraceConfig cfg; ///< Active configuration.
    std::unordered_map<const il::core::Function *,
                       std::unordered_map<const il::core::Instr *, InstrLocation>>
        instrLocations; ///< Per-function instruction lookup cache.
    std::unordered_map<uint32_t, FileCacheEntry> fileCache; ///< Cached source files.

    // Session-level locale state (set once at construction, restored at destruction)
    bool localeSet_ = false;
    std::string savedCLocale_;
    std::locale savedStreamLocale_;
};

/// @brief Block-entry breakpoint identified by an interned label symbol.
struct Breakpoint
{
    il::support::Symbol label; ///< Target block label symbol.
};

/**
 * @brief Owns breakpoint, variable-watch, memory-watch, and layout debug state.
 *
 * Label and variable names share a controller-local string interner. Source
 * breakpoints store normalized full paths and basenames, and coalesce repeated
 * hits at the same source location. Variable watches print changed values to
 * stderr; memory watches enqueue owning `MemWatchHit` records for the host.
 *
 * @ownership The optional source manager and watched address ranges are
 *            borrowed. All names, tags, layouts, indices, and events are owned.
 */
class DebugCtrl
{
  public:
    /**
     * @brief Intern a block-label spelling in controller-owned storage.
     * @param label Borrowed label bytes copied when first encountered.
     * @return Stable symbol suitable for `addBreak()` and lookup.
     */
    il::support::Symbol internLabel(std::string_view label);

    /**
     * @brief Add an idempotent block-entry breakpoint.
     * @param sym Symbol created by this controller's interner. An invalid symbol
     *            is ignored.
     */
    void addBreak(il::support::Symbol sym);

    /**
     * @brief Test whether a block label has a registered breakpoint.
     * @param blk Borrowed block whose label is interned for lookup.
     * @return `true` when its interned symbol is present.
     */
    [[nodiscard]] bool shouldBreak(const il::core::BasicBlock &blk) const;

    /**
     * @brief Add a source-line breakpoint by normalized path and basename.
     * @param file Path copied, separator-normalized, and lexically normalized.
     * @param line One-based source line used as the primary lookup key.
     *
     * @note Duplicate breakpoints are retained but have equivalent behavior.
     */
    void addBreakSrcLine(std::string file, uint32_t line);

    /**
     * @brief Query whether at least one source-line breakpoint is registered.
     * @return `true` when the source breakpoint vector is non-empty.
     */
    [[nodiscard]] bool hasSrcLineBPs() const;

    /**
     * @brief Match an instruction against normalized source-line breakpoints.
     *
     * @param instr Borrowed instruction containing the candidate source location.
     * @return `true` for the first matching full-path or basename breakpoint
     *         after execution enters a new file/line pair.
     *
     * @note Returns `false` without a source manager, usable file/line metadata,
     *       a matching line key, or a resolvable source path. A successful pair
     *       is coalesced until `resetLastHit()` is called or another pair hits.
     */
    [[nodiscard]] bool shouldBreakOn(const il::core::Instr &instr) const;

    /**
     * @brief Install the source manager used for file-id resolution.
     * @param sm Borrowed manager, or null to disable source path resolution.
     * @pre A non-null manager outlives this controller.
     */
    void setSourceManager(const il::support::SourceManager *sm);

    /**
     * @brief Retrieve the installed borrowed source manager.
     * @return The exact pointer supplied to `setSourceManager()`, possibly null.
     */
    [[nodiscard]] const il::support::SourceManager *getSourceManager() const;

    /**
     * @brief Replace the class-layout sidecar used for field expansion.
     * @param layouts Table moved into controller-owned storage.
     *
     * @see docs/adr/0138-debug-class-layout-sidecar.md
     */
    void setClassLayouts(DebugClassLayoutTable layouts);

    /**
     * @brief Access the installed class-layout sidecar.
     * @return Borrowed reference valid until the table is replaced or the
     *         controller is destroyed.
     */
    [[nodiscard]] const DebugClassLayoutTable &classLayouts() const;

    /**
     * @brief Normalize a path for stable breakpoint comparison.
     *
     * Backslashes become forward slashes and lexical dot segments are removed.
     * On Windows, ASCII letters are lowercased to match source-manager paths.
     *
     * @param path Path consumed and transformed by value.
     * @return Generic path text; empty input and empty relative normalization
     *         produce `.`, while an empty absolute normalization produces `/`.
     *
     * @note This is lexical normalization and performs no filesystem I/O.
     */
    static std::string normalizePath(std::string path);

    /**
     * @brief Register or retrieve a variable watch.
     * @param name Borrowed identifier interned into controller-owned storage.
     * @return Stable nonzero watch ID, an existing ID for a duplicate name, or
     *         zero when interning does not yield a valid symbol.
     */
    uint32_t addWatch(std::string_view name);

    /**
     * @brief Perform the O(1) symbol-to-watch fast-path lookup.
     * @param sym Interned symbol, normally from the same controller.
     * @return Nonzero watch ID when registered; otherwise zero.
     */
    [[nodiscard]] uint32_t getWatchId(il::support::Symbol sym) const noexcept;

    /**
     * @brief Observe a variable store by name and print changes to stderr.
     *
     * The name is interned on every call before delegating to `onStoreById`.
     * Unwatched names produce no output or state change.
     *
     * @param name Variable identifier used for lookup and diagnostics.
     * @param ty Stored IL type kind.
     * @param i64 Integer payload for I1, I16, I32, or I64.
     * @param f64 Floating payload for F64.
     * @param fn Function name included in changed-value output.
     * @param blk Block label included in changed-value output.
     * @param ip Instruction index included in changed-value output.
     */
    void onStore(std::string_view name,
                 il::core::Type::Kind ty,
                 int64_t i64,
                 double f64,
                 std::string_view fn,
                 std::string_view blk,
                 size_t ip);

    /**
     * @brief Observe a variable store through a precomputed watch ID.
     *
     * Supported integer and F64 values are printed only on their first
     * observation, type change, or unequal payload. Unsupported types print an
     * `[unsupported]` line and do not update the remembered value. Invalid IDs
     * are ignored.
     *
     * @param watchId ID returned by `addWatch()` or `getWatchId()`.
     * @param name Identifier used only in diagnostics.
     * @param ty Stored IL type kind.
     * @param i64 Integer payload for supported integer kinds.
     * @param f64 Floating payload for F64.
     * @param fn Function name included in output.
     * @param blk Block label included in output.
     * @param ip Instruction index included in output.
     */
    void onStoreById(uint32_t watchId,
                     std::string_view name,
                     il::core::Type::Kind ty,
                     int64_t i64,
                     double f64,
                     std::string_view fn,
                     std::string_view blk,
                     size_t ip);

    /**
     * @brief Forget the last source breakpoint hit.
     *
     * The next matching instruction may stop again at the same file and line.
     */
    void resetLastHit();

    // Memory watch API -------------------------------------------------------
    /**
     * @brief Register a tagged half-open memory range.
     * @param addr Borrowed non-null start address.
     * @param size Nonzero byte length. Overflowing end addresses saturate at
     *             `UINTPTR_MAX` rather than wrapping.
     * @param tag Owned label copied into each matching event.
     * @return A new nonzero watch ID, or zero for a null address or zero size.
     *
     * @note The controller compares numeric addresses only and never
     *       dereferences the watched storage.
     */
    uint32_t addMemWatch(const void *addr, std::size_t size, std::string tag);

    /**
     * @brief Remove the first watch matching its address, size, and tag.
     * @param addr Original registered address.
     * @param size Original registered byte length.
     * @param tag Original tag text.
     * @return `true` when an entry was erased; otherwise `false`.
     */
    bool removeMemWatch(const void *addr, std::size_t size, std::string_view tag);

    /**
     * @brief Query whether the memory-watch collection is non-empty.
     * @return `true` when at least one range is installed.
     */
    [[nodiscard]] bool hasMemWatches() const noexcept;

    /**
     * @brief Query whether the variable-watch map is non-empty.
     * @return `true` when at least one variable symbol is watched.
     */
    [[nodiscard]] bool hasVarWatches() const noexcept;

    /**
     * @brief Enqueue one event for every watch intersected by a write.
     *
     * @param addr Non-null start of the written range.
     * @param size Nonzero write size. Overflowing end addresses saturate.
     *
     * Fewer than eight watches use a linear scan. Larger collections are sorted
     * lazily and searched in `O(log n + k)` time for `k` intersections. Null or
     * empty writes and calls with no watches are ignored.
     */
    void onMemWrite(const void *addr, std::size_t size);

    /**
     * @brief Transfer all pending memory-watch events to the caller.
     * @return Owning events in insertion order.
     * @post The controller's pending event vector is empty.
     */
    std::vector<MemWatchHit> drainMemWatchEvents();

  private:
    /**
     * @brief Normalize a path and derive its final path component.
     * @param path Path consumed by value.
     * @return Pair containing `normalizePath(path)` and its basename.
     */
    static std::pair<std::string, std::string> normalizePathWithBase(std::string path);

    mutable il::support::StringInterner interner_;
    std::unordered_set<il::support::Symbol> breaks_;

    struct SrcLineBP
    {
        std::string normFile;
        std::string base;
        uint32_t line = 0;
    };

    const il::support::SourceManager *sm_ = nullptr;
    DebugClassLayoutTable classLayouts_; ///< Class-layout sidecar (ADR 0138).
    std::vector<SrcLineBP> srcLineBPs_;
    std::unordered_map<uint32_t, std::vector<size_t>> srcLineBPsByLine_; ///< Line -> indices into srcLineBPs_
    mutable std::optional<std::pair<uint32_t, uint32_t>> lastHitSrc_;

    // -------------------------------------------------------------------------
    // Internal ID-based watch representation
    // -------------------------------------------------------------------------
    // Variable watches use a two-level structure:
    //   1. symbolToWatchId_: O(1) lookup from interned Symbol to watch ID
    //   2. watchEntries_: indexed by watch ID for direct access
    // This avoids re-interning strings and map lookups in the hot path.
    //
    // Memory watches use sorted ranges for O(log n) intersection queries when
    // many watches are active. The memWatchesSorted_ flag tracks whether the
    // vector needs re-sorting after modifications.
    // -------------------------------------------------------------------------

    struct WatchEntry
    {
        il::core::Type::Kind type = il::core::Type::Kind::Void;
        int64_t i64 = 0;
        double f64 = 0.0;
        bool hasValue = false;
        il::support::Symbol sym{}; ///< Back-reference to symbol for diagnostics.
    };

    std::unordered_map<il::support::Symbol, uint32_t> symbolToWatchId_;
    std::vector<WatchEntry> watchEntries_; ///< Index 0 unused; IDs start at 1.

    struct MemWatchRange
    {
        std::uintptr_t start = 0;   ///< Start address as integer for comparison.
        std::uintptr_t end = 0;     ///< End address (exclusive) for comparison.
        const void *addr = nullptr; ///< Original pointer for user reporting.
        std::size_t size = 0;       ///< Original size for user reporting.
        std::string tag;            ///< User-provided tag.
        uint32_t id = 0;            ///< Internal ID for fast reference.
    };

    std::vector<MemWatchRange> memWatches_;
    std::vector<MemWatchHit> memEvents_;
    uint32_t nextMemWatchId_ = 1;  ///< Next ID to assign.
    bool memWatchesSorted_ = true; ///< True when memWatches_ is sorted by start.
};

/// @brief Operation requested by a parsed debugger automation command.
enum class DebugActionKind
{
    Continue, ///< Resume without an instruction-count target.
    Step,     ///< Execute a requested number of instructions.
    StepOver, ///< Continue to a new line at the current or shallower depth.
    StepOut,  ///< Continue until the current call depth returns.
};

/// @brief One queued debugger operation and its optional repetition count.
struct DebugAction
{
    DebugActionKind kind = DebugActionKind::Continue; ///< Requested operation.
    uint64_t count = 0; ///< Instruction count used by `Step`; zero for other actions.
};

/**
 * @brief Owns a FIFO queue of debugger automation actions.
 *
 * Script files recognize `continue`, `step`, `step N`, `step-over`, and
 * `step-out` after trimming surrounding whitespace. File-open failures and
 * unrecognized or malformed lines are reported to stderr and leave parsing
 * available for subsequent lines.
 */
class DebugScript
{
  public:
    /// @brief Construct an empty action queue.
    DebugScript() = default;

    /**
     * @brief Parse debugger actions from a text file.
     * @param path Borrowed filesystem path opened for line-oriented reading.
     *
     * Actions are queued in file order. Failure to open the file produces an
     * empty script after writing a diagnostic to stderr.
     */
    explicit DebugScript(const std::string &path);

    /**
     * @brief Query whether all queued actions have been consumed.
     * @return `true` when the queue is empty.
     */
    bool empty() const;

    /**
     * @brief Append a step operation to the queue tail.
     * @param count Instruction count stored verbatim, including zero.
     */
    void addStep(uint64_t count);

    /**
     * @brief Consume the next queued action.
     * @return The FIFO head, or `{Continue, 0}` without modifying an empty
     *         queue.
     */
    DebugAction nextAction();

  private:
    std::queue<DebugAction> actions;
};

} // namespace il::vm
