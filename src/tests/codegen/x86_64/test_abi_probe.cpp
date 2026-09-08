//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/x86_64/test_abi_probe.cpp
// Purpose: Ensure the x86-64 backend honours the SysV ABI when marshalling
//          call arguments: parameters that already sit in their argument
//          register are forwarded without a move, and a permutation of the
//          arguments writes every argument register before the call.
// Key invariants:
//   - probe_caller has no move between its label and the call.
//   - probe_rotator writes RDI..R9 and XMM0..XMM5 before the call.
// Ownership/Lifetime: The test builds an IL module locally and inspects the
//   assembly text it produces.
// Links: src/codegen/x86_64/CallLowering.cpp, src/codegen/x86_64/FrameLowering.cpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/Backend.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace zanna::codegen::x64 {
namespace {
[[nodiscard]] ILValue makeParam(int id, ILValue::Kind kind) noexcept {
    ILValue value{};
    value.kind = kind;
    value.id = id;
    return value;
}

[[nodiscard]] ILValue makeLabel(std::string name) {
    ILValue value{};
    value.kind = ILValue::Kind::LABEL;
    value.label = std::move(name);
    return value;
}

/// @brief One function that forwards its six integer and six double
///        parameters to `rt_probe_echo`, in order when @p rotate is false and
///        rotated by one position within each class when true.
[[nodiscard]] ILFunction makeProbeFunction(const char *name, bool rotate) {
    ILFunction func{};
    func.name = name;

    ILBlock entry{};
    entry.name = func.name;

    for (int i = 0; i < 6; ++i) {
        entry.paramIds.push_back(i);
        entry.paramKinds.push_back(ILValue::Kind::I64);
    }
    for (int i = 0; i < 6; ++i) {
        entry.paramIds.push_back(6 + i);
        entry.paramKinds.push_back(ILValue::Kind::F64);
    }

    ILInstr callInstr{};
    callInstr.opcode = "call";
    callInstr.ops.push_back(makeLabel("rt_probe_echo"));

    for (int i = 0; i < 6; ++i) {
        const int id = rotate ? (i + 1) % 6 : i;
        callInstr.ops.push_back(makeParam(id, ILValue::Kind::I64));
    }
    for (int i = 0; i < 6; ++i) {
        const int id = 6 + (rotate ? (i + 1) % 6 : i);
        callInstr.ops.push_back(makeParam(id, ILValue::Kind::F64));
    }

    ILInstr retInstr{};
    retInstr.opcode = "ret";

    entry.instrs.push_back(callInstr);
    entry.instrs.push_back(retInstr);

    func.blocks.push_back(entry);
    return func;
}

[[nodiscard]] ILModule makeProbeModule() {
    ILModule module{};
    module.funcs.push_back(makeProbeFunction("probe_caller", false));
    module.funcs.push_back(makeProbeFunction("probe_rotator", true));
    return module;
}

template <std::size_t N>
[[nodiscard]] bool containsAll(const std::string &asmText,
                               const std::array<std::string_view, N> &patterns) {
    for (const std::string_view pattern : patterns) {
        if (asmText.find(pattern) == std::string::npos) {
            return false;
        }
    }
    return true;
}

/// @brief The body of function @p name up to and including its call.
[[nodiscard]] std::string bodyBeforeCall(const std::string &asmText, const std::string &name) {
    const std::size_t begin = asmText.find(name + ":\n");
    if (begin == std::string::npos)
        return {};
    const std::size_t call = asmText.find("callq rt_probe_echo", begin);
    if (call == std::string::npos)
        return {};
    return asmText.substr(begin, call - begin);
}

[[nodiscard]] bool verifyProbeAssembly(const std::string &asmText) {
    // SysV ABI: integer arguments in RDI, RSI, RDX, RCX, R8, R9 and doubles
    // in XMM0-XMM5. The test targets Linux explicitly, so the Win64 order is
    // not in play here.
    //
    // probe_caller passes every parameter through in place: the allocator
    // assigns each parameter its own argument register from the entry copy
    // hint, so nothing writes an argument register before the call.
    constexpr std::array<std::string_view, 6> kGprPatterns{
        ", %rdi", ", %rsi", ", %rdx", ", %rcx", ", %r8", ", %r9"};
    constexpr std::array<std::string_view, 6> kXmmPatterns{
        ", %xmm0", ", %xmm1", ", %xmm2", ", %xmm3", ", %xmm4", ", %xmm5"};
    const std::string passThrough = bodyBeforeCall(asmText, "probe_caller");
    if (passThrough.empty())
        return false;
    for (const std::string_view pattern : kGprPatterns) {
        if (passThrough.find(pattern) != std::string::npos)
            return false;
    }
    for (const std::string_view pattern : kXmmPatterns) {
        if (passThrough.find(pattern) != std::string::npos)
            return false;
    }

    // probe_rotator needs a real permutation of both classes: every argument
    // register is written before the call.
    const std::string rotated = bodyBeforeCall(asmText, "probe_rotator");
    if (rotated.empty())
        return false;
    return containsAll(rotated, kGprPatterns) && containsAll(rotated, kXmmPatterns);
}

} // namespace
} // namespace zanna::codegen::x64

int main() {
    using namespace zanna::codegen::x64;

    const ILModule module = makeProbeModule();
    CodegenOptions options{};
    options.targetPlatform = CodegenOptions::TargetPlatform::Linux;
    const CodegenResult result = emitModuleToAssembly(module, options);

    if (!result.errors.empty() || !verifyProbeAssembly(result.asmText)) {
        std::cerr << "Assembly verification failed:\n" << result.asmText;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
