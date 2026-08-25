//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/codegen/x86_64/LowerILToMIR.hpp
// Purpose: Declare a bridge that adapts front-end IL to Machine IR.
// Key invariants:
//   - SSA identities are preserved during lowering.
//   - Virtual register IDs are assigned deterministically starting from 1.
//   - Block parameter copies are emitted as parallel copy pseudo-ops.
// Ownership/Lifetime:
//   - The adapter borrows IL inputs, constructs fresh MIR graphs by value,
//     and records call plans for later frame lowering.
// Links: src/codegen/x86_64/LowerILToMIR.cpp,
//        src/codegen/x86_64/MachineIR.hpp,
//        src/codegen/x86_64/LoweringRules.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "AsmEmitter.hpp"
#include "CallLowering.hpp"
#include "MachineIR.hpp"
#include "TargetX64.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * @file
 * @brief Declares the x86-64 adapter from compiler IL records to Machine IR.
 *
 * The adapter owns per-function SSA-to-vreg, block, call-plan, label, and stack
 * placeholder state while borrowing the target and literal pool. MIRBuilder is
 * the restricted facade passed to declarative lowering rules so rule code can
 * emit located instructions without directly manipulating adapter internals.
 */

namespace zanna::codegen::x64 {

/**
 * @brief Backend-facing, self-contained representation of an IL operand.
 *
 * Nonnegative @c id values identify SSA definitions; negative ids select the
 * appropriate inline payload. Only fields relevant to @c kind are meaningful.
 */
struct ILValue {
    /// @brief Value categories understood by the x86-64 lowering rules.
    enum class Kind { I64, F64, I1, PTR, LABEL, STR };

    Kind kind{Kind::I64};    ///< Static type of the value.
    int id{-1};              ///< SSA identifier (>= 0) or -1 for immediates.
    std::uint8_t bits{64};   ///< Integer scalar width when kind is I1/I64.
    double f64{0.0};         ///< Payload for floating constants.
    int64_t i64{0};          ///< Payload for integer constants.
    std::string label{};     ///< Payload for label references.
    std::string str{};       ///< Payload for string literal bytes.
    std::uint64_t strLen{0}; ///< Length in bytes for string literals.
};

/**
 * @brief Backend-facing IL instruction with result type and source location.
 */
struct ILInstr {
    std::string opcode{};                         ///< Mnemonic name of the IL opcode.
    std::vector<ILValue> ops{};                   ///< Ordered operands.
    int resultId{-1};                             ///< SSA identifier assigned to the result.
    ILValue::Kind resultKind{ILValue::Kind::I64}; ///< Static type of the result.
    std::uint8_t resultBits{64};                  ///< Integer scalar width from the source IL type.
    il::support::SourceLoc loc{};                 ///< Source location (for debug info).
};

/**
 * @brief Ordered IL block plus explicit block-parameter edge metadata.
 */
struct ILBlock {
    /**
     * @brief Arguments transferred along one terminator successor edge.
     *
     * @c argIds provides fast SSA lookup; @c argValues retains full constant
     * payloads for arguments that must be materialized in an edge-copy block.
     */
    struct EdgeArg {
        std::string to{};          ///< Destination block label.
        std::vector<int> argIds{}; ///< SSA ids mapped onto destination params (-1 for constants).
        std::vector<ILValue>
            argValues{}; ///< Full IL values for each argument (enables constant materialization).
    };

