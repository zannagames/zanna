//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ra/IntervalAssign.hpp
// Purpose: Backend-independent core of the function-wide register
//          allocators: the position/range model (sorted range lists with
//          holes), the whole-interval linear scan that gives each interval a
//          register or spills it (hints, callee-saved preference across
//          calls, weight-based eviction, fixed physical ranges), and the
//          first-fit sharing of spill slots among non-intersecting intervals.
// Key invariants:
//   - A RangeList is sorted, disjoint, and merged; two values may share a
//     register or a slot iff their range lists do not intersect.
//   - Registers are plain ordinals; the backend supplies the per-class
//     preference orders, the callee-saved flags, and the fixed ranges, and
//     maps the results back to its PhysReg type.
//   - Deterministic: intervals are processed in (start, id) order, candidate
//     registers in the supplied order, ties broken by id.
// Ownership/Lifetime:
//   - Header-only. The assigner borrows the interval vector for its lifetime.
// Links: src/codegen/aarch64/ra/GlobalAllocator.hpp,
//        src/codegen/x86_64/ra/GlobalAllocator.hpp,
//        docs/adr/0338-aarch64-function-wide-register-allocation.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief Shared position model and whole-interval assignment for the
///        function-wide register allocators.

namespace zanna::codegen::ra {

/// @brief A position in a function's linear order. Instruction i of a block
///        reads at base + 2i and writes at base + 2i + 1; each block also owns
///        an exit position after its last write.
using Pos = uint32_t;

/// @brief Sentinel for "no position".
inline constexpr Pos kNoPos = std::numeric_limits<Pos>::max();

/// @brief Sentinel for "no register" (spilled, or no hint).
inline constexpr unsigned kNoReg = std::numeric_limits<unsigned>::max();

/// @brief Class tag meaning "every class" for slot sharing.
inline constexpr unsigned kAnyClass = std::numeric_limits<unsigned>::max();

/// @brief One inclusive live range `[start, end]` in positions.
struct LiveRange {
    Pos start{0};
    Pos end{0};
};

/// @brief Sorted, disjoint, merged list of live ranges.
struct RangeList {
    std::vector<LiveRange> ranges;

    /// @brief Add `[start, end]`, merging with overlapping or adjacent ranges.
    void add(Pos start, Pos end) {
        if (end < start)
            std::swap(start, end);
        // First range whose end is not before start - 1 (the first range that
        // may overlap or touch the new one).
        auto it =
            std::lower_bound(ranges.begin(), ranges.end(), start, [](const LiveRange &r, Pos s) {
                return r.end + 1 < s;
            });
        LiveRange merged{start, end};
        auto eraseBegin = it;
        while (it != ranges.end() && it->start <= merged.end + 1) {
            merged.start = std::min(merged.start, it->start);
            merged.end = std::max(merged.end, it->end);
            ++it;
        }
        it = ranges.erase(eraseBegin, it);
        ranges.insert(it, merged);
    }

    /// @brief Union another list in.
    void addAll(const RangeList &other) {
        for (const LiveRange &r : other.ranges)
            add(r.start, r.end);
    }

    /// @brief Whether @p pos lies in some range.
    [[nodiscard]] bool contains(Pos pos) const noexcept {
        auto it =
            std::upper_bound(ranges.begin(), ranges.end(), pos, [](Pos p, const LiveRange &r) {
                return p < r.start;
            });
        if (it == ranges.begin())
            return false;
        --it;
        return it->start <= pos && pos <= it->end;
    }

    /// @brief First position (in @p other's order) that lies in both lists,
    ///        or kNoPos.
    [[nodiscard]] Pos firstIntersection(const RangeList &other) const noexcept {
        // Walk other's ranges (usually the short list) and binary-search this one.
        for (const LiveRange &o : other.ranges) {
            auto it = std::lower_bound(ranges.begin(),
                                       ranges.end(),
                                       o.start,
                                       [](const LiveRange &r, Pos s) { return r.end < s; });
            if (it != ranges.end() && it->start <= o.end)
                return std::max(it->start, o.start);
        }
        return kNoPos;
    }

    /// @brief Whether any position lies in both lists.
    [[nodiscard]] bool intersects(const RangeList &other) const noexcept {
        return firstIntersection(other) != kNoPos;
    }

    /// @brief Whether the list is empty.
    [[nodiscard]] bool empty() const noexcept {
        return ranges.empty();
    }

    /// @brief Smallest position (kNoPos when empty).
    [[nodiscard]] Pos start() const noexcept {
        return ranges.empty() ? kNoPos : ranges.front().start;
    }

    /// @brief Largest position (kNoPos when empty).
    [[nodiscard]] Pos end() const noexcept {
        return ranges.empty() ? kNoPos : ranges.back().end;
    }

    /// @brief Render as `[a,b] [c,d] …` for diagnostics and tests.
    [[nodiscard]] std::string toString() const {
        std::ostringstream os;
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            if (i)
                os << ' ';
            os << '[' << ranges[i].start << ',' << ranges[i].end << ']';
        }
        return os.str();
    }
};

