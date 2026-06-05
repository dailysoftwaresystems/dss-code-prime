#include "opt/analysis/call_graph_scc.hpp"

#include "mir/mir_opcode.hpp"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dss::opt::analysis {

namespace {

// A self-contained direct call graph over `MirFuncId.v` node ids. Built
// once from the module: `nodes` is the dense list of defined-function
// ids (Tarjan iterates these); `succ` maps a caller id to its callee
// ids (deduplicated — a multiplicity of calls to the same callee is one
// edge for SCC purposes). A SELF-edge (caller==callee) is KEPT: it is
// the structural marker of self-recursion, and Tarjan correctly makes
// such a node its own (cyclic) singleton component.
struct CallGraph {
    std::vector<std::uint32_t>                                  nodes;
    std::unordered_map<std::uint32_t, std::vector<std::uint32_t>> succ;
};

[[nodiscard]] CallGraph buildCallGraph(Mir const& mir) {
    CallGraph g;

    // (1) symbol-id → defined-function id. Mirrors the inliner's
    // `analyzeModule` resolution: a symbol the module does not define
    // (extern / library import) has NO entry, so a Call to it resolves
    // to no edge. DUPLICATE symbols (weak alias / hand-built fixture)
    // are dropped from the map (ambiguous → unresolvable, exactly like
    // the inliner) so no edge is guessed against a wrong body.
    std::size_t const nf = mir.moduleFuncCount();
    std::unordered_map<std::uint32_t, std::uint32_t> symToFunc;
    std::unordered_set<std::uint32_t> ambiguousSyms;
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const sv = mir.funcSymbol(f).v;
        if (sv == 0) continue;  // anonymous — never a call target
        auto const [it, inserted] = symToFunc.emplace(sv, f.v);
        if (!inserted) ambiguousSyms.insert(sv);
    }
    for (std::uint32_t sv : ambiguousSyms) symToFunc.erase(sv);

    // (2) one node per defined function + its outgoing direct-call edges.
    // Per-callee dedup keeps `succ` minimal (Tarjan is correct either
    // way, but a deduped graph is cheaper + the SCC result is identical).
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        g.nodes.push_back(f.v);
        std::unordered_set<std::uint32_t> seen;
        std::vector<std::uint32_t> edges;
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const id = mir.blockInstAt(b, ii);
                if (mir.instOpcode(id) != MirOpcode::Call) continue;
                auto const ops = mir.instOperands(id);
                if (ops.empty()) continue;  // malformed; verifier's job
                MirInstId const callee = ops[0];
                if (mir.instOpcode(callee) != MirOpcode::GlobalAddr) {
                    continue;  // indirect call — no static edge
                }
                std::uint32_t const calleeSym = mir.globalAddrSymbol(callee).v;
                auto const it = symToFunc.find(calleeSym);
                if (it == symToFunc.end()) continue;  // extern/ambiguous
                if (seen.insert(it->second).second) edges.push_back(it->second);
            }
        }
        g.succ.emplace(f.v, std::move(edges));
    }
    return g;
}

// A stable empty edge list for nodes absent from `succ` (defensive — the
// builder inserts an entry for every node, so this is belt-and-suspenders
// against a future builder change). Static-local so the reference outlives
// the loop iteration.
[[nodiscard]] std::vector<std::uint32_t> const& emptyEdges() {
    static std::vector<std::uint32_t> const empty;
    return empty;
}

// Iterative Tarjan SCC over the call graph. Explicit stacks (NOT native
// recursion) so a deep call chain can't blow the C++ stack — the inliner
// must never crash a user's build on a legal-but-deep module. Assigns a
// dense SCC id (in completion order) to every node; returns funcId.v →
// sccId. Singleton non-cyclic functions each get a unique id; a self-edge
// or a multi-member cycle collapses its members to one shared id.
[[nodiscard]] std::unordered_map<std::uint32_t, std::uint32_t>
tarjan(CallGraph const& g) {
    std::unordered_map<std::uint32_t, std::uint32_t> sccOf;

    // Per-node Tarjan bookkeeping, keyed by funcId.v.
    std::unordered_map<std::uint32_t, std::uint32_t> index;    // DFS discovery index
    std::unordered_map<std::uint32_t, std::uint32_t> lowlink;  // low-link value
    std::unordered_set<std::uint32_t>                onStack;  // currently on the SCC stack
    std::vector<std::uint32_t>                       sccStack; // the Tarjan node stack

    std::uint32_t nextIndex = 0;
    std::uint32_t nextScc   = 0;

    // Explicit DFS frame: the node + the position of the next successor
    // edge to consider when this frame resumes.
    struct Frame {
        std::uint32_t node;
        std::size_t   nextEdge;
    };

    for (std::uint32_t root : g.nodes) {
        if (index.count(root)) continue;  // already in a completed SCC tree

        std::vector<Frame> dfs;
        dfs.push_back({root, 0});
        index[root]   = nextIndex;
        lowlink[root] = nextIndex;
        ++nextIndex;
        sccStack.push_back(root);
        onStack.insert(root);

        while (!dfs.empty()) {
            Frame& fr = dfs.back();
            auto const succIt = g.succ.find(fr.node);
            std::vector<std::uint32_t> const& edges =
                succIt != g.succ.end() ? succIt->second
                                       : emptyEdges();

            if (fr.nextEdge < edges.size()) {
                std::uint32_t const w = edges[fr.nextEdge++];
                if (!index.count(w)) {
                    // Tree edge — descend into `w`.
                    index[w]   = nextIndex;
                    lowlink[w] = nextIndex;
                    ++nextIndex;
                    sccStack.push_back(w);
                    onStack.insert(w);
                    dfs.push_back({w, 0});
                } else if (onStack.count(w)) {
                    // Back/cross edge to a node still on the stack — it
                    // is in the current SCC; tighten this node's lowlink.
                    lowlink[fr.node] = std::min(lowlink[fr.node], index[w]);
                }
                continue;
            }

            // All edges of `fr.node` exhausted. If it is an SCC root
            // (lowlink == index), pop its component off the stack.
            if (lowlink[fr.node] == index[fr.node]) {
                std::uint32_t const sccId = nextScc++;
                while (true) {
                    std::uint32_t const m = sccStack.back();
                    sccStack.pop_back();
                    onStack.erase(m);
                    sccOf[m] = sccId;
                    if (m == fr.node) break;
                }
            }

            // Pop this frame; propagate its lowlink to the parent (the
            // node above it on the DFS stack), the iterative analog of
            // `lowlink[parent] = min(lowlink[parent], lowlink[child])`.
            std::uint32_t const finished = fr.node;
            dfs.pop_back();
            if (!dfs.empty()) {
                std::uint32_t const parent = dfs.back().node;
                lowlink[parent] = std::min(lowlink[parent], lowlink[finished]);
            }
        }
    }

    return sccOf;
}

} // namespace

std::unordered_map<std::uint32_t, std::uint32_t>
computeCallGraphSccs(Mir const& mir) {
    CallGraph const g = buildCallGraph(mir);
    return tarjan(g);
}

} // namespace dss::opt::analysis
