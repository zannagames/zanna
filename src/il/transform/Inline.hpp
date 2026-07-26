//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/transform/Inline.hpp
// Purpose: Lightweight direct-call inliner module pass with a configurable
//          cost model (instruction/block budgets, constant-argument bonus,
//          single-use bonus, code-growth cap, inline-depth limit). Clones
//          callee CFG into caller, remaps params, rewires returns.
// Key invariants:
//   - Recursive calls are never inlined.
//   - EH-sensitive callees are skipped.
//   - Total code growth is capped by maxCodeGrowth.
// Ownership/Lifetime: ModulePass instantiated by the registry; configuration
//          is held by value in InlineCostConfig.
// Links: il/transform/PassRegistry.hpp, il/analysis/CallGraph.hpp
//
//===----------------------------------------------------------------------===//

/**
 * @file
 * @brief Declares the configurable direct-call IL inliner module pass.
 *
 * @details The inliner evaluates non-recursive callees with a bounded cost
 *          model, clones supported CFGs into callers, remaps SSA values and
 *          block parameters, and joins returns through a continuation block.
 *          Configuration limits candidate size, nesting depth, call frequency,
 *          and aggregate module code growth.
 */

#pragma once

#include "il/transform/PassRegistry.hpp"

namespace il::transform {

/// @brief Configuration for the inline cost model.
struct InlineCostConfig {
    /// @brief Base instruction count threshold for inlining.
    /// Raised from 32 to 80 to capture medium-sized helper functions.
    unsigned instrThreshold = 80;

    /// @brief Maximum number of blocks in a callee.
    /// Limited to single-block callees until multi-block inlining value-flow
    /// issues are resolved (zannastudio, chess-zia crash at O1 with blockBudget>1).
    unsigned blockBudget = 1;

    /// @brief Maximum inline depth for nested inlining.
    /// Raised from 2 to 3 to allow deeper utility-function chains to collapse.
    unsigned maxInlineDepth = 3;

    /// @brief Bonus subtracted from cost for each constant argument.
    unsigned constArgBonus = 4;

    /// @brief Bonus for functions with one call site and subsequent DCE potential.
    unsigned singleUseBonus = 10;

    /// @brief Bonus for functions containing at most eight instructions.
    unsigned tinyFunctionBonus = 16;

    /// @brief Maximum total instruction-count growth allowed per module.
    /// Raised from 1000 to 2000 to allow more aggressive inlining in O2
    /// where multiple call sites benefit from constant-argument specialization.
    unsigned maxCodeGrowth = 2000;

    /// @brief Enable bounded repeated inlining rounds and aggressive settings.
    bool aggressive = false;

    /// @brief Require multi-block callees to have a single return continuation.
    bool requireSingleReturnForMultiBlock = true;
};

/// @brief Direct-call inliner module pass with a configurable cost model.
/// @details Scans for direct call sites that satisfy the cost model thresholds
///          (instruction budget, block budget, constant-argument bonuses, etc.),
///          clones the callee's CFG into the caller, remaps parameters to
///          arguments, and rewires return values. Recursive and EH-sensitive
///          callees are always skipped. Total code growth across the module is
///          capped by @ref InlineCostConfig::maxCodeGrowth.
class Inliner : public ModulePass {
  public:
    /// @brief Construct an inliner with default cost configuration.
    Inliner() = default;

    /// @brief Construct an inliner with a custom cost configuration.
    /// @param config Cost model thresholds controlling inlining decisions.
    explicit Inliner(const InlineCostConfig &config) : config_(config) {}

    /// @brief Return the pass identifier string ("inline").
    std::string_view id() const override;

    /// @brief Run the inliner over all functions in @p module.
    /// @param module Module containing functions to consider for inlining.
    /// @param analysis Analysis manager providing call graph and dominators.
    /// @return PreservedAnalyses indicating which analyses remain valid.
    PreservedAnalyses run(core::Module &module, AnalysisManager &analysis) override;

    /// @brief Override the instruction count threshold for inlining decisions.
    /// @param n New threshold (callee instructions must not exceed this).
    void setInstructionThreshold(unsigned n) {
        config_.instrThreshold = n;
    }

    /// @brief Replace the entire cost configuration.
    /// @param config New cost model configuration to use.
    void setConfig(const InlineCostConfig &config) {
        config_ = config;
    }

  private:
    /// @brief Cost-model configuration retained by value for each run.
    InlineCostConfig config_;
};

/// @brief Register the inliner pass with the provided registry.
/// @param registry PassRegistry to register the pass into.
void registerInlinePass(PassRegistry &registry);

} // namespace il::transform
