//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/passes/BinaryEmitPass.cpp
// Purpose: Encode AArch64 MIR into machine code bytes via A64BinaryEncoder,
//          producing CodeSection output for direct .o emission.
// Key invariants:
//   - Requires register allocation to have completed (operates on physical regs)
//   - Populates AArch64Module::binaryText and AArch64Module::binaryRodata
//   - RodataPool entries emitted as raw bytes with NUL terminators (.asciz semantics)
// Ownership/Lifetime:
//   - Stateless pass; mutates AArch64Module binary fields
// Links: src/codegen/aarch64/binenc/A64BinaryEncoder.hpp,
//        src/codegen/aarch64/RodataPool.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/passes/BinaryEmitPass.hpp"

#include "codegen/aarch64/binenc/A64BinaryEncoder.hpp"
#include "codegen/common/Parallelism.hpp"
#include "codegen/common/objfile/DebugLineTable.hpp"

#include <algorithm>
#include <atomic>
#include <exception>
#include <filesystem>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Implements direct, optionally parallel AArch64 object-section emission.
 *
 * String/scalar data sections are seeded before code so address fixups can
 * resolve same-object symbols. Functions are encoded independently for dead
 * stripping, then optionally coalesced while preserving deterministic order.
 */

namespace zanna::codegen::aarch64::passes {

namespace {

/**
 * @brief Seeds file-table slots through the largest source file ID in a MIR module.
 * @param[in,out] table Debug line table to extend.
 * @param mir Functions whose instruction locations are scanned.
 * @param debugSourcePath Source path, normalized when non-empty; empty becomes
 *        `"<source>"`.
 */
void seedDebugFiles(DebugLineTable &table,
                    const std::vector<MFunction> &mir,
                    std::string_view debugSourcePath) {
    uint32_t maxFileId = 1;
    for (const auto &fn : mir) {
        for (const auto &bb : fn.blocks) {
            for (const auto &mi : bb.instrs) {
                if (mi.loc.file_id > maxFileId)
                    maxFileId = mi.loc.file_id;
            }
        }
    }

    std::string filePath = std::string(debugSourcePath);
    if (filePath.empty())
        filePath = "<source>";
    else
        filePath = std::filesystem::path(filePath).lexically_normal().string();

    for (uint32_t fileId = 1; fileId <= maxFileId; ++fileId)
        table.addFileSlot(filePath);
}

/**
 * @brief Seeds file-table slots through the largest source file ID in one function.
 * @param[in,out] table Per-function debug line table to extend.
 * @param fn Function whose instruction locations are scanned.
 * @param debugSourcePath Source path, normalized when non-empty; empty becomes
 *        `"<source>"`.
 */
void seedDebugFiles(DebugLineTable &table, const MFunction &fn, std::string_view debugSourcePath) {
    uint32_t maxFileId = 1;
    for (const auto &bb : fn.blocks) {
        for (const auto &mi : bb.instrs) {
            if (mi.loc.file_id > maxFileId)
                maxFileId = mi.loc.file_id;
        }
    }

    std::string filePath = std::string(debugSourcePath);
    if (filePath.empty())
        filePath = "<source>";
    else
        filePath = std::filesystem::path(filePath).lexically_normal().string();

    for (uint32_t fileId = 1; fileId <= maxFileId; ++fileId)
        table.addFileSlot(filePath);
}

} // namespace

/**
 * @brief Encodes module MIR and globals into object-writer-ready sections.
 *
 * @param[in,out] module Pipeline module whose prior binary/debug products are
 *        cleared or replaced.
 * @param[in,out] diags Sink receiving the first reported encoding failure.
 * @return `true` on complete emission, including an empty MIR module.
 */
bool BinaryEmitPass::run(AArch64Module &module, Diagnostics &diags) {
    if (!module.ti) {
        diags.error("BinaryEmitPass: ti must be non-null");
        return false;
    }

    module.binaryTextSections.clear();
    module.debugLineData.clear();

    if (module.mir.empty()) {
        // Not an error — modules with no functions produce empty output.
        module.binaryText.emplace();
        module.binaryRodata.emplace();
        return true;
    }

    const auto abi = module.ti->abiFormat;

    objfile::CodeSection rodata;

    // Seed rodata before encoding so cross-section fixups can identify
    // same-object rodata targets without relying on writer-side heuristics.
    for (const auto &[label, content] : module.rodataPool.entries()) {
        rodata.defineSymbol(label, objfile::SymbolBinding::Local, objfile::SymbolSection::Rodata);
        rodata.emitBytes(content.data(), content.size());
        rodata.emit8(0); // NUL terminator
    }

    // Seed writable scalar globals as a separate __DATA section so gaddr/store/load
    // resolve to a real, writable symbol (text relocations to the name coalesce to it).
    objfile::CodeSection data;
    for (const auto &dg : module.rodataPool.dataGlobals()) {
        if (dg.bytes.empty())
            continue;
        data.alignTo(static_cast<size_t>(dg.sizeBytes));
        data.defineSymbol(dg.name, objfile::SymbolBinding::Global, objfile::SymbolSection::Data);
        data.emitBytes(dg.bytes.data(), dg.bytes.size());
    }

    // Set up debug line table for address→line mapping when requested.
    zanna::codegen::DebugLineTable debugLines;
    if (module.emitDebugLines)
        seedDebugFiles(debugLines, module.mir, module.debugSourcePath);
    uint64_t debugBias = 0;

    /**
     * @brief Encodes functions serially while accumulating stable debug-line bias.
     * @return `false` after reporting the first encoder exception.
     */
    auto encodeSequentially = [&]() {
        for (const auto &fn : module.mir) {
            // Emit each function into its own CodeSection for per-function dead stripping.
            module.binaryTextSections.emplace_back();
            zanna::codegen::DebugLineTable funcDebugLines;
            if (module.emitDebugLines)
                seedDebugFiles(funcDebugLines, fn, module.debugSourcePath);
            binenc::A64BinaryEncoder funcEncoder;
            if (module.emitDebugLines)
                funcEncoder.setDebugLineTable(&funcDebugLines);
            try {
                funcEncoder.encodeFunction(fn, module.binaryTextSections.back(), rodata, abi);
            } catch (const std::exception &ex) {
                module.binaryTextSections.pop_back();
                diags.error("BinaryEmitPass: failed to encode AArch64 function '" + fn.name +
                            "': " + ex.what());
                return false;
            }
            if (module.emitDebugLines)
                debugLines.append(funcDebugLines, debugBias);
            debugBias += static_cast<uint64_t>(module.binaryTextSections.back().currentOffset());
        }
        return true;
    };

    const std::size_t workerCount = common::codegenWorkerCount(module.mir.size());
    if (module.emitDebugLines || module.mir.size() < 2 || workerCount < 2) {
        if (!encodeSequentially())
            return false;
    } else {
        struct EncodedFunction {
            objfile::CodeSection text;
            std::string error;
        };

        std::vector<EncodedFunction> encoded(module.mir.size());
        std::atomic_size_t next{0};
        std::atomic_bool failed{false};
        std::vector<std::thread> workers;
        workers.reserve(workerCount);

        for (size_t worker = 0; worker < workerCount; ++worker) {
            /// @brief Claims and encodes function indices until work ends or a peer fails.
            workers.emplace_back([&]() {
                while (!failed.load(std::memory_order_relaxed)) {
                    const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                    if (index >= module.mir.size())
                        return;

                    binenc::A64BinaryEncoder encoder;
                    try {
                        encoder.encodeFunction(module.mir[index], encoded[index].text, rodata, abi);
                    } catch (const std::exception &ex) {
                        encoded[index].error = ex.what();
                        failed.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            });
        }

        for (auto &worker : workers)
            worker.join();

        for (size_t i = 0; i < encoded.size(); ++i) {
            if (!encoded[i].error.empty()) {
                diags.error("BinaryEmitPass: failed to encode AArch64 function '" +
                            module.mir[i].name + "': " + encoded[i].error);
                return false;
            }
        }

        module.binaryTextSections.reserve(encoded.size());
        for (auto &fn : encoded)
            module.binaryTextSections.push_back(std::move(fn.text));
    }

    if (module.coalesceTextSections && module.binaryTextSections.size() > 1) {
        objfile::CodeSection merged;
        for (const auto &section : module.binaryTextSections)
            merged.appendSection(section);
        module.binaryTextSections.clear();
        module.binaryTextSections.push_back(std::move(merged));
    }

    // Encode DWARF .debug_line if any entries were recorded.
    if (module.emitDebugLines && !debugLines.empty())
        module.debugLineData = debugLines.encodeDwarf5(8);

    objfile::CodeSection mergedText;
    for (const auto &section : module.binaryTextSections)
        mergedText.appendSection(section);
    module.binaryText = std::move(mergedText);
    module.binaryRodata = std::move(rodata);
    module.binaryData = std::move(data);
    return true;
}

} // namespace zanna::codegen::aarch64::passes
