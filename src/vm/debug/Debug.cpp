//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// Implements the debugger control surface that powers breakpoint and watch
// handling in the IL virtual machine.  The translation unit provides utilities
// for normalising paths, tracking per-block and per-source breakpoints, and
// reporting watch state changes to the user.
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Debugger control utilities for the IL virtual machine.
/// @details The helpers in this file manage breakpoint normalisation, interact
///          with the shared string interner, and surface source-level watch
///          notifications for developers.  They borrow VM-owned state such as
///          the source manager to avoid duplicating heavyweight resources.

#include "zanna/vm/debug/Debug.hpp"
#include "il/core/BasicBlock.hpp"
#include "il/core/Instr.hpp"
#include "support/source_location.hpp"
#include "support/source_manager.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>

namespace il::vm {
namespace {
/// @brief Compile-time switch for verbose source-breakpoint diagnostics.
[[maybe_unused]] constexpr bool kDebugBreakpoints = false;

/// @brief Add a byte count to an address without wrapping.
/// @details Returns the exclusive end address for a watched or written range.
///          When the mathematical end would exceed uintptr_t, the value is
///          clamped to the maximum address so interval intersection checks stay
///          monotonic instead of wrapping to the low address range.
/// @param start Inclusive start address.
/// @param size Number of bytes in the range.
/// @return Exclusive end address, clamped on overflow.
std::uintptr_t saturatingRangeEnd(std::uintptr_t start, std::size_t size) noexcept {
    const auto maxAddress = std::numeric_limits<std::uintptr_t>::max();
    if (size > maxAddress - start)
        return maxAddress;
    return start + size;
}
} // namespace

/// @brief Normalise a file-system path so breakpoint comparisons are stable.
///
/// @details Replaces Windows-style separators with forward slashes, feeds the
///          result through `std::filesystem::path::lexically_normal()`, and
///          returns the generic string representation.  Empty inputs collapse to
///          "." so the debugger never returns an empty path, while absolute
///          roots remain intact.
///
/// @param p Path string to normalise; modified in place.
/// @return Canonical path with redundant segments removed.
std::string DebugCtrl::normalizePath(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');

    if (p.empty())
        return ".";

    std::filesystem::path normalized = std::filesystem::path(p).lexically_normal();
    std::string generic = normalized.generic_string();

    if (generic.empty())
        return p.front() == '/' ? std::string{"/"} : std::string{"."};

#ifdef _WIN32
    // Match the source manager's lowercasing on Windows for case-insensitive
    // path comparisons.
    for (char &ch : generic) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
    }
#endif

    return generic;
}

/// @brief Produce both the canonical path and basename for breakpoint matching.
///
/// @details Breakpoints can trigger by either full path or basename.  This helper
///          normalises the supplied path and splits the final segment so both
///          representations stay in sync.  Returning by value allows callers to
///          stash the pair without further allocation.
///
/// @param path Original path supplied by the user; consumed by the function.
/// @return Pair of canonical path and basename strings.
std::pair<std::string, std::string> DebugCtrl::normalizePathWithBase(std::string path) {
    std::string normFile = normalizePath(std::move(path));
    size_t pos = normFile.find_last_of('/');
    std::string base = (pos == std::string::npos) ? normFile : normFile.substr(pos + 1);
    return {std::move(normFile), std::move(base)};
}

/// @brief Intern a block label for breakpoint lookup.
///
/// @details The controller stores breakpoints using interned symbols to avoid
///          repeated allocations during dispatch.  Interning here guarantees the
///          same symbol identity as other call sites using the shared interner.
///
/// @param label Block label text.
/// @return Interned symbol suitable for set membership queries.
il::support::Symbol DebugCtrl::internLabel(std::string_view label) {
    return interner_.intern(label);
}

