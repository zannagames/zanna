//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/passes/BinaryEmitPass.cpp
// Purpose: Implement the binary emission pass that encodes MIR into machine
//          code bytes via the Backend::emitModuleToBinary entry point.
// Key invariants:
//   - Requires register allocation to have completed
//   - Populates Module::binaryTextSections and Module::binaryRodata
// Ownership/Lifetime:
//   - Pass borrows Module during run(), does not own any state beyond options_
// Links: src/codegen/x86_64/passes/BinaryEmitPass.hpp,
//        src/codegen/x86_64/Backend.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/passes/BinaryEmitPass.hpp"

#include "codegen/common/ScalarGlobalLayout.hpp"
#include "codegen/common/objfile/CodeSection.hpp"
#include "il/core/Global.hpp"
#include "il/core/Module.hpp"
#include "il/core/Type.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

/// @file
/// @brief Implements binary-section emission for an allocated x86-64 module.

namespace zanna::codegen::x64::passes {

namespace {

/// @brief Builds the native initialized-data section from scalar IL globals.
/// @details Every type accepted by @ref common::scalarGlobalLayout becomes a
///          globally bound data symbol followed by naturally aligned,
///          little-endian initializer bytes. Non-scalar kinds, including
///          strings handled by the read-only-data pool, are skipped. Text
///          relocations to matching names coalesce with these definitions in
///          the object writer.
/// @param mod Source module whose global declarations are inspected.
/// @return Owned data section containing scalar symbols and initializers.
objfile::CodeSection buildScalarDataSection(const il::core::Module &mod) {
    objfile::CodeSection data;
    for (const auto &g : mod.globals) {
        const auto layout = common::scalarGlobalLayout(g.type.kind);
        if (layout.sizeBytes == 0)
            continue; // strings (rodata) / void / error / resumetok — nothing to emit
        const uint64_t raw = common::scalarGlobalRawBits(g.init, layout.isFloat);
        data.alignTo(static_cast<size_t>(layout.sizeBytes));
        data.defineSymbol(g.name, objfile::SymbolBinding::Global, objfile::SymbolSection::Data);
        for (int i = 0; i < layout.sizeBytes; ++i)
            data.emit8(static_cast<uint8_t>((raw >> (8 * i)) & 0xFF));
    }
    return data;
}

} // namespace

/// @brief Construct the binary emit pass with backend configuration.
/// @details Stores @p options by value so the pass survives the caller's local
///          options object. Used to drive @ref emitModuleToBinary later.
/// @param options Target ABI, object format, optimization, and debug policy.
BinaryEmitPass::BinaryEmitPass(CodegenOptions options) noexcept : options_(std::move(options)) {}

/// @brief Emit raw machine code for a lowered and allocated module.
/// @details Mirrors @ref EmitPass::run but produces binary CodeSections instead
///          of assembly text. Validates that register allocation completed,
///          target/frame state are consistent, then forwards to the backend's
///          @ref emitMIRToBinary which fills @p module.binaryTextSections and
///          @p module.binaryRodata. A merged @c binaryText is produced only
///          when debug line emission needs one contiguous address space, apart
///          from the deliberately present empty section for an empty module.
/// @param module Shared pipeline state whose binary artifacts are replaced.
/// @param diags Diagnostic sink receiving all expected failures.
/// @return @c true on success; otherwise @c false after recording an error.
bool BinaryEmitPass::run(Module &module, Diagnostics &diags) {
    if (!module.registersAllocated) {
        diags.error("binary emit: register allocation has not completed");
        return false;
    }
    if (module.target == nullptr) {
        diags.error("binary emit: target selection is missing prior to emission");
        return false;
    }
    if (module.mir.size() != module.frames.size()) {
        diags.error("binary emit: MIR/frame state is inconsistent");
        return false;
    }

    BinaryEmitResult result =
        emitMIRToBinary(module.mir, module.frames, module.roData, *module.target, options_);
    if (!result.errors.empty()) {
        std::string message = "error: x64 binary codegen failed:\n";
        message += result.errors;
        message.push_back('\n');
        diags.error(std::move(message));
        return false;
    }

    if (!result.text.empty() || module.mir.empty() || !result.debugLineData.empty())
        module.binaryText = std::move(result.text);
    else
        module.binaryText.reset();
    module.binaryRodata = std::move(result.rodata);
    module.binaryData = buildScalarDataSection(module.il);
    module.binaryTextSections = std::move(result.textSections);
    module.debugLineData = std::move(result.debugLineData);
    return true;
}

} // namespace zanna::codegen::x64::passes