/// @brief What the assigner needs to know about one interval.
struct IntervalInfo {
    uint32_t id{0};                ///< Virtual register id (unique across classes).
    unsigned cls{0};               ///< Register class tag (backend-defined).
    RangeList live;                ///< Positions at which the value is live.
    double weight{0.0};            ///< Spill weight (higher = keep in a register).
    bool crossesCall{false};       ///< Live at the write position of some call.
    bool crossesEhPush{false};     ///< Live across a setjmp-like call: never in a register.
    unsigned hintPhys{kNoReg};     ///< Preferred register ordinal, or kNoReg.
    std::vector<uint32_t> hintIds; ///< Virtual registers this one is copied to/from.
};

/// @brief The backend's register file as ordinals.
struct RegisterFile {
    /// Per class tag: allocatable registers in preference order (caller-saved
    /// first, then callee-saved).
    std::vector<std::vector<unsigned>> orderByClass;
    std::vector<unsigned char> allocatable; ///< Per ordinal.
    std::vector<unsigned char> calleeSaved; ///< Per ordinal.
    std::vector<unsigned> classOf;          ///< Per ordinal.

    /// @brief Number of register ordinals described.
    [[nodiscard]] std::size_t size() const noexcept {
        return allocatable.size();
    }
};

/// @brief Whole-interval linear scan over intervals with holes.
class IntervalAssigner {
  public:
    /// @brief Bind to @p intervals (borrowed), the register file, and the
    ///        fixed occupancy of every register ordinal (size == regs.size()).
    IntervalAssigner(const std::vector<IntervalInfo> &intervals,
                     RegisterFile regs,
                     std::vector<RangeList> fixed)
        : intervals_(intervals), regs_(std::move(regs)), fixed_(std::move(fixed)),
          occupied_(fixed_), assigned_(intervals.size(), kNoReg), assignedTo_(regs_.size()) {
        uint32_t maxId = 0;
        for (const IntervalInfo &iv : intervals_)
            maxId = std::max(maxId, iv.id);
        indexById_.assign(static_cast<std::size_t>(maxId) + 1, SIZE_MAX);
        for (std::size_t i = 0; i < intervals_.size(); ++i)
            indexById_[intervals_[i].id] = i;
    }