/// @brief Register a block-level breakpoint.
///
/// @details Inserts the interned symbol into the `breaks_` set.  Duplicate calls
///          are harmless because the underlying container is idempotent.
///
/// @param sym Interned symbol identifying the target block.
void DebugCtrl::addBreak(il::support::Symbol sym) {
    if (sym)
        breaks_.insert(sym);
}

/// @brief Determine whether the currently executing block has a breakpoint.
///
/// @details The block label is interned using the same symbol table as
///          registration so lookups become O(1) hash checks.  This keeps the
///          runtime overhead negligible even when many breakpoints exist.
///
/// @param blk Block currently under execution.
/// @return @c true when a breakpoint has been registered for @p blk.
bool DebugCtrl::shouldBreak(const il::core::BasicBlock &blk) const {
    il::support::Symbol sym = interner_.intern(blk.label);
    return breaks_.contains(sym);
}

/// @brief Register a source-location breakpoint.
///
/// @details Normalises the provided file path and stores both the canonical path
///          and its basename together with the one-based line number.  Matching
///          code compares against both strings so users can specify either form.
///
/// @param file Source file path supplied by the user.
/// @param line One-based line number that should trigger a breakpoint.
void DebugCtrl::addBreakSrcLine(std::string file, uint32_t line) {
    auto [normFile, base] = normalizePathWithBase(std::move(file));
    const size_t idx = srcLineBPs_.size();
    srcLineBPs_.push_back({std::move(normFile), std::move(base), line});
    srcLineBPsByLine_[line].push_back(idx);
}

/// @brief Query whether any source-level breakpoints exist.
///
/// @return @c true when the controller holds at least one source breakpoint.
bool DebugCtrl::hasSrcLineBPs() const {
    return !srcLineBPs_.empty();
}

/// @brief Install the source manager used to resolve file identifiers.
///
/// @details The debugger does not own the source manager; it simply stores a
///          pointer so it can translate @ref il::support::SourceLoc identifiers
///          back into canonical paths when evaluating breakpoints.
///
/// @param sm Source manager responsible for path resolution.
void DebugCtrl::setSourceManager(const il::support::SourceManager *sm) {
    sm_ = sm;
}

/// @brief Access the source manager previously provided to the debugger.
///
/// @return Pointer installed via @ref setSourceManager or @c nullptr when unset.
const il::support::SourceManager *DebugCtrl::getSourceManager() const {
    return sm_;
}

/// @brief Install the class-layout sidecar used to expand user class instances
///        field-by-field at debugger stops (ADR 0138).
///
/// @param layouts Class id -> field layout table produced from the frontend's
///        compile of the debugged module; ownership transfers to the controller.
void DebugCtrl::setClassLayouts(DebugClassLayoutTable layouts) {
    classLayouts_ = std::move(layouts);
}

/// @brief Access the installed class-layout sidecar (empty when none was set).
/// @return Immutable class layout table owned by the debugger controller.
const DebugClassLayoutTable &DebugCtrl::classLayouts() const {
    return classLayouts_;
}

/// @brief Determine whether the given instruction hits a source breakpoint.
///
/// @details The helper resolves the instruction's source file identifier through
///          the installed source manager, normalises the resulting path, and
///          compares both the canonical path and basename against registered
///          breakpoints.  The last-hit cache prevents the debugger from stopping
///          repeatedly on the same line unless execution leaves and re-enters it.
///
/// @param I Instruction currently being executed.
/// @return @c true when a matching breakpoint is found.
bool DebugCtrl::shouldBreakOn(const il::core::Instr &I) const {
    if (!sm_ || srcLineBPs_.empty() || !I.loc.hasFile() || !I.loc.hasLine())
        return false;

    const uint32_t fileId = I.loc.file_id;
    const uint32_t line = I.loc.line;
    if (lastHitSrc_ && lastHitSrc_->first == fileId && lastHitSrc_->second == line)
        return false;

    std::string_view pathView = sm_->getPath(fileId);
    if (pathView.empty()) {
        if constexpr (kDebugBreakpoints) {
            std::cerr << "[DEBUG][DebugCtrl] unresolved file id " << fileId
                      << " while checking breakpoint for line " << line << "\n";
        }
        return false;
    }

    // O(1) lookup by line number, then check file matches for that line only.
    auto lineIt = srcLineBPsByLine_.find(line);
    if (lineIt == srcLineBPsByLine_.end())
        return false;

    auto [normFile, base] = normalizePathWithBase(std::string(pathView));
    for (const size_t bpIdx : lineIt->second) {
        const auto &bp = srcLineBPs_[bpIdx];
        if (normFile == bp.normFile || base == bp.base) {
            lastHitSrc_ = std::make_pair(fileId, line);
            return true;
        }
    }
    return false;
}

