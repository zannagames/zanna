//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/linker/ICF.cpp
// Purpose: Implements Identical Code Folding for the native linker.
//          Operates between deduplicateStrings() and dead-symbol cleanup in the
//          link pipeline.
// Key invariants:
//   - Only .text.* sections with one Global symbol at offset 0 are candidates
//   - Identity includes both bytes AND relocation signatures (type, target, addend)
//   - Address-taken functions are excluded from folding
//   - Non-canonical sections have both data AND relocs cleared
// Links: codegen/common/linker/ICF.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file ICF.cpp
 * @brief Implements relocation-aware identical-code folding with address-identity guards.
 */

#include "codegen/common/linker/ICF.hpp"

#include "codegen/common/linker/LinkTypes.hpp"
#include "codegen/common/linker/ObjFileReader.hpp"
#include "codegen/common/linker/RelocClassify.hpp"
#include "codegen/common/linker/RelocConstants.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace zanna::codegen::linker {

namespace {

/// @brief Computes FNV-1a over a byte range.
/// @param data Pointer to @p len bytes.
/// @param len Number of bytes to hash.
/// @return 64-bit FNV-1a digest.
uint64_t fnv1a(const uint8_t *data, size_t len) {
    uint64_t h = 14695981039346656037ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/// @brief Mixes all eight little-endian bytes of an integer into an FNV-1a state.
/// @param h Existing hash state.
/// @param val Integer payload.
/// @return Updated hash state.
uint64_t fnv1aMix(uint64_t h, uint64_t val) {
    for (int i = 0; i < 8; ++i) {
        h ^= static_cast<uint8_t>(val >> (i * 8));
        h *= 1099511628211ULL;
    }
    return h;
}

/// @brief Normalized relocation signature used in section identity comparisons.
struct RelocSig {
    size_t offset = 0;
    uint32_t type = 0;
    std::string targetName;
    size_t targetObjIdx = 0;
    uint32_t targetSectionIndex = 0;
    size_t targetOffset = 0;
    bool targetIsLocalDefinition = false;
    int64_t addend = 0;

    /// @brief Tests every normalized relocation field for equality.
    /// @param o Signature to compare.
    /// @return `true` when the relocation identities match exactly.
    bool operator==(const RelocSig &o) const {
        return offset == o.offset && type == o.type && targetName == o.targetName &&
               targetObjIdx == o.targetObjIdx && targetSectionIndex == o.targetSectionIndex &&
               targetOffset == o.targetOffset &&
               targetIsLocalDefinition == o.targetIsLocalDefinition && addend == o.addend;
    }

    /// @brief Orders signatures lexicographically for deterministic normalization.
    /// @param o Signature to compare.
    /// @return `true` when this signature precedes @p o.
    bool operator<(const RelocSig &o) const {
        if (offset != o.offset)
            return offset < o.offset;
        if (type != o.type)
            return type < o.type;
        if (targetName != o.targetName)
            return targetName < o.targetName;
        if (targetObjIdx != o.targetObjIdx)
            return targetObjIdx < o.targetObjIdx;
        if (targetSectionIndex != o.targetSectionIndex)
            return targetSectionIndex < o.targetSectionIndex;
        if (targetOffset != o.targetOffset)
            return targetOffset < o.targetOffset;
        if (targetIsLocalDefinition != o.targetIsLocalDefinition)
            return targetIsLocalDefinition < o.targetIsLocalDefinition;
        return addend < o.addend;
    }
};

/// @brief ICF candidate representing one per-function executable section.
struct Candidate {
    size_t objIdx = 0;
    size_t secIdx = 0;
    std::string funcSymName; ///< The Global symbol this section defines.
    uint64_t hash = 0;       ///< Combined hash of bytes + reloc sigs.
    std::vector<RelocSig> sigs;
};

/// @brief Identifies an exact code location whose address is observable.
struct CodeAddressKey {
    size_t objIdx = 0;
    uint32_t secIdx = 0;
    size_t offset = 0;

    /// @brief Compares object, section, and byte offset.
    /// @param o Address key to compare.
    /// @return `true` when both keys identify the same code byte.
    bool operator==(const CodeAddressKey &o) const {
        return objIdx == o.objIdx && secIdx == o.secIdx && offset == o.offset;
    }
};

/// @brief Hash adapter for address-taken code-location sets.
struct CodeAddressKeyHash {
    /// @brief Combines an object's section-location tuple.
    /// @param k Code address key.
    /// @return Host-sized hash value.
    size_t operator()(const CodeAddressKey &k) const noexcept {
        size_t h = std::hash<size_t>{}(k.objIdx);
        h ^= std::hash<uint32_t>{}(k.secIdx) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= std::hash<size_t>{}(k.offset) + 0x9e3779b9u + (h << 6) + (h >> 2);
        return h;
    }
};

/// @brief Adds a signed relocation addend to a host-sized section offset.
/// @param base Unsigned base offset.
/// @param addend Signed relocation addend.
/// @param out Receives the checked result.
/// @return `true` when the result is nonnegative and representable.
bool addSignedOffset(size_t base, int64_t addend, size_t &out) {
    if (addend >= 0) {
        const auto u = static_cast<uint64_t>(addend);
        if (u > std::numeric_limits<size_t>::max() ||
            base > std::numeric_limits<size_t>::max() - static_cast<size_t>(u))
            return false;
        out = base + static_cast<size_t>(u);
        return true;
    }

    const uint64_t mag = static_cast<uint64_t>(-(addend + 1)) + 1ULL;
    if (mag > base)
        return false;
    out = base - static_cast<size_t>(mag);
    return true;
}

/// @brief Builds sorted normalized relocation signatures for a section.
/// @param obj Object owning the section and referenced symbols.
/// @param objIdx Object index used to qualify local definitions.
/// @param sec Candidate executable section.
/// @return Relocation signatures sorted by all identity fields.
std::vector<RelocSig> buildRelocSigs(const ObjFile &obj, size_t objIdx, const ObjSection &sec) {
    std::vector<RelocSig> sigs;
    sigs.reserve(sec.relocs.size());
    for (const auto &rel : sec.relocs) {
        RelocSig sig;
        sig.offset = rel.offset;
        sig.type = rel.type;
        sig.addend = rel.addend;
        // Resolve symbol index to name.
        if (rel.symIndex < obj.symbols.size()) {
            const auto &target = obj.symbols[rel.symIndex];
            sig.targetName = target.name;
            if (target.binding == ObjSymbol::Local && target.sectionIndex != 0) {
                sig.targetIsLocalDefinition = true;
                sig.targetObjIdx = objIdx;
                sig.targetSectionIndex = target.sectionIndex;
                sig.targetOffset = target.offset;
            }
        } else {
            sig.targetName = "$invalid:" + std::to_string(rel.symIndex);
        }
        sigs.push_back(std::move(sig));
    }
    std::sort(sigs.begin(), sigs.end());
    return sigs;
}

/// @brief Computes a combined hash of section bytes and relocation identities.
/// @param sec Candidate section containing instruction bytes.
/// @param sigs Normalized, sorted relocation signatures.
/// @return Hash used to partition candidates before exact comparison.
uint64_t hashCandidate(const ObjSection &sec, const std::vector<RelocSig> &sigs) {
    uint64_t h = fnv1a(sec.data.data(), sec.data.size());
    for (const auto &sig : sigs) {
        h = fnv1aMix(h, sig.offset);
        h = fnv1aMix(h, sig.type);
        for (char c : sig.targetName) {
            h ^= static_cast<uint8_t>(c);
            h *= 1099511628211ULL;
        }
        h = fnv1aMix(h, sig.targetObjIdx);
        h = fnv1aMix(h, sig.targetSectionIndex);
        h = fnv1aMix(h, sig.targetOffset);
        h = fnv1aMix(h, sig.targetIsLocalDefinition ? 1 : 0);
        h = fnv1aMix(h, static_cast<uint64_t>(sig.addend));
    }
    return h;
}

/// @brief Performs exact byte and relocation comparison after a hash match.
/// @param objects Object collection owning both candidates.
/// @param a First candidate.
/// @param b Second candidate.
/// @return `true` when both executable sections are safely interchangeable.
bool candidatesIdentical(const std::vector<ObjFile> &objects,
                         const Candidate &a,
                         const Candidate &b) {
    const auto &secA = objects[a.objIdx].sections[a.secIdx];
    const auto &secB = objects[b.objIdx].sections[b.secIdx];

    // Compare bytes.
    if (secA.data.size() != secB.data.size())
        return false;
    if (secA.data != secB.data)
        return false;

    // Compare relocation signatures.
    if (a.sigs.size() != b.sigs.size())
        return false;
    for (size_t i = 0; i < a.sigs.size(); ++i) {
        if (!(a.sigs[i] == b.sigs[i]))
            return false;
    }
    return true;
}

/// @brief Maps an object-format machine value to the linker's architecture enum.
/// @details Unknown machine codes return `std::nullopt`; callers conservatively
///          skip folding rather than interpreting relocations in the wrong
///          architecture's number space.
/// @param obj Object whose machine code is inspected.
/// @return x86-64 or AArch64 when recognized, otherwise `std::nullopt`.
std::optional<LinkArch> archForObject(const ObjFile &obj) {
    if (obj.machine == 62 || obj.machine == 0x8664) // EM_X86_64 / IMAGE_FILE_MACHINE_AMD64
        return LinkArch::X86_64;
    if (obj.machine == 183 || obj.machine == 0xAA64) // EM_AARCH64 / IMAGE_FILE_MACHINE_ARM64
        return LinkArch::AArch64;
    return std::nullopt;
}

/// @brief Distinguishes direct branch relocations from address-materializing uses.
/// @param obj Object providing the native relocation namespace.
/// @param action Neutral relocation classification.
/// @param type Original format-specific relocation type.
/// @return `true` for recognized direct call/jump/conditional-branch relocations.
bool isKnownBranchReloc(const ObjFile &obj, RelocAction action, uint32_t type) {
    if (action == RelocAction::Branch26 || action == RelocAction::CondBr19)
        return true;
    if (obj.format == ObjFileFormat::ELF && type == elf_x64::kPLT32)
        return true;
    if (obj.format == ObjFileFormat::MachO && type == macho_x64::kBranch)
        return true;
    return false;
}

/// @brief Determines whether a relocation makes its target function address observable.
/// @param obj Object owning the relocation.
/// @param sec Section containing the relocation.
/// @param rel Relocation to classify.
/// @return `false` only for recognized direct branches in executable sections;
///         unknown machines and all other uses are conservative address-takes.
bool isAddressTakingReloc(const ObjFile &obj, const ObjSection &sec, const ObjReloc &rel) {
    const auto arch = archForObject(obj);
    if (!arch)
        return true; // Unknown machine — conservatively treat every reloc as address-taking.
    const RelocAction action = classifyReloc(obj.format, *arch, rel.type);
    if (action == RelocAction::Abs64 || action == RelocAction::Abs32)
        return true;
    if (!sec.executable)
        return true;
    return !isKnownBranchReloc(obj, action, rel.type);
}

} // namespace

/// @copydoc foldIdenticalCode(std::vector<ObjFile> &, std::unordered_map<std::string, GlobalSymEntry> &)
size_t foldIdenticalCode(std::vector<ObjFile> &allObjects,
                         std::unordered_map<std::string, GlobalSymEntry> &globalSyms) {
    // Step 1: Identify address-taken functions.
    // A function is address-taken if a non-branch relocation references it.
    // Data references are address-taking by definition. Executable sections are
    // scanned too so RIP-relative LEA / ADRP+ADD materialization preserves
    // observable function identity while direct branches remain foldable.
    std::unordered_set<std::string> addressTaken;
    std::unordered_set<CodeAddressKey, CodeAddressKeyHash> addressTakenAddresses;
    for (size_t oi = 0; oi < allObjects.size(); ++oi) {
        const auto &obj = allObjects[oi];
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const auto &sec = obj.sections[si];
            if (sec.data.empty())
                continue;
            for (const auto &rel : sec.relocs) {
                if (!isAddressTakingReloc(obj, sec, rel))
                    continue;
                if (rel.symIndex >= obj.symbols.size())
                    continue;
                const auto &targetSym = obj.symbols[rel.symIndex];
                if (targetSym.sectionIndex > 0 && targetSym.sectionIndex < obj.sections.size()) {
                    // If the target is in an executable section, this is an address-taken ref.
                    if (obj.sections[targetSym.sectionIndex].executable) {
                        addressTaken.insert(targetSym.name);
                        size_t targetOffset = 0;
                        if (addSignedOffset(targetSym.offset, rel.addend, targetOffset))
                            addressTakenAddresses.insert(
                                CodeAddressKey{oi, targetSym.sectionIndex, targetOffset});
                        else
                            addressTakenAddresses.insert(
                                CodeAddressKey{oi, targetSym.sectionIndex, targetSym.offset});
                    }
                    continue;
                }

                if (targetSym.name.empty())
                    continue;

                auto git = globalSyms.find(targetSym.name);
                if (git == globalSyms.end())
                    continue;
                if (git->second.objIndex >= allObjects.size())
                    continue;
                const auto &targetObj = allObjects[git->second.objIndex];
                if (git->second.secIndex == 0 || git->second.secIndex >= targetObj.sections.size())
                    continue;
                if (targetObj.sections[git->second.secIndex].executable) {
                    addressTaken.insert(targetSym.name);
                    size_t targetOffset = 0;
                    if (addSignedOffset(git->second.offset, rel.addend, targetOffset))
                        addressTakenAddresses.insert(CodeAddressKey{
                            git->second.objIndex, git->second.secIndex, targetOffset});
                }
            }
        }
    }

