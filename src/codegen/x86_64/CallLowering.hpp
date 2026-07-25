//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/CallLowering.hpp
// Purpose: Declare utilities that translate high-level call descriptions into
//          Machine IR argument-setup sequences for the selected x86-64 ABI.
// Key invariants:
//   - Register and stack placement comes from TargetInfo and CallArgLayout.
//   - Argument setup is inserted before an existing CALL pseudo.
//   - FrameInfo records the maximum frame-resident outgoing argument area.
// Ownership/Lifetime:
//   - Callers retain ownership of Machine IR structures and frame info;
//     lowerCall mutates block in-place.
// Links: src/codegen/x86_64/CallLowering.cpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/TargetX64.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "MachineIR.hpp"
#include "TargetX64.hpp"
#include "codegen/common/CallArgLayout.hpp"
#include "codegen/common/CallLoweringPlan.hpp"

#include <cstddef>

/**
 * @file
 * @brief Declares x86-64 ABI lowering for the arguments of a planned call.
 *
 * Call planning and ABI-neutral argument layout live in the common codegen
 * layer. This header exposes those common plan types in the x86-64 namespace
 * and declares the target-specific step that materializes argument registers,
 * stack stores, variadic metadata, and outgoing-frame requirements.
 */

namespace zanna::codegen::x64 {

/// @brief Stack-frame summary updated by call lowering.
struct FrameInfo;

/// @name ABI-neutral call-planning aliases
/// @{
/// @brief Description of one source-level call argument.
using CallArg = zanna::codegen::common::CallArg;
/// @brief Register-bank classification assigned to an argument.
using CallArgClass = zanna::codegen::common::CallArgClass;
/// @brief Scalar or aggregate kind carried by an argument.
using CallArgKind = zanna::codegen::common::CallArgKind;
/// @brief Complete ABI placement calculated for a call's arguments.
using CallArgLayout = zanna::codegen::common::CallArgLayout;
/// @brief Placement of one scalar argument or aggregate chunk.
using CallArgLocation = zanna::codegen::common::CallArgLocation;
/// @brief ABI constraints supplied to the common layout planner.
using CallArgLayoutConfig = zanna::codegen::common::CallArgLayoutConfig;
/// @brief Lowering metadata associated with a placeholder CALL instruction.
using CallLoweringPlan = zanna::codegen::common::CallLoweringPlan;
/// @brief Policy for independent versus position-shared register banks.
using CallSlotModel = zanna::codegen::common::CallSlotModel;
/// @brief ABI strategy used to pass an aggregate value.
using AggregatePassKind = zanna::codegen::common::AggregatePassKind;
/// @}

/**
 * @brief Insert ABI-specific argument preparation before a placeholder call.
 *
 * The routine computes argument locations from @p target, copies scalar and
 * aggregate values into ABI registers or frame-resident outgoing stack slots,
 * and emits the required SysV or Win64 variadic metadata. The CALL instruction
 * itself already exists in @p block and is not inserted or replaced here.
 *
 * @param block Basic block that owns the existing call and receives setup MIR.
 * @param insertIdx Index immediately before which the first setup instruction
 *        is inserted.
 * @param plan Argument values, aggregate metadata, and variadic-call policy.
 * @param target ABI register orders, capacities, and shadow-space size.
 * @param frame Frame summary updated with the maximum outgoing argument area.
 * @throws std::out_of_range If @p insertIdx is beyond the instruction vector.
 * @throws std::invalid_argument If an aggregate chunk is described as immediate.
 * @throws std::length_error If an outgoing area or displacement is unrepresentable.
 */
void lowerCall(MBasicBlock &block,
               std::size_t insertIdx,
               const CallLoweringPlan &plan,
               const TargetInfo &target,
               FrameInfo &frame);

} // namespace zanna::codegen::x64
