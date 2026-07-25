//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/common/CallArgLayout.hpp
// Purpose: Shared ABI slot planning helpers for native backends.
//
//===----------------------------------------------------------------------===//

#pragma once

#include "codegen/common/CallLoweringPlan.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

/// @file
/// @brief Declares target-configurable ABI argument-slot planning.

namespace zanna::codegen::common {

/// @brief How an ABI consumes register argument slots.
enum class CallSlotModel : uint8_t {
    IndependentRegisterBanks, ///< Separate integer and FP register banks.
    UnifiedRegisterPositions, ///< One positional register window shared by both classes.
};

/// @brief Configures register windows and aggregate policy for one target ABI.
///
/// Register capacities are counts, not physical register identifiers. Under a
/// unified model, the usable positional window is the smaller of the GPR and
/// FPR capacities. Direct aggregates are divided into eight-byte GPR chunks.
struct CallArgLayoutConfig {
    std::size_t maxGPRArgs{0}; ///< Maximum GPR arguments passed in registers.
    std::size_t maxFPRArgs{0}; ///< Maximum FP arguments passed in registers.
    CallSlotModel slotModel{CallSlotModel::IndependentRegisterBanks};
    bool variadicTailOnStack{false}; ///< True when variadic args must spill to the stack.
    std::size_t numNamedArgs{0};     ///< Number of non-variadic arguments.
    std::size_t maxDirectAggregateRegisterBytes{
        16}; ///< Direct aggregate byte limit eligible for register chunks.
};

/// @brief Describes one scalar argument or direct-aggregate chunk after assignment.
///
/// A register location uses `regIndex`; a stack location uses
/// `stackSlotIndex`. Aggregate arguments may produce multiple entries sharing
/// `argIndex`, ordered by `partIndex`.
struct CallArgLocation {
    std::size_t argIndex{0}; ///< Source-order argument index.
    CallArgClass cls{CallArgClass::GPR};
    bool isVariadic{false};        ///< True when this argument is part of a variadic tail.
    bool inRegister{false};        ///< True when the argument is passed in a register.
    std::size_t regIndex{0};       ///< Class-specific or positional register slot index.
    std::size_t stackSlotIndex{0}; ///< 0-based stack slot index among spilled arguments.
    bool isAggregatePart{false};   ///< True when this location copies one aggregate chunk.
    std::size_t partIndex{0};      ///< 0-based aggregate chunk index, or 0 for scalars.
    std::size_t byteOffset{0};     ///< Byte offset within aggregate storage.
    std::size_t byteSize{8};       ///< Number of bytes represented by this ABI location.
};

/// @brief Owns all assigned locations and aggregate resource counts.
struct CallArgLayout {
    std::vector<CallArgLocation> locations{};
    std::size_t gprRegsUsed{0};
    std::size_t fprRegsUsed{0};
    std::size_t registerPositionsUsed{0};
    std::size_t stackSlotsUsed{0};
};

/// @brief Assign outgoing abstract call arguments to ABI register or stack slots.
///
/// Scalar and indirect-aggregate arguments consume one slot. Direct aggregates
/// are chunked into at most eight-byte pieces and placed wholly in registers
/// only when the complete non-variadic group fits; otherwise all chunks use
/// consecutive stack slots.
///
/// @param args Source-order call arguments.
/// @param config Target ABI slot capacities and policy.
/// @return Ordered locations plus register and stack consumption totals.
/// @throws std::length_error if rounding an aggregate size to slots overflows.
CallArgLayout planCallArgs(std::span<const CallArg> args, const CallArgLayoutConfig &config);

/// @brief Assign incoming scalar parameter classes to ABI register or stack slots.
/// @param classes Source-order GPR/FPR parameter classifications.
/// @param config Target ABI slot capacities and variadic-tail policy.
/// @return One location per class plus register and stack consumption totals.
CallArgLayout planParamClasses(std::span<const CallArgClass> classes,
                               const CallArgLayoutConfig &config);

} // namespace zanna::codegen::common