    // Step 2: Scan for ICF candidates — per-function .text.* sections.
    std::vector<Candidate> candidates;
    for (size_t oi = 0; oi < allObjects.size(); ++oi) {
        const auto &obj = allObjects[oi];
        if (!archForObject(obj))
            continue; // Unknown machine — don't risk wrong-arch reloc classification.
        if (obj.format == ObjFileFormat::COFF)
            continue; // COFF COMDATs may have associated .pdata/.xdata siblings.
        for (size_t si = 1; si < obj.sections.size(); ++si) {
            const auto &sec = obj.sections[si];
            if (!sec.executable || sec.data.empty())
                continue;

            // Must be a per-function section (.text.funcname).
            // Generic ".text" sections with multiple functions are excluded.
            if (sec.name == ".text" || sec.name == "__text" || sec.name == "__TEXT,__text")
                continue;

            // Find the Global symbol at offset 0.
            std::string funcName;
            bool hasGlobalAtZero = false;
            for (size_t sym_i = 1; sym_i < obj.symbols.size(); ++sym_i) {
                const auto &sym = obj.symbols[sym_i];
                if (sym.sectionIndex == si && sym.binding == ObjSymbol::Global && sym.offset == 0) {
                    funcName = sym.name;
                    hasGlobalAtZero = true;
                    break;
                }
            }
            if (!hasGlobalAtZero)
                continue;
            bool hasOtherSectionSymbol = false;
            for (size_t sym_i = 1; sym_i < obj.symbols.size(); ++sym_i) {
                const auto &sym = obj.symbols[sym_i];
                if (sym.sectionIndex == si && !(sym.binding == ObjSymbol::Global &&
                                                sym.offset == 0 && sym.name == funcName)) {
                    hasOtherSectionSymbol = true;
                    break;
                }
            }
            if (hasOtherSectionSymbol)
                continue;

            // Skip address-taken functions.
            if (addressTaken.count(funcName) ||
                addressTakenAddresses.count(CodeAddressKey{oi, static_cast<uint32_t>(si), 0}))
                continue;

            // Build candidate.
            Candidate cand;
            cand.objIdx = oi;
            cand.secIdx = si;
            cand.funcSymName = funcName;
            cand.sigs = buildRelocSigs(obj, oi, sec);
            cand.hash = hashCandidate(sec, cand.sigs);
            candidates.push_back(std::move(cand));
        }
    }