    std::string name{};                      ///< Block label.
    std::vector<ILInstr> instrs{};           ///< Instruction body.
    std::vector<int> paramIds{};             ///< SSA ids for block parameters.
    std::vector<ILValue::Kind> paramKinds{}; ///< Kinds for block parameters.
    std::vector<EdgeArg> terminatorEdges{};  ///< Successor edges for block terminators.
};

/// @brief Backend-facing IL function, including variadic ABI metadata.
struct ILFunction {
    std::string name{};            ///< Function symbol.
    std::vector<ILBlock> blocks{}; ///< Ordered blocks.
    bool isVarArg{false};          ///< True when the function uses a C-style variadic ABI.
};

/// @brief Ordered collection of backend-facing IL functions.
struct ILModule {
    std::vector<ILFunction> funcs{}; ///< Module-level functions.
};

/// @brief Stateful adapter that lowers backend-facing IL into Machine IR.
class LowerILToMIR;

/// \brief Thin façade that exposes MIR construction helpers to lowering rules.
/// \details Lowering rules receive a MIRBuilder bound to the current block and
///          use it to emit machine instructions, allocate virtual registers, and
///          convert IL values to MIR operands. The builder delegates to the
///          LowerILToMIR adapter for state management (vreg allocation, block
///          info lookup, call plan recording).
class MIRBuilder {
  public:
    /// @brief Construct a builder targeting a specific block within a lowering session.
    /// @param lower The owning LowerILToMIR adapter (provides vreg allocation, target info).
    /// @param block The machine basic block to append instructions to.
    MIRBuilder(LowerILToMIR &lower, MBasicBlock &block) noexcept;

    /// @brief Get the active machine basic block (mutable).
    /// @return Reference to the block being built.
    [[nodiscard]] MBasicBlock &block() noexcept;

    /// @brief Get the active machine basic block (const).
    /// @return Const reference to the block being built.
    [[nodiscard]] const MBasicBlock &block() const noexcept;

    /// @brief Get the owning LowerILToMIR adapter (mutable).
    /// @return Reference to the adapter managing the lowering session.
    [[nodiscard]] LowerILToMIR &lower() noexcept;

    /// @brief Get the owning LowerILToMIR adapter (const).
    /// @return Const reference to the adapter.
    [[nodiscard]] const LowerILToMIR &lower() const noexcept;

    /// @brief Get the target description (register classes, calling convention, etc.).
    /// @return Const reference to the TargetInfo for x86-64.
    [[nodiscard]] const TargetInfo &target() const noexcept;

    /// @brief Get the read-only data pool for string literals and constants.
    /// @return Reference to the RoDataPool managed by the assembly emitter.
    [[nodiscard]] AsmEmitter::RoDataPool &roData() const noexcept;

    /// @brief Map an IL value kind to the appropriate machine register class.
    /// @param kind The IL value kind (I64, F64, I1, PTR, etc.).
    /// @return The RegClass to use for values of this kind (GPR or XMM).
    [[nodiscard]] RegClass regClassFor(ILValue::Kind kind) const noexcept;

    /// @brief Get or create a virtual register for a given IL SSA identifier.
    /// @details If the SSA id already has a vreg allocated, returns it. Otherwise
    ///          allocates a new vreg of the appropriate register class.
    /// @param id The IL SSA identifier (>= 0).
    /// @param kind The IL value kind, used to determine the register class.
    /// @return The virtual register assigned to this SSA value.
    [[nodiscard]] VReg ensureVReg(int id, ILValue::Kind kind);

    /// @brief Allocate a fresh temporary virtual register.
    /// @param cls The register class for the temporary (GPR or XMM).
    /// @return A newly allocated virtual register.
    [[nodiscard]] VReg makeTempVReg(RegClass cls);

    /// @brief Convert an IL value to a MIR operand (register, immediate, or address).
    /// @details Handles immediates inline. For SSA references, ensures a vreg exists
    ///          and returns a register operand. For labels, returns a label operand.
    /// @param value The IL value to convert.
    /// @param cls The expected register class (used for vreg allocation).
    /// @return The MIR operand representing this value.
    [[nodiscard]] Operand makeOperandForValue(const ILValue &value, RegClass cls);

    /// @brief Try to convert an IL value to a MIR operand without mutating lowering state.
    /// @details This read-only variant is intended for speculative optimizations. It returns
    ///          existing SSA register mappings and literal operands that need no materialization,
    ///          but declines values that would allocate vregs, update literal pools, or append
    ///          helper instructions to the active block.
    /// @param value The IL value to inspect.
    /// @param cls The expected register class for any returned register operand.
    /// @return Operand when the value can be represented without side effects, otherwise nullopt.
    [[nodiscard]] std::optional<Operand> tryGetOperandForValue(const ILValue &value,
                                                               RegClass cls) const;

    /// @brief Convert an IL label value to a MIR label operand.
    /// @param value The IL value (must have kind == LABEL).
    /// @return A label operand referencing the named block.
    [[nodiscard]] Operand makeLabelOperand(const ILValue &value) const;