/// @brief Register a variable to watch for changes.
///
/// @details Watching a value interns the identifier and allocates an entry in the
///          watch table with a numeric ID.  The ID enables O(1) lookups in the hot
///          path via getWatchId() and onStoreById(), avoiding repeated string
///          interning and map lookups during execution.
///
/// @param name Identifier of the variable to track.
/// @return Watch ID (>= 1) for use with fast-path methods, or 0 on failure.
uint32_t DebugCtrl::addWatch(std::string_view name) {
    il::support::Symbol sym = interner_.intern(name);
    if (!sym)
        return 0;

    // Check if already watched
    auto it = symbolToWatchId_.find(sym);
    if (it != symbolToWatchId_.end())
        return it->second;

    // Allocate new watch entry
    const uint32_t id = static_cast<uint32_t>(watchEntries_.size());
    if (id == 0) {
        // Reserve index 0 as "not watched" sentinel
        watchEntries_.emplace_back();
    }
    watchEntries_.emplace_back();
    watchEntries_.back().sym = sym;
    symbolToWatchId_[sym] = static_cast<uint32_t>(watchEntries_.size() - 1);
    return static_cast<uint32_t>(watchEntries_.size() - 1);
}

/// @brief Fast O(1) lookup to check if a symbol is watched.
///
/// @details This method is designed for use in the hot path.  The caller should
///          pre-intern the variable name once per function/block and reuse the
///          resulting symbol for all stores within that scope.
///
/// @param sym Interned symbol to check.
/// @return Watch ID if watched, or 0 if not watched.
uint32_t DebugCtrl::getWatchId(il::support::Symbol sym) const noexcept {
    auto it = symbolToWatchId_.find(sym);
    return (it != symbolToWatchId_.end()) ? it->second : 0;
}

/// @brief Handle a store to a watched variable and report changes (slow path).
///
/// @details After interning the identifier, the helper delegates to onStoreById().
///          This method is provided for backward compatibility; prefer the fast-path
///          onStoreById() when the watch ID is already known.
///
/// @param name Identifier being stored to.
/// @param ty Type of the stored value.
/// @param i64 Integer payload when @p ty is an integer type.
/// @param f64 Floating payload when @p ty is F64.
/// @param fn Function name containing the store.
/// @param blk Basic block label.
/// @param ip Instruction index within the block.
void DebugCtrl::onStore(std::string_view name,
                        il::core::Type::Kind ty,
                        int64_t i64,
                        double f64,
                        std::string_view fn,
                        std::string_view blk,
                        size_t ip) {
    il::support::Symbol sym = interner_.intern(name);
    const uint32_t watchId = getWatchId(sym);
    if (watchId == 0)
        return;
    onStoreById(watchId, name, ty, i64, f64, fn, blk, ip);
}

