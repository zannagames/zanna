//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: il/verify/EhModel.cpp
// Purpose: Implement the verifier's exception-handling CFG model.
// Key invariants:
//   - The model never mutates the function it describes.
//   - Handler and continuation token parameters retain distinct classifications.
// Ownership/Lifetime:
//   - Stored pointers and string views borrow storage from the source function.
//   - The caller keeps that function alive for the complete model lifetime.
// Links: il/verify/EhModel.hpp, il/verify/EhChecks.cpp,
//        docs/adr/0005-resume-token-provenance.md
//
//===----------------------------------------------------------------------===//

/// @file
/// @brief Implements construction and queries for the verifier's EH CFG model.
/// @details Construction performs all function-local indexing once so
///          path-sensitive EH checks can use resolved block and push-site
///          pointers while leaving the borrowed IL unchanged.

#include "il/verify/EhModel.hpp"

#include "il/verify/ControlFlowChecker.hpp"

#include <algorithm>
#include <cstddef>

using namespace il::core;

namespace il::verify {

/// @brief Capture exception-handling structure for @p function.
/// @details Builds label lookups for all basic blocks and records the entry
///          block so later analyses can answer reachability queries without
///          recomputing metadata.  The model stores raw pointers into the
///          original function and therefore must not outlive it. It also
///          identifies whether any EH-relevant opcode is present and records
///          each labelled `eh.push` with its resolved handler when available.
/// @param function Function whose EH layout should be modelled.
EhModel::EhModel(const Function &function) : fn(&function) {
    if (!function.blocks.empty())
        entryBlock = &function.blocks.front();

    blocks.reserve(function.blocks.size());
    for (const auto &block : function.blocks) {
        // Use emplace with string_view key referencing block.label.
        // The Function must outlive this EhModel for the view to remain valid.
        blocks.emplace(std::string_view{block.label}, &block);
    }

    for (const auto &block : function.blocks) {
        if (hasEh)
            break;

        for (const auto &instr : block.instructions) {
            switch (instr.op) {
                case Opcode::EhPush:
                case Opcode::EhPop:
                case Opcode::EhEntry:
                case Opcode::ResumeSame:
                case Opcode::ResumeNext:
                case Opcode::ResumeLabel:
                case Opcode::Trap:
                case Opcode::TrapFromErr:
                    hasEh = true;
                    break;
                default:
                    break;
            }
            if (hasEh)
                break;
        }
    }

    for (const auto &block : function.blocks) {
        for (const auto &instr : block.instructions) {
            if (instr.op != Opcode::EhPush || instr.labels.empty())
                continue;

            EhHandlerPushSite site;
            site.id = handlerPushSites.size();
            site.block = &block;
            site.instr = &instr;
            site.handler = findBlock(instr.labels[0]);
            pushSiteByInstr.emplace(&instr, site.id);
            handlerPushSites.push_back(site);
        }
    }
}

/// @brief Locate a basic block by its label.
/// @details Consults the pre-built label map and returns the corresponding
///          basic-block pointer when it exists.  Missing labels yield @c nullptr
///          so callers can report diagnostics without dereferencing invalid
///          pointers.
/// @param label Name of the basic block to retrieve.
/// @return Pointer to the block when present, otherwise nullptr.
const BasicBlock *EhModel::findBlock(std::string_view label) const {
    auto it = blocks.find(label);
    if (it == blocks.end())
        return nullptr;
    return it->second;
}

/// @brief Enumerate successor blocks referenced by a terminator instruction.
/// @details Handles `br`, `cbr`, `switch.i32`, and `resume.label`. Labels are
///          resolved through @ref findBlock so downstream checks receive direct
///          block pointers. Missing labels and terminators without explicit CFG
///          successors are omitted.
/// @param terminator Terminator instruction whose outgoing edges are requested.
/// @return Vector containing zero or more successor block pointers.
std::vector<const BasicBlock *> EhModel::gatherSuccessors(const Instr &terminator) const {
    std::vector<const BasicBlock *> successors;
    for (const EhSuccessorEdge &edge : gatherSuccessorEdges(terminator)) {
        if (edge.target)
            successors.push_back(edge.target);
    }
    return successors;
}

/// @brief Enumerate resolved EH-aware successor edges for @p terminator.
/// @details This variant preserves the label index so callers can inspect the
///          corresponding branch argument bundle and distinguish ordinary
///          control flow from `resume.label` transfers.
/// @param terminator Terminator whose supported labelled edges are resolved.
/// @return Resolved edges in source-label order; malformed labels are omitted.
std::vector<EhSuccessorEdge> EhModel::gatherSuccessorEdges(const Instr &terminator) const {
    std::vector<EhSuccessorEdge> successors;
    /// @brief Resolve and append one label-indexed edge when possible.
    /// @param labelIndex Index into @p terminator's label vector.
    /// @param kind Semantic classification assigned to the edge.
    auto addEdge = [&](std::size_t labelIndex, EhEdgeKind kind) {
        if (labelIndex >= terminator.labels.size())
            return;
        if (const BasicBlock *target = findBlock(terminator.labels[labelIndex]))
            successors.push_back(EhSuccessorEdge{target, labelIndex, kind});
    };

    switch (terminator.op) {
        case Opcode::Br:
            addEdge(0, EhEdgeKind::Normal);
            break;
        case Opcode::CBr:
        case Opcode::SwitchI32:
            for (std::size_t labelIndex = 0; labelIndex < terminator.labels.size(); ++labelIndex)
                addEdge(labelIndex, EhEdgeKind::Normal);
            break;
        case Opcode::ResumeLabel:
            addEdge(0, EhEdgeKind::Resume);
            break;
        default:
            break;
    }
    return successors;
}

/// @brief Retrieve the terminator instruction for @p block.
/// @details Scans the block's instruction list and returns the first
///          instruction classified as a terminator.  Non-terminating blocks
///          yield @c nullptr, allowing callers to differentiate between
///          fallthrough and explicit control transfers.
/// @param block Basic block whose terminator is requested.
/// @return Pointer to the terminator instruction, or nullptr when absent.
const Instr *EhModel::findTerminator(const BasicBlock &block) const {
    for (const auto &instr : block.instructions) {
        if (isTerminator(instr.op))
            return &instr;
    }
    return nullptr;
}

/// @brief Identify handler-shaped blocks by their leading marker.
/// @details Signature validation is performed elsewhere; this helper is a
///          lightweight CFG classifier for EH provenance and edge checks.
/// @param block Block whose first instruction is inspected.
/// @return `true` when @p block begins with `eh.entry`.
bool EhModel::isHandlerBlock(const BasicBlock &block) const noexcept {
    return !block.instructions.empty() && block.instructions.front().op == Opcode::EhEntry;
}

/// @brief Return the resume-token parameter id for handler-compatible blocks.
/// @details The block must begin with `eh.entry` and its first two parameters
///          must be `Error` and `ResumeTok`; later parameters are ignored.
///          Helper-shaped or malformed blocks return no value so callers can
///          defer to existing structural diagnostics.
/// @param block Candidate handler block whose parameter prefix is inspected.
/// @return Identifier of the second parameter, or no value when incompatible.
std::optional<unsigned> EhModel::handlerResumeTokenParam(const BasicBlock &block) const noexcept {
    if (!isHandlerBlock(block) || block.params.size() < 2)
        return std::nullopt;
    if (block.params[0].type.kind != Type::Kind::Error ||
        block.params[1].type.kind != Type::Kind::ResumeTok)
        return std::nullopt;
    return block.params[1].id;
}

/// @brief Find the unique resume-token parameter accepted by a CFG continuation.
/// @details Ordinary continuation blocks can carry the active capability under
///          ADR 0005. Returning no value for multiple token parameters keeps
///          malformed or ambiguous blocks from changing provenance state here;
///          structural verification reports their independent diagnostics.
/// @param block Block whose parameter types are scanned.
/// @return Unique `ResumeTok` parameter identifier, or no value for zero or
///         multiple matches.
std::optional<unsigned> EhModel::resumeTokenParam(const BasicBlock &block) const noexcept {
    std::optional<unsigned> tokenId;
    for (const Param &param : block.params) {
        if (param.type.kind != Type::Kind::ResumeTok)
            continue;
        if (tokenId)
            return std::nullopt;
        tokenId = param.id;
    }
    return tokenId;
}

/// @brief Look up push-site metadata for a known instruction address.
/// @details The model indexes `eh.push` instructions during construction using
///          addresses from the borrowed function. A missing entry means the
///          instruction was not a well-formed push in this model.
/// @param instr Borrowed instruction whose address is used as the lookup key.
/// @return Pointer into the model's push-site vector, or null when absent.
const EhHandlerPushSite *EhModel::findPushSite(const Instr &instr) const noexcept {
    auto it = pushSiteByInstr.find(&instr);
    if (it == pushSiteByInstr.end() || it->second >= handlerPushSites.size())
        return nullptr;
    return &handlerPushSites[it->second];
}

} // namespace il::verify
