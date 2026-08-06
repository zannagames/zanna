//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/SymbolResolver.cpp
// Purpose: Implementation of global symbol resolution.
//          Iteratively extracts archive members until all symbols resolved.
// Key invariants:
//   - Strong > Weak > Undefined precedence
//   - Multiple strong definitions of the same symbol = linker error
//   - Regular COFF definitions override selectany/COMDAT ANY fallbacks
//   - Archives re-scanned until fixed point (handles cross-archive deps)
// Ownership/Lifetime:
//   - Caller-owned object and archive collections are mutated in place during resolution.
//   - Extracted archive members remain owned by the aggregate object list through final layout.
// Links: codegen/common/linker/SymbolResolver.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file SymbolResolver.cpp
 * @brief Implements global symbol resolution, archive extraction, and COMDAT selection.
 *
 * Resolution combines direct objects with lazily extracted archive members,
 * applies strong/weak/common precedence, validates COFF COMDAT selection rules,
 * and classifies only allowlisted unresolved names as dynamic imports. The pass
 * also materializes tentative common definitions and target-specific synthetic
 * symbols needed by later layout stages.
 */

#include "codegen/common/linker/SymbolResolver.hpp"
#include "codegen/common/linker/DynamicSymbolPolicy.hpp"
#include "codegen/common/linker/NameMangling.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace zanna::codegen::linker {

static bool preferArchiveDefinition(const std::string &name, LinkPlatform platform);
static bool isPreferredArchiveDefinitionObject(const ObjFile &obj, LinkPlatform platform);
static bool allowDuplicateStrongDefinition(const std::string &name,
                                           LinkPlatform platform,
                                           bool allowArchiveDefinitionPreference);
static void synthesizeWindowsThreadSafeStaticGuards(
    std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
    std::unordered_set<std::string> &undefined);
static bool materializeCommonSymbols(std::vector<ObjFile> &allObjects,
                                     std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                                     std::ostream &err);
static void discardComdatLosers(std::vector<ObjFile> &allObjects,
                                std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                                const std::vector<InputSectionKey> &comdatLosers);

/// @brief Comparable metadata for one non-associative COFF COMDAT definition.
struct ComdatDefinition {
    ///< Selection policy governing duplicate definitions.
    ComdatSelection selection = ComdatSelection::None;
    ///< Linker key shared by equivalent COMDAT groups.
    std::string key;
    ///< Index of the defining object in the accumulated object vector.
    size_t objIdx = 0;
    ///< One-based index of the COMDAT leader section.
    uint32_t secIdx = 0;
    ///< Logical section size used by SAME_SIZE and LARGEST selection.
    size_t size = 0;
    ///< Content-and-relocation hash used by EXACT_MATCH selection.
    uint64_t hash = 0;
};

/// @brief Validate an alignment accepted for a materialized common symbol.
/// @param alignment Requested byte alignment.
/// @return `true` for a nonzero power of two representable by `uint32_t`.
static bool isSupportedCommonAlignment(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1)) == 0 &&
           alignment <= std::numeric_limits<uint32_t>::max();
}

