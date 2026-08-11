//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/StringDedup.cpp
// Purpose: Implements cross-module string deduplication for the native linker.
//          Operates between deadStrip() and mergeSections() in the link pipeline.
// Key invariants:
//   - Only scans loadable C-string sections with symbols at exact string starts
//   - Non-string rodata (floats, alignment padding) is skipped
//   - All duplicates share one collision-free synthetic global name
//   - Canonical copy is first occurrence; others become aliases
// Links: codegen/common/linker/StringDedup.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file StringDedup.cpp
 * @brief Implements safe cross-object deduplication of C string literals.
 *
 * The pass discovers symbolized, NUL-terminated strings only in sections
 * explicitly identified as C-string storage. Duplicate groups are redirected
 * through collision-free synthetic globals, after which fully understood
 * sections may be compacted without invalidating relocations or unrelated
 * symbols.
 */

#include "codegen/common/linker/StringDedup.hpp"

#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

/// @copydoc zanna::codegen::linker::deduplicateStrings
size_t deduplicateStrings(std::vector<ObjFile> &allObjects,
                          std::unordered_map<std::string, GlobalSymEntry> &globalSyms) {
    /// Location and compaction state for one candidate string symbol.
    struct SymLoc {
        ///< Index of the object containing the string.
        size_t objIdx = 0;
        ///< Index of the string's symbol-table entry.
        size_t symIdx = 0;
        ///< One-based index of the containing input section.
        uint32_t secIdx = 0;
        ///< Byte offset before any section compaction.
        size_t originalOffset = 0;
        ///< String byte count including its NUL terminator.
        size_t strLen = 0;
        ///< Whether this occurrence remains in the compacted section.
        bool keepBytes = true;
    };

    std::unordered_set<std::string> usedSymbolNames;
    usedSymbolNames.reserve(globalSyms.size());
    for (const auto &entry : globalSyms)
        usedSymbolNames.insert(entry.first);
    for (const auto &obj : allObjects) {
        for (const auto &sym : obj.symbols) {
            if (!sym.name.empty())
                usedSymbolNames.insert(sym.name);
        }
    }

    /// Generate and reserve the next collision-free synthetic global name.
    auto makeDedupSymbolName = [&](size_t &counter) -> std::string {
        for (;;) {
            std::string name = "__zanna_dedup_str_" + std::to_string(counter++);
            if (usedSymbolNames.insert(name).second)
                return name;
        }
    };

    std::unordered_map<std::string, std::vector<size_t>> contentMap;
    std::unordered_map<InputSectionKey, std::vector<size_t>, InputSectionKeyHash> sectionMap;
    std::unordered_set<InputSectionKey, InputSectionKeyHash> sectionsWithIncomingRelocs;
    std::vector<SymLoc> locations;

    for (size_t oi = 0; oi < allObjects.size(); ++oi) {
        const auto &obj = allObjects[oi];
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const auto &sec = obj.sections[si];
            for (const auto &rel : sec.relocs) {
                if (rel.symIndex >= obj.symbols.size())
                    continue;
                const auto &target = obj.symbols[rel.symIndex];
                if (target.sectionIndex == 0 || target.sectionIndex >= obj.sections.size())
                    continue;
                if (target.sectionIndex != si)
                    sectionsWithIncomingRelocs.insert(InputSectionKey{oi, target.sectionIndex});
            }
        }
    }

    for (size_t oi = 0; oi < allObjects.size(); ++oi) {
        auto &obj = allObjects[oi];
        for (size_t si = 1; si < obj.symbols.size(); ++si) {
            const auto &sym = obj.symbols[si];

            // Only LOCAL symbols with a valid section reference.
            if (sym.binding != ObjSymbol::Local)
                continue;
            // A nameless local symbol is the section symbol: it denotes the
            // whole section rather than the string that happens to start it.
            // References through it carry the target's offset in the addend —
            // which is how a compiler addresses every string in a mergeable
            // section — so merging it away by the content at offset zero
            // silently redirects every one of those references into another
            // object's copy of that first string.
            if (sym.name.empty())
                continue;
            if (sym.sectionIndex == 0 || sym.sectionIndex >= obj.sections.size())
                continue;

            const auto &sec = obj.sections[sym.sectionIndex];

            // Only sections that are known to contain NUL-terminated C strings.
            // Scanning generic rodata/const sections (e.g., Mach-O __const) is
            // unsafe: binary data like integer arrays may start with a byte
            // followed by NUL, which would be misidentified as a short string
            // and merged with an unrelated real string, corrupting references.
            if (!sec.alloc || !sec.isCStringSection)
                continue;
            if (sec.data.empty())
                continue;

            // Extract NUL-terminated content starting at sym.offset.
            if (sym.offset >= sec.data.size())
                continue;
            if (sym.offset > 0 && sec.data[sym.offset - 1] != 0)
                continue;

            const uint8_t *start = sec.data.data() + sym.offset;
            size_t maxLen = sec.data.size() - sym.offset;

            // Find NUL terminator within section bounds.
            const void *nulPos = std::memchr(start, '\0', maxLen);
            if (!nulPos)
                continue; // No NUL found — not a string.

            size_t strLen = static_cast<size_t>(static_cast<const uint8_t *>(nulPos) - start) +
                            1; // Include NUL.

            // Skip degenerate "strings" that are just a NUL byte. Non-string data
            // (bitmap fonts, lookup tables) often starts with 0x00 and would be
            // misidentified as an empty string, causing incorrect merging.
            if (strLen <= 1)
                continue;
            if (sym.size != 0 && sym.size != strLen)
                continue;

            // Use the raw bytes (including NUL) as the content key.
            std::string content(reinterpret_cast<const char *>(start), strLen);
            const size_t locIdx = locations.size();
            locations.push_back({oi, si, sym.sectionIndex, sym.offset, strLen, true});
            contentMap[content].push_back(locIdx);
            sectionMap[InputSectionKey{oi, sym.sectionIndex}].push_back(locIdx);
        }
    }

    // Step 2: For each group with 2+ occurrences, promote to shared global symbol.
    size_t dedupCounter = 0;
    size_t eliminated = 0;

    std::vector<const std::string *> contentKeys;
    contentKeys.reserve(contentMap.size());
    for (const auto &entry : contentMap)
        contentKeys.push_back(&entry.first);
    /// Sort content keys by byte value so synthetic naming is deterministic
    /// despite unordered-map iteration order.
    std::sort(contentKeys.begin(), contentKeys.end(), [](const auto *lhs, const auto *rhs) {
        return *lhs < *rhs;
    });

    for (const std::string *content : contentKeys) {
        auto &locs = contentMap[*content];
        if (locs.size() < 2)
            continue;

        // Generate synthetic global name.
        std::string synthName = makeDedupSymbolName(dedupCounter);

        // First occurrence is canonical.
        const auto &canonical = locations[locs[0]];
        auto &canonObj = allObjects[canonical.objIdx];
        auto &canonSym = canonObj.symbols[canonical.symIdx];

        // Register canonical in globalSyms.
        GlobalSymEntry entry;
        entry.name = synthName;
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = canonical.objIdx;
        entry.secIndex = canonSym.sectionIndex;
        entry.offset = canonSym.offset;
        globalSyms[synthName] = entry;

        // Rename all occurrences (including canonical) to the synthetic name.
        for (size_t i = 0; i < locs.size(); ++i) {
            auto &loc = locations[locs[i]];
            auto &sym = allObjects[loc.objIdx].symbols[loc.symIdx];
            sym.name = synthName;
            sym.binding = ObjSymbol::Global;
            if (i != 0) {
                loc.keepBytes = false;
                sym.sectionIndex = 0;
                sym.offset = 0;
            }
        }

        eliminated += locs.size() - 1;
    }

    // Step 3: Compact cstring sections when every byte belongs to a symbolized
    // string. This turns dedup from symbol aliasing into actual size reduction.
    std::vector<InputSectionKey> sectionKeys;
    sectionKeys.reserve(sectionMap.size());
    for (const auto &entry : sectionMap)
        sectionKeys.push_back(entry.first);
    /// Sort candidate sections by object and section index for deterministic
    /// compaction and symbol-table updates.
    std::sort(sectionKeys.begin(),
              sectionKeys.end(),
              [](const InputSectionKey &a, const InputSectionKey &b) {
                  if (a.objIndex != b.objIndex)
                      return a.objIndex < b.objIndex;
                  return a.secIndex < b.secIndex;
              });

    for (const auto &sectionKey : sectionKeys) {
        auto &locIndices = sectionMap[sectionKey];
        if (locIndices.empty())
            continue;

        const SymLoc &firstLoc = locations[locIndices.front()];
        auto &obj = allObjects[firstLoc.objIdx];
        auto &sec = obj.sections[firstLoc.secIdx];
        if (!sec.relocs.empty())
            continue;
        if (sectionsWithIncomingRelocs.count(sectionKey))
            continue;
        bool hasUnsafeSymbol = false;
        std::unordered_set<size_t> coveredSymbolIndices;
        coveredSymbolIndices.reserve(locIndices.size());
        for (size_t locIdx : locIndices)
            coveredSymbolIndices.insert(locations[locIdx].symIdx);
        for (size_t symIdx = 1; symIdx < obj.symbols.size(); ++symIdx) {
            const auto &sym = obj.symbols[symIdx];
            if (sym.sectionIndex != firstLoc.secIdx)
                continue;
            if (coveredSymbolIndices.count(symIdx) == 0) {
                hasUnsafeSymbol = true;
                break;
            }
        }
        if (hasUnsafeSymbol)
            continue;

        std::vector<size_t> sortedByOffset = locIndices;
        /// Restore the physical string order within this input section.
        std::sort(sortedByOffset.begin(), sortedByOffset.end(), [&](size_t aIdx, size_t bIdx) {
            return locations[aIdx].originalOffset < locations[bIdx].originalOffset;
        });

        size_t cursor = 0;
        bool fullyCovered = true;
        for (size_t locIdx : sortedByOffset) {
            const auto &loc = locations[locIdx];
            if (loc.originalOffset != cursor) {
                fullyCovered = false;
                break;
            }
            if (loc.strLen > std::numeric_limits<size_t>::max() - cursor) {
                fullyCovered = false;
                break;
            }
            cursor += loc.strLen;
        }
        if (!fullyCovered || cursor != sec.data.size())
            continue;

        std::vector<uint8_t> newData;
        newData.reserve(sec.data.size());
        for (size_t locIdx : sortedByOffset) {
            auto &loc = locations[locIdx];
            auto &sym = obj.symbols[loc.symIdx];
            if (!loc.keepBytes)
                continue;

            const size_t newOffset = newData.size();
            newData.insert(newData.end(),
                           sec.data.begin() + static_cast<std::ptrdiff_t>(loc.originalOffset),
                           sec.data.begin() +
                               static_cast<std::ptrdiff_t>(loc.originalOffset + loc.strLen));
            sym.sectionIndex = firstLoc.secIdx;
            sym.offset = newOffset;

            auto git = globalSyms.find(sym.name);
            if (git != globalSyms.end() && git->second.objIndex == loc.objIdx &&
                git->second.secIndex == firstLoc.secIdx) {
                git->second.offset = newOffset;
            }
        }
        sec.data = std::move(newData);
        sec.memSize = sec.data.size();
    }

    return eliminated;
}

} // namespace zanna::codegen::linker
