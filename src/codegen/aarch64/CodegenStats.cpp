//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/aarch64/CodegenStats.cpp
// Purpose: Implements the AArch64 codegen statistics counters.
// Key invariants:
//   - Opcode classification is by opcode only; operand shapes are read only
//     to recognise the frame-offset prefix and the scratch base it feeds.
//   - Every counter is monotone in the instruction stream, so per-function
//     records sum to the module record.
// Ownership/Lifetime:
//   - Stateless.
// Links: src/codegen/aarch64/CodegenStats.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/aarch64/CodegenStats.hpp"

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <vector>

/// @file
/// @brief Implements computeCodegenStats() and formatCodegenStats().

namespace zanna::codegen::aarch64 {

namespace {

[[nodiscard]] bool isCall(MOpcode opc) noexcept {
    return opc == MOpcode::Bl || opc == MOpcode::Blr;
}

[[nodiscard]] bool isBranch(MOpcode opc) noexcept {
    return opc == MOpcode::Br || opc == MOpcode::BCond || opc == MOpcode::Cbz ||
           opc == MOpcode::Cbnz || opc == MOpcode::Tbz || opc == MOpcode::Tbnz ||
           opc == MOpcode::JumpTable || opc == MOpcode::Ret;
}

[[nodiscard]] bool isMove(MOpcode opc) noexcept {
    return opc == MOpcode::MovRR || opc == MOpcode::FMovRR;
}

/// @brief Loads whose address is x29-relative.
[[nodiscard]] bool isFrameLoad(MOpcode opc) noexcept {
    return opc == MOpcode::LdrRegFpImm || opc == MOpcode::Ldr8RegFpImm ||
           opc == MOpcode::Ldr16RegFpImm || opc == MOpcode::Ldr32RegFpImm ||
           opc == MOpcode::LdrFprFpImm || opc == MOpcode::LdpRegFpImm ||
           opc == MOpcode::LdpFprFpImm;
}

/// @brief Loads whose address is a base register plus an immediate.
[[nodiscard]] bool isBaseLoad(MOpcode opc) noexcept {
    return opc == MOpcode::LdrRegBaseImm || opc == MOpcode::Ldr8RegBaseImm ||
           opc == MOpcode::Ldr16RegBaseImm || opc == MOpcode::Ldr32RegBaseImm ||
           opc == MOpcode::LdrFprBaseImm;
}

[[nodiscard]] bool isOtherLoad(MOpcode opc) noexcept {
    return opc == MOpcode::LdrRegBaseRegLsl || opc == MOpcode::Ldr32RegBaseRegLsl ||
           opc == MOpcode::LdrFprBaseRegLsl;
}

/// @brief Stores whose address is x29-relative.
[[nodiscard]] bool isFrameStore(MOpcode opc) noexcept {
    return opc == MOpcode::StrRegFpImm || opc == MOpcode::Str8RegFpImm ||
           opc == MOpcode::Str16RegFpImm || opc == MOpcode::Str32RegFpImm ||
           opc == MOpcode::StrFprFpImm || opc == MOpcode::StpRegFpImm ||
           opc == MOpcode::StpFprFpImm;
}

[[nodiscard]] bool isBaseStore(MOpcode opc) noexcept {
    return opc == MOpcode::StrRegBaseImm || opc == MOpcode::Str8RegBaseImm ||
           opc == MOpcode::Str16RegBaseImm || opc == MOpcode::Str32RegBaseImm ||
           opc == MOpcode::StrFprBaseImm;
}

[[nodiscard]] bool isOtherStore(MOpcode opc) noexcept {
    return opc == MOpcode::StrRegSpImm || opc == MOpcode::StrFprSpImm ||
           opc == MOpcode::StrRegBaseRegLsl || opc == MOpcode::Str32RegBaseRegLsl ||
           opc == MOpcode::StrFprBaseRegLsl;
}

/// @brief Physical register named by operand @p idx, if any.
[[nodiscard]] std::optional<PhysReg> physAt(const MInstr &mi, std::size_t idx) noexcept {
    if (idx >= mi.ops.size() || mi.ops[idx].kind != MOperand::Kind::Reg)
        return std::nullopt;
    const MReg &reg = mi.ops[idx].reg;
    if (!reg.isPhys)
        return std::nullopt;
    return static_cast<PhysReg>(reg.idOrPhys);
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
CodegenStats computeCodegenStats(const MFunction &fn) {
    CodegenStats s;
    s.functions = 1;
    s.blocks = fn.blocks.size();
    s.frameBytes = fn.frame.totalBytes > 0 ? static_cast<std::size_t>(fn.frame.totalBytes) : 0;
    s.calleeSaved = fn.savedGPRs.size() + fn.savedFPRs.size();

    std::vector<int> offsets;
    offsets.reserve(fn.frame.spills.size());
    for (const auto &slot : fn.frame.spills)
        offsets.push_back(slot.offset);
    std::sort(offsets.begin(), offsets.end());
    s.spillSlots =
        static_cast<std::size_t>(std::unique(offsets.begin(), offsets.end()) - offsets.begin());

    for (const auto &block : fn.blocks) {
        s.instructions += block.instrs.size();
        // The register a `MovRI` just wrote, and the register the last
        // `MovRI xS,#off; AddRRR xS,x29,xS` prefix pointed into the frame.
        std::optional<PhysReg> lastMovRI;
        std::optional<PhysReg> frameScratch;
        for (const auto &mi : block.instrs) {
            const MOpcode opc = mi.opc;

            if (opc == MOpcode::AddRRR && lastMovRI) {
                const auto dst = physAt(mi, 0);
                const auto lhs = physAt(mi, 1);
                const auto rhs = physAt(mi, 2);
                if (dst && lhs && rhs && *dst == *lastMovRI && *lhs == PhysReg::X29 &&
                    *rhs == *lastMovRI) {
                    ++s.offsetPrefixes;
                    frameScratch = dst;
                    lastMovRI.reset();
                    continue;
                }
            }
            lastMovRI = opc == MOpcode::MovRI ? physAt(mi, 0) : std::nullopt;

            if (isCall(opc))
                ++s.calls;
            if (isBranch(opc))
                ++s.branches;
            if (isMove(opc))
                ++s.moves;

            const bool baseLoad = isBaseLoad(opc);
            const bool baseStore = isBaseStore(opc);
            const bool viaFrameScratch =
                (baseLoad || baseStore) && frameScratch && physAt(mi, 1) == frameScratch;
            if (viaFrameScratch)
                frameScratch.reset();

            if (isFrameLoad(opc) || baseLoad || isOtherLoad(opc)) {
                ++s.loads;
                if (isFrameLoad(opc) || viaFrameScratch)
                    ++s.frameLoads;
            }
            if (isFrameStore(opc) || baseStore || isOtherStore(opc)) {
                ++s.stores;
                if (isFrameStore(opc) || viaFrameScratch)
                    ++s.frameStores;
            }

            // Any other write to the scratch ends the prefix's reach.
            if (frameScratch && !viaFrameScratch && physAt(mi, 0) == frameScratch &&
                !(baseLoad || baseStore))
                frameScratch.reset();
        }
    }
    return s;
}

/// @copydoc formatCodegenStats
std::string formatCodegenStats(const CodegenStats &st, const std::string &name) {
    std::string out = "[codegen-stats] arch=arm64 fn=" + name;
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

} // namespace zanna::codegen::aarch64