    /// @brief Check whether an IL value can be represented as an immediate operand.
    /// @param value The IL value to test.
    /// @return True if the value is an inline constant (no vreg needed).
    [[nodiscard]] bool isImmediate(const ILValue &value) const noexcept;

    /// @brief Set the source location to stamp onto subsequent MInstrs.
    /// @param loc The source location for instructions emitted by the current rule.
    void setCurrentLoc(il::support::SourceLoc loc) noexcept;

    /// @brief Append a machine instruction to the active basic block.
    /// @details Automatically stamps the current source location onto the instruction.
    /// @param instr The MInstr to append (moved into the block's instruction list).
    void append(MInstr instr);

    /// @brief Record a call lowering plan for later frame lowering.
    /// @details Call plans describe argument passing, return handling, and callee info.
    ///          They are collected during lowering and consumed by the frame builder.
    /// @param plan The call lowering plan to record.
    /// @return Stable index stored on the corresponding CALL instruction.
    [[nodiscard]] uint32_t recordCallPlan(CallLoweringPlan plan);

    /// @brief Reserve placeholder stack slots for a fixed-size alloca.
    /// @details Returns the negative %rbp-relative displacement placeholder that
    ///          FrameLowering will later shift below any callee-saved area.
    /// @param sizeBytes Requested allocation size in bytes.
    /// @param alignBytes Requested byte alignment, defaulting to one backend slot.
    /// @return Negative RBP-relative symbolic displacement.
    [[nodiscard]] int32_t reserveStackLocalPlaceholder(int sizeBytes,
                                                       int alignBytes = kSlotSizeBytes);

  private:
    /// Borrowed lowering adapter that outlives the builder.
    LowerILToMIR *lower_{nullptr};
    /// Borrowed destination block that outlives the builder.
    MBasicBlock *block_{nullptr};
    il::support::SourceLoc currentLoc_{}; ///< Loc stamped onto emitted MInstrs.
};

/// @brief Adapter that lowers temporary IL structures into Machine IR (MIR).
/// @details Processes each IL function by walking its blocks and instructions,
///          assigning virtual registers, emitting machine instructions via
///          lowering rules, and resolving block parameter copies as parallel
///          copy pseudo-ops. The resulting MFunction is ready for register
///          allocation and assembly emission.
class LowerILToMIR {
  public:
    /// @brief Construct a lowering adapter for the given target and rodata pool.
    /// @param target The x86-64 target description (registers, calling convention).
    /// @param roData The read-only data pool for string literals and constants.
    explicit LowerILToMIR(const TargetInfo &target, AsmEmitter::RoDataPool &roData) noexcept;

    /**
     * @brief Lower one IL function into a fresh Machine IR function.
     * @param func Source function borrowed for the duration of lowering.
     * @return Fully populated MIR blocks in deterministic IL order, followed by
     *         any synthetic edge-copy blocks.
     * @throws std::runtime_error Through @c phaseAUnsupported for malformed or
     *         unsupported IL.
     */
    [[nodiscard]] MFunction lower(const ILFunction &func);

    /**
     * @brief Retrieve call plans collected during the most recent lowering.
     * @return Borrowed plan vector whose indices match CALL plan identifiers.
     */
    [[nodiscard]] const std::vector<CallLoweringPlan> &callPlans() const noexcept;

    /// @brief Replace the set of direct-call targets that must use vararg call lowering.
    /// @param callees Symbol names moved into module-level lookup state.
    void setKnownVarArgCallees(std::unordered_set<std::string> callees);

    /// @brief Query whether @p callee is a user-defined variadic function in the current module.
    /// @param callee Direct-call symbol name.
    /// @return @c true when the symbol was registered as variadic.
    [[nodiscard]] bool isKnownVarArgCallee(std::string_view callee) const;

    /// @brief True when the string load producing @p resultId may skip its
    ///        defensive retain: its only use is an explicit release call
    ///        (see StringRetainPolicy.hpp).
    [[nodiscard]] bool isStrLoadRetainElidable(int resultId) const {
        return strLoadRetainElidable_.count(resultId) != 0;
    }

