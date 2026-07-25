//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/ra/ArchTraits.hpp
// Purpose: Defines the ArchTraits concept for register allocator abstractions.
//          Backend-specific traits structs implement this interface to enable
//          shared algorithms (victim selection, spill cost, liveness queries)
//          to work across x86-64 and AArch64 without code duplication.
//
// Key invariants:
//   - Traits are purely static — no runtime polymorphism overhead.
//   - Each backend provides a concrete struct satisfying the required API.
//   - Template instantiation enforces compile-time interface compliance.
//
// Ownership/Lifetime: Header-only, no state.
// Links: codegen/common/ra/VictimSelection.hpp,
//        codegen/common/ra/DataflowLiveness.hpp,
//        plans/audit-08-shared-regalloc.md
//
//===----------------------------------------------------------------------===//

/**
 * @file ArchTraits.hpp
 * @brief Documents shared register-allocation traits and victim heuristics.
 */

#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace zanna::codegen::ra {

/// @brief Documents the interface a backend must implement for shared RA algorithms.
///
/// This is NOT a base class — it's a documentation-only struct showing the
/// required static methods. Backends create their own traits struct with the
/// same signatures. Shared algorithms are templated on the traits type and
/// call these methods statically.
///
/// @code
/// struct AArch64Traits {
///     using RegId = uint16_t;
///     // ... implement all methods below ...
/// };
///
/// auto victim = selectFurthestVictim(activeSet, nextUseMap);
/// @endcode
///
/// Required type aliases:
///   - RegId: Integer type identifying a virtual register (typically uint16_t)
///
/// Required static methods:
///   - getNextUseDistance(RegId, context) -> unsigned
///     Returns the distance (in instruction indices) to the next use of the
///     given vreg. Returns UINT_MAX if no future use exists.
///
struct ArchTraitsDocumentation {
    /// @brief Canonical virtual-register identifier type.
    using RegId = uint16_t;

    // Not instantiated — this struct exists only for documentation.
    /// @brief Prevent construction of this documentation-only interface sketch.
    ArchTraitsDocumentation() = delete;
};

/// @brief Select the active vreg with the furthest next-use distance.
///
/// @details Implements the classic "furthest-first" victim selection heuristic
///          used by both x86-64 and AArch64 register allocators. The algorithm
///          iterates over the active set and picks the vreg whose next use is
///          furthest away, minimizing the impact of the spill.
///
///          This is a pure function — it does not modify the active set or
///          perform the actual spill. The caller is responsible for spilling
///          the selected victim and updating allocator state.
///
/// @tparam GetNextUse Callable: (uint16_t vregId) -> unsigned distance.
///         Returns UINT_MAX for vregs with no future use.
/// @param activeSet The set of currently active vreg IDs to consider.
/// @param getNextUse Callable providing the next-use distance for each vreg.
/// @return The vreg ID with the furthest next use, or the first element if
///         all distances are equal. Returns 0 if the active set is empty.
template <typename GetNextUse>
uint16_t selectFurthestVictim(const std::vector<uint16_t> &activeSet, GetNextUse getNextUse) {
    if (activeSet.empty())
        return 0;

    uint16_t bestVreg = activeSet.front();
    unsigned bestDist = getNextUse(bestVreg);

    for (std::size_t i = 1; i < activeSet.size(); ++i) {
        const unsigned dist = getNextUse(activeSet[i]);
        if (dist > bestDist) {
            bestDist = dist;
            bestVreg = activeSet[i];
        }
    }

    return bestVreg;
}

/// @brief Select the active vreg that was least recently used (LRU).
///
/// @details Complements furthest-first by considering temporal recency.
///          Used by the AArch64 allocator as a tiebreaker or alternative
///          heuristic. Picks the vreg with the smallest "last use" index.
///
/// @tparam GetLastUse Callable: (uint16_t vregId) -> unsigned lastUseIdx.
/// @param activeSet The set of currently active vreg IDs.
/// @param getLastUse Callable providing the last-use position for each vreg.
/// @return The vreg ID that was used least recently, or zero for an empty set.
template <typename GetLastUse>
uint16_t selectLRUVictim(const std::vector<uint16_t> &activeSet, GetLastUse getLastUse) {
    if (activeSet.empty())
        return 0;

    uint16_t bestVreg = activeSet.front();
    unsigned bestTime = getLastUse(bestVreg);

    for (std::size_t i = 1; i < activeSet.size(); ++i) {
        const unsigned time = getLastUse(activeSet[i]);
        if (time < bestTime) {
            bestTime = time;
            bestVreg = activeSet[i];
        }
    }

    return bestVreg;
}

} // namespace zanna::codegen::ra