/// @brief Return an ASCII-lowercased copy of @p value for Windows archive-name checks.
/// @param value String to normalize in place.
/// @return The normalized string; non-ASCII bytes are preserved by `tolower`.
static std::string asciiLower(std::string value) {
    for (char &ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

/// @brief Extract the archive path component from an object display name.
/// @details Archive members are named as "path/to/archive.lib(member.obj)".
///          Non-archive objects are returned unchanged, which lets the caller
///          reject them by basename policy.
/// @param objectName Object display name, optionally ending in `(member)`.
/// @return The path portion preceding the archive-member suffix.
static std::string archivePathFromObjectName(const std::string &objectName) {
    const size_t memberStart = objectName.find('(');
    return memberStart == std::string::npos ? objectName : objectName.substr(0, memberStart);
}

/// @brief Extract the final path component from @p path using both host separators.
/// @param path Path that may contain slash or backslash separators.
/// @return The final component, or @p path itself when it has no separator.
static std::string basenameFromPath(const std::string &path) {
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// @brief Check whether an archive basename is one of Zanna's runtime archives.
/// @details This intentionally does not search the whole object path. Only a
///          basename such as libzanna_rt_base.a, zanna_rt_core.lib, or
///          zanna_runtime.lib is allowed to activate duplicate-runtime shim
///          preference.
/// @param basename Archive filename without directory components.
/// @return `true` when the normalized stem names a Zanna runtime archive.
static bool isZannaRuntimeArchiveBasename(const std::string &basename) {
    std::string name = asciiLower(basename);
    if (name.size() > 4 && name.substr(name.size() - 4) == ".lib")
        name.resize(name.size() - 4);
    else if (name.size() > 2 && name.substr(name.size() - 2) == ".a")
        name.resize(name.size() - 2);
    if (name.rfind("lib", 0) == 0)
        name.erase(0, 3);

    return name == "zanna_runtime" || name.rfind("zanna_rt_", 0) == 0 ||
           name.rfind("zanna-runtime", 0) == 0;
}

/// @brief Replace a global entry with a concrete object-symbol definition.
/// @param entry Table entry to overwrite.
/// @param sym Defined symbol supplying section, offset, and absolute state.
/// @param objIdx Index of the defining object.
/// @param binding Resolved strong or weak binding to record.
static void setEntryFromSymbol(GlobalSymEntry &entry,
                               const ObjSymbol &sym,
                               size_t objIdx,
                               GlobalSymEntry::Binding binding) {
    entry.binding = binding;
    entry.objIndex = objIdx;
    entry.secIndex = sym.sectionIndex;
    entry.offset = sym.offset;
    entry.resolvedAddr = sym.absolute ? static_cast<uint64_t>(sym.offset) : 0;
    entry.resolvedAddrValid = sym.absolute;
    entry.absolute = sym.absolute;
    entry.common = false;
    entry.commonSize = 0;
    entry.commonAlignment = 1;
}

/// @brief Merge a tentative common-symbol declaration into a global entry.
/// @details Common declarations coalesce by retaining the maximum requested
///          size and alignment until materialization.
/// @param entry Table entry to update.
/// @param sym Common symbol declaration.
/// @param objIdx Index of the declaring object.
/// @param binding Strong or weak binding to record.
static void setEntryFromCommon(GlobalSymEntry &entry,
                               const ObjSymbol &sym,
                               size_t objIdx,
                               GlobalSymEntry::Binding binding) {
    entry.binding = binding;
    entry.objIndex = objIdx;
    entry.secIndex = 0;
    entry.offset = 0;
    entry.resolvedAddr = 0;
    entry.resolvedAddrValid = false;
    entry.absolute = false;
    entry.common = true;
    entry.commonSize = std::max(entry.commonSize, sym.size);
    entry.commonAlignment = std::max(entry.commonAlignment, sym.commonAlignment);
}

/// @brief Compute an FNV-1a-style hash of a byte vector.
/// @param bytes Bytes to hash in order.
/// @return Deterministic 64-bit hash value.
static uint64_t hashBytes(const std::vector<uint8_t> &bytes) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t b : bytes) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    return h;
}

/// @brief Fold an unsigned 64-bit integer into an existing hash.
/// @param value Value serialized least-significant byte first.
/// @param h Hash accumulator updated in place.
static void hashU64(uint64_t value, uint64_t &h) {
    for (unsigned i = 0; i < 8; ++i) {
        h ^= static_cast<uint8_t>(value >> (i * 8));
        h *= 1099511628211ULL;
    }
}

/// @brief Fold a length-delimited string into an existing hash.
/// @param value String bytes to hash.
/// @param h Hash accumulator updated in place.
static void hashString(const std::string &value, uint64_t &h) {
    hashU64(value.size(), h);
    for (unsigned char ch : value) {
        h ^= ch;
        h *= 1099511628211ULL;
    }
}

/// @brief Fold a relocation target's semantic identity into a COMDAT hash.
/// @details Symbol spelling, binding, offsets, common state, and target-section
///          COMDAT metadata are included so EXACT_MATCH does not equate
///          relocations with materially different targets.
/// @param obj Object that owns both relocation and symbol table.
/// @param rel Relocation whose target identity is hashed.
/// @param h Hash accumulator updated in place.
static void hashRelocTarget(const ObjFile &obj, const ObjReloc &rel, uint64_t &h) {
    if (rel.symIndex >= obj.symbols.size()) {
        hashU64(rel.symIndex, h);
        return;
    }

    const auto &sym = obj.symbols[rel.symIndex];
    hashString(sym.name, h);
    hashU64(sym.binding, h);
    hashU64(sym.offset, h);
    hashU64(sym.size, h);
    hashU64(sym.absolute ? 1 : 0, h);
    hashU64(sym.common ? 1 : 0, h);
    hashU64(sym.commonAlignment, h);

    if (sym.sectionIndex == 0 || sym.absolute || sym.common) {
        hashU64(sym.sectionIndex, h);
        return;
    }
    if (sym.sectionIndex >= obj.sections.size()) {
        hashU64(sym.sectionIndex, h);
        return;
    }

    const auto &targetSec = obj.sections[sym.sectionIndex];
    hashString(targetSec.name, h);
    hashString(targetSec.comdatKey, h);
    hashU64(static_cast<uint64_t>(targetSec.comdatSelection), h);
    hashU64(targetSec.associativeSection, h);
}

/// @brief Hash the bytes, storage attributes, and relocations of a COMDAT section.
/// @param obj Object supplying relocation target symbols.
/// @param sec Candidate COMDAT section.
/// @return Deterministic semantic hash for EXACT_MATCH comparison.
static uint64_t hashComdatSection(const ObjFile &obj, const ObjSection &sec) {
    uint64_t h = hashBytes(sec.data);
    hashU64(objSectionMemSize(sec), h);
    hashU64(sec.memSize, h);
    hashU64(sec.alignment, h);
    hashU64(sec.executable ? 1 : 0, h);
    hashU64(sec.writable ? 1 : 0, h);
    hashU64(sec.alloc ? 1 : 0, h);
    hashU64(sec.tls ? 1 : 0, h);
    hashU64(sec.zeroFill ? 1 : 0, h);
    hashU64(sec.dataSegment ? 1 : 0, h);
    hashU64(sec.associativeSection, h);
    hashU64(sec.relocs.size(), h);
    for (const auto &rel : sec.relocs) {
        hashU64(rel.offset, h);
        hashU64(rel.type, h);
        hashU64(static_cast<uint64_t>(rel.addend), h);
        hashU64(rel.pcrel ? 1 : 0, h);
        hashU64(rel.length, h);
        hashU64(rel.sectionRelative ? 1 : 0, h);
        hashRelocTarget(obj, rel, h);
    }
    return h;
}

/// @brief Determine whether a COFF weak external should trigger archive search.
/// @param sym Weak-external symbol record to inspect.
/// @return `true` for legacy, LIBRARY, or ALIAS fallback characteristics.
static bool weakExternalSearchesFallbackLibrary(const ObjSymbol &sym) {
    // COFF values: 1=NOLIBRARY, 2=LIBRARY, 3=ALIAS. ALIAS must also seed
    // archive extraction; otherwise an archive-only fallback is never loaded.
    // Zanna-created tests and older readers used 0; keep that legacy value as
    // "search fallback".
    if (!sym.weakExternal || sym.weakDefaultName.empty())
        return false;
    return sym.weakExternalCharacteristics == 0 || sym.weakExternalCharacteristics == 2 ||
           sym.weakExternalCharacteristics == 3;
}

/// @brief Recognize MSVC inline comparison-category constants emitted pick-any.
/// @param name Decorated COFF symbol name.
/// @return `true` for supported `std::*_ordering` comparison constants.
static bool isMsvcStdComparisonCategoryInlineSymbol(const std::string &name) {
    const bool isComparisonConstant =
        name.rfind("?less@", 0) == 0 || name.rfind("?equal@", 0) == 0 ||
        name.rfind("?equivalent@", 0) == 0 || name.rfind("?greater@", 0) == 0 ||
        name.rfind("?unordered@", 0) == 0;
    if (!isComparisonConstant)
        return false;

    return name.find("@strong_ordering@std@@2U") != std::string::npos ||
           name.find("@weak_ordering@std@@2U") != std::string::npos ||
           name.find("@partial_ordering@std@@2U") != std::string::npos;
}

/// @brief Recognize MSVC `std` type-info and virtual-table symbols.
/// @param name Decorated COFF symbol name.
/// @return `true` for `std` RTTI or vftable spellings.
static bool isMsvcStdTypeInfoOrVftableSymbol(const std::string &name) {
    const bool isTypeInfoOrVftable = name.rfind("??_7", 0) == 0 || name.rfind("??_R", 0) == 0;
    return isTypeInfoOrVftable && name.find("@std@@") != std::string::npos;
}

/// @brief Recognize an MSVC decorated string-literal symbol.
/// @param name Decorated COFF symbol name.
/// @return `true` when the name begins with the `??_C@` literal prefix.
static bool isMsvcDecoratedStringLiteral(const std::string &name) {
    return name.rfind("??_C@", 0) == 0;
}

/// @brief Recognize MSVC exception metadata associated with `std` types.
/// @param name Decorated COFF symbol name.
/// @return `true` for supported catchable-type and throw-info spellings.
static bool isMsvcStdExceptionMetadataSymbol(const std::string &name) {
    const bool isExceptionMetadata = name.rfind("_CT", 0) == 0 || name.rfind("_TI", 0) == 0;
    return isExceptionMetadata && name.find("@std@@") != std::string::npos;
}

/// @brief Recognize MSVC inline constant data defined by the standard library.
/// @param name Decorated COFF symbol name.
/// @return `true` for the supported `std` inline-constant pattern.
static bool isMsvcStdInlineConstDataSymbol(const std::string &name) {
    return name.rfind("?", 0) == 0 && name.find("@std@@3") != std::string::npos &&
           name.size() >= 2 && name.compare(name.size() - 2, 2, "@B") == 0;
}

/// @brief Recognize compiler-generated floating-point and vector constants.
/// @param name COFF symbol name.
/// @return `true` for `__real@`, `__xmm@`, or `__ymm@` constants.
static bool isMsvcGeneratedConstantSymbol(const std::string &name) {
    return name.rfind("__real@", 0) == 0 || name.rfind("__xmm@", 0) == 0 ||
           name.rfind("__ymm@", 0) == 0;
}

/// @brief Recognize statically supplied MSVC standard-library helper families.
/// @param name COFF symbol name.
/// @return `true` when the helper is expected from Zanna's runtime archives.
static bool isMsvcStaticStdHelperSymbol(const std::string &name) {
    return name.rfind("__std_count_trivial_", 0) == 0 || name.rfind("__std_find_", 0) == 0 ||
           name.rfind("__std_find_end_", 0) == 0 || name.rfind("__std_find_first_", 0) == 0 ||
           name.rfind("__std_find_last_", 0) == 0 || name.rfind("__std_find_not_", 0) == 0 ||
           name.rfind("__std_fs_", 0) == 0 || name.rfind("__std_min_", 0) == 0 ||
           name.rfind("__std_min_element_", 0) == 0 || name.rfind("__std_mismatch_", 0) == 0 ||
           name.rfind("__std_remove_", 0) == 0 || name.rfind("__std_replace_copy_", 0) == 0 ||
           name.rfind("__std_reverse_trivially_swappable_", 0) == 0 ||
           name.rfind("__std_search_", 0) == 0 || name.rfind("__std_system_error_", 0) == 0;
}

/// @brief Extract duplicate-selection metadata for a symbol's COMDAT leader.
/// @param obj Object containing the symbol definition.
/// @param objIdx Index of @p obj in the accumulated object vector.
/// @param sym Defined global symbol to inspect.
/// @param out Receives comparable COMDAT metadata on success.
/// @return `true` for a keyed, non-associative COMDAT definition.
static bool getComdatDefinition(const ObjFile &obj,
                                size_t objIdx,
                                const ObjSymbol &sym,
                                ComdatDefinition &out) {
    if (sym.sectionIndex == 0 || sym.sectionIndex >= obj.sections.size())
        return false;
    const auto &sec = obj.sections[sym.sectionIndex];
    if (sec.comdatSelection == ComdatSelection::None ||
        sec.comdatSelection == ComdatSelection::Associative || sec.comdatKey.empty())
        return false;
    out.selection = sec.comdatSelection;
    out.key = sec.comdatKey;
    out.objIdx = objIdx;
    out.secIdx = sym.sectionIndex;
    out.size = objSectionMemSize(sec);
    out.hash = sec.comdatSelection == ComdatSelection::ExactMatch ? hashComdatSection(obj, sec) : 0;
    return true;
}

/// @brief Resolve a duplicate COMDAT definition, reporting which copy loses.
/// @details On success (@return true) the winning definition is left in @p entry
///          (switched to @p candidate for the LARGEST case when it is bigger).
///          @p loser is set to the section of the discarded copy so the caller
///          can strip it and its associative group; @p hasLoser is false only
///          when both copies are kept (distinct ANY groups sharing a name).
/// @param obj Object containing the candidate definition, used in diagnostics.
/// @param sym Candidate symbol definition.
/// @param objIdx Index of @p obj in the accumulated object vector.
/// @param existing Metadata for the previously selected definition.
/// @param candidate Metadata for the new definition.
/// @param entry Global table entry updated if a larger candidate wins.
/// @param loser Receives the losing leader's object/section key.
/// @param hasLoser Receives whether one copy should be stripped.
/// @param err Stream that receives COMDAT-policy mismatch diagnostics.
/// @return `true` when the selection policy permits the duplicate.
static bool selectComdatDuplicate(const ObjFile &obj,
                                  const ObjSymbol &sym,
                                  size_t objIdx,
                                  const ComdatDefinition &existing,
                                  const ComdatDefinition &candidate,
                                  GlobalSymEntry &entry,
                                  InputSectionKey &loser,
                                  bool &hasLoser,
                                  std::ostream &err) {
    hasLoser = false;
    const InputSectionKey candidateKey{candidate.objIdx, candidate.secIdx};
    const InputSectionKey existingKey{existing.objIdx, existing.secIdx};

    if (existing.key != candidate.key) {
        if (existing.selection == ComdatSelection::Any &&
            candidate.selection == ComdatSelection::Any)
            return true;
        err << "error: multiply defined symbol '" << sym.name << "' has mismatched COMDAT keys\n";
        return false;
    }

    const ComdatSelection selection =
        existing.selection != ComdatSelection::None ? existing.selection : candidate.selection;
    switch (selection) {
        case ComdatSelection::Any:
            loser = candidateKey;
            hasLoser = true;
            return true;
        case ComdatSelection::SameSize:
            if (existing.size == candidate.size) {
                loser = candidateKey;
                hasLoser = true;
                return true;
            }
            err << "error: COMDAT SAME_SIZE symbol '" << sym.name
                << "' has different section sizes\n";
            return false;
        case ComdatSelection::ExactMatch:
            if (existing.size == candidate.size && existing.hash == candidate.hash) {
                loser = candidateKey;
                hasLoser = true;
                return true;
            }
            err << "error: COMDAT EXACT_MATCH symbol '" << sym.name << "' has different contents\n";
            return false;
        case ComdatSelection::Largest:
            if (candidate.size > existing.size) {
                // Candidate wins: the previously-recorded copy is discarded.
                setEntryFromSymbol(entry, sym, objIdx, GlobalSymEntry::Global);
                loser = existingKey;
            } else {
                loser = candidateKey;
            }
            hasLoser = true;
            return true;
        case ComdatSelection::NoDuplicates:
            err << "error: COMDAT NODUPLICATES symbol '" << sym.name
                << "' is defined more than once\n";
            return false;
        case ComdatSelection::None:
        case ComdatSelection::Associative:
            err << "error: multiply defined symbol '" << sym.name << "' in " << obj.name << "\n";
            return false;
    }
    return false;
}

/// @brief Merge one object's symbols into the global resolution state.
/// @details Applies undefined, weak, strong, common, and COMDAT precedence;
///          queues newly discovered undefined references for archive search;
///          and records COMDAT sections that lose duplicate selection.
/// @param obj Object file whose symbol table is consumed.
/// @param objIdx Index of @p obj in the accumulated object vector.
/// @param globalSyms Global symbol table updated in place.
/// @param comdatDefs Selected COMDAT metadata keyed by global symbol name.
/// @param undefined Currently unresolved names updated by definitions/references.
/// @param platform Target symbol spelling and duplicate policy.
/// @param allowArchiveDefinitionPreference Whether this object belongs to a
///        trusted runtime archive whose shim duplicates may be ignored.
/// @param newUndefined Optional work queue receiving newly unresolved names.
/// @param comdatLosers Optional list receiving discarded COMDAT leader keys.
/// @param err Stream that receives alignment or multiple-definition diagnostics.
/// @return `true` when all symbols are compatible with the current table.
static bool addObjSymbols(const ObjFile &obj,
                          size_t objIdx,
                          std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                          std::unordered_map<std::string, ComdatDefinition> &comdatDefs,
                          std::unordered_set<std::string> &undefined,
                          LinkPlatform platform,
                          bool allowArchiveDefinitionPreference,
                          std::vector<std::string> *newUndefined,
                          std::vector<InputSectionKey> *comdatLosers,
                          std::ostream &err) {
    /// Remove a resolved name and its alternate Mach-O underscore spelling
    /// from the unresolved work set.
    auto eraseUndefinedVariants = [&](const std::string &name) {
        undefined.erase(name);
        if (platform == LinkPlatform::macOS) {
            if (!name.empty() && name[0] == '_')
                undefined.erase(name.substr(1));
            if (!name.empty())
                undefined.erase("_" + name);
        }
    };

    for (size_t i = 1; i < obj.symbols.size(); ++i) {
        const auto &sym = obj.symbols[i];
        if (sym.name.empty())
            continue;

        if (sym.binding == ObjSymbol::Undefined) {
            /// Register one unresolved name in both the table and archive-search
            /// queue unless a platform-equivalent definition already exists.
            auto addUndefined = [&](const std::string &undefName) {
                auto it = findWithPlatformFallback(globalSyms, undefName, platform);
                if (it == globalSyms.end() || it->second.binding == GlobalSymEntry::Undefined) {
                    const bool inserted = undefined.insert(undefName).second;
                    if (inserted && newUndefined)
                        newUndefined->push_back(undefName);
                }
                if (it == globalSyms.end()) {
                    GlobalSymEntry e;
                    e.name = undefName;
                    e.binding = GlobalSymEntry::Undefined;
                    globalSyms[undefName] = std::move(e);
                }
            };

            if (sym.weakExternal) {
                if (weakExternalSearchesFallbackLibrary(sym))
                    addUndefined(sym.weakDefaultName);
                continue;
            }

            addUndefined(sym.name);
            continue;
        }

        if (sym.binding == ObjSymbol::Local)
            continue; // Locals don't participate in global resolution.

        const bool isWeak = (sym.binding == ObjSymbol::Weak);
        if (sym.common && !isSupportedCommonAlignment(sym.commonAlignment)) {
            err << "error: common symbol '" << sym.name << "' has unsupported alignment\n";
            return false;
        }

        auto it = findWithPlatformFallback(globalSyms, sym.name, platform);
        ComdatDefinition candidateComdat;
        const bool hasCandidateComdat = getComdatDefinition(obj, objIdx, sym, candidateComdat);
        if (it == globalSyms.end()) {
            // New symbol.
            GlobalSymEntry e;
            e.name = sym.name;
            if (sym.common) {
                setEntryFromCommon(
                    e, sym, objIdx, isWeak ? GlobalSymEntry::Weak : GlobalSymEntry::Global);
            } else {
                setEntryFromSymbol(
                    e, sym, objIdx, isWeak ? GlobalSymEntry::Weak : GlobalSymEntry::Global);
            }
            globalSyms[sym.name] = std::move(e);
            if (hasCandidateComdat)
                comdatDefs[sym.name] = candidateComdat;
            else
                comdatDefs.erase(sym.name);
            eraseUndefinedVariants(sym.name);
        } else {
            auto &existing = it->second;
            if (existing.binding == GlobalSymEntry::Undefined) {
                // Was undefined, now defined.
                if (sym.common) {
                    setEntryFromCommon(existing,
                                       sym,
                                       objIdx,
                                       isWeak ? GlobalSymEntry::Weak : GlobalSymEntry::Global);
                } else {
                    setEntryFromSymbol(existing,
                                       sym,
                                       objIdx,
                                       isWeak ? GlobalSymEntry::Weak : GlobalSymEntry::Global);
                }
                if (hasCandidateComdat)
                    comdatDefs[sym.name] = candidateComdat;
                else
                    comdatDefs.erase(sym.name);
                eraseUndefinedVariants(sym.name);
            } else if (sym.common) {
                if (existing.common) {
                    existing.commonSize = std::max(existing.commonSize, sym.size);
                    existing.commonAlignment =
                        std::max(existing.commonAlignment, sym.commonAlignment);
                    if (!isWeak && existing.binding == GlobalSymEntry::Weak) {
                        existing.binding = GlobalSymEntry::Global;
                        existing.objIndex = objIdx;
                    }
                    comdatDefs.erase(sym.name);
                    eraseUndefinedVariants(sym.name);
                } else if (existing.binding == GlobalSymEntry::Weak && !isWeak) {
                    // A strong common definition overrides a weak real definition.
                    setEntryFromCommon(existing, sym, objIdx, GlobalSymEntry::Global);
                    comdatDefs.erase(sym.name);
                    eraseUndefinedVariants(sym.name);
                }
                // Existing strong real definitions override tentative definitions.
            } else if (existing.common) {
                if (!isWeak) {
                    // A strong real definition overrides any tentative definition.
                    setEntryFromSymbol(existing, sym, objIdx, GlobalSymEntry::Global);
                    if (hasCandidateComdat)
                        comdatDefs[sym.name] = candidateComdat;
                    else
                        comdatDefs.erase(sym.name);
                    eraseUndefinedVariants(sym.name);
                }
                // Weak real definitions do not override common definitions.
            } else if (existing.binding == GlobalSymEntry::Weak && !isWeak) {
                // Strong overrides weak.
                setEntryFromSymbol(existing, sym, objIdx, GlobalSymEntry::Global);
                if (hasCandidateComdat)
                    comdatDefs[sym.name] = candidateComdat;
                else
                    comdatDefs.erase(sym.name);
                eraseUndefinedVariants(sym.name);
            } else if (existing.binding == GlobalSymEntry::Global && !isWeak) {
                auto comdatIt = findWithPlatformFallback(comdatDefs, sym.name, platform);
                if (hasCandidateComdat && comdatIt != comdatDefs.end()) {
                    InputSectionKey loser;
                    bool hasLoser = false;
                    if (!selectComdatDuplicate(obj,
                                               sym,
                                               objIdx,
                                               comdatIt->second,
                                               candidateComdat,
                                               existing,
                                               loser,
                                               hasLoser,
                                               err))
                        return false;
                    if (hasLoser && comdatLosers)
                        comdatLosers->push_back(loser);
                    if (candidateComdat.selection == ComdatSelection::Largest &&
                        candidateComdat.size > comdatIt->second.size)
                        comdatIt->second = candidateComdat;
                    eraseUndefinedVariants(sym.name);
                    continue;
                }
                if (platform == LinkPlatform::Windows && hasCandidateComdat &&
                    candidateComdat.selection == ComdatSelection::Any &&
                    comdatIt == comdatDefs.end()) {
                    // MSVC emits __declspec(selectany) fallbacks as ordinary external symbols in
                    // COMDAT ANY sections. A non-COMDAT definition is the intentional override;
                    // keep it and discard the later selectany section as link.exe/lld-link do.
                    if (comdatLosers) {
                        comdatLosers->push_back(
                            InputSectionKey{candidateComdat.objIdx, candidateComdat.secIdx});
                    }
                    eraseUndefinedVariants(sym.name);
                    continue;
                }
                if (platform == LinkPlatform::Windows && !hasCandidateComdat &&
                    comdatIt != comdatDefs.end() &&
                    comdatIt->second.selection == ComdatSelection::Any) {
                    // The regular definition arrived after the selectany fallback. Replace the
                    // selected symbol before stripping the earlier COMDAT contribution.
                    const InputSectionKey loser{comdatIt->second.objIdx, comdatIt->second.secIdx};
                    setEntryFromSymbol(existing, sym, objIdx, GlobalSymEntry::Global);
                    comdatDefs.erase(comdatIt);
                    if (comdatLosers)
                        comdatLosers->push_back(loser);
                    eraseUndefinedVariants(sym.name);
                    continue;
                }
                if (allowDuplicateStrongDefinition(
                        sym.name, platform, allowArchiveDefinitionPreference))
                    continue;
                err << "error: multiply defined symbol '" << sym.name << "' in " << obj.name
                    << "\n";
                return false;
            } else {
                // Weak doesn't override anything.
            }
        }
    }
    return true;
}

/// @brief Tests whether a Windows symbol should be resolved from a runtime archive.
///
/// Runtime archives provide Windows compatibility shims for a small set of
/// formatting functions. Those definitions must participate in archive
/// resolution instead of being forced down the dynamic-import path.
/// @param name Unresolved symbol name.
/// @param platform Target platform.
/// @return `true` when Windows resolution should prefer a runtime archive definition.
static bool preferArchiveDefinition(const std::string &name, LinkPlatform platform) {
    if (platform != LinkPlatform::Windows)
        return false;

    // The Zia completion bridge (rt_zia_*) ships a non-weak stub in
    // zanna_rt_base on MSVC (RT_WEAK expands to nothing). When
    // zia_editor_services is force-loaded its strong definitions are added
    // first; treat a later duplicate strong stub definition as the same "runtime archive
    // provides an overridable shim" case the CRT names below already use, so it
    // is preferred-away rather than reported as multiply defined.
    if (name.rfind("rt_zia_", 0) == 0)
        return true;

    if (name == "?_OptionsStorage@?1??__local_stdio_printf_options@@9@9" ||
        name == "?_OptionsStorage@?1??__local_stdio_scanf_options@@9@9")
        return false;

    if (name == "fprintf" || name == "snprintf" || name == "vsnprintf" || name == "_vfprintf_l" ||
        name == "_vfscanf_l" || name == "_vsprintf_l" || name == "_vsnprintf_l" ||
        name == "_vswprintf_l" || name == "_vfwprintf_l" || name == "fstat" ||
        name == "_fstat64i32" || name == "stat" || name == "_stat64i32" ||
        name == "mainCRTStartup" || name == "WinMainCRTStartup" || name == "wmainCRTStartup" ||
        name == "wWinMainCRTStartup" || name == "__security_check_cookie" ||
        name == "__security_init_cookie" || name == "__GSHandlerCheck" ||
        name == "__GSHandlerCheck_EH4" || name == "__chkstk" || isMsvcStaticStdHelperSymbol(name))
        return true;

    return name.find("__scrt_") != std::string::npos ||
           name.find("__local_stdio_printf_options") != std::string::npos ||
           name.find("__local_stdio_scanf_options") != std::string::npos ||
           name.rfind("__xi_", 0) == 0 || name.rfind("__xc_", 0) == 0 ||
           name.rfind("__xl_", 0) == 0 || name.rfind("__dyn_tls_", 0) == 0 ||
           name.rfind("__tls_", 0) == 0;
}

/// @brief Check whether an object comes from a trusted Zanna runtime archive.
/// @param obj Extracted object whose display name encodes its archive path.
/// @param platform Target platform.
/// @return `true` only for Windows objects extracted from recognized runtime archives.
static bool isPreferredArchiveDefinitionObject(const ObjFile &obj, LinkPlatform platform) {
    if (platform != LinkPlatform::Windows)
        return false;

    // Keep the Windows CRT/runtime shim exception scoped to Zanna's own runtime
    // archives. User archives that accidentally define the same names should
    // still participate in normal multiple-definition diagnostics.
    return isZannaRuntimeArchiveBasename(basenameFromPath(archivePathFromObjectName(obj.name)));
}

/// @brief Tests whether a duplicate strong definition follows an approved pick-any rule.
///
/// MSVC emits some CRT inline-function local statics in every object that uses
/// the inline helper. Link.exe picks one definition; keep normal user duplicate
/// strong definitions strict while modeling that pick-any behavior.
/// @param name Duplicate strong symbol name.
/// @param platform Target platform.
/// @param allowArchiveDefinitionPreference Whether the candidate comes from a
///        trusted runtime archive.
/// @return `true` when the duplicate may be treated as pick-any.
static bool allowDuplicateStrongDefinition(const std::string &name,
                                           LinkPlatform platform,
                                           bool allowArchiveDefinitionPreference) {
    if (platform == LinkPlatform::Windows) {
        if (isWindowsStdioOptionsStorageSymbol(name))
            return true;
        if (isMsvcStdComparisonCategoryInlineSymbol(name))
            return true;
        if (isMsvcStdTypeInfoOrVftableSymbol(name))
            return true;
        if (isMsvcDecoratedStringLiteral(name))
            return true;
        if (isMsvcStdExceptionMetadataSymbol(name))
            return true;
        if (isMsvcStdInlineConstDataSymbol(name))
            return true;
        if (isMsvcGeneratedConstantSymbol(name))
            return true;
    }
    return allowArchiveDefinitionPreference && preferArchiveDefinition(name, platform);
}

/// @copydoc zanna::codegen::linker::resolveSymbols(const std::vector<ObjFile>&,
/// const std::vector<const Archive*>&,
/// std::unordered_map<std::string, GlobalSymEntry>&, std::vector<ObjFile>&,
/// std::unordered_set<std::string>&, std::ostream&, LinkPlatform)
bool resolveSymbols(const std::vector<ObjFile> &initialObjects,
                    const std::vector<const Archive *> &archives,
                    std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                    std::vector<ObjFile> &allObjects,
                    std::unordered_set<std::string> &dynamicSyms,
                    std::ostream &err,
                    LinkPlatform platform) {
    globalSyms.clear();
    dynamicSyms.clear();
    // Start with initial objects.
    allObjects = initialObjects;
    std::unordered_set<std::string> undefined;
    std::unordered_map<std::string, ComdatDefinition> comdatDefs;
    std::vector<std::string> pendingUndefined;
    std::vector<InputSectionKey> comdatLosers;
    size_t pendingIndex = 0;

    for (size_t i = 0; i < allObjects.size(); ++i) {
        if (!addObjSymbols(allObjects[i],
                           i,
                           globalSyms,
                           comdatDefs,
                           undefined,
                           platform,
                           false,
                           &pendingUndefined,
                           &comdatLosers,
                           err))
            return false;
    }

    // Iteratively resolve from archives until fixed point.
    std::unordered_set<InputSectionKey, InputSectionKeyHash> extractedMembers;
    size_t maxArchiveExtractions = 0;
    for (const Archive *archive : archives) {
        if (archive == nullptr) {
            err << "error: null archive supplied to symbol resolution\n";
            return false;
        }
        if (archive->members.size() > std::numeric_limits<size_t>::max() - maxArchiveExtractions) {
            err << "error: archive member count exceeds addressable size\n";
            return false;
        }
        maxArchiveExtractions += archive->members.size();
    }
    size_t extractedCount = 0;
    while (pendingIndex < pendingUndefined.size()) {
        const std::string undef = pendingUndefined[pendingIndex++];
        if (undefined.find(undef) == undefined.end())
            continue;

        bool extractedForUndef = false;
        for (size_t ai = 0; ai < archives.size(); ++ai) {
            const Archive &ar = *archives[ai];
            // Mach-O archives use underscore-prefixed symbol names.
            auto candIt = findWithPlatformFallback(ar.symbolCandidates, undef, platform);
            std::vector<size_t> legacyCandidate;
            const std::vector<size_t> *candidates = nullptr;
            if (candIt != ar.symbolCandidates.end()) {
                candidates = &candIt->second;
            } else {
                auto symIt = findWithPlatformFallback(ar.symbolIndex, undef, platform);
                if (symIt == ar.symbolIndex.end())
                    continue;
                legacyCandidate.push_back(symIt->second);
                candidates = &legacyCandidate;
            }
            if (candidates == nullptr || candidates->empty())
                continue;

            for (size_t memberIdx : *candidates) {
                if (memberIdx >= ar.members.size()) {
                    err << "error: archive symbol index references missing member " << memberIdx
                        << "\n";
                    return false;
                }

                InputSectionKey key{ai, memberIdx};
                if (extractedMembers.count(key))
                    continue;

                // Extract and parse this member. Only mark it extracted after
                // it has been successfully materialized; otherwise a malformed
                // archive member could be skipped permanently for later symbols.
                auto memberData = memberDataView(ar, ar.members[memberIdx]);
                if (memberData.data == nullptr || memberData.size == 0) {
                    err << "error: archive member '" << ar.members[memberIdx].name
                        << "' has no object data\n";
                    return false;
                }

                if (isCoffImportLibraryMember(memberData.data, memberData.size)) {
                    extractedMembers.insert(key);
                    if (++extractedCount > maxArchiveExtractions) {
                        err << "error: symbol resolution exceeded archive extraction bound\n";
                        return false;
                    }
                    continue;
                }

                ObjFile memberObj;
                std::ostringstream memberErr;
                if (!readObjFile(memberData.data,
                                 memberData.size,
                                 ar.path + "(" + ar.members[memberIdx].name + ")",
                                 memberObj,
                                 memberErr)) {
                    err << memberErr.str();
                    return false;
                }
                extractedMembers.insert(key);
                if (++extractedCount > maxArchiveExtractions) {
                    err << "error: symbol resolution exceeded archive extraction bound\n";
                    return false;
                }

                size_t newIdx = allObjects.size();
                allObjects.push_back(std::move(memberObj));
                const bool allowArchiveDefinitionPreference =
                    isPreferredArchiveDefinitionObject(allObjects[newIdx], platform);
                if (!addObjSymbols(allObjects[newIdx],
                                   newIdx,
                                   globalSyms,
                                   comdatDefs,
                                   undefined,
                                   platform,
                                   allowArchiveDefinitionPreference,
                                   &pendingUndefined,
                                   &comdatLosers,
                                   err))
                    return false;
                extractedForUndef = true;
                break;
            }
            if (extractedForUndef)
                break;
        }
        if (extractedForUndef && undefined.find(undef) != undefined.end())
            pendingUndefined.push_back(undef);
    }

    if (platform == LinkPlatform::Windows)
        synthesizeWindowsThreadSafeStaticGuards(globalSyms, undefined);

    // Discard the sections of every losing COMDAT copy before layout so a
    // duplicate body (and any always-live section it drags in) is not emitted.
    discardComdatLosers(allObjects, globalSyms, comdatLosers);

    if (!materializeCommonSymbols(allObjects, globalSyms, err))
        return false;

    // Mark remaining undefined as dynamic only when the symbol is explicitly
    // allowlisted as a shared-library import on this platform.
    // Unknown unresolveds remain hard link errors so we do not silently emit
    // binaries that fail at load time due to missing symbols.
    std::vector<std::string> unresolvedErrors;
    for (const auto &undef : undefined) {
        auto it = globalSyms.find(undef);
        if (it != globalSyms.end() && it->second.binding != GlobalSymEntry::Undefined)
            continue; // Was resolved during iteration.

        const bool allowSynthetic =
            platform == LinkPlatform::Windows && isWindowsLinkerHelperSymbol(undef);
        const bool allowDynamic = allowSynthetic || (isKnownDynamicSymbol(undef, platform) &&
                                                     !preferArchiveDefinition(undef, platform));
        if (!allowDynamic) {
            unresolvedErrors.push_back(undef);
            continue;
        }

        dynamicSyms.insert(undef);
        if (it != globalSyms.end()) {
            it->second.binding = GlobalSymEntry::Dynamic;
            it->second.resolvedAddrValid = it->second.resolvedAddr != 0;
        }
    }

    if (!unresolvedErrors.empty()) {
        std::sort(unresolvedErrors.begin(), unresolvedErrors.end());
        unresolvedErrors.erase(std::unique(unresolvedErrors.begin(), unresolvedErrors.end()),
                               unresolvedErrors.end());
        for (const auto &name : unresolvedErrors) {
            err << "error: undefined symbol '" << name << "'";
            if (preferArchiveDefinition(name, platform))
                err << " (expected a static/archive definition)";
            err << "\n";
        }
        return false;
    }

    return true;
}

/// @copydoc zanna::codegen::linker::resolveSymbols(const std::vector<ObjFile>&,
/// std::vector<Archive>&, std::unordered_map<std::string, GlobalSymEntry>&,
/// std::vector<ObjFile>&, std::unordered_set<std::string>&, std::ostream&,
/// LinkPlatform)
bool resolveSymbols(const std::vector<ObjFile> &initialObjects,
                    std::vector<Archive> &archives,
                    std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                    std::vector<ObjFile> &allObjects,
                    std::unordered_set<std::string> &dynamicSyms,
                    std::ostream &err,
                    LinkPlatform platform) {
    std::vector<const Archive *> archiveViews;
    archiveViews.reserve(archives.size());
    for (const Archive &archive : archives)
        archiveViews.push_back(&archive);
    return resolveSymbols(
        initialObjects, archiveViews, globalSyms, allObjects, dynamicSyms, err, platform);
}

/// @brief Strip losing COMDAT groups and redirect displaced winner symbols.
/// @details Each losing leader and its associative sections are made inert.
///          Global entries still referring to a stripped LARGEST predecessor
///          are then repointed at same-named definitions in the surviving group.
/// @param allObjects Accumulated objects modified in place.
/// @param globalSyms Global entries updated when a selected winner moved.
/// @param comdatLosers Object/section keys of losing COMDAT leaders.
static void discardComdatLosers(std::vector<ObjFile> &allObjects,
                                std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                                const std::vector<InputSectionKey> &comdatLosers) {
    if (comdatLosers.empty())
        return;

    /// Remove the file bytes, relocations, and logical allocation of one losing
    /// section while retaining identity metadata needed to find its winner.
    auto stripSection = [](ObjSection &sec) {
        sec.stripped = true;
        sec.data.clear();
        sec.relocs.clear();
        sec.memSize = 0;
        sec.zeroFill = false;
    };

    // 1. Strip each losing leader plus its associative group members. comdatKey is
    //    retained (not cleared) so the surviving winner can be located below.
    std::unordered_set<InputSectionKey, InputSectionKeyHash> strippedKeys;
    for (const auto &loser : comdatLosers) {
        if (loser.objIndex >= allObjects.size())
            continue;
        auto &obj = allObjects[loser.objIndex];
        if (loser.secIndex == 0 || loser.secIndex >= obj.sections.size())
            continue;
        stripSection(obj.sections[loser.secIndex]);
        strippedKeys.insert(loser);
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            if (obj.sections[si].associativeSection == loser.secIndex) {
                stripSection(obj.sections[si]);
                strippedKeys.insert(InputSectionKey{loser.objIndex, si});
            }
        }
    }

    // 2. Map each COMDAT key to its one surviving winner leader.
    std::unordered_map<std::string, InputSectionKey> winnerByKey;
    for (size_t oi = 0; oi < allObjects.size(); ++oi) {
        const auto &obj = allObjects[oi];
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const auto &sec = obj.sections[si];
            if (sec.stripped || sec.comdatSelection == ComdatSelection::None ||
                sec.comdatKey.empty() || sec.associativeSection != 0)
                continue;
            winnerByKey.emplace(sec.comdatKey, InputSectionKey{oi, si});
        }
    }

    // 3. Redirect any global symbol still pointing into a stripped loser section to
    //    the same-named definition in the winning group. SELECT_ANY already leaves
    //    globalSyms pointing at the winner (the loser hit the dedup path without
    //    overwriting it); this repoint only matters for the rare LARGEST winner
    //    switch, where symbols of the displaced old winner were already registered.
    for (auto &[name, entry] : globalSyms) {
        if (entry.binding == GlobalSymEntry::Undefined ||
            entry.binding == GlobalSymEntry::Dynamic || entry.secIndex == 0)
            continue;
        if (!strippedKeys.count(InputSectionKey{entry.objIndex, entry.secIndex}))
            continue;
        const auto &loserSec = allObjects[entry.objIndex].sections[entry.secIndex];
        auto wit = winnerByKey.find(loserSec.comdatKey);
        if (wit == winnerByKey.end())
            continue;
        const auto &winObj = allObjects[wit->second.objIndex];
        for (size_t k = 1; k < winObj.symbols.size(); ++k) {
            const auto &wsym = winObj.symbols[k];
            if (wsym.name == name && wsym.binding != ObjSymbol::Undefined &&
                wsym.binding != ObjSymbol::Local && !wsym.common) {
                setEntryFromSymbol(entry, wsym, wit->second.objIndex, entry.binding);
                break;
            }
        }
    }
}