    /// @brief True when the string call result @p resultId may skip its
    ///        defensive retain: the callee transfers ownership and the
    ///        result's reference-spending uses fit in that one transferred
    ///        reference (see StringRetainPolicy.hpp).
    [[nodiscard]] bool isStrCallRetainElidable(int resultId) const {
        return strCallRetainElidable_.count(resultId) != 0;
    }

    /// @brief Return the next per-function local label id and advance the counter.
    /// @details Used for internal labels (trap sites, conversion branches) that need
    ///          unique names within a function. Resets to 0 at the start of each function
    ///          to ensure deterministic output across compilations.
    [[nodiscard]] uint32_t nextLocalLabelId() noexcept;

    /// @brief Record that @p id is an alloca result (a provably mapped stack address).
    /// @details Loads/stores through such bases skip the null-page guard.
    void noteAllocaResult(int id) {
        allocaResultIds_.insert(id);
    }

    /// @brief Instruction producing SSA id @p id in the current function.
    /// @details Used by the null-address guard to chase a derived address
    ///          (gep / pointer-add chain) back to its root pointer.
    /// @return Borrowed pointer into the function being lowered, or nullptr.
    [[nodiscard]] const ILInstr *ilProducerOf(int id) const {
        const auto it = ilProducers_.find(id);
        return it == ilProducers_.end() ? nullptr : it->second;
    }

    /// @brief True when @p id names an alloca result in the current function.
    [[nodiscard]] bool isAllocaResult(int id) const {
        return allocaResultIds_.count(id) != 0;
    }

    /// @brief Record a null-page guard for @p vregId inside @p blockLabel.
    /// @details Per-block scoping means a guard is only elided when
    ///          straight-line execution guarantees an earlier guard on the
    ///          same vreg already ran; no dominance analysis is required.
    /// @return `false` when the vreg was already guarded in that block.
    [[nodiscard]] bool noteNullGuardedBase(const std::string &blockLabel, uint16_t vregId) {
        return nullGuardedBases_[blockLabel].insert(vregId).second;
    }

    /// @brief The per-function null trap block label; marks the block as required.
    /// @details `lower()` appends the block (CALL rt_trap_null; UD2) after the
    ///          main lowering loop when any guard referenced it.
    [[nodiscard]] std::string requestNullTrapLabel() {
        nullTrapRequested_ = true;
        return ".Ltrap_null_" + currentFunctionName_;
    }

    friend class MIRBuilder;

  private:
    /// @brief MIR block index and destination registers for its IL parameters.
    struct BlockInfo {
        std::size_t index{0};           ///< Index within the MIR function.
        std::vector<VReg> paramVRegs{}; ///< Destination vregs for block params.
    };

    /// Borrowed target descriptor.
    const TargetInfo *target_{nullptr};
    /// Next per-function virtual-register identifier.
    uint16_t nextVReg_{1};
    /// IL SSA identifier to MIR virtual-register mapping.
    std::unordered_map<int, VReg> valueToVReg_{};
    /// IL block name to pre-created MIR block metadata.
    std::unordered_map<std::string, BlockInfo> blockInfo_{};
    /// Plans consumed later by target call lowering.
    std::vector<CallLoweringPlan> callPlans_{};
    /// Borrowed module-level literal pool.
    AsmEmitter::RoDataPool *roDataPool_{nullptr};
    /// Module-wide counter used to keep internal assembly labels unique.
    uint32_t nextLocalLabel_{0};
    /// Next symbolic fixed-size alloca slot within the current function.
    int nextStackLocalSlot_{0};
    /// Direct-call symbols declared variadic in the current module.
    std::unordered_set<std::string> knownVarArgCallees_{};
    /// String load results whose defensive retain may be omitted.
    std::unordered_set<int> strLoadRetainElidable_{};
    /// Transferring string call results whose defensive retain may be omitted.
    std::unordered_set<int> strCallRetainElidable_{};
    /// Name of the function currently being lowered (null trap label suffix).
    std::string currentFunctionName_{};
    /// Alloca result ids in the current function (bases that skip null guards).
    std::unordered_set<int> allocaResultIds_{};
    /// SSA id -> producing instruction for the current function (borrowed).
    std::unordered_map<int, const ILInstr *> ilProducers_{};
    /// Base vregs already null-guarded, per MIR block label.
    std::unordered_map<std::string, std::unordered_set<uint16_t>> nullGuardedBases_{};
    /// True once any load/store guard referenced the null trap block.
    bool nullTrapRequested_{false};

