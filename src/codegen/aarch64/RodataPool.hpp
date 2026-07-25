//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/RodataPool.hpp
// Purpose: Deduplicate string literals and emit a rodata section for AArch64.
// Key invariants: Identical string contents are pooled to a single label;
//                 ordered_ preserves first-seen insertion order for deterministic output.
// Ownership/Lifetime: Constructed per-function or per-module; borrows IL Module
//                     data during buildFromModule but does not retain references.
// Links: src/codegen/aarch64/RodataPool.cpp,
//        src/codegen/x86_64/AsmEmitter.hpp (x86-64 equivalent),
//        docs/internals/architecture.md
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "codegen/aarch64/TargetAArch64.hpp"

/**
 * @file
 * @brief Declares deterministic string pooling and scalar-global emission for AArch64.
 *
 * `RodataPool` collects string and scalar global initializers from IL. Strings
 * are deduplicated by byte content for assembly rodata emission, while writable
 * scalars retain per-global storage and little-endian bytes for assembly and
 * direct object emission paths.
 */

namespace il::core {
struct Module;
}

namespace zanna::codegen::aarch64 {

/**
 * @brief Owns pooled string literals and writable scalar globals for AArch64 output.
 *
 * String labels are assigned in first-seen content order and remain stable for
 * the lifetime of the pool. IL global names map to those labels even when
 * several names share identical bytes. Scalar globals are not deduplicated.
 *
 * @invariant `ordered_` contains exactly one entry for each key in
 *            `contentToLabel_`.
 * @invariant Every value in `nameToLabel_` names an entry in `ordered_`.
 */
class RodataPool {
  public:
    /**
     * @brief Returns the IL-global-name to pooled-label mapping.
     *
     * @return Const reference valid until this pool is mutated or destroyed.
     */
    const std::unordered_map<std::string, std::string> &nameToLabel() const noexcept {
        return nameToLabel_;
    }

    /**
     * @brief Returns unique string entries in deterministic first-seen order.
     *
     * @return Const reference to owned `(label, raw bytes)` pairs, valid until
     *         this pool is mutated or destroyed.
     */
    const std::vector<std::pair<std::string, std::string>> &entries() const noexcept {
        return ordered_;
    }

    /**
     * @brief Collects supported string and scalar globals from an IL module.
     *
     * String contents are deduplicated; supported non-string scalar types
     * produce writable `DataGlobal` entries with serialized object bytes.
     * Existing pool contents are retained, so callers normally invoke this
     * once per freshly constructed pool.
     *
     * @param mod Module borrowed for the duration of the scan.
     * @post No reference into @p mod is retained.
     */
    void buildFromModule(const il::core::Module &mod);

    /**
     * @brief Emits pooled strings as platform-specific assembly rodata.
     *
     * Linux uses `.rodata`, Windows uses `.rdata`, and Mach-O uses
     * `__TEXT,__const`. An empty pool emits no text.
     *
     * @param[in,out] os Stream that receives assembly directives.
     * @param target Target ABI selecting section syntax.
     */
    void emit(std::ostream &os, const TargetInfo &target) const;

    /**
     * @brief Emits writable scalar globals to a target-specific data section.
     *
     * Each global receives alignment, external visibility, target symbol
     * mangling, and a size/type-appropriate initializer directive. An empty
     * scalar-global collection emits no text.
     *
     * @param[in,out] os Stream that receives assembly directives.
     * @param target Target ABI selecting section and symbol syntax.
     */
    void emitData(std::ostream &os, const TargetInfo &target) const;

    /// @brief Owns the assembly and object-emission representation of one writable scalar.
    struct DataGlobal {
        std::string name;          ///< Unmangled IL global name (e.g. "counter").
        int sizeBytes = 0;         ///< 1, 2, 4, or 8.
        bool isFloat = false;      ///< True for f64.
        std::string init;          ///< Serialized initializer; empty means zero.
        std::vector<uint8_t> bytes;///< Little-endian initializer bytes for the object path.
    };

    /**
     * @brief Returns writable scalar globals in module traversal order.
     *
     * @return Const reference to owned global records, valid until mutation or destruction.
     */
    const std::vector<DataGlobal> &dataGlobals() const noexcept {
        return dataGlobals_;
    }

  private:
    /// @brief Map from string content to its deduplicated assembly label.
    std::unordered_map<std::string, std::string> contentToLabel_;

    /// @brief Map from IL global name to the pooled assembly label.
    std::unordered_map<std::string, std::string> nameToLabel_;

    /// @brief Ordered list of (label, content) pairs for deterministic emission.
    std::vector<std::pair<std::string, std::string>> ordered_;

    /// @brief Writable scalar (non-string) globals collected from the module.
    std::vector<DataGlobal> dataGlobals_;

    /**
     * @brief Generates the assembler label for a string-pool index.
     *
     * @param index Zero-based insertion index.
     * @return Owned label of the form `L.str.<index>`.
     */
    static std::string makeLabel(std::size_t index);

    /**
     * @brief Escapes raw bytes for inclusion inside an assembler `.asciz` string.
     *
     * Quotes and backslashes receive backslash escapes, newline/tab use named
     * escapes, printable ASCII is copied, and remaining bytes use three-digit
     * octal escapes.
     *
     * @param bytes Raw byte sequence; embedded nulls are supported.
     * @return Owned escaped text without surrounding quotes.
     */
    static std::string escapeAsciz(std::string_view bytes);

    /**
     * @brief Adds or aliases an IL string global by byte content.
     *
     * @param ilName IL global name to map.
     * @param bytes Raw string bytes used as the deduplication key.
     * @post `nameToLabel_[ilName]` identifies the unique entry for @p bytes.
     */
    void addString(const std::string &ilName, const std::string &bytes);
};

} // namespace zanna::codegen::aarch64
