//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
/// @file
/// @brief Implements direct-call graph construction and SCC analysis.
/// @details Scans each function's blocks and instructions, records edges for
///          direct `call` instructions with resolved callee names, and
///          accumulates per-callee call counts. After building the edge set,
///          Tarjan's algorithm computes strongly connected components (SCCs)
///          in reverse topological order (callees before callers). This
///          enables correct bottom-up interprocedural analysis: process leaf
///          SCCs first so their results are available when processing callers.
//
//===----------------------------------------------------------------------===//

#include "il/analysis/CallGraph.hpp"

#include "il/core/Function.hpp"
#include "il/core/Instr.hpp"
#include "il/core/Module.hpp"
#include "il/core/Opcode.hpp"

#include <stack>
#include <unordered_set>

namespace zanna::analysis {

namespace {

/// @brief State for Tarjan's iterative SCC algorithm.
struct TarjanState {
    std::unordered_map<std::string, unsigned> index;
    std::unordered_map<std::string, unsigned> lowlink;
    std::unordered_set<std::string> onStack;
    std::stack<std::string> stack;
    unsigned nextIndex = 0;
};

/// @brief Iterative Tarjan's strongly connected components.
/// @details Uses an explicit call-stack to avoid recursion depth issues.
///          Produces SCCs in reverse topological order (emitted when a root
///          is found: leaf SCCs first, callers last).
/// @param start Name of the function to start DFS from.
/// @param edges Caller-to-callee adjacency list with unique topology edges.
/// @param allFunctions Set of all function names known to the module.
/// @param state Shared Tarjan state across multiple DFS starts.
/// @param sccs Output: SCCs appended in the order they are finalized.
void tarjanDFS(const std::string &start,
               const std::unordered_map<std::string, std::vector<std::string>> &edges,
               const std::unordered_set<std::string> &allFunctions,
               TarjanState &state,
               std::vector<std::vector<std::string>> &sccs) {
    // Iterative DFS using an explicit worklist.
    // Each stack frame records: (node, edge_iterator_index).
    struct Frame {
        std::string node;
        std::size_t edgeIdx{0}; // index into the callee list for this node
    };

    std::vector<Frame> callStack;
    callStack.push_back({start, 0});
    state.index[start] = state.nextIndex;
    state.lowlink[start] = state.nextIndex;
    ++state.nextIndex;
    state.stack.push(start);
    state.onStack.insert(start);

    while (!callStack.empty()) {
        Frame &frame = callStack.back();
        const std::string &v = frame.node;

        auto edgeIt = edges.find(v);
        bool advanced = false;

        if (edgeIt != edges.end()) {
            const auto &callees = edgeIt->second;
            while (frame.edgeIdx < callees.size()) {
                const std::string &w = callees[frame.edgeIdx];
                ++frame.edgeIdx;

                // Only follow edges to known module functions.
                if (allFunctions.find(w) == allFunctions.end())
                    continue;

                if (state.index.find(w) == state.index.end()) {
                    // Tree edge: recurse into w.
                    state.index[w] = state.nextIndex;
                    state.lowlink[w] = state.nextIndex;
                    ++state.nextIndex;
                    state.stack.push(w);
                    state.onStack.insert(w);
                    callStack.push_back({w, 0});
                    advanced = true;
                    break;
                } else if (state.onStack.count(w)) {
                    // Back edge: update lowlink.
                    state.lowlink[v] = std::min(state.lowlink[v], state.index[w]);
                }
            }
        }

        if (!advanced) {
            // All successors processed: check if v is an SCC root.
            if (state.lowlink[v] == state.index[v]) {
                std::vector<std::string> scc;
                while (true) {
                    std::string w = state.stack.top();
                    state.stack.pop();
                    state.onStack.erase(w);
                    scc.push_back(w);
                    if (w == v)
                        break;
                }
                sccs.push_back(std::move(scc));
            }
            // Save lowlink before popping — v is a reference into callStack
            // and becomes dangling after pop_back().
            const unsigned vLowlink = state.lowlink[v];
            callStack.pop_back();
            if (!callStack.empty()) {
                // Propagate lowlink upward to the parent frame.
                const std::string &parent = callStack.back().node;
                state.lowlink[parent] = std::min(state.lowlink[parent], vLowlink);
            }
        }
    }
}

} // namespace

/// @brief Test whether a function belongs to a recursive call-graph component.
/// @param fn Function name to query.
/// @return True for a multi-node SCC or a singleton with a self-edge.
bool CallGraph::isRecursive(const std::string &fn) const {
    auto it = sccIndex.find(fn);
    if (it == sccIndex.end())
        return false;
    const auto &scc = sccs[it->second];
    if (scc.size() > 1)
        return true;
    // Single-node SCC — check for a self-edge.
    auto edgeIt = edges.find(fn);
    if (edgeIt == edges.end())
        return false;
    for (const auto &callee : edgeIt->second)
        if (callee == fn)
            return true;
    return false;
}

/// @brief Build a direct-call graph for an IL module.
/// @details Walks every block and instruction in each function; when it finds a
///          direct call (`Opcode::Call` with a non-empty callee name) it appends
///          the callee to the caller's edge list and increments the callee's
///          call count. Indirect calls or unresolved callees are skipped, and
///          repeated call sites increase the count without duplicating topology edges.
///          After building the edge set, Tarjan's SCC algorithm is applied to
///          produce SCCs in reverse topological order.
/// @param module Module to scan; the IL is not modified.
/// @return Call graph with edges, call counts, and SCCs.
CallGraph buildCallGraph(il::core::Module &module) {
    CallGraph cg;

    // Collect all function names first (needed to filter external edges in SCC).
    std::unordered_set<std::string> allFunctions;
    allFunctions.reserve(module.functions.size());
    for (const auto &fn : module.functions)
        allFunctions.insert(fn.name);

    // Build unique topology edges and independent call-site counts.
    for (auto &fn : module.functions) {
        std::unordered_set<std::string> seenCallees;
        for (auto &B : fn.blocks) {
            for (auto &I : B.instructions) {
                if (I.isDirectCall()) {
                    ++cg.callCounts[I.callee];
                    if (seenCallees.insert(I.callee).second)
                        cg.edges[fn.name].push_back(I.callee);
                }
            }
        }
    }

    // Compute SCCs via Tarjan's algorithm.
    TarjanState state;
    state.index.reserve(module.functions.size());
    state.lowlink.reserve(module.functions.size());

    for (const auto &fn : module.functions) {
        if (state.index.find(fn.name) == state.index.end())
            tarjanDFS(fn.name, cg.edges, allFunctions, state, cg.sccs);
    }

    // Build sccIndex for O(1) SCC membership queries.
    cg.sccIndex.reserve(module.functions.size());
    for (std::size_t i = 0; i < cg.sccs.size(); ++i)
        for (const auto &name : cg.sccs[i])
            cg.sccIndex[name] = i;

    return cg;
}

} // namespace zanna::analysis