    /// @brief Populate strLoadRetainElidable_ for @p func (see StringRetainPolicy.hpp).
    /// @param func Function whose string ownership uses are analyzed.
    void computeStrLoadRetainElidable(const ILFunction &func);

    /// @brief Reset all per-function lowering state (vreg counter, maps, call plans).
    void resetFunctionState();

    /// @brief Record a call plan and return the stable plan identifier.
    /// @param plan Plan moved into the per-function plan table.
    /// @return Zero-based plan index.
    [[nodiscard]] uint32_t recordCallPlan(CallLoweringPlan plan);

    /// @brief Reserve placeholder stack slots for a fixed-size alloca.
    /// @param sizeBytes Requested allocation size.
    /// @param alignBytes Required byte alignment.
    /// @return Negative RBP-relative symbolic displacement.
    [[nodiscard]] int32_t reserveStackLocalPlaceholder(int sizeBytes, int alignBytes);

    /// @brief Map an IL value kind to the appropriate machine register class.
    /// @param kind The IL value kind.
    /// @return RegClass::GPR for integer/pointer/bool, RegClass::XMM for floats.
    [[nodiscard]] static RegClass regClassFor(ILValue::Kind kind) noexcept;

    /// @brief Get or create a virtual register for a given IL SSA identifier.
    /// @param id The IL SSA identifier (>= 0).
    /// @param kind The IL value kind for register class selection.
    /// @return The virtual register mapped to this SSA id.
    [[nodiscard]] VReg ensureVReg(int id, ILValue::Kind kind);

    /// @brief Allocate a fresh temporary virtual register in the given class.
    /// @param cls The register class (GPR or XMM).
    /// @return A newly allocated virtual register.
    [[nodiscard]] VReg makeTempVReg(RegClass cls);

    /// @brief Convert an IL value to a MIR operand within a specific block context.
    /// @param block The machine block for context (used for immediate materialization).
    /// @param value The IL value to convert.
    /// @param cls The expected register class.
    /// @return The MIR operand representing this value.
    [[nodiscard]] Operand makeOperandForValue(MBasicBlock &block,
                                              const ILValue &value,
                                              RegClass cls);

    /// @brief Look up a MIR operand for @p value without allocating or emitting.
    /// @details Used by speculative folds that must be side-effect free. Existing SSA
    ///          mappings and integer immediates are returned directly; string/floating
    ///          literals and unknown SSA ids return nullopt because representing them would
    ///          mutate the lowering adapter or current block.
    [[nodiscard]] std::optional<Operand> tryGetOperandForValue(const ILValue &value,
                                                               RegClass cls) const;

    /// @brief Convert an IL label value to a MIR label operand.
    /// @param value The IL value (must be a LABEL kind).
    /// @return A label operand for the named block.
    [[nodiscard]] Operand makeLabelOperand(const ILValue &value) const;

    /// @brief Check if an IL value is an inline immediate (no register needed).
    /// @param value The IL value to test.
    /// @return True if the value can be encoded as an immediate operand.
    [[nodiscard]] bool isImmediate(const ILValue &value) const noexcept;

    /// @brief Materialize one successor edge through a dedicated copy block when needed.
    /// @details Builds a synthetic block that executes any required PX_COPY argument moves and
    ///          then jumps to the real destination. Returns an empty string when the edge can
    ///          branch directly without a helper block.
    /// @param func Machine function receiving any new edge block.
    /// @param edge Successor edge metadata carrying destination params and arguments.
    /// @param sourceBlock Source machine block whose terminator will target the helper block.
    /// @return Helper block label or empty string when no helper block was required.
    [[nodiscard]] std::string buildEdgeCopyBlock(MFunction &func,
                                                 const ILBlock::EdgeArg &edge,
                                                 const MBasicBlock &sourceBlock);
};

} // namespace zanna::codegen::x64
