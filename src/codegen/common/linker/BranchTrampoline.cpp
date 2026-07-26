//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/BranchTrampoline.cpp
// Purpose: Implements AArch64 branch trampoline insertion.
//          After section merging, scans all Branch26 relocations for out-of-range
//          displacements. For each, appends an ADRP+ADD+BR x16 trampoline to
//          the .text section, applies trampoline relocs inline, and patches
//          the original branch to target the trampoline.
// Key invariants:
//   - Trampoline dedup: one trampoline per unique out-of-range target symbol
//   - Inline reloc application: ADRP+ADD computed directly (no RelocApplier pass)
//   - VA re-assignment after .text extension
// Links: codegen/common/linker/BranchTrampoline.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file BranchTrampoline.cpp
 * @brief Implements deterministic AArch64 branch-island placement and patching.
 */

#include "codegen/common/linker/BranchTrampoline.hpp"

#include "codegen/common/linker/AlignUtil.hpp"
#include "codegen/common/linker/NameMangling.hpp"
#include "codegen/common/linker/RelocClassify.hpp"
#include "codegen/common/linker/SectionMerger.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <ostream>
#include <unordered_map>

namespace zanna::codegen::linker {

namespace {

/// @brief Writes a 32-bit value to an unaligned little-endian byte location.
/// @param p Pointer to at least four writable bytes.
/// @param v Value to encode.
void writeLE32At(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

/// @brief Reads a 32-bit value from an unaligned little-endian byte location.
/// @param p Pointer to at least four readable bytes.
/// @return Decoded host-order value.
uint32_t readLE32At(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

/// @brief Adds two unsigned virtual-address values with overflow checking.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the sum when representable.
/// @return `true` on success; `false` on overflow.
bool checkedAddU64(uint64_t a, uint64_t b, uint64_t &out) {
    if (a > std::numeric_limits<uint64_t>::max() - b)
        return false;
    out = a + b;
    return true;
}

/// @brief Adds two host sizes with overflow checking.
/// @param a Left operand.
/// @param b Right operand.
/// @param out Receives the sum when representable.
/// @return `true` on success; `false` on overflow.
bool checkedAddSize(size_t a, size_t b, size_t &out) {
    if (a > std::numeric_limits<size_t>::max() - b)
        return false;
    out = a + b;
    return true;
}

/// @brief Multiplies two host sizes with overflow checking.
/// @param a Left factor.
/// @param b Right factor.
/// @param out Receives the product when representable.
/// @return `true` on success; `false` on overflow.
bool checkedMulSize(size_t a, size_t b, size_t &out) {
    if (a != 0 && b > std::numeric_limits<size_t>::max() / a)
        return false;
    out = a * b;
    return true;
}

/// @brief Computes a signed virtual-address difference without overflow.
/// @param lhs Address from which @p rhs is subtracted.
/// @param rhs Address to subtract.
/// @param out Receives `lhs - rhs` when it fits in `int64_t`.
/// @return `true` when the signed difference is representable.
bool checkedAddressDelta(uint64_t lhs, uint64_t rhs, int64_t &out) {
    if (lhs >= rhs) {
        const uint64_t delta = lhs - rhs;
        if (delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return false;
        out = static_cast<int64_t>(delta);
        return true;
    }

    const uint64_t delta = rhs - lhs;
    const uint64_t minMag = static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL;
    if (delta > minMag)
        return false;
    out = (delta == minMag) ? std::numeric_limits<int64_t>::min() : -static_cast<int64_t>(delta);
    return true;
}

/// @brief Add a signed relocation addend to an unsigned virtual address.
/// @details Branch targets may be represented as a section VA plus a signed
///          relocation addend. This helper rejects underflow for negative addends
///          and overflow for positive addends before the trampoline range checks
///          consume the computed address.
/// @param base   Unsigned base virtual address.
/// @param addend Signed relocation addend.
/// @param out    Receives the checked sum on success.
/// @return true when the sum is representable as uint64_t.
bool checkedAddI64(uint64_t base, int64_t addend, uint64_t &out) {
    if (addend >= 0)
        return checkedAddU64(base, static_cast<uint64_t>(addend), out);
    const uint64_t magnitude = static_cast<uint64_t>(-(addend + 1)) + 1ULL;
    if (base < magnitude)
        return false;
    out = base - magnitude;
    return true;
}

/// @brief Records one input section's placement inside a merged output section.
struct OutputLocation {
    size_t outSecIdx = 0;
    size_t outputOffset = 0;
    size_t inputSize = 0;
};

/// @brief Build a reverse lookup from input sections to merged output placement.
/// @details Trampoline insertion consults this map repeatedly while resolving
///          local and global branch targets. Duplicate non-synthetic chunks for
///          the same input section are rejected because target resolution would be
///          ambiguous after layout mutation.
using LocationMap = std::unordered_map<InputSectionKey, OutputLocation, InputSectionKeyHash>;

/// @brief Populate the input-section placement map for branch trampoline passes.
/// @param layout Current merged link layout.
/// @param map    Destination map, cleared before population.
/// @param err    Receives diagnostics for duplicate placements.
/// @return false if a non-synthetic input section appears in more than one output chunk.
bool buildLocMap(const LinkLayout &layout, LocationMap &map, std::ostream &err) {
    map.clear();
    for (size_t si = 0; si < layout.sections.size(); ++si) {
        for (const auto &chunk : layout.sections[si].chunks) {
            if (chunk.synthetic)
                continue;
            InputSectionKey key{chunk.inputObjIndex, chunk.inputSecIndex};
            if (map.find(key) != map.end()) {
                err << "error: input section (" << chunk.inputObjIndex << ", "
                    << chunk.inputSecIndex
                    << ") appears in multiple output chunks before trampoline insertion\n";
                return false;
            }
            map[key] = OutputLocation{si, chunk.outputOffset, chunk.size};
        }
    }
    return true;
}

/// @brief Resolves a section-relative or absolute object symbol.
/// @param sym Symbol to resolve.
/// @param objIdx Index of the symbol's owning object.
/// @param locMap Input-to-output section placement map.
/// @param layout Current merged layout.
/// @param addr Receives the resolved virtual address.
/// @return `true` when the symbol is defined, placed, in bounds, and its
///         address is representable.
bool resolveLocalSymbol(const ObjSymbol &sym,
                        size_t objIdx,
                        const LocationMap &locMap,
                        const LinkLayout &layout,
                        uint64_t &addr) {
    if (sym.absolute) {
        addr = static_cast<uint64_t>(sym.offset);
        return true;
    }
    if (sym.sectionIndex == 0)
        return false;
    auto it = locMap.find(InputSectionKey{objIdx, sym.sectionIndex});
    if (it == locMap.end())
        return false;
    const auto &outSec = layout.sections[it->second.outSecIdx];
    if (it->second.outputOffset > outputSectionMemSize(outSec) || sym.offset > it->second.inputSize)
        return false;
    uint64_t withChunk = 0;
    if (!checkedAddU64(
            outSec.virtualAddr, static_cast<uint64_t>(it->second.outputOffset), withChunk) ||
        !checkedAddU64(withChunk, static_cast<uint64_t>(sym.offset), addr))
        return false;
    return true;
}

/// @brief Looks up a resolved global symbol using platform name fallbacks.
/// @param name Object-format symbol name.
/// @param layout Current merged layout.
/// @param platform Target platform controlling fallback mangling.
/// @param addr Receives the resolved virtual address.
/// @return `true` when a usable resolved global entry exists.
bool resolveGlobalSymbol(const std::string &name,
                         const LinkLayout &layout,
                         LinkPlatform platform,
                         uint64_t &addr) {
    auto it = findWithPlatformFallback(layout.globalSyms, name, platform);
    if (it == layout.globalSyms.end())
        return false;
    if (!it->second.resolvedAddrValid && it->second.resolvedAddr == 0)
        return false;
    addr = it->second.resolvedAddr;
    return true;
}

/// @brief Resolves a relocation target through local definition then global lookup.
/// @param sym Relocation target symbol.
/// @param objIdx Index of the symbol's owning object.
/// @param locMap Input-to-output section placement map.
/// @param layout Current merged layout.
/// @param platform Target platform controlling global-name fallback.
/// @param addr Receives the resolved virtual address.
/// @return `true` when either resolution path succeeds.
bool resolveRelocSymbol(const ObjSymbol &sym,
                        size_t objIdx,
                        const LocationMap &locMap,
                        const LinkLayout &layout,
                        LinkPlatform platform,
                        uint64_t &addr) {
    if ((sym.sectionIndex > 0 || sym.absolute) &&
        resolveLocalSymbol(sym, objIdx, locMap, layout, addr))
        return true;
    if (!sym.name.empty() && resolveGlobalSymbol(sym.name, layout, platform, addr))
        return true;
    return false;
}

constexpr size_t kTrampolineSize = 12;
constexpr uint64_t kBranch26MaxForward = static_cast<uint64_t>(0x1FFFFFF) << 2;

/// @brief Tests whether one address can reach another with an AArch64 `imm26` branch.
/// @param from Address of the branch instruction.
/// @param to Target address.
/// @return `true` when the byte displacement is four-byte aligned and lies in
///         the signed 26-bit word-offset range.
bool branch26Reachable(uint64_t from, uint64_t to) {
    int64_t disp = 0;
    if (!checkedAddressDelta(to, from, disp))
        return false;
    if ((disp & 0x3) != 0)
        return false;
    const int64_t imm26 = disp >> 2;
    return imm26 <= 0x1FFFFFF && imm26 >= -0x2000000;
}

/// @brief Collects sorted, unique instruction-aligned text chunk boundaries.
/// @param textSec Executable output section whose chunks define island sites.
/// @param boundaries Destination vector, cleared before collection.
/// @param err Stream that receives an overflow diagnostic.
/// @return `true` on success; `false` if a chunk end cannot be represented.
bool collectChunkBoundaries(const OutputSection &textSec,
                            std::vector<size_t> &boundaries,
                            std::ostream &err) {
    boundaries.clear();
    /// @brief Adds an instruction-aligned candidate boundary to the unsorted result.
    /// @param boundary Candidate text-section byte offset.
    auto addBoundary = [&](size_t boundary) {
        if ((boundary & 0x3u) == 0)
            boundaries.push_back(boundary);
    };
    addBoundary(0);
    for (const auto &chunk : textSec.chunks) {
        size_t end = 0;
        if (!checkedAddSize(chunk.outputOffset, chunk.size, end)) {
            err << "error: text chunk boundary overflows addressable size\n";
            return false;
        }
        addBoundary(chunk.outputOffset);
        addBoundary(end);
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
    return true;
}

/// @brief Chooses the nearest island boundary that remains safely reachable.
/// @details @p reachSlack reserves room for worst-case island growth so a
///          branch selected before insertion stays in range afterward.
/// @param boundaries Sorted candidate offsets in the executable section.
/// @param sourceOffset Branch offset within that section.
/// @param reachSlack Conservative number of growth bytes to reserve.
/// @param chosenBoundary Receives the closest suitable boundary.
/// @return `true` when a boundary satisfies the range and slack constraints.
bool chooseReachableBoundary(const std::vector<size_t> &boundaries,
                             uint64_t sourceOffset,
                             size_t reachSlack,
                             size_t &chosenBoundary) {
    bool found = false;
    uint64_t bestDistance = 0;
    for (size_t boundary : boundaries) {
        const uint64_t from = sourceOffset;
        const uint64_t to = boundary;
        if (!branch26Reachable(from, to))
            continue;
        const uint64_t dist = (from > to) ? (from - to) : (to - from);
        if (reachSlack > kBranch26MaxForward || dist > kBranch26MaxForward - reachSlack)
            continue;
        if (!found || dist < bestDistance) {
            found = true;
            bestDistance = dist;
            chosenBoundary = boundary;
        }
    }
    return found;
}

/// @brief Checks both input and merged symbol tables for a proposed synthetic name.
/// @param objects Input object files.
/// @param layout Current merged layout.
/// @param name Proposed trampoline symbol name.
/// @return `true` if @p name is already in use.
bool trampolineSymbolExists(const std::vector<ObjFile> &objects,
                            const LinkLayout &layout,
                            const std::string &name) {
    if (layout.globalSyms.find(name) != layout.globalSyms.end())
        return true;
    for (const auto &obj : objects) {
        for (const auto &sym : obj.symbols) {
            if (sym.name == name)
                return true;
        }
    }
    return false;
}

/// @brief Generates a collision-free synthetic trampoline symbol name.
/// @param objects Input object files searched for collisions.
/// @param layout Current merged layout searched for collisions.
/// @param counter Monotonic suffix counter, advanced for every attempted name.
/// @return A previously unused `__zanna_trampoline_N` name.
std::string makeTrampolineSymbolName(const std::vector<ObjFile> &objects,
                                     const LinkLayout &layout,
                                     size_t &counter) {
    for (;;) {
        std::string name = "__zanna_trampoline_" + std::to_string(counter++);
        if (!trampolineSymbolExists(objects, layout, name))
            return name;
    }
}

/// @brief Records one out-of-range branch and its selected trampoline.
struct OutOfRangeBranch {
    size_t objIdx = 0;
    size_t secIdx = 0;
    size_t relocIdx = 0;
    uint32_t targetSymIndex = 0;
    int64_t targetAddend = 0;
    std::string targetSymName;
    uint64_t targetAddr = 0;
    size_t islandBoundary = 0;
    std::string trampolineSymName;
};

/// @brief Describes one deduplicated trampoline emitted into a branch island.
struct TrampolineEntry {
    std::string targetSymName;
    size_t targetObjIdx = 0;
    uint32_t targetSymIndex = 0;
    int64_t targetAddend = 0;
    uint64_t targetAddr = 0;
    size_t islandBoundary = 0;
    size_t actualOffset = 0;
    std::string symbolName;
};

} // namespace

/// @copydoc insertBranchTrampolines(std::vector<ObjFile> &, LinkLayout &, LinkArch, LinkPlatform, std::ostream &)
bool insertBranchTrampolines(std::vector<ObjFile> &objects,
                             LinkLayout &layout,
                             LinkArch arch,
                             LinkPlatform platform,
                             std::ostream &err) {
    // Only AArch64 needs trampolines.
    if (arch != LinkArch::AArch64)
        return true;

    // Build location map.
    LocationMap locMap;
    if (!buildLocMap(layout, locMap, err))
        return false;

    // First: resolve all global symbol addresses (same as RelocApplier first pass).
    for (auto &[name, entry] : layout.globalSyms) {
        if (entry.binding == GlobalSymEntry::Undefined || entry.binding == GlobalSymEntry::Dynamic)
            continue;
        if (entry.absolute) {
            entry.resolvedAddr = static_cast<uint64_t>(entry.offset);
            entry.resolvedAddrValid = true;
            continue;
        }
        auto it = locMap.find(InputSectionKey{entry.objIndex, entry.secIndex});
        if (it != locMap.end()) {
            const auto &outSec = layout.sections[it->second.outSecIdx];
            uint64_t withChunk = 0;
            if (!checkedAddU64(outSec.virtualAddr,
                               static_cast<uint64_t>(it->second.outputOffset),
                               withChunk) ||
                !checkedAddU64(
                    withChunk, static_cast<uint64_t>(entry.offset), entry.resolvedAddr)) {
                err << "error: symbol address overflow while resolving '" << name << "'\n";
                return false;
            }
            if (it->second.outputOffset > outputSectionMemSize(outSec) ||
                entry.offset > it->second.inputSize) {
                err << "error: symbol '" << name << "' is outside its input section bounds\n";
                return false;
            }
            entry.resolvedAddrValid = true;
        } else {
            if (entry.resolvedAddrValid || entry.resolvedAddr != 0)
                continue;
            err << "error: defined symbol '" << name
                << "' references an input section that was not placed before trampoline "
                   "insertion\n";
            return false;
        }
    }

    // Scan for out-of-range Branch26 relocations.
    std::vector<OutOfRangeBranch> outOfRange;

    for (size_t oi = 0; oi < objects.size(); ++oi) {
        const auto &obj = objects[oi];
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const auto &sec = obj.sections[si];
            if (sec.relocs.empty() || sec.data.empty())
                continue;

            auto mapIt = locMap.find(InputSectionKey{oi, si});
            if (mapIt == locMap.end())
                continue;

            const size_t outSecIdx = mapIt->second.outSecIdx;
            const size_t chunkBase = mapIt->second.outputOffset;
            const uint64_t secVA = layout.sections[outSecIdx].virtualAddr;

            for (size_t ri = 0; ri < sec.relocs.size(); ++ri) {
                const auto &rel = sec.relocs[ri];
                RelocAction action = classifyReloc(obj.format, arch, rel.type);
                if (action != RelocAction::Branch26)
                    continue;

                // Resolve target symbol address.
                if (rel.symIndex >= obj.symbols.size()) {
                    err << "error: " << obj.name
                        << ": branch relocation references invalid symbol index " << rel.symIndex
                        << "\n";
                    return false;
                }
                const ObjSymbol &targetSym = obj.symbols[rel.symIndex];
                const std::string &symName = targetSym.name;
                uint64_t S = 0;
                if (!resolveRelocSymbol(targetSym, oi, locMap, layout, platform, S))
                    continue;
                const int64_t A = rel.addend;
                uint64_t P = 0;
                uint64_t target = 0;
                if (!checkedAddU64(secVA, static_cast<uint64_t>(chunkBase), P) ||
                    !checkedAddU64(P, static_cast<uint64_t>(rel.offset), P) ||
                    !checkedAddI64(S, A, target)) {
                    err << "error: branch trampoline address overflow for '" << symName << "'\n";
                    return false;
                }
                if (branch26Reachable(P, target))
                    continue; // In range — no trampoline needed.

                OutOfRangeBranch oob;
                oob.objIdx = oi;
                oob.secIdx = si;
                oob.relocIdx = ri;
                oob.targetSymIndex = rel.symIndex;
                oob.targetAddend = A;
                oob.targetSymName =
                    symName.empty()
                        ? ("$local@" + std::to_string(oi) + ":" + std::to_string(rel.symIndex))
                        : symName;
                oob.targetAddr = target;
                outOfRange.push_back(std::move(oob));
            }
        }
    }

    if (outOfRange.empty())
        return true; // Nothing to do.

    // Find the .text output section.
    size_t textSecIdx = SIZE_MAX;
    for (size_t i = 0; i < layout.sections.size(); ++i) {
        if (layout.sections[i].executable) {
            textSecIdx = i;
            break;
        }
    }
    if (textSecIdx == SIZE_MAX) {
        err << "error: no executable section found for trampoline insertion\n";
        return false;
    }

    auto &textSec = layout.sections[textSecIdx];
    std::vector<size_t> chunkBoundaries;
    if (!collectChunkBoundaries(textSec, chunkBoundaries, err))
        return false;
    std::unordered_map<std::string, TrampolineEntry> trampolines;
    size_t trampolineNameCounter = 0;
    size_t reachSlack = 0;
    if (!checkedMulSize(outOfRange.size(), kTrampolineSize, reachSlack)) {
        err << "error: trampoline reach slack overflows addressable size\n";
        return false;
    }

    for (auto &oob : outOfRange) {
        size_t chosenBoundary = 0;
        const auto &rel = objects[oob.objIdx].sections[oob.secIdx].relocs[oob.relocIdx];
        auto mapIt = locMap.find(InputSectionKey{oob.objIdx, oob.secIdx});
        if (mapIt == locMap.end())
            continue;
        size_t sourceOffsetSize = 0;
        if (!checkedAddSize(mapIt->second.outputOffset, rel.offset, sourceOffsetSize)) {
            err << "error: branch relocation source offset overflows for '" << oob.targetSymName
                << "'\n";
            return false;
        }
        const uint64_t sourceOffset = static_cast<uint64_t>(sourceOffsetSize);

        TrampolineEntry *chosenExisting = nullptr;
        uint64_t bestExistingDistance = 0;
        const std::string *chosenKey = nullptr;
        for (auto &[key, trampoline] : trampolines) {
            if (trampoline.targetSymName != oob.targetSymName ||
                trampoline.targetAddend != oob.targetAddend ||
                !branch26Reachable(sourceOffset, trampoline.islandBoundary))
                continue;
            const uint64_t dist = (sourceOffset > trampoline.islandBoundary)
                                      ? (sourceOffset - trampoline.islandBoundary)
                                      : (trampoline.islandBoundary - sourceOffset);
            if (reachSlack > kBranch26MaxForward || dist > kBranch26MaxForward - reachSlack)
                continue;
            // Closest reachable island wins; ties are broken by key so the choice
            // does not depend on unordered_map iteration order (reproducible builds).
            if (chosenExisting == nullptr || dist < bestExistingDistance ||
                (dist == bestExistingDistance && key < *chosenKey)) {
                chosenExisting = &trampoline;
                bestExistingDistance = dist;
                chosenKey = &key;
            }
        }
        if (chosenExisting != nullptr) {
            oob.islandBoundary = chosenExisting->islandBoundary;
            oob.trampolineSymName = chosenExisting->symbolName;
            continue;
        }

        if (!chooseReachableBoundary(chunkBoundaries, sourceOffset, reachSlack, chosenBoundary)) {
            err << "error: no reachable trampoline island boundary for branch to '"
                << oob.targetSymName << "'\n";
            return false;
        }
        oob.islandBoundary = chosenBoundary;

        const std::string trampolineKey = oob.targetSymName + "+" +
                                          std::to_string(oob.targetAddend) + "@" +
                                          std::to_string(oob.islandBoundary);
        auto [it, inserted] = trampolines.emplace(trampolineKey, TrampolineEntry{});
        if (inserted) {
            it->second.targetSymName = oob.targetSymName;
            it->second.targetObjIdx = oob.objIdx;
            it->second.targetSymIndex = oob.targetSymIndex;
            it->second.targetAddend = oob.targetAddend;
            it->second.targetAddr = oob.targetAddr;
            it->second.islandBoundary = oob.islandBoundary;
            it->second.symbolName =
                makeTrampolineSymbolName(objects, layout, trampolineNameCounter);
        }
        oob.trampolineSymName = it->second.symbolName;
    }

    if (trampolines.empty())
        return true;

    std::map<size_t, std::vector<TrampolineEntry *>> islands;
    for (auto &[key, trampoline] : trampolines)
        islands[trampoline.islandBoundary].push_back(&trampoline);

    // The island map is keyed by boundary (ordered), but the entry vectors were
    // populated from unordered_map iteration. Sort each island's entries by a
    // stable key so per-slot offsets — and thus the emitted ADRP/ADD immediates
    // and patched imm26 fields — are reproducible across runs.
    for (auto &island : islands) {
        /// @brief Orders trampoline slots reproducibly by target name and addend.
        /// @param a Left trampoline entry.
        /// @param b Right trampoline entry.
        /// @return `true` when `a` precedes `b`.
        std::sort(island.second.begin(),
                  island.second.end(),
                  [](const TrampolineEntry *a, const TrampolineEntry *b) {
                      if (a->targetSymName != b->targetSymName)
                          return a->targetSymName < b->targetSymName;
                      return a->targetAddend < b->targetAddend;
                  });
    }

    const std::vector<uint8_t> originalText = textSec.data;
    std::vector<uint8_t> newText;
    size_t totalGrowth = 0;
    for (const auto &[boundary, entries] : islands) {
        size_t islandSize = 0;
        if (!checkedMulSize(entries.size(), kTrampolineSize, islandSize) ||
            !checkedAddSize(totalGrowth, islandSize, totalGrowth)) {
            err << "error: trampoline island growth overflows addressable size\n";
            return false;
        }
    }
    size_t reservedSize = 0;
    if (!checkedAddSize(originalText.size(), totalGrowth, reservedSize)) {
        err << "error: trampoline-expanded text size overflows addressable size\n";
        return false;
    }
    newText.reserve(reservedSize);

    /// @brief Records inserted island growth for shifting original text chunks.
    struct IslandPlacement {
        size_t boundary = 0;
        size_t size = 0;
    };

    std::vector<IslandPlacement> placements;
    placements.reserve(islands.size());

    size_t cursor = 0;
    for (const auto &[boundary, entries] : islands) {
        const size_t clampedBoundary = std::min(boundary, originalText.size());
        if (cursor > clampedBoundary) {
            err << "error: trampoline island boundaries are not monotonically ordered\n";
            return false;
        }
        newText.insert(newText.end(),
                       originalText.begin() + static_cast<std::ptrdiff_t>(cursor),
                       originalText.begin() + static_cast<std::ptrdiff_t>(clampedBoundary));
        const size_t islandOffset = newText.size();
        for (size_t i = 0; i < entries.size(); ++i) {
            size_t localOffset = 0;
            if (!checkedMulSize(i, kTrampolineSize, localOffset) ||
                !checkedAddSize(islandOffset, localOffset, entries[i]->actualOffset)) {
                err << "error: trampoline entry offset overflows addressable size\n";
                return false;
            }
            size_t nextSize = 0;
            if (!checkedAddSize(newText.size(), kTrampolineSize, nextSize)) {
                err << "error: trampoline-expanded text size overflows addressable size\n";
                return false;
            }
            newText.resize(nextSize, 0);
        }
        size_t islandSize = 0;
        if (!checkedMulSize(entries.size(), kTrampolineSize, islandSize)) {
            err << "error: trampoline island size overflows addressable size\n";
            return false;
        }
        placements.push_back({boundary, islandSize});
        cursor = clampedBoundary;
    }
    newText.insert(newText.end(),
                   originalText.begin() + static_cast<std::ptrdiff_t>(cursor),
                   originalText.end());
    textSec.data = std::move(newText);
    textSec.memSize = textSec.data.size();

    for (auto &chunk : textSec.chunks) {
        size_t shift = 0;
        for (const auto &placement : placements) {
            if (chunk.outputOffset >= placement.boundary) {
                if (!checkedAddSize(shift, placement.size, shift)) {
                    err << "error: trampoline chunk shift overflows addressable size\n";
                    return false;
                }
            }
        }
        if (!checkedAddSize(chunk.outputOffset, shift, chunk.outputOffset)) {
            err << "error: trampoline chunk shift overflows addressable size\n";
            return false;
        }
    }
    for (const auto &[boundary, entries] : islands) {
        for (const auto *entry : entries) {
            textSec.chunks.push_back(InputChunk{std::numeric_limits<size_t>::max(),
                                                std::numeric_limits<size_t>::max(),
                                                entry->actualOffset,
                                                kTrampolineSize,
                                                true});
        }
    }
    /// @brief Restores ascending output-offset order after synthetic chunk insertion.
    /// @param a Left input chunk.
    /// @param b Right input chunk.
    /// @return `true` when `a` begins before `b`.
    std::stable_sort(textSec.chunks.begin(),
                     textSec.chunks.end(),
                     [](const auto &a, const auto &b) { return a.outputOffset < b.outputOffset; });

    if (!assignSectionVirtualAddresses(layout, platform, err))
        return false;
    if (!buildLocMap(layout, locMap, err))
        return false;

    for (auto &[name, entry] : layout.globalSyms) {
        if (entry.binding == GlobalSymEntry::Undefined || entry.binding == GlobalSymEntry::Dynamic)
            continue;
        if (entry.absolute) {
            entry.resolvedAddr = static_cast<uint64_t>(entry.offset);
            entry.resolvedAddrValid = true;
            continue;
        }
        auto it = locMap.find(InputSectionKey{entry.objIndex, entry.secIndex});
        if (it != locMap.end()) {
            const auto &outSec = layout.sections[it->second.outSecIdx];
            uint64_t withChunk = 0;
            if (!checkedAddU64(outSec.virtualAddr,
                               static_cast<uint64_t>(it->second.outputOffset),
                               withChunk) ||
                !checkedAddU64(
                    withChunk, static_cast<uint64_t>(entry.offset), entry.resolvedAddr)) {
                err << "error: symbol address overflow while resolving '" << name << "'\n";
                return false;
            }
            if (it->second.outputOffset > outputSectionMemSize(outSec) ||
                entry.offset > it->second.inputSize) {
                err << "error: symbol '" << name << "' is outside its input section bounds\n";
                return false;
            }
            entry.resolvedAddrValid = true;
        } else {
            if (entry.resolvedAddrValid || entry.resolvedAddr != 0)
                continue;
            err << "error: defined symbol '" << name
                << "' references an input section that was not placed after trampoline insertion\n";
            return false;
        }
    }

    for (auto &[key, trampoline] : trampolines) {
        if (trampoline.targetObjIdx >= objects.size() ||
            trampoline.targetSymIndex >= objects[trampoline.targetObjIdx].symbols.size()) {
            err << "error: trampoline target symbol index is invalid for '"
                << trampoline.targetSymName << "'\n";
            return false;
        }
        uint64_t resolvedTarget = 0;
        const auto &targetSym = objects[trampoline.targetObjIdx].symbols[trampoline.targetSymIndex];
        if (!resolveRelocSymbol(
                targetSym, trampoline.targetObjIdx, locMap, layout, platform, resolvedTarget) ||
            !checkedAddI64(resolvedTarget, trampoline.targetAddend, trampoline.targetAddr)) {
            err << "error: trampoline target address overflow for '" << trampoline.targetSymName
                << "'\n";
            return false;
        }

        uint64_t tramVA = 0;
        if (!checkedAddU64(
                textSec.virtualAddr, static_cast<uint64_t>(trampoline.actualOffset), tramVA)) {
            err << "error: trampoline address overflow for '" << trampoline.targetSymName << "'\n";
            return false;
        }
        GlobalSymEntry entry;
        entry.name = trampoline.symbolName;
        entry.binding = GlobalSymEntry::Global;
        entry.resolvedAddr = tramVA;
        entry.resolvedAddrValid = true;
        layout.globalSyms[trampoline.symbolName] = std::move(entry);

        uint8_t *tramp = textSec.data.data() + trampoline.actualOffset;
        writeLE32At(tramp + 0, 0x90000010); // ADRP x16
        writeLE32At(tramp + 4, 0x91000210); // ADD x16, x16
        writeLE32At(tramp + 8, 0xD61F0200); // BR x16

        uint32_t adrpInsn = readLE32At(tramp);
        uint64_t pageS = trampoline.targetAddr & ~0xFFFULL;
        uint64_t pageP = tramVA & ~0xFFFULL;
        int64_t pageDelta = 0;
        if (!checkedAddressDelta(pageS, pageP, pageDelta)) {
            err << "error: trampoline ADRP delta out of range for '" << trampoline.targetSymName
                << "'\n";
            return false;
        }
        int64_t immHiLo = pageDelta >> 12;
        if (immHiLo > 0xFFFFF || immHiLo < -0x100000) {
            err << "error: trampoline ADRP out of range for '" << trampoline.targetSymName << "'\n";
            return false;
        }
        uint32_t immlo = static_cast<uint32_t>(immHiLo) & 0x3;
        uint32_t immhi = (static_cast<uint32_t>(immHiLo) >> 2) & 0x7FFFF;
        adrpInsn = (adrpInsn & 0x9F00001F) | (immlo << 29) | (immhi << 5);
        writeLE32At(tramp, adrpInsn);

        uint32_t addInsn = readLE32At(tramp + 4);
        uint32_t pageOff = static_cast<uint32_t>(trampoline.targetAddr) & 0xFFF;
        addInsn = (addInsn & 0xFFC003FF) | (pageOff << 10);
        writeLE32At(tramp + 4, addInsn);
    }

    // Patch original branches to target their trampolines.
    std::unordered_map<std::string, uint32_t> syntheticSymIndexByObjectAndName;
    for (auto &oob : outOfRange) {
        auto mapIt = locMap.find(InputSectionKey{oob.objIdx, oob.secIdx});
        if (mapIt == locMap.end())
            continue;

        const size_t outSecIdx = mapIt->second.outSecIdx;
        const size_t chunkBase = mapIt->second.outputOffset;
        auto &outSec = layout.sections[outSecIdx];

        auto &rel = objects[oob.objIdx].sections[oob.secIdx].relocs[oob.relocIdx];
        size_t patchOff = 0;
        if (!checkedAddSize(chunkBase, rel.offset, patchOff)) {
            err << "error: branch trampoline patch site offset overflow for '" << oob.targetSymName
                << "'\n";
            return false;
        }
        size_t patchEnd = 0;
        if (!checkedAddSize(patchOff, 4u, patchEnd) || patchEnd > outSec.data.size()) {
            // The trampoline island was already inserted upstream and any
            // chunk shifts have been recorded. If the original branch site
            // is now out of range, we cannot redirect it — silently dropping
            // the patch would leave an unreachable branch pointing at the
            // original target, producing a binary that fails at run time
            // with no diagnostic.
            err << "error: " << objects[oob.objIdx].name
                << ": branch trampoline patch site at offset " << patchOff
                << " is out of bounds in output section '" << outSec.name
                << "' (size=" << outSec.data.size() << ")\n";
            return false;
        }

        /// @brief Tests for the deduplicated trampoline selected for this branch record.
        /// @param entry Candidate trampoline map entry.
        /// @return `true` when the entry owns the requested synthetic symbol.
        const auto trampIt =
            std::find_if(trampolines.begin(), trampolines.end(), [&](const auto &entry) {
                return entry.second.symbolName == oob.trampolineSymName;
            });
        if (trampIt == trampolines.end())
            continue;
        const size_t tramOff = trampIt->second.actualOffset;
        uint64_t tramVA = 0;
        uint64_t P = 0;
        if (!checkedAddU64(textSec.virtualAddr, static_cast<uint64_t>(tramOff), tramVA) ||
            !checkedAddU64(outSec.virtualAddr, static_cast<uint64_t>(patchOff), P)) {
            err << "error: trampoline branch address overflow for '" << oob.targetSymName << "'\n";
            return false;
        }
        int64_t disp = 0;
        if (!checkedAddressDelta(tramVA, P, disp)) {
            err << "error: trampoline branch delta out of range for '" << oob.targetSymName
                << "'\n";
            return false;
        }
        if ((disp & 0x3) != 0) {
            err << "error: trampoline at offset " << tramOff
                << " is not instruction-aligned for branch at VA 0x" << std::hex << P << std::dec
                << "\n";
            return false;
        }
        const int64_t imm26 = disp >> 2;

        if (imm26 > 0x1FFFFFF || imm26 < -0x2000000) {
            err << "error: trampoline at offset " << tramOff
                << " is unreachable from branch at VA 0x" << std::hex << P << std::dec
                << " (.text exceeds 128MB; interleaved trampoline islands not yet supported)\n";
            return false;
        }

        // Patch the branch instruction to target the trampoline.
        uint8_t *patch = outSec.data.data() + patchOff;
        uint32_t insn = readLE32At(patch);
        insn = (insn & 0xFC000000) | (static_cast<uint32_t>(imm26) & 0x03FFFFFF);
        writeLE32At(patch, insn);

        auto &obj = objects[oob.objIdx];
        const std::string symbolKey = std::to_string(oob.objIdx) + ":" + oob.trampolineSymName;
        auto symIt = syntheticSymIndexByObjectAndName.find(symbolKey);
        uint32_t trampolineSymIdx = 0;
        if (symIt != syntheticSymIndexByObjectAndName.end()) {
            trampolineSymIdx = symIt->second;
        } else {
            ObjSymbol trampolineSym;
            trampolineSym.name = oob.trampolineSymName;
            trampolineSym.binding = ObjSymbol::Undefined;
            if (obj.symbols.size() >= std::numeric_limits<uint32_t>::max()) {
                err << "error: too many symbols while inserting branch trampoline for '"
                    << oob.targetSymName << "'\n";
                return false;
            }
            obj.symbols.push_back(std::move(trampolineSym));
            trampolineSymIdx = static_cast<uint32_t>(obj.symbols.size() - 1);
            syntheticSymIndexByObjectAndName[symbolKey] = trampolineSymIdx;
        }
        rel.symIndex = trampolineSymIdx;
        rel.addend = 0;
    }

    return true;
}

} // namespace zanna::codegen::linker
