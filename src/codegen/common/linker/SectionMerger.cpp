//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: codegen/common/linker/SectionMerger.cpp
// Purpose: Merges input sections by class and assigns virtual addresses.
// Key invariants:
//   - Order: text → rodata → data → tls_data → bss → tls_bss
//   - Each segment starts on a page boundary
//   - Chunks within a section respect their alignment requirements
// Links: codegen/common/linker/SectionMerger.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file SectionMerger.cpp
 * @brief Implements input-section coalescing and output virtual-address layout.
 *
 * The merger groups live input sections by semantic class, preserves the
 * platform-defined ordering of initialization, TLS, metadata, unwind, and
 * debug subsections, and records every input chunk's output placement for
 * subsequent symbol and relocation resolution. It then orders output sections
 * by memory permissions and assigns page- and section-aligned addresses.
 */

#include "codegen/common/linker/SectionMerger.hpp"

#include "codegen/common/linker/AlignUtil.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <limits>
#include <map>

namespace zanna::codegen::linker {

namespace {

/// @brief Detect Windows CRT initialiser subsections like ".CRT$XCU".
/// @details The MSVC CRT relies on lexicographic sort of these subsections
///          producing the correct C++ static-init / TLS-callback order.
/// @param name COFF section name to inspect.
/// @return `true` when @p name begins with `.CRT$`.
bool isWindowsCrtSubsection(const std::string &name) {
    return name.rfind(".CRT$", 0) == 0;
}

/// @brief Detect any Windows TLS subsection (".tls", ".tls$T", etc.).
/// @param name COFF section name to inspect.
/// @return `true` when @p name begins with `.tls`.
bool isWindowsTlsSubsection(const std::string &name) {
    return name.rfind(".tls", 0) == 0;
}

/// @brief Sort key recovered from an ELF .init_array / .fini_array section name.
/// @details ELF allows priorities like ".init_array.123"; the loader runs lower
///          numbers first. @c family separates preinit/init/fini groups.
struct InitArraySortKey {
    ///< Array family order: pre-initializers, initializers, then finalizers.
    int family = 0;
    ///< Parsed numeric priority, or the maximum value for an invalid name.
    uint32_t priority = std::numeric_limits<uint32_t>::max();
    ///< Whether the section name and optional priority parsed successfully.
    bool isInitArray = false;
};

/// @brief Parse an ELF init/fini-array section name into its sort key.
/// @details Recognises `.preinit_array[.N]`, `.init_array[.N]`, `.fini_array[.N]`.
///          Returns a key with @c family=N and @c priority=N where appropriate;
///          @c isInitArray remains false if the name does not parse so the caller
///          can fall back to default ordering.
/// @param name ELF section name to parse.
/// @return Family, priority, and validity fields used by the stable chunk sort.
InitArraySortKey elfInitArraySortKey(const std::string &name) {
    /// Parse the optional decimal suffix following a recognized array prefix.
    /// An unsuffixed standard name receives the ABI default priority 65535.
    auto parsePriority = [&](const char *prefix) -> uint32_t {
        const size_t prefixLen = std::char_traits<char>::length(prefix);
        if (name.size() == prefixLen)
            return 65535;
        if (name.size() <= prefixLen || name[prefixLen] != '.')
            return std::numeric_limits<uint32_t>::max();
        uint32_t value = 0;
        bool sawDigit = false;
        for (size_t i = prefixLen + 1; i < name.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(name[i])))
                return std::numeric_limits<uint32_t>::max();
            sawDigit = true;
            const uint32_t digit = static_cast<uint32_t>(name[i] - '0');
            if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10)
                return std::numeric_limits<uint32_t>::max();
            value = value * 10 + digit;
        }
        return sawDigit ? value : std::numeric_limits<uint32_t>::max();
    };

    InitArraySortKey key;
    if (name.rfind(".preinit_array", 0) == 0) {
        key.family = 0;
        key.priority = parsePriority(".preinit_array");
        key.isInitArray = key.priority != std::numeric_limits<uint32_t>::max();
    } else if (name.rfind(".init_array", 0) == 0) {
        key.family = 1;
        key.priority = parsePriority(".init_array");
        key.isInitArray = key.priority != std::numeric_limits<uint32_t>::max();
    } else if (name.rfind(".fini_array", 0) == 0) {
        key.family = 2;
        key.priority = parsePriority(".fini_array");
        key.isInitArray = key.priority != std::numeric_limits<uint32_t>::max();
    }
    return key;
}

