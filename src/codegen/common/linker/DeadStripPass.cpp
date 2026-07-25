//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/DeadStripPass.cpp
// Purpose: Mark-and-sweep dead section stripping for the native linker.
//          Follows relocations to transitively mark live sections, then
//          clears data from unreachable sections so they occupy zero bytes.
// Key invariants:
//   - Entry point, synthetic helpers, TLS, and init/runtime metadata are roots
//   - Archive-extracted sections are live only if reachable
//   - ObjC metadata, TLS, and init/fini sections are always roots
//   - Liveness propagates through relocation symbol references
// Links: codegen/common/linker/DeadStripPass.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file DeadStripPass.cpp
 * @brief Implements root discovery, liveness propagation, and dead-section sweeping.
 */

#include "codegen/common/linker/DeadStripPass.hpp"

#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/NameMangling.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"

#include <algorithm>
#include <limits>
#include <queue>
#include <unordered_set>

namespace zanna::codegen::linker {

/// @brief Tests whether a section name begins with a literal prefix.
/// @param s Section name to inspect.
/// @param prefix NUL-terminated prefix to match.
/// @return `true` when @p prefix occurs at offset zero.
static bool hasPrefix(const std::string &s, const char *prefix) {
    return s.rfind(prefix, 0) == 0;
}

/// @brief Checks whether platform runtime discovery makes a section an implicit root.
/// @param name Input section name.
/// @return `true` for Objective-C metadata, initialization/finalization arrays,
///         unwind metadata, or COFF CRT contribution sections.
static bool isAlwaysLiveSection(const std::string &name) {
    // ObjC metadata — runtime discovers classes, selectors, protocols by section name.
    if (name.find("__objc_") != std::string::npos)
        return true;
    if (name.find("objc_") != std::string::npos)
        return true;

    // Mach-O init/fini function pointer sections.
    if (name.find("__mod_init_func") != std::string::npos)
        return true;
    if (name.find("__mod_term_func") != std::string::npos)
        return true;

    // ELF init/fini arrays, including priority-suffixed forms such as
    // .init_array.101 and .preinit_array.
    if (hasPrefix(name, ".init_array") || hasPrefix(name, ".fini_array") ||
        hasPrefix(name, ".preinit_array"))
        return true;
    if (hasPrefix(name, ".ctors") || hasPrefix(name, ".dtors"))
        return true;
    if (name == ".init" || name == ".fini")
        return true;

    // Unwind and exception metadata are discovered by platform runtimes.
    if (name == ".eh_frame" || name == ".gcc_except_table" ||
        name.find("__compact_unwind") != std::string::npos ||
        name.find("__eh_frame") != std::string::npos)
        return true;

    // COFF CRT init/term tables. Keep every .CRT$* contribution alive so
    // sentinel ranges like __xi_a..__xi_z and __xc_a..__xc_z still bracket
    // the actual initializer callbacks after archive extraction.
    if (name.rfind(".CRT$", 0) == 0)
        return true;

    return false;
}

/// @brief Tests for a Windows unwind metadata section.
/// @param name COFF input section name.
/// @return `true` for `.pdata*` or `.xdata*` sections.
static bool isWindowsUnwindSection(const std::string &name) {
    return name.rfind(".pdata", 0) == 0 || name.rfind(".xdata", 0) == 0;
}

/// @brief Tests whether a non-allocating section carries recognized debug data.
/// @param sec Input section to classify.
/// @return `true` for ELF/DWARF or Mach-O debug section naming conventions.
static bool isDebugSection(const ObjSection &sec) {
    if (sec.alloc)
        return false;
    return hasPrefix(sec.name, ".debug") || sec.name.find("__DWARF") != std::string::npos ||
           sec.name.find("__debug") != std::string::npos;
}

/// @brief Caches non-relocation liveness edges for one input object.
struct ObjectLivenessIndex {
    std::vector<std::vector<size_t>> associativeChildren;
    std::vector<std::vector<size_t>> coffUnwindByCodeSection;
};

/// @brief Adds a valid directed section-liveness edge.
/// @param edges Per-source-section adjacency lists.
/// @param fromSec One-based source section index.
/// @param toSec One-based destination section index.
static void addSectionEdge(std::vector<std::vector<size_t>> &edges, size_t fromSec, size_t toSec) {
    if (fromSec == 0 || fromSec >= edges.size() || toSec == 0)
        return;
    edges[fromSec].push_back(toSec);
}

/// @brief Builds associative-COMDAT and reverse Windows-unwind liveness indices.
/// @param allObjects Input objects to scan.
/// @param globalSyms Global definitions used to resolve named unwind targets.
/// @param platform Target platform controlling symbol-name fallback.
/// @return One cached liveness index per input object.
static std::vector<ObjectLivenessIndex> buildObjectLivenessIndexes(
    const std::vector<ObjFile> &allObjects,
    const std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
    LinkPlatform platform) {
    std::vector<ObjectLivenessIndex> indexes(allObjects.size());

    for (size_t oi = 0; oi < allObjects.size(); ++oi) {
        const auto &obj = allObjects[oi];
        auto &index = indexes[oi];
        index.associativeChildren.resize(obj.sections.size());

        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const uint32_t parent = obj.sections[si].associativeSection;
            if (parent > 0 && parent < obj.sections.size())
                index.associativeChildren[parent].push_back(si);
        }

        if (obj.format != ObjFileFormat::COFF)
            continue;

        // Windows x64 unwind tables are reverse-referenced: .pdata points at
        // code labels, and .xdata is then pulled in from .pdata. Build that
        // reverse map once per object instead of re-scanning every unwind
        // section for every live code section.
        index.coffUnwindByCodeSection.resize(obj.sections.size());
        for (size_t unwindSi = 1; unwindSi < obj.sections.size(); ++unwindSi) {
            const auto &unwind = obj.sections[unwindSi];
            if (!isWindowsUnwindSection(unwind.name))
                continue;

            for (const auto &rel : unwind.relocs) {
                if (rel.symIndex >= obj.symbols.size())
                    continue;
                const auto &targetSym = obj.symbols[rel.symIndex];
                if (targetSym.sectionIndex > 0 && targetSym.sectionIndex < obj.sections.size()) {
                    addSectionEdge(index.coffUnwindByCodeSection, targetSym.sectionIndex, unwindSi);
                }
                if (!targetSym.name.empty()) {
                    auto git = findWithPlatformFallback(globalSyms, targetSym.name, platform);
                    if (git != globalSyms.end() && git->second.objIndex == oi &&
                        git->second.secIndex > 0 && git->second.secIndex < obj.sections.size()) {
                        addSectionEdge(
                            index.coffUnwindByCodeSection, git->second.secIndex, unwindSi);
                    }
                }
            }
        }
        for (auto &targets : index.coffUnwindByCodeSection) {
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        }
    }