/// @brief Materialize unresolved MSVC thread-safe-static guards as common data.
/// @param globalSyms Global table receiving four-byte common definitions.
/// @param undefined Unresolved set from which synthesized guard names are removed.
static void synthesizeWindowsThreadSafeStaticGuards(
    std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
    std::unordered_set<std::string> &undefined) {
    std::vector<std::string> guardNames;
    guardNames.reserve(undefined.size());
    for (const auto &name : undefined) {
        if (isMsvcThreadSafeStaticGuardSymbol(name))
            guardNames.push_back(name);
    }

    for (const auto &name : guardNames) {
        auto existing = globalSyms.find(name);
        if (existing != globalSyms.end() && existing->second.binding != GlobalSymEntry::Undefined) {
            undefined.erase(name);
            continue;
        }

        auto &entry = globalSyms[name];
        entry.name = name;
        entry.binding = GlobalSymEntry::Global;
        entry.objIndex = 0;
        entry.secIndex = 0;
        entry.offset = 0;
        entry.resolvedAddr = 0;
        entry.resolvedAddrValid = false;
        entry.absolute = false;
        entry.common = true;
        entry.commonSize = std::max<size_t>(entry.commonSize, 4);
        entry.commonAlignment = std::max<size_t>(entry.commonAlignment, 4);
        undefined.erase(name);
    }
}