/// @brief Detect Mach-O sections whose original segment is data-like.
/// @details Some synthetic inputs carry Mach-O section names but not all reader
///          metadata flags. The preserved name is still enough to keep them in
///          the data segment when writing Mach-O executables.
/// @param name Preserved Mach-O `segment,section` name.
/// @return `true` when the original segment is one of the data-like segments.
bool isMachODataSegmentSection(const std::string &name) {
    return name.rfind("__DATA,", 0) == 0 || name.rfind("__DATA_CONST,", 0) == 0 ||
           name.rfind("__DATA_DIRTY,", 0) == 0 || name.rfind("__AUTH,", 0) == 0 ||
           name.rfind("__AUTH_CONST,", 0) == 0;
}

/// @brief Permission-bucket sort key — lower buckets land at lower addresses.
/// @details Order: text (RX) → rodata (R) → data (RW) → tls_data → tls_bss → bss
///          → debug. This both produces sensible W^X segment groupings and
///          mirrors the order that ELF/Mach-O/PE writers expect.
/// @param s Output section whose runtime permissions are classified.
/// @return Integer sort bucket, where lower values receive lower addresses.
int permClass(const OutputSection &s) {
    if (!s.alloc)
        return 6; // Non-alloc sections (debug) sort last.
    if (s.executable)
        return 0;
    if (s.dataSegment && !s.writable && !s.zeroFill && !s.tls)
        return 2;
    if (s.writable && !s.zeroFill && !s.tls)
        return 2;
    if (s.tls && !s.zeroFill)
        return 3;
    if (s.tls && s.zeroFill)
        return 4;
    if (s.writable && s.zeroFill && !s.tls)
        return 5;
    return 1; // readonly
}

/// @brief Tie-breaker among sections that share the same @c permClass.
/// @details Used after permClass to keep e.g. all Text-class sections together
///          before falling back to alphabetical order.
/// @param cls Semantic section class to order.
/// @return Stable integer ordering key for @p cls.
int sectionClassOrder(SectionClass cls) {
    switch (cls) {
        case SectionClass::Text:
            return 0;
        case SectionClass::Rodata:
            return 1;
        case SectionClass::Data:
            return 2;
        case SectionClass::TlsData:
            return 3;
        case SectionClass::TlsBss:
            return 4;
        case SectionClass::Bss:
            return 5;
        case SectionClass::ObjC:
        case SectionClass::Preserved:
            return 6;
        case SectionClass::Other:
            return 7;
    }
    return 7;
}

/// @brief Reclassify a merged output section for final VA ordering.
/// @details Output sections no longer carry their original PendingChunk class,
///          so the final sort derives the same class key from the merged
///          section's name and attributes before using sectionClassOrder().
/// @param section Merged section to reclassify.
/// @return The semantic class inferred from its name and storage attributes.
SectionClass outputSectionClass(const OutputSection &section) {
    return classifySection(
        section.name, section.executable, section.writable, section.tls, section.zeroFill);
}

} // namespace

