//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: tests/codegen/x86_64/test_regalloc_consistency.cpp
// Purpose: Integration tests validating register allocation outputs against
//          a simple function (no spills, deterministic assignment) and a
//          pressure function (spill slots and spill stores present).
// Key invariants:
//   - The simple function gets two registers and no slot; the pressure
//     function (15 simultaneously live values on a 12-register pool) spills.
// Ownership/Lifetime: Standalone test binary.
// Links: src/codegen/x86_64/RegAllocLinear.hpp, src/codegen/x86_64/ra/GlobalAllocator.hpp
//
//===----------------------------------------------------------------------===//

#include "codegen/x86_64/MachineIR.hpp"
#include "codegen/x86_64/RegAllocLinear.hpp"
#include "codegen/x86_64/TargetX64.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <variant>

using namespace zanna::codegen::x64;

namespace {

[[nodiscard]] MInstr makeMovImm(uint16_t id, int64_t value) {
    return MInstr::make(MOpcode::MOVri,
                        {makeVRegOperand(RegClass::GPR, id), makeImmOperand(value)});
}

[[nodiscard]] MInstr makeAdd(uint16_t dst, uint16_t rhs) {
    return MInstr::make(MOpcode::ADDrr,
                        {makeVRegOperand(RegClass::GPR, dst), makeVRegOperand(RegClass::GPR, rhs)});
}

[[nodiscard]] MInstr makeRet(uint16_t src) {
    return MInstr::make(MOpcode::MOVrr,
                        {makePhysRegOperand(RegClass::GPR, static_cast<uint16_t>(PhysReg::RAX)),
                         makeVRegOperand(RegClass::GPR, src)});
}

void addSimpleFunction(MFunction &func) {
    MBasicBlock block{};
    block.label = "simple";
    block.instructions.push_back(makeMovImm(1, 10));
    block.instructions.push_back(makeMovImm(2, 20));
    block.instructions.push_back(makeAdd(1, 2));
    block.instructions.push_back(makeRet(1));
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));
}

void addPressureFunction(MFunction &func) {
    MBasicBlock block{};
    block.label = "pressure";
    // Define 15 vregs, then read them all so they are simultaneously live.
    for (uint16_t id = 1; id <= 15; ++id) {
        block.instructions.push_back(makeMovImm(id, static_cast<int64_t>(id)));
    }
    for (uint16_t id = 2; id <= 15; ++id) {
        block.instructions.push_back(makeAdd(1, id)); // v1 += v<id>
    }
    block.instructions.push_back(makeRet(1));
    block.instructions.push_back(MInstr::make(MOpcode::RET, {}));
    func.blocks.push_back(std::move(block));
}

} // namespace

int main() {
    const TargetInfo &target = sysvTarget();

    MFunction simple{};
    addSimpleFunction(simple);
    auto simpleResult = allocate(simple, target);
    if (simpleResult.vregToPhys.size() != 2U) {
        std::cerr << "Simple allocation: expected 2 vregs\n";
        return EXIT_FAILURE;
    }
    // v1 is hinted to RAX by the move that returns it; v2 gets another register.
    if (simpleResult.vregToPhys[1] != PhysReg::RAX ||
        simpleResult.vregToPhys[2] == simpleResult.vregToPhys[1]) {
        std::cerr << "Simple allocation: unexpected vreg assignments\n";
        return EXIT_FAILURE;
    }
    if (simpleResult.spillSlotsGPR != 0) {
        std::cerr << "Simple allocation: expected 0 spill slots\n";
        return EXIT_FAILURE;
    }

    // Pressure test: 15 simultaneously live values on the 12 allocatable
    // GPRs (RSP, RBP, R10, R11 are reserved) must spill at least three.
    MFunction pressure{};
    addPressureFunction(pressure);
    auto pressureResult = allocate(pressure, target);

    if (pressureResult.spillSlotsGPR < 3) {
        std::cerr << "Pressure allocation: expected at least 3 spill slots, got "
                  << pressureResult.spillSlotsGPR << "\n";
        std::cerr << "  vregToPhys.size() = " << pressureResult.vregToPhys.size() << "\n";
        return EXIT_FAILURE;
    }

    // Verify spill stores and reloads were actually emitted.
    const auto &pressureBlock = pressure.blocks.front().instructions;
    const bool hasSpillStore =
        std::any_of(pressureBlock.begin(), pressureBlock.end(), [](const MInstr &instr) {
            return instr.opcode == MOpcode::MOVrm && instr.operands.size() == 2 &&
                   std::holds_alternative<OpMem>(instr.operands[0]);
        });
    const bool hasReload =
        std::any_of(pressureBlock.begin(), pressureBlock.end(), [](const MInstr &instr) {
            return instr.opcode == MOpcode::MOVmr && instr.operands.size() == 2 &&
                   std::holds_alternative<OpMem>(instr.operands[1]);
        });
    if (!hasSpillStore || !hasReload) {
        std::cerr << "Pressure allocation: expected spill store and reload\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
