//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/CodegenStats.cpp
// Purpose: Implements the x86-64 codegen statistics counters.
// Key invariants:
//   - Per-function records sum to the module record.
// Ownership/Lifetime:
//   - Stateless.
// Links: src/codegen/x86_64/CodegenStats.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/CodegenStats.hpp"

#include <cstdlib>
#include <variant>

/// @file
/// @brief Implements computeCodegenStats() and formatCodegenStats() for x86-64.

namespace zanna::codegen::x64 {

namespace {

[[nodiscard]] bool isLoad(MOpcode opc) noexcept {
    return opc == MOpcode::MOVmr || opc == MOpcode::MOVSDmr || opc == MOpcode::MOVUPSmr ||
           opc == MOpcode::POP;
}

[[nodiscard]] bool isStore(MOpcode opc) noexcept {
    return opc == MOpcode::MOVrm || opc == MOpcode::MOVSDrm || opc == MOpcode::MOVUPSrm ||
           opc == MOpcode::PUSH;
}

[[nodiscard]] bool isBranch(MOpcode opc) noexcept {
    return opc == MOpcode::JMP || opc == MOpcode::JCC || opc == MOpcode::JUMPTABLE ||
           opc == MOpcode::RET;
}

/// @brief Whether @p mi addresses the frame through RBP outside the
///        callee-saved save area.
/// @details The save area occupies the first @p calleeSavedCount 8-byte slots
///          below RBP (`assignSpillSlots` lays it out first); the prologue and
///          epilogue moves through it are register preservation, not value
///          traffic, and AArch64 emits its equivalent outside MIR, so they are
///          left out on both backends.
[[nodiscard]] bool touchesFrame(const MInstr &mi, std::size_t calleeSavedCount) noexcept {
    const int32_t saveAreaLow = -static_cast<int32_t>(calleeSavedCount) * 8;
    for (const auto &op : mi.operands) {
        if (const auto *mem = std::get_if<OpMem>(&op)) {
            if (!mem->base.isPhys || static_cast<PhysReg>(mem->base.idOrPhys) != PhysReg::RBP)
                continue;
            if (mem->disp < 0 && mem->disp >= saveAreaLow)
                continue;
            return true;
        }
    }
    return false;
}

} // namespace

/// @copydoc CodegenStats::add
void CodegenStats::add(const CodegenStats &other) noexcept {
    functions += other.functions;
    blocks += other.blocks;
    instructions += other.instructions;
    calls += other.calls;
    branches += other.branches;
    moves += other.moves;
    loads += other.loads;
    stores += other.stores;
    frameLoads += other.frameLoads;
    frameStores += other.frameStores;
    offsetPrefixes += other.offsetPrefixes;
    spillSlots += other.spillSlots;
    frameBytes += other.frameBytes;
    calleeSaved += other.calleeSaved;
}

/// @copydoc codegenStatsEnabled
bool codegenStatsEnabled() noexcept {
    if (const char *value = std::getenv("ZANNA_CODEGEN_STATS"))
        return value[0] != '\0' && value[0] != '0';
    return false;
}

/// @copydoc computeCodegenStats
CodegenStats computeCodegenStats(const MFunction &fn, const FrameInfo &frame) {
    CodegenStats s;
    s.functions = 1;
    s.blocks = fn.blocks.size();
    const int spillBytes = frame.spillAreaGPR + frame.spillAreaXMM;
    s.spillSlots = spillBytes > 0 ? static_cast<std::size_t>(spillBytes) / 8U : 0;
    s.frameBytes = frame.frameSize > 0 ? static_cast<std::size_t>(frame.frameSize) : 0;
    s.calleeSaved = frame.usedCalleeSaved.size();

    for (const auto &block : fn.blocks) {
        s.instructions += block.instructions.size();
        for (const auto &mi : block.instructions) {
            const MOpcode opc = mi.opcode;
            if (opc == MOpcode::CALL)
                ++s.calls;
            if (isBranch(opc))
                ++s.branches;
            if (opc == MOpcode::MOVrr || opc == MOpcode::MOVSDrr)
                ++s.moves;
            if (isLoad(opc))
                ++s.loads;
            if (isStore(opc))
                ++s.stores;
            if (opc != MOpcode::LEA && touchesFrame(mi, frame.usedCalleeSaved.size())) {
                if (isStore(opc))
                    ++s.frameStores;
                else
                    ++s.frameLoads;
            }
        }
    }
    return s;
}

/// @copydoc formatCodegenStats
std::string formatCodegenStats(const CodegenStats &st, const std::string &name) {
    std::string out = "[codegen-stats] arch=x64 fn=" + name;
    const auto kv = [&out](const char *key, std::size_t value) {
        out += ' ';
        out += key;
        out += '=';
        out += std::to_string(value);
    };
    kv("functions", st.functions);
    kv("blocks", st.blocks);
    kv("instrs", st.instructions);
    kv("calls", st.calls);
    kv("branches", st.branches);
    kv("moves", st.moves);
    kv("loads", st.loads);
    kv("stores", st.stores);
    kv("frameLoads", st.frameLoads);
    kv("frameStores", st.frameStores);
    kv("offsetPrefixes", st.offsetPrefixes);
    kv("spillSlots", st.spillSlots);
    kv("frameBytes", st.frameBytes);
    kv("calleeSaved", st.calleeSaved);
    return out;
}

} // namespace zanna::codegen::x64