/// @copydoc zanna::codegen::linker::assignSectionVirtualAddresses
bool assignSectionVirtualAddresses(LinkLayout &layout, LinkPlatform platform, std::ostream &err) {
    if (layout.imageBase == 0)
        layout.imageBase = defaultImageBaseForPlatform(platform);
    const uint64_t imageBase = layout.imageBase;
    if (imageBase > std::numeric_limits<uint64_t>::max() - layout.pageSize) {
        err << "error: image base plus page size exceeds 64-bit address range\n";
        return false;
    }
    uint64_t currentAddr = imageBase + layout.pageSize;
    int prevClass = -1;
    for (auto &sec : layout.sections) {
        if (!sec.alloc)
            continue;
        const int cls = permClass(sec);
        try {
            if (platform == LinkPlatform::Windows) {
                currentAddr = alignUp(currentAddr, layout.pageSize);
                prevClass = cls;
            } else if (cls != prevClass) {
                currentAddr = alignUp(currentAddr, layout.pageSize);
                prevClass = cls;
            }
            currentAddr = alignUp(currentAddr, sec.alignment);
        } catch (const std::exception &ex) {
            err << "error: virtual address alignment failed for '" << sec.name << "': " << ex.what()
                << "\n";
            return false;
        }
        sec.virtualAddr = currentAddr;
        const size_t memSize = outputSectionMemSize(sec);
        if (memSize > std::numeric_limits<uint64_t>::max() - currentAddr) {
            err << "error: section '" << sec.name << "' exceeds 64-bit address range\n";
            return false;
        }
        currentAddr += memSize;
    }
    return true;
}