/// @brief Handle a store to a watched variable by ID (fast path).
///
/// @details Compares the new payload against the last observed value using direct
///          array indexing.  Unsupported types yield a short diagnostic while
///          numeric and floating-point types trigger an update message when the
///          value changes.  Watches remember the most recent value so subsequent
///          stores can detect differences.
///
/// @param watchId Watch ID from getWatchId() or addWatch().
/// @param name Identifier being stored to (for diagnostics).
/// @param ty Type of the stored value.
/// @param i64 Integer payload when @p ty is an integer type.
/// @param f64 Floating payload when @p ty is F64.
/// @param fn Function name containing the store.
/// @param blk Basic block label.
/// @param ip Instruction index within the block.
void DebugCtrl::onStoreById(uint32_t watchId,
                            std::string_view name,
                            il::core::Type::Kind ty,
                            int64_t i64,
                            double f64,
                            std::string_view fn,
                            std::string_view blk,
                            size_t ip) {
    if (watchId == 0 || watchId >= watchEntries_.size())
        return;

    WatchEntry &w = watchEntries_[watchId];
    if (ty != il::core::Type::Kind::I1 && ty != il::core::Type::Kind::I16 &&
        ty != il::core::Type::Kind::I32 && ty != il::core::Type::Kind::I64 &&
        ty != il::core::Type::Kind::F64) {
        std::cerr << "[WATCH] " << name << "=[unsupported]  (fn=@" << fn << " blk=" << blk
                  << " ip=#" << ip << ")\n";
        return;
    }
    const bool typeChanged = w.hasValue && w.type != ty;
    bool changed = !w.hasValue || typeChanged;

    /// @brief Test whether a type kind uses an integer watch-value representation.
    /// @param kind IL type kind to inspect.
    /// @return `true` for I1, I16, I32, or I64.
    auto isIntegerKind = [](il::core::Type::Kind kind) {
        return kind == il::core::Type::Kind::I1 || kind == il::core::Type::Kind::I16 ||
               kind == il::core::Type::Kind::I32 || kind == il::core::Type::Kind::I64;
    };

    if (!changed) {
        if (ty == il::core::Type::Kind::F64) {
            if (w.type == il::core::Type::Kind::F64 && w.f64 != f64)
                changed = true;
        } else if (isIntegerKind(ty)) {
            if (isIntegerKind(w.type) && w.i64 != i64)
                changed = true;
        }
    }

    if (changed) {
        std::cerr << "[WATCH] " << name << "=" << il::core::kindToString(ty) << ":";
        if (ty == il::core::Type::Kind::F64)
            std::cerr << f64;
        else
            std::cerr << i64;
        std::cerr << "  (fn=@" << fn << " blk=" << blk << " ip=#" << ip << ")\n";
    }

    if (ty == il::core::Type::Kind::F64) {
        if (typeChanged)
            w.i64 = 0;
        w.f64 = f64;
    } else {
        if (typeChanged)
            w.f64 = 0.0;
        w.i64 = i64;
    }

    w.type = ty;
    w.hasValue = true;
}

/// @brief Forget the last source-line breakpoint location that was triggered.
///
/// @details Clearing the cache allows the debugger to stop again on the same
///          line, for example after the user single-steps past it.
void DebugCtrl::resetLastHit() {
    lastHitSrc_.reset();
}

/// @brief Register a memory watch entry consisting of an address range and tag.
///
/// @details Stores the range with precomputed integer addresses for efficient
///          intersection testing.  The internal vector is marked unsorted so
///          subsequent onMemWrite() calls will re-sort before binary search.
///
/// @param addr Inclusive start address of the watched range.
/// @param size Number of watched bytes; zero is rejected.
/// @param tag Descriptive label copied into any resulting hit event.
/// @return Internal watch ID for the new entry.
uint32_t DebugCtrl::addMemWatch(const void *addr, std::size_t size, std::string tag) {
    if (!addr || size == 0)
        return 0;
    const auto start = reinterpret_cast<std::uintptr_t>(addr);
    const auto end = saturatingRangeEnd(start, size);
    const uint32_t id = nextMemWatchId_++;
    memWatches_.push_back(MemWatchRange{start, end, addr, size, std::move(tag), id});
    memWatchesSorted_ = false;
    return id;
}