    if (candidates.empty())
        return 0;

    // Step 3: Group candidates by hash.
    std::unordered_map<uint64_t, std::vector<size_t>> hashGroups;
    for (size_t i = 0; i < candidates.size(); ++i)
        hashGroups[candidates[i].hash].push_back(i);

    // Step 4: Within each hash group, find identical clusters and fold.
    size_t folded = 0;
    for (auto &[hash, indices] : hashGroups) {
        if (indices.size() < 2)
            continue;

        // Track which candidates have already been folded.
        std::vector<bool> processed(indices.size(), false);

        for (size_t i = 0; i < indices.size(); ++i) {
            if (processed[i])
                continue;

            // This is the canonical copy. Find all identical matches.
            const auto &canonical = candidates[indices[i]];
            std::vector<size_t> cluster; // indices into `indices`
            cluster.push_back(i);

            for (size_t j = i + 1; j < indices.size(); ++j) {
                if (processed[j])
                    continue;
                if (candidatesIdentical(allObjects, canonical, candidates[indices[j]])) {
                    cluster.push_back(j);
                    processed[j] = true;
                }
            }

            if (cluster.size() < 2)
                continue;

            // Fold: redirect all non-canonical symbols to canonical's section.
            const auto &canonCand = candidates[indices[cluster[0]]];
            for (size_t k = 1; k < cluster.size(); ++k) {
                const auto &foldCand = candidates[indices[cluster[k]]];

                // Redirect globalSyms entry for the folded function.
                auto it = globalSyms.find(foldCand.funcSymName);
                if (it != globalSyms.end()) {
                    it->second.objIndex = canonCand.objIdx;
                    it->second.secIndex = static_cast<uint32_t>(canonCand.secIdx);
                    it->second.offset = 0;
                    it->second.resolvedAddr = 0;
                    it->second.resolvedAddrValid = false;
                }

                // Keep relocations in the folded object's other sections from
                // falling back to a now-stripped same-object symbol definition.
                for (auto &sym : allObjects[foldCand.objIdx].symbols) {
                    if (sym.sectionIndex != foldCand.secIdx || sym.name != foldCand.funcSymName)
                        continue;
                    if (sym.binding == ObjSymbol::Global || sym.binding == ObjSymbol::Weak) {
                        sym.binding = ObjSymbol::Undefined;
                        sym.sectionIndex = 0;
                        sym.offset = 0;
                    }
                }

                // Clear both data AND relocs on the folded section.
                auto &foldSec = allObjects[foldCand.objIdx].sections[foldCand.secIdx];
                foldSec.data.clear();
                foldSec.relocs.clear();
                foldSec.memSize = 0;
                foldSec.zeroFill = false;
                foldSec.stripped = true;

                ++folded;
            }
        }
    }

    return folded;
}

} // namespace zanna::codegen::linker