/// @copydoc zanna::codegen::linker::mergeSections
bool mergeSections(const std::vector<ObjFile> &objects,
                   LinkPlatform platform,
                   LinkArch arch,
                   LinkLayout &layout,
                   std::ostream &err) {
    layout.sections.clear();

    // Determine page size.
    if (platform == LinkPlatform::macOS && arch == LinkArch::AArch64)
        layout.pageSize = 0x4000; // 16KB
    else
        layout.pageSize = 0x1000; // 4KB

    // Collect input sections by class.
    /// Description of one live input section awaiting output coalescing.
    struct PendingChunk {
        ///< Index of the source object in @p objects.
        size_t objIdx = 0;
        ///< One-based section index within the source object.
        size_t secIdx = 0;
        ///< Semantic merge and ordering class.
        SectionClass cls = SectionClass::Other;
        ///< Original section name used for platform-specific ordering.
        std::string name;
        ///< Required chunk alignment in the merged output.
        uint32_t alignment = 1;
        ///< Discovery order used as the final stable-sort tie breaker.
        size_t inputOrder = 0;
    };

    std::vector<PendingChunk> pending;

    /// Determine whether a nominally empty input section still defines a symbol
    /// and therefore must remain present in the merged layout.
    auto sectionHasLiveSymbol = [](const ObjFile &obj, size_t secIdx) {
        for (size_t symIdx = 1; symIdx < obj.symbols.size(); ++symIdx) {
            const auto &sym = obj.symbols[symIdx];
            if (sym.binding != ObjSymbol::Undefined && sym.sectionIndex == secIdx)
                return true;
        }
        return false;
    };

    for (size_t oi = 0; oi < objects.size(); ++oi) {
        const auto &obj = objects[oi];
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const auto &sec = obj.sections[si];
            if (sec.stripped)
                continue;
            if (!sec.alloc)
                continue;
            if (objSectionMemSize(sec) == 0 && sec.relocs.empty() && !sectionHasLiveSymbol(obj, si))
                continue;

            SectionClass cls =
                classifySection(sec.name, sec.executable, sec.writable, sec.tls, sec.zeroFill);
            pending.push_back({oi, si, cls, sec.name, sec.alignment, pending.size()});
        }
    }

    // Sort chunks by class, then within each class.
    // Default: higher-alignment chunks first to minimize inter-chunk padding.
    // Windows exception: COFF subsection families such as .CRT$X* and .tls$*
    // must preserve lexicographic subsection order so the CRT startup ranges
    // and TLS template remain valid after merging.
    /// Order pending chunks by semantic class and each platform's required
    /// subsection rules, using alignment and discovery order as fallbacks.
    std::stable_sort(
        pending.begin(), pending.end(), [platform](const PendingChunk &a, const PendingChunk &b) {
            if (a.cls != b.cls)
                return sectionClassOrder(a.cls) < sectionClassOrder(b.cls);

            if (platform == LinkPlatform::Linux) {
                const auto aInit = elfInitArraySortKey(a.name);
                const auto bInit = elfInitArraySortKey(b.name);
                if (aInit.isInitArray || bInit.isInitArray) {
                    if (aInit.isInitArray != bInit.isInitArray)
                        return aInit.isInitArray;
                    if (aInit.family != bInit.family)
                        return aInit.family < bInit.family;
                    if (aInit.priority != bInit.priority)
                        return aInit.priority < bInit.priority;
                    return a.inputOrder < b.inputOrder;
                }
            }

            if (platform == LinkPlatform::macOS) {
                const bool aMod = isMachOModInitTermSection(a.name);
                const bool bMod = isMachOModInitTermSection(b.name);
                if (aMod || bMod) {
                    if (aMod != bMod)
                        return aMod;
                    return a.inputOrder < b.inputOrder;
                }
            }

            if (platform == LinkPlatform::Windows) {
                const bool aCrt = isWindowsCrtSubsection(a.name);
                const bool bCrt = isWindowsCrtSubsection(b.name);
                if (aCrt != bCrt)
                    return aCrt;
                if (aCrt) {
                    if (a.name != b.name)
                        return a.name < b.name;
                    return a.inputOrder < b.inputOrder;
                }

                const bool aTls = isWindowsTlsSubsection(a.name);
                const bool bTls = isWindowsTlsSubsection(b.name);
                if (aTls && bTls) {
                    if (a.name != b.name)
                        return a.name < b.name;
                    if (a.alignment != b.alignment)
                        return a.alignment > b.alignment;
                    return a.inputOrder < b.inputOrder;
                }
            }

            if (a.alignment != b.alignment)
                return a.alignment > b.alignment;
            return a.inputOrder < b.inputOrder;
        });

    // Create output sections in order.
    /// Append and initialize a standard merged output section.
    /// The semantic @p cls argument identifies the caller's merge group; the
    /// explicit flags determine the stored section attributes.
    auto addOutputSection =
        [&](SectionClass cls, const char *name, bool exec, bool write, bool tls, bool zeroFill)
        -> OutputSection & {
        layout.sections.push_back({});
        auto &out = layout.sections.back();
        out.name = name;
        out.executable = exec;
        out.writable = write;
        out.tls = tls;
        out.zeroFill = zeroFill;
        out.dataSegment = write || tls || zeroFill;
        return out;
    };

    /// Append one input section to a merged output section with checked
    /// alignment, size growth, zero-fill handling, and placement recording.
    auto appendChunk = [&](OutputSection &out,
                           size_t objIdx,
                           size_t secIdx,
                           const ObjSection &sec,
                           uint32_t requestedAlign) -> bool {
        if (out.zeroFill != sec.zeroFill) {
            err << "error: cannot merge zero-fill and file-backed input section '" << sec.name
                << "' into output section '" << out.name << "'\n";
            return false;
        }

        uint32_t align = std::max(requestedAlign, 1u);
        if (align > out.alignment)
            out.alignment = align;

        const size_t logicalSize = outputSectionMemSize(out);
        size_t padded = 0;
        try {
            padded = alignUp(logicalSize, align);
        } catch (const std::exception &ex) {
            err << "error: section merge alignment failed for '" << out.name << "': " << ex.what()
                << "\n";
            return false;
        }
        if (padded < logicalSize) {
            err << "error: section merge alignment overflow for '" << out.name << "'\n";
            return false;
        }

        if (out.zeroFill) {
            out.memSize = padded;
        } else if (padded > out.data.size()) {
            out.data.resize(padded, 0);
            out.memSize = out.data.size();
        }

        const size_t chunkSize = objSectionMemSize(sec);
        const size_t currentSize = outputSectionMemSize(out);
        if (chunkSize > std::numeric_limits<size_t>::max() - currentSize) {
            err << "error: merged section '" << out.name << "' exceeds addressable size\n";
            return false;
        }

        InputChunk chunk;
        chunk.inputObjIndex = objIdx;
        chunk.inputSecIndex = secIdx;
        chunk.outputOffset = currentSize;
        chunk.size = chunkSize;
        out.chunks.push_back(chunk);

        if (out.zeroFill) {
            out.memSize = currentSize + chunkSize;
        } else {
            const size_t newSize = currentSize + chunkSize;
            if (sec.data.size() > chunkSize) {
                err << "error: input section '" << sec.name
                    << "' has more file bytes than logical memory size\n";
                return false;
            }
            out.data.resize(newSize, 0);
            std::copy(sec.data.begin(), sec.data.end(), out.data.begin() + currentSize);
            out.memSize = newSize;
        }
        return true;
    };

    // Merge chunks into output sections.
    /// Append every pending chunk in one semantic class to @p out.
    auto mergeClass = [&](SectionClass cls, OutputSection &out) -> bool {
        for (const auto &pc : pending) {
            if (pc.cls != cls)
                continue;

            const auto &sec = objects[pc.objIdx].sections[pc.secIdx];
            if (!appendChunk(out, pc.objIdx, pc.secIdx, sec, pc.alignment))
                return false;
        }
        return true;
    };

    // Text section.
    bool hasText = false;
    for (const auto &p : pending)
        if (p.cls == SectionClass::Text) {
            hasText = true;
            break;
        }
    if (hasText) {
        auto &text = addOutputSection(SectionClass::Text, ".text", true, false, false, false);
        if (!mergeClass(SectionClass::Text, text))
            return false;
    }

    // Rodata section.
    bool hasRodata = false;
    for (const auto &p : pending)
        if (p.cls == SectionClass::Rodata) {
            hasRodata = true;
            break;
        }
    if (hasRodata) {
        auto &rodata =
            addOutputSection(SectionClass::Rodata, ".rodata", false, false, false, false);
        if (!mergeClass(SectionClass::Rodata, rodata))
            return false;
    }

    // Data section.
    bool hasData = false;
    for (const auto &p : pending)
        if (p.cls == SectionClass::Data) {
            hasData = true;
            break;
        }
    if (hasData) {
        auto &data = addOutputSection(SectionClass::Data, ".data", false, true, false, false);
        if (!mergeClass(SectionClass::Data, data))
            return false;
    }

    // BSS section — placed before TLS to keep TLS sections contiguous.
    // Mach-O dyld requires __thread_vars and __thread_data to be adjacent
    // within the __DATA segment for correct TLS template discovery.
    bool hasBss = false;
    for (const auto &p : pending)
        if (p.cls == SectionClass::Bss) {
            hasBss = true;
            break;
        }
    if (hasBss) {
        auto &bss = addOutputSection(SectionClass::Bss, ".bss", false, true, false, true);
        if (!mergeClass(SectionClass::Bss, bss))
            return false;
    }

    // TLS data — on Mach-O, TLV descriptors (__thread_vars) and template data
    // (__thread_data) must be in separate output sections. dyld validates that
    // __thread_vars size is a multiple of 24 bytes (one descriptor = {thunk(8),
    // key(8), offset(8)}). Mixing template data into the same section produces
    // an invalid size and a dyld abort.
    {
        bool hasTlvDescriptors = false;
        bool hasTlvTemplateData = false;
        for (const auto &p : pending) {
            if (p.cls != SectionClass::TlsData)
                continue;
            const auto &sec = objects[p.objIdx].sections[p.secIdx];
            if (sec.name.find("thread_vars") != std::string::npos)
                hasTlvDescriptors = true;
            else
                hasTlvTemplateData = true;
        }

        // TLV descriptors → .tdata (mapped to __thread_vars in MachOExeWriter).
        if (hasTlvDescriptors) {
            auto &tdata =
                addOutputSection(SectionClass::TlsData, ".tdata", false, true, true, false);
            tdata.tlvDescriptors = true;
            for (const auto &pc : pending) {
                if (pc.cls != SectionClass::TlsData)
                    continue;
                const auto &sec = objects[pc.objIdx].sections[pc.secIdx];
                if (sec.name.find("thread_vars") == std::string::npos)
                    continue;

                if (!appendChunk(tdata, pc.objIdx, pc.secIdx, sec, pc.alignment))
                    return false;
            }
        }

        // TLS template data → .tdata on ELF/PE and .tdata_template on Mach-O
        // (mapped to __thread_data in MachOExeWriter).
        if (hasTlvTemplateData) {
            const char *tmplName = platform == LinkPlatform::macOS ? ".tdata_template" : ".tdata";
            auto &tmpl =
                addOutputSection(SectionClass::TlsData, tmplName, false, true, true, false);
            for (const auto &pc : pending) {
                if (pc.cls != SectionClass::TlsData)
                    continue;
                const auto &sec = objects[pc.objIdx].sections[pc.secIdx];
                if (sec.name.find("thread_vars") != std::string::npos)
                    continue;

                if (!appendChunk(tmpl, pc.objIdx, pc.secIdx, sec, pc.alignment))
                    return false;
            }
        }
    }

    // TLS BSS (zero-initialized thread-local data).
    bool hasTlsBss = false;
    for (const auto &p : pending)
        if (p.cls == SectionClass::TlsBss) {
            hasTlsBss = true;
            break;
        }
    if (hasTlsBss) {
        auto &tbss = addOutputSection(SectionClass::TlsBss, ".tbss", false, true, true, true);
        if (!mergeClass(SectionClass::TlsBss, tbss))
            return false;
    }

    // Preserved-name sections — each unique section name gets its own output
    // section. This keeps ObjC metadata discoverable by name and preserves
    // Windows unwind tables (.pdata/.xdata) for the PE writer.
    {
        std::map<std::string, std::vector<size_t>> preservedGroups;
        std::vector<std::string> preservedOrder;
        for (size_t pi = 0; pi < pending.size(); ++pi) {
            if (pending[pi].cls != SectionClass::ObjC && pending[pi].cls != SectionClass::Preserved)
                continue;
            const auto &sec = objects[pending[pi].objIdx].sections[pending[pi].secIdx];
            if (preservedGroups.find(sec.name) == preservedGroups.end())
                preservedOrder.push_back(sec.name);
            preservedGroups[sec.name].push_back(pi);
        }
        for (const auto &name : preservedOrder) {
            auto &indices = preservedGroups[name];
            const auto &firstPc = pending[indices[0]];
            const auto &firstSec = objects[firstPc.objIdx].sections[firstPc.secIdx];

            bool executable = false;
            bool writable = false;
            bool dataSegment = isMachODataSegmentSection(name);
            for (size_t pi : indices) {
                const auto &pc = pending[pi];
                const auto &sec = objects[pc.objIdx].sections[pc.secIdx];
                if (sec.tls != firstSec.tls || sec.zeroFill != firstSec.zeroFill ||
                    sec.alloc != firstSec.alloc) {
                    err << "error: preserved section '" << name
                        << "' has incompatible storage attributes across input objects\n";
                    return false;
                }
                executable = executable || sec.executable;
                writable = writable || sec.writable;
                dataSegment = dataSegment || sec.dataSegment;
            }

            layout.sections.push_back({});
            auto &out = layout.sections.back();
            out.name = name;
            out.executable = executable;
            out.writable = writable;
            out.tls = firstSec.tls;
            out.zeroFill = firstSec.zeroFill;
            out.alloc = firstSec.alloc;
            out.dataSegment = dataSegment || writable || firstSec.tls || firstSec.zeroFill;

            for (size_t pi : indices) {
                const auto &pc = pending[pi];
                const auto &sec = objects[pc.objIdx].sections[pc.secIdx];
                if (!appendChunk(out, pc.objIdx, pc.secIdx, sec, pc.alignment))
                    return false;
            }
        }
    }

    // Collect non-alloc sections (e.g., .debug_line) into separate output sections.
    // These have no virtual address and are not mapped into the process address space.
    // Debuggers read them directly from the file.
    {
        std::map<std::string, std::vector<std::pair<size_t, size_t>>> debugGroups;
        std::vector<std::string> debugOrder;
        for (size_t oi = 0; oi < objects.size(); ++oi) {
            const auto &obj = objects[oi];
            for (size_t si = 1; si < obj.sections.size(); ++si) {
                const auto &sec = obj.sections[si];
                if (sec.stripped)
                    continue;
                if (sec.alloc)
                    continue;
                if (sec.data.empty() && sec.relocs.empty() && !sectionHasLiveSymbol(obj, si))
                    continue;
                if (debugGroups.find(sec.name) == debugGroups.end())
                    debugOrder.push_back(sec.name);
                debugGroups[sec.name].push_back({oi, si});
            }
        }
        for (const auto &name : debugOrder) {
            auto &pairs = debugGroups[name];
            layout.sections.push_back({});
            auto &out = layout.sections.back();
            out.name = name;
            out.alloc = false;

            for (auto [oi, si] : pairs) {
                const auto &sec = objects[oi].sections[si];
                uint32_t align = std::max(sec.alignment, 1u);
                if (align > out.alignment)
                    out.alignment = align;
                size_t padded = 0;
                try {
                    padded = alignUp(out.data.size(), align);
                } catch (const std::exception &ex) {
                    err << "error: debug section merge alignment failed for '" << out.name
                        << "': " << ex.what() << "\n";
                    return false;
                }
                if (padded > out.data.size())
                    out.data.resize(padded, 0);
                if (sec.data.size() > std::numeric_limits<size_t>::max() - out.data.size()) {
                    err << "error: merged debug section '" << out.name
                        << "' exceeds addressable size\n";
                    return false;
                }

                InputChunk chunk;
                chunk.inputObjIndex = oi;
                chunk.inputSecIndex = si;
                chunk.outputOffset = out.data.size();
                chunk.size = sec.data.size();
                out.chunks.push_back(chunk);
                out.data.insert(out.data.end(), sec.data.begin(), sec.data.end());
                out.memSize = out.data.size();
            }
        }
    }

    // Assign virtual addresses.
    // Sort sections by permission class before VA assignment.
    // ObjC metadata sections are appended after the standard sections above,
    // but their permission class (writable vs readonly) must be respected so
    // that all non-writable sections come before writable ones in the VA space.
    // Without this, non-writable ObjC sections (e.g., __objc_classname) get
    // VAs after writable sections, causing __TEXT/__DATA segment overlap in
    // the Mach-O executable (SIGKILL on macOS ARM64).
    /// Order merged sections by permission and semantic class while preserving
    /// construction order for exact ties.
    std::stable_sort(layout.sections.begin(),
                     layout.sections.end(),
                     [](const OutputSection &a, const OutputSection &b) {
                         const int aPerm = permClass(a);
                         const int bPerm = permClass(b);
                         if (aPerm != bPerm)
                             return aPerm < bPerm;

                         const int aClass = sectionClassOrder(outputSectionClass(a));
                         const int bClass = sectionClassOrder(outputSectionClass(b));
                         if (aClass != bClass)
                             return aClass < bClass;

                         return false;
                     });

    if (!assignSectionVirtualAddresses(layout, platform, err))
        return false;

    return true;
}

} // namespace zanna::codegen::linker
