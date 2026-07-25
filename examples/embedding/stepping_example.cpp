//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: examples/embedding/stepping_example.cpp
// Purpose: Demonstrate instruction stepping followed by continuous VM execution.
// Key invariants:
//   - The generated entry block is terminated by an IL return instruction.
//   - continueRun() resumes the same Runner state advanced by step().
// Ownership/Lifetime:
//   - The IL module and RunConfig outlive the Runner that references them.
//   - All builder references remain valid while their owning module is alive.
// Links: include/zanna/vm/VM.hpp, docs/tools/debugging.md
//
//===----------------------------------------------------------------------===//

/**
 * @file stepping_example.cpp
 * @brief Demonstrates the Runner step-and-continue execution API.
 *
 * @details
 * The programmatically constructed IL entry point computes `40 + 2` and
 * returns the result. The host executes one VM instruction with `step()`,
 * inspects its status, then resumes the same execution state with
 * `continueRun()`. This is the minimal control flow used by debuggers and
 * other hosts that interleave VM work with external orchestration.
 */

#include "il/build/IRBuilder.hpp"
#include "zanna/vm/VM.hpp"
#include <iostream>

/**
 * @brief Build a two-instruction IL program and exercise step/resume execution.
 *
 * A module containing one addition and one return is created through
 * `IRBuilder`. The same Runner first executes a single step and then continues
 * until it reaches another terminal execution status. Both statuses are
 * written to standard output.
 *
 * @return Zero after the step and continuation attempts have completed.
 *
 * @note The example exposes execution failures through printed status values
 *       rather than mapping them to the process exit code.
 */
int main() {
  using namespace il::core;
  Module m;
  il::build::IRBuilder b(m);
  auto &fn = b.startFunction("main", Type(Type::Kind::I64), {});
  auto &bb = b.addBlock(fn, "entry");
  b.setInsertPoint(bb);
  // t0 = add 40, 2 ; ret t0
  Instr add;
  add.result = b.reserveTempId();
  add.op = Opcode::Add;
  add.type = Type(Type::Kind::I64);
  add.operands.push_back(Value::constInt(40));
  add.operands.push_back(Value::constInt(2));
  bb.instructions.push_back(add);
  Instr ret;
  ret.op = Opcode::Ret;
  ret.type = Type(Type::Kind::Void);
  ret.operands.push_back(Value::temp(*add.result));
  bb.instructions.push_back(ret);
  bb.terminated = true;

  il::vm::RunConfig cfg; // defaults
  il::vm::Runner runner(m, cfg);
  auto s1 = runner.step();
  std::cout << "Step status: " << static_cast<int>(s1.status) << "\n";
  auto rs = runner.continueRun();
  std::cout << "Run status: " << static_cast<int>(rs) << "\n";
  return 0;
}