    return indexes;
}

/// @copydoc deadStrip(std::vector<ObjFile> &, size_t, const std::unordered_map<std::string, GlobalSymEntry> &, const std::string &, LinkPlatform, bool, std::ostream &)
void deadStrip(std::vector<ObjFile> &allObjects,
               size_t userObjCount,
               const std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
               const std::string &entrySymbol,
               LinkPlatform platform,
               bool preserveDebugSections,
               std::ostream &err) {
    // Set of live (objIdx, secIdx) pairs.
    std::unordered_set<InputSectionKey, InputSectionKeyHash> live;
    std::queue<InputSectionKey> worklist;
    const auto livenessIndexes = buildObjectLivenessIndexes(allObjects, globalSyms, platform);

    /// Marks a section once and schedules it for transitive edge traversal.
    auto markLive = [&](size_t objIdx, size_t secIdx) {
        InputSectionKey key{objIdx, secIdx};
        if (live.insert(key).second)
            worklist.push(key);
    };

    // Phase 1: Identify roots.

    // Stub objects generated by the linker are always live.
    for (size_t oi = userObjCount; oi < allObjects.size(); ++oi) {
        const auto &obj = allObjects[oi];
        if (obj.synthetic) {
            for (size_t si = 1; si < obj.sections.size(); ++si)
                markLive(oi, si);
        }
    }

    // Entry point section is a root.
    {
        auto it = findWithPlatformFallback(globalSyms, entrySymbol, platform);
        if (it != globalSyms.end() && it->second.secIndex > 0)
            markLive(it->second.objIndex, it->second.secIndex);
    }

    // Always-live sections (ObjC, TLS, init/fini) across all input objects, plus
    // sections explicitly retained via SHF_GNU_RETAIN / S_ATTR_NO_DEAD_STRIP and
    // sections owning an N_NO_DEAD_STRIP (__attribute__((used))) symbol.
    for (size_t oi = 0; oi < allObjects.size(); ++oi) {
        const auto &obj = allObjects[oi];
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const auto &sec = obj.sections[si];
            if (sec.tls || sec.noDeadStrip || (preserveDebugSections && isDebugSection(sec)) ||
                isAlwaysLiveSection(sec.name))
                markLive(oi, si);
        }
        for (const auto &sym : obj.symbols) {
            if (sym.noDeadStrip && sym.sectionIndex > 0 && sym.sectionIndex < obj.sections.size())
                markLive(oi, sym.sectionIndex);
        }
        if (platform == LinkPlatform::Windows && obj.format == ObjFileFormat::COFF) {
            for (const auto &sym : obj.symbols) {
                if (sym.sectionIndex > 0 && sym.sectionIndex < obj.sections.size() &&
                    isMsvcThreadSafeStaticGuardSymbol(sym.name)) {
                    markLive(oi, sym.sectionIndex);
                }
            }
        }
    }

    // Phase 2: Mark — follow relocations transitively.
    // Build a quick lookup: symbol name → (objIdx, secIdx) for defined symbols.
    // We reuse globalSyms for global/weak symbols and handle locals per-object.

    while (!worklist.empty()) {
        InputSectionKey key = worklist.front();
        worklist.pop();

        size_t oi = key.objIndex;
        size_t si = key.secIndex;

        if (oi >= allObjects.size())
            continue;
        const auto &obj = allObjects[oi];
        if (si >= obj.sections.size())
            continue;
        const auto &sec = obj.sections[si];

        if (sec.associativeSection > 0 && sec.associativeSection < obj.sections.size())
            markLive(oi, sec.associativeSection);
        if (oi < livenessIndexes.size() && si < livenessIndexes[oi].associativeChildren.size()) {
            for (size_t childSi : livenessIndexes[oi].associativeChildren[si])
                markLive(oi, childSi);
        }

        if (obj.format == ObjFileFormat::COFF && sec.executable && oi < livenessIndexes.size() &&
            si < livenessIndexes[oi].coffUnwindByCodeSection.size()) {
            for (size_t unwindSi : livenessIndexes[oi].coffUnwindByCodeSection[si])
                markLive(oi, unwindSi);
        }

        // Follow each relocation to its target symbol's section.
        for (const auto &rel : sec.relocs) {
            if (rel.symIndex == 0 || rel.symIndex >= obj.symbols.size())
                continue;

            const auto &sym = obj.symbols[rel.symIndex];

            // Global/weak symbols: look up in global table.
            bool followedGlobal = false;
            if (!sym.name.empty()) {
                auto git = findWithPlatformFallback(globalSyms, sym.name, platform);
                if (git != globalSyms.end() && git->second.secIndex > 0) {
                    markLive(git->second.objIndex, git->second.secIndex);
                    followedGlobal = sym.binding != ObjSymbol::Local;
                }

                if (sym.weakExternal && !sym.weakDefaultName.empty()) {
                    auto fallback =
                        findWithPlatformFallback(globalSyms, sym.weakDefaultName, platform);
                    if (fallback != globalSyms.end() && fallback->second.secIndex > 0)
                        markLive(fallback->second.objIndex, fallback->second.secIndex);
                }
            }

            // Local symbols resolve within the same object. For global/weak
            // symbols with a same-object definition, prefer the global table so
            // strong definitions in other objects own liveness.
            if (!followedGlobal && sym.sectionIndex > 0 && sym.sectionIndex < obj.sections.size())
                markLive(oi, sym.sectionIndex);
        }
    }

    // Phase 3: Sweep — clear data from dead sections.
    size_t strippedSections = 0;
    size_t strippedBytes = 0;
    for (size_t oi = 0; oi < allObjects.size(); ++oi) {
        auto &obj = allObjects[oi];
        // Skip synthetic objects (already marked live).
        if (obj.synthetic)
            continue;

        for (size_t si = 1; si < obj.sections.size(); ++si) {
            InputSectionKey key{oi, si};
            if (live.count(key))
                continue;

            auto &sec = obj.sections[si];
            const size_t deadSize = objSectionMemSize(sec);
            if (deadSize == 0 && sec.relocs.empty())
                continue;

            if (deadSize > std::numeric_limits<size_t>::max() - strippedBytes)
                strippedBytes = std::numeric_limits<size_t>::max();
            else
                strippedBytes += deadSize;
            ++strippedSections;
            sec.stripped = true;
            sec.data.clear();
            sec.relocs.clear();
            sec.memSize = 0;
            sec.zeroFill = false;
        }
    }

    if (strippedSections > 0)
        err << "dead-strip: removed " << strippedSections << " sections (" << strippedBytes
            << " bytes)\n";
}

} // namespace zanna::codegen::linker
