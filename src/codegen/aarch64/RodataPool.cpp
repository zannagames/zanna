//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/RodataPool.cpp
// Purpose: Read-only data pool — string literal deduplication and emission
//          for AArch64 assembly output.
//
// Key invariants:
//   - Strings are deduplicated by content; identical bytes share one label.
//   - Labels are emitted in first-seen insertion order for determinism.
//   - Section directive is selected from the requested target ABI, not host OS.
//
// Ownership/Lifetime:
//   - See RodataPool.hpp.
//
// Links: src/codegen/aarch64/RodataPool.hpp,
//        src/il/core/Module.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/RodataPool.hpp"

#include "codegen/common/LabelUtil.hpp"
#include "codegen/common/ScalarGlobalLayout.hpp"
#include "il/core/Global.hpp"
#include "il/core/Module.hpp"

#include <cstdint>

/**
 * @file
 * @brief Implements AArch64 literal pooling and target-specific global-data assembly.
 *
 * The collection path shares scalar layout and initializer conversion with
 * other native backends. Emission is selected from the requested `TargetInfo`
 * rather than compile-host macros, preserving cross-compilation behavior.
 */

namespace zanna::codegen::aarch64 {

/**
 * @brief Formats a deterministic label from a string's pool index.
 * @param index Zero-based unique-content insertion index.
 * @return Label of the form `L.str.<index>`.
 */
std::string RodataPool::makeLabel(std::size_t index) {
    return std::string("L.str.") + std::to_string(index);
}

/**
 * @brief Escapes arbitrary bytes for an assembler `.asciz` operand.
 * @param bytes Raw bytes to encode.
 * @return Escaped contents without quotes or a terminating null.
 */
std::string RodataPool::escapeAsciz(std::string_view bytes) {
    std::string s;
    s.reserve(bytes.size());
    for (unsigned char c : bytes) {
        switch (c) {
            case '"':
            case '\\':
                s.push_back('\\');
                s.push_back(static_cast<char>(c));
                break;
            case '\n':
                s += "\\n";
                break;
            case '\t':
                s += "\\t";
                break;
            default:
                if (c >= 32 && c < 127) {
                    s.push_back(static_cast<char>(c));
                } else {
                    s.push_back('\\');
                    s.push_back(static_cast<char>('0' + ((c >> 6) & 0x7)));
                    s.push_back(static_cast<char>('0' + ((c >> 3) & 0x7)));
                    s.push_back(static_cast<char>('0' + (c & 0x7)));
                }
        }
    }
    return s;
}

/**
 * @brief Interns string contents and associates an IL global name with the label.
 * @param ilName Global name to record or replace in the name map.
 * @param bytes Raw contents to intern.
 */
void RodataPool::addString(const std::string &ilName, const std::string &bytes) {
    auto it = contentToLabel_.find(bytes);
    if (it == contentToLabel_.end()) {
        const std::string label = makeLabel(ordered_.size());
        contentToLabel_.emplace(bytes, label);
        ordered_.emplace_back(label, bytes);
        nameToLabel_[ilName] = label;
    } else {
        nameToLabel_[ilName] = it->second;
    }
}

/**
 * @brief Appends supported globals from an IL module to the pool.
 * @param mod Module whose globals are scanned in storage order.
 * @post String references are copied and scalar initializers have owned
 *       little-endian object bytes.
 */
void RodataPool::buildFromModule(const il::core::Module &mod) {
    using Kind = il::core::Type::Kind;
    for (const auto &g : mod.globals) {
        if (g.type.kind == Kind::Str) {
            addString(g.name, g.init);
            continue;
        }
        // Writable scalar globals (e.g. `global i64 @counter = 41`) need a real
        // .data symbol, otherwise gaddr @counter resolves to an undefined symbol.
        // Layout/initializer rules are shared with the x86-64 path.
        const auto layout = zanna::codegen::common::scalarGlobalLayout(g.type.kind);
        if (layout.sizeBytes == 0)
            continue; // void / error / resumetok — nothing to emit
        DataGlobal dg;
        dg.name = g.name;
        dg.init = g.init;
        dg.sizeBytes = layout.sizeBytes;
        dg.isFloat = layout.isFloat;
        // Compute little-endian initializer bytes for the binary object path.
        const uint64_t raw = zanna::codegen::common::scalarGlobalRawBits(g.init, layout.isFloat);
        dg.bytes.resize(static_cast<size_t>(dg.sizeBytes));
        for (int i = 0; i < dg.sizeBytes; ++i)
            dg.bytes[static_cast<size_t>(i)] = static_cast<uint8_t>((raw >> (8 * i)) & 0xFF);
        dataGlobals_.push_back(std::move(dg));
    }
}

/**
 * @brief Writes writable scalar globals using target-specific assembly syntax.
 * @param[in,out] os Destination assembly stream.
 * @param target ABI selecting section names and external symbol spelling.
 */
void RodataPool::emitData(std::ostream &os, const TargetInfo &target) const {
    if (dataGlobals_.empty())
        return;
    if (target.isLinux())
        os << ".data\n";
    else if (target.isWindows())
        os << ".section .data,\"dw\"\n";
    else
        os << ".section __DATA,__data\n";
    for (const auto &dg : dataGlobals_) {
        const std::string sanitized = zanna::codegen::common::sanitizeLabel(dg.name);
        const std::string sym =
            target.isLinux() || target.isWindows() ? sanitized : "_" + sanitized;
        const int p2align =
            dg.sizeBytes >= 8 ? 3 : dg.sizeBytes >= 4 ? 2 : dg.sizeBytes >= 2 ? 1 : 0;
        const char *dir = dg.isFloat ? ".double"
                          : dg.sizeBytes == 8 ? ".quad"
                          : dg.sizeBytes == 4 ? ".long"
                          : dg.sizeBytes == 2 ? ".short"
                                              : ".byte";
        std::string value = dg.init;
        // trim surrounding whitespace
        const auto b = value.find_first_not_of(" \t\r\n");
        const auto e = value.find_last_not_of(" \t\r\n");
        value = (b == std::string::npos) ? std::string() : value.substr(b, e - b + 1);
        if (value.empty())
            value = dg.isFloat ? "0.0" : "0";
        os << "  .p2align " << p2align << "\n";
        os << "  .globl " << sym << "\n";
        os << sym << ":\n";
        os << "  " << dir << " " << value << "\n";
    }
    os << "\n";
}

/**
 * @brief Writes unique string contents to the target's read-only data section.
 * @param[in,out] os Destination assembly stream.
 * @param target ABI selecting the read-only section directive.
 */
void RodataPool::emit(std::ostream &os, const TargetInfo &target) const {
    if (ordered_.empty())
        return;
    if (target.isLinux())
        os << ".section .rodata\n";
    else if (target.isWindows())
        os << ".section .rdata,\"dr\"\n";
    else
        os << ".section __TEXT,__const\n";
    for (const auto &pair : ordered_) {
        const std::string &label = pair.first;
        const std::string &bytes = pair.second;
        os << label << ":\n";
        os << "  .asciz \"" << escapeAsciz(bytes) << "\"\n";
    }
    os << "\n";
}

} // namespace zanna::codegen::aarch64