    /// @brief Assign every non-empty interval in (start, id) order.
    void run() {
        std::vector<std::size_t> order;
        order.reserve(intervals_.size());
        for (std::size_t i = 0; i < intervals_.size(); ++i) {
            if (!intervals_[i].live.empty())
                order.push_back(i);
        }
        std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
            const Pos sa = intervals_[a].live.start();
            const Pos sb = intervals_[b].live.start();
            if (sa != sb)
                return sa < sb;
            return intervals_[a].id < intervals_[b].id;
        });
        for (std::size_t idx : order)
            assignOne(idx);
    }

    /// @brief Register ordinal of interval @p idx, or kNoReg when spilled.
    [[nodiscard]] unsigned assigned(std::size_t idx) const noexcept {
        return idx < assigned_.size() ? assigned_[idx] : kNoReg;
    }

    /// @brief Index of the interval of virtual register @p id, or SIZE_MAX.
    [[nodiscard]] std::size_t indexOf(uint32_t id) const noexcept {
        return id < indexById_.size() ? indexById_[id] : SIZE_MAX;
    }

    /// @brief Positions at which register ordinal @p reg is occupied (fixed
    ///        ranges plus every assigned interval).
    [[nodiscard]] const RangeList &occupied(unsigned reg) const noexcept {
        return occupied_[reg];
    }

    /// @brief Number of intervals without a register.
    [[nodiscard]] std::size_t spilledCount() const noexcept {
        return spilled_;
    }

    /// @brief Group the spilled intervals of class @p cls (or every class
    ///        with kAnyClass) into shared slots: hottest first, first-fit by
    ///        non-intersection. Each group is a vector of interval indices.
    /// @details An interval live across a setjmp-like call keeps a private
    ///          slot: the longjmp that returns into its reader is an edge no
    ///          CFG models, so its range has a hole over the protected region
    ///          where another value could otherwise reuse the slot and
    ///          overwrite it before the handler reads it.
    [[nodiscard]] std::vector<std::vector<std::size_t>> shareSlots(unsigned cls) const {
        std::vector<std::size_t> spilled;
        std::vector<std::vector<std::size_t>> groups;
        for (std::size_t i = 0; i < intervals_.size(); ++i) {
            if (assigned_[i] != kNoReg || intervals_[i].live.empty() ||
                (cls != kAnyClass && intervals_[i].cls != cls))
                continue;
            if (intervals_[i].crossesEhPush)
                groups.push_back({i});
            else
                spilled.push_back(i);
        }
        std::sort(spilled.begin(), spilled.end(), [&](std::size_t a, std::size_t b) {
            if (intervals_[a].weight != intervals_[b].weight)
                return intervals_[a].weight > intervals_[b].weight;
            return intervals_[a].id < intervals_[b].id;
        });

        struct Slot {
            RangeList occ;
            std::vector<std::size_t> occupants;
        };

        std::vector<Slot> slots;
        for (std::size_t idx : spilled) {
            bool placed = false;
            for (Slot &s : slots) {
                if (!s.occ.intersects(intervals_[idx].live)) {
                    s.occ.addAll(intervals_[idx].live);
                    s.occupants.push_back(idx);
                    placed = true;
                    break;
                }
            }
            if (!placed) {
                Slot s;
                s.occ = intervals_[idx].live;
                s.occupants.push_back(idx);
                slots.push_back(std::move(s));
            }
        }
        groups.reserve(groups.size() + slots.size());
        for (Slot &s : slots)
            groups.push_back(std::move(s.occupants));
        return groups;
    }

  private:
    const std::vector<IntervalInfo> &intervals_;
    RegisterFile regs_;
    std::vector<RangeList> fixed_;
    std::vector<RangeList> occupied_;
    std::vector<unsigned> assigned_;
    std::vector<std::vector<std::size_t>> assignedTo_;
    std::vector<std::size_t> indexById_;
    std::size_t spilled_{0};

    [[nodiscard]] const std::vector<unsigned> &orderFor(unsigned cls) const noexcept {
        static const std::vector<unsigned> kEmpty;
        return cls < regs_.orderByClass.size() ? regs_.orderByClass[cls] : kEmpty;
    }

    /// @brief Give interval @p idx a register, spilling it or evicting lighter
    ///        occupants when every candidate is taken.
    void assignOne(std::size_t idx) {
        const IntervalInfo &iv = intervals_[idx];
        if (iv.crossesEhPush) {
            ++spilled_;
            return; // memory-homed across setjmp (EH-1)
        }

        // Candidate order: hints, then the class pool with callee-saved
        // registers first when the interval is live across a call.
        std::vector<unsigned> candidates;
        candidates.reserve(orderFor(iv.cls).size() + 4);
        const auto push = [&](unsigned r) {
            if (r >= regs_.size() || !regs_.allocatable[r] || regs_.classOf[r] != iv.cls)
                return;
            if (std::find(candidates.begin(), candidates.end(), r) == candidates.end())
                candidates.push_back(r);
        };
        if (iv.hintPhys != kNoReg)
            push(iv.hintPhys);
        for (uint32_t h : iv.hintIds) {
            const std::size_t hi = indexOf(h);
            if (hi != SIZE_MAX && assigned_[hi] != kNoReg)
                push(assigned_[hi]);
        }
        const auto &pool = orderFor(iv.cls);
        if (iv.crossesCall) {
            for (unsigned r : pool)
                if (regs_.calleeSaved[r])
                    push(r);
            for (unsigned r : pool)
                if (!regs_.calleeSaved[r])
                    push(r);
        } else {
            for (unsigned r : pool)
                push(r);
        }

        for (unsigned r : candidates) {
            if (!occupied_[r].intersects(iv.live)) {
                place(idx, r);
                return;
            }
        }

        // Every candidate conflicts: find the register whose conflicting
        // occupants are lightest; a fixed conflict makes a register unusable.
        unsigned best = kNoReg;
        double bestWeight = std::numeric_limits<double>::infinity();
        std::vector<std::size_t> bestConflicts;
        for (unsigned r : pool) {
            if (!regs_.allocatable[r] || regs_.classOf[r] != iv.cls ||
                fixed_[r].intersects(iv.live))
                continue;
            double weight = 0.0;
            std::vector<std::size_t> conflicts;
            for (std::size_t j : assignedTo_[r]) {
                const IntervalInfo &other = intervals_[j];
                if (other.live.end() < iv.live.start() || iv.live.end() < other.live.start())
                    continue;
                if (other.live.intersects(iv.live)) {
                    weight += other.weight;
                    conflicts.push_back(j);
                }
            }
            if (weight < bestWeight) {
                bestWeight = weight;
                best = r;
                bestConflicts = std::move(conflicts);
            }
        }

        if (best == kNoReg || bestWeight >= iv.weight) {
            ++spilled_;
            return;
        }
        for (std::size_t j : bestConflicts)
            unassign(j);
        place(idx, best);
    }

    /// @brief Record the assignment of interval @p idx to @p reg.
    void place(std::size_t idx, unsigned reg) {
        assigned_[idx] = reg;
        occupied_[reg].addAll(intervals_[idx].live);
        assignedTo_[reg].push_back(idx);
    }

    /// @brief Evict interval @p idx (spilled from now on) and rebuild its
    ///        register's occupancy from the remaining occupants.
    void unassign(std::size_t idx) {
        const unsigned reg = assigned_[idx];
        assigned_[idx] = kNoReg;
        ++spilled_;
        auto &list = assignedTo_[reg];
        list.erase(std::remove(list.begin(), list.end(), idx), list.end());
        RangeList occ = fixed_[reg];
        for (std::size_t j : list)
            occ.addAll(intervals_[j].live);
        occupied_[reg] = std::move(occ);
    }
};

} // namespace zanna::codegen::ra