/// @brief Materialize coalesced common symbols in a synthetic zero-fill object.
/// @details Common entries are sorted for deterministic layout, aligned within
///          one `.common` section, converted to concrete symbol definitions,
///          and appended as a new input object.
/// @param allObjects Accumulated objects receiving the synthetic common object.
/// @param globalSyms Common entries rewritten to concrete section definitions.
/// @param err Stream that receives alignment or section-size diagnostics.
/// @return `true` when every common allocation fits supported limits.
static bool materializeCommonSymbols(std::vector<ObjFile> &allObjects,
                                     std::unordered_map<std::string, GlobalSymEntry> &globalSyms,
                                     std::ostream &err) {
    std::vector<std::string> names;
    names.reserve(globalSyms.size());
    for (const auto &kv : globalSyms) {
        const auto &entry = kv.second;
        if (entry.common && entry.binding != GlobalSymEntry::Undefined &&
            entry.binding != GlobalSymEntry::Dynamic)
            names.push_back(kv.first);
    }
    if (names.empty())
        return true;

    std::sort(names.begin(), names.end());

    ObjFile commonObj;
    commonObj.name = "common";
    if (!allObjects.empty()) {
        commonObj.format = allObjects.front().format;
        commonObj.is64bit = allObjects.front().is64bit;
        commonObj.isLittleEndian = allObjects.front().isLittleEndian;
        commonObj.machine = allObjects.front().machine;
    }

    commonObj.sections.push_back(ObjSection{});
    commonObj.symbols.push_back(ObjSymbol{});

    ObjSection commonSec;
    commonSec.name = ".common";
    commonSec.writable = true;
    commonSec.alloc = true;
    commonSec.zeroFill = true;
    commonSec.alignment = 1;

    for (const auto &name : names) {
        auto it = globalSyms.find(name);
        if (it == globalSyms.end())
            continue;
        auto &entry = it->second;
        if (!isSupportedCommonAlignment(entry.commonAlignment)) {
            err << "error: common symbol '" << entry.name << "' has unsupported alignment\n";
            return false;
        }
        if (entry.commonSize > kMaxObjSectionBytes) {
            err << "error: common symbol '" << entry.name << "' exceeds section size limit\n";
            return false;
        }

        const size_t rem = commonSec.memSize % entry.commonAlignment;
        const size_t padding = (rem == 0) ? 0 : entry.commonAlignment - rem;
        if (padding > kMaxObjSectionBytes - commonSec.memSize ||
            entry.commonSize > kMaxObjSectionBytes - commonSec.memSize - padding) {
            err << "error: materialized common section exceeds size limit\n";
            return false;
        }

        commonSec.memSize += padding;
        const size_t offset = commonSec.memSize;
        commonSec.memSize += entry.commonSize;
        if (entry.commonAlignment > commonSec.alignment)
            commonSec.alignment = static_cast<uint32_t>(entry.commonAlignment);

        ObjSymbol sym;
        sym.name = entry.name.empty() ? name : entry.name;
        sym.binding = entry.binding == GlobalSymEntry::Weak ? ObjSymbol::Weak : ObjSymbol::Global;
        sym.sectionIndex = 1;
        sym.offset = offset;
        sym.size = entry.commonSize;
        commonObj.symbols.push_back(std::move(sym));

        entry.objIndex = allObjects.size();
        entry.secIndex = 1;
        entry.offset = offset;
        entry.absolute = false;
        entry.resolvedAddr = 0;
        entry.resolvedAddrValid = false;
        entry.common = false;
    }

    commonObj.sections.push_back(std::move(commonSec));
    allObjects.push_back(std::move(commonObj));
    return true;
}

} // namespace zanna::codegen::linker
