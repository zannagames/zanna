//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: examples/embedding/register_times2.cpp
// Purpose: Show how an embedding host registers and invokes a native VM extern.
// Key invariants:
//   - The native handler, runtime signature, and IL declaration all use i64.
//   - Native argument and result storage is accessed only during the callback.
// Ownership/Lifetime:
//   - The module and RunConfig remain alive while the Runner executes.
//   - The VM owns callback storage; the handler borrows it for one invocation.
// Links: include/zanna/vm/RuntimeBridge.hpp, docs/il/il-guide.md
//
//===----------------------------------------------------------------------===//

/**
 * @file register_times2.cpp
 * @brief Demonstrate registering a simple extern with the VM runtime bridge.
 *
 * @details
 * The example describes a native `(i64) -> i64` function to the VM, declares
 * the matching extern in a programmatically constructed IL module, and invokes
 * it with the value 21. It illustrates the agreement required among an erased
 * native callback, an `ExternDesc` signature, and the IL-level declaration.
 */

#include "il/build/IRBuilder.hpp"
#include "zanna/vm/RuntimeBridge.hpp"
#include "zanna/vm/VM.hpp"
#include <cstdint>
#include <iostream>

/**
 * @brief Implement the native `times2` extern using the VM callback ABI.
 *
 * @param args Borrowed argument-vector storage. Element zero must point to a
 *             readable signed 64-bit integer.
 * @param result Borrowed output storage in which the signed 64-bit product is
 *               written.
 *
 * @pre `args`, `args[0]`, and `result` are non-null and correctly aligned.
 * @pre The active extern signature is exactly `(i64) -> i64`.
 * @post The result storage contains twice the value supplied through `args[0]`.
 *
 * @note The demonstration passes 21, so the multiplication cannot overflow.
 */
static void times2_handler(void **args, void *result) {
  const auto x = *reinterpret_cast<const int64_t *>(args[0]);
  *reinterpret_cast<int64_t *>(result) = x * 2;
}

/**
 * @brief Register `times2`, build a caller module, and execute it in the VM.
 *
 * The function creates a native extern descriptor and a matching IL extern
 * declaration, emits an IL `main` that returns `times2(21)`, and supplies the
 * descriptor through `RunConfig`. The final VM run status is printed to
 * standard output.
 *
 * @return Zero after the VM execution attempt has completed.
 *
 * @note VM failure is visible in the printed status; this compact embedding
 *       example does not translate that status into a nonzero process result.
 */
int main() {
  using il::runtime::signatures::SigParam;
  // Prepare extern description to inject via RunConfig.
  il::vm::ExternDesc ext;
  ext.name = "times2";
  ext.signature = il::runtime::signatures::make_signature(
      "times2", {SigParam::Kind::I64}, {SigParam::Kind::I64});
  ext.fn = reinterpret_cast<void *>(&times2_handler);

  // Build a tiny IL module that calls @times2(21) and returns the result.
  il::core::Module m;
  il::build::IRBuilder b(m);
  b.addExtern("times2", il::core::Type(il::core::Type::Kind::I64),
              {il::core::Type(il::core::Type::Kind::I64)});
  auto &fn = b.startFunction("main", il::core::Type(il::core::Type::Kind::I64), {});
  auto &bb = b.addBlock(fn, "entry");
  b.setInsertPoint(bb);
  auto c21 = il::core::Value::constInt(21);
  auto dst = b.reserveTempId();
  il::core::Instr call;
  call.result = dst;
  call.op = il::core::Opcode::Call;
  call.type = il::core::Type(il::core::Type::Kind::I64);
  call.callee = "times2";
  call.operands.push_back(c21);
  bb.instructions.push_back(call);
  il::core::Instr ret;
  ret.op = il::core::Opcode::Ret;
  ret.type = il::core::Type(il::core::Type::Kind::Void);
  ret.operands.push_back(il::core::Value::temp(dst));
  bb.instructions.push_back(ret);
  bb.terminated = true;

  il::vm::RunConfig cfg;
  cfg.externs.push_back(ext);
  il::vm::Runner r(m, cfg);
  auto status = r.continueRun();
  std::cout << "run status=" << static_cast<int>(status) << "\n";
  return 0;
}