/// @brief Remove a memory watch entry matching the triple (addr,size,tag).
///
/// @details Searches for the matching entry and removes it, marking the vector
///          as potentially unsorted if the removal affects ordering.
/// @param addr Start address used when the watch was registered.
/// @param size Byte count used when the watch was registered.
/// @param tag Tag used when the watch was registered.
/// @return @c true when a matching watch was removed.
bool DebugCtrl::removeMemWatch(const void *addr, std::size_t size, std::string_view tag) {
    for (auto it = memWatches_.begin(); it != memWatches_.end(); ++it) {
        if (it->addr == addr && it->size == size && it->tag == tag) {
            memWatches_.erase(it);
            // Removal from sorted vector preserves sorting unless we erased from the middle
            // For simplicity, just mark as potentially unsorted
            memWatchesSorted_ = memWatches_.empty();
            return true;
        }
    }
    return false;
}

/// @brief Query whether any memory ranges are currently watched.
/// @return @c true when at least one memory watch is installed.
bool DebugCtrl::hasMemWatches() const noexcept {
    return !memWatches_.empty();
}

/// @brief Query whether any named variables are currently watched.
/// @return @c true when at least one variable watch is installed.
bool DebugCtrl::hasVarWatches() const noexcept {
    return !symbolToWatchId_.empty();
}

/// @brief Check memory write against installed ranges and enqueue hits.
///
/// @details For small watch sets (< 8), uses linear scan.  For larger sets,
///          sorts ranges by start address and uses binary search to find the
///          first potentially intersecting range, then scans forward.  This
///          gives O(log n + k) complexity where k is the number of intersections.
/// @param addr Inclusive start address of the completed write.
/// @param size Number of bytes written; zero produces no event.
void DebugCtrl::onMemWrite(const void *addr, std::size_t size) {
    if (memWatches_.empty() || !addr || size == 0)
        return;

    const auto writeStart = reinterpret_cast<std::uintptr_t>(addr);
    const auto writeEnd = saturatingRangeEnd(writeStart, size); // exclusive

    // For small watch sets, linear scan is faster than sort + binary search
    constexpr std::size_t kLinearThreshold = 8;
    if (memWatches_.size() < kLinearThreshold) {
        for (const auto &w : memWatches_) {
            const bool intersects = !(writeEnd <= w.start || w.end <= writeStart);
            if (intersects)
                memEvents_.push_back(MemWatchHit{addr, size, w.tag});
        }
        return;
    }

    // Sort by start address if needed
    if (!memWatchesSorted_) {
        /// @brief Order watched memory ranges by their starting address.
        /// @param a Left-hand range.
        /// @param b Right-hand range.
        /// @return `true` when `a` begins before `b`.
        std::sort(memWatches_.begin(),
                  memWatches_.end(),
                  [](const MemWatchRange &a, const MemWatchRange &b) { return a.start < b.start; });
        memWatchesSorted_ = true;
    }

    // Binary search for first range that could intersect: range.end > writeStart
    // A range intersects if: writeEnd > range.start AND range.end > writeStart
    /// @brief Test whether a watched range ends before a candidate write address.
    /// @param r Watched range.
    /// @param val Candidate starting address.
    /// @return `true` when `r` cannot intersect a write beginning at `val`.
    auto it =
        std::lower_bound(memWatches_.begin(),
                         memWatches_.end(),
                         writeStart,
                         [](const MemWatchRange &r, std::uintptr_t val) { return r.end <= val; });

    // Scan forward while ranges could still intersect
    for (; it != memWatches_.end() && it->start < writeEnd; ++it) {
        const bool intersects = !(writeEnd <= it->start || it->end <= writeStart);
        if (intersects)
            memEvents_.push_back(MemWatchHit{addr, size, it->tag});
    }
}

/// @brief Drain pending memory watch hit events for external consumption.
/// @return All queued hits in observation order; the controller queue is emptied.
std::vector<MemWatchHit> DebugCtrl::drainMemWatchEvents() {
    std::vector<MemWatchHit> out;
    out.swap(memEvents_);
    return out;
}

} // namespace il::vm
