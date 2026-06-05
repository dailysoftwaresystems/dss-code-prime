// Call-graph SCC substrate unit tests (`computeCallGraphSccs`).
//
// The inliner's recursion-safety gate (OPT7 cycle 3) refuses to inline a
// call whose caller + callee share an SCC. These pins lock the SCC
// computation itself, standalone from the inliner:
//   (a) a 3-cycle A→B→C→A — all three share ONE scc id;
//   (b) an acyclic chain A→B→C — three DISTINCT scc ids;
//   (c) a self-edge A→A — A's own (singleton, cyclic) scc;
//   (d) a mix — a cycle {A,B} plus an acyclic tail C, D.
// Strict: exact scc-id equality where a cycle is expected, exact
// distinctness where acyclicity is expected. RED-on-disable: stubbing
// the SCC merge to identity (every node its own component) breaks (a),
// (c), and (d)'s cycle assertions — proving the cycle-collapse is the
// load-bearing behavior, not a vacuous all-distinct map.

#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "opt/analysis/call_graph_scc.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace dss;
using namespace dss::opt::analysis;

namespace {

// One function in the test call graph: a symbol id + the symbol ids of
// the functions it DIRECTLY calls (each emitted as a GlobalAddr+Call in
// its single entry block). A `callee == this` entry is a self-edge.
struct FuncSpec {
    std::uint32_t              symbol;
    std::vector<std::uint32_t> calls;
};

// Built module + the funcId.v of each function, keyed by its symbol id —
// the SCC map is keyed by funcId.v, so the assertions translate
// symbol→funcId.v through this.
struct Graph {
    Mir                                              mir;
    std::unordered_map<std::uint32_t, std::uint32_t> funcVBySymbol;
};

// Build a module from `specs`. Every function shares a nullary i32 fn
// signature; each emits, in its entry block, one `GlobalAddr(callee) +
// Call` per outgoing edge, then `return 0`. The GlobalAddr's symbol is
// the callee's symbol id, so `computeCallGraphSccs`'s internal
// symbol→func resolution recovers the edge.
Graph buildGraph(TypeInterner& interner, std::vector<FuncSpec> const& specs) {
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;

    Graph g;
    // First pass: declare every function so a forward call (e.g. A calls
    // B declared later) still names a real symbol. MirBuilder needs each
    // function's block filled before the next addFunction, so we declare
    // + fill in one pass but reference callees purely by SYMBOL id (the
    // GlobalAddr carries the symbol, not a function handle), which does
    // not require the callee to exist yet.
    for (FuncSpec const& s : specs) {
        MirFuncId const f =
            mb.addFunction(fnSig, SymbolId{s.symbol}, SymbolBinding::Global,
                           SymbolVisibility::Default);
        g.funcVBySymbol.emplace(s.symbol, f.v);
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        for (std::uint32_t callee : s.calls) {
            MirInstId const addr = mb.addGlobalAddr(SymbolId{callee}, fnSig);
            MirInstId const callOps[] = {addr};
            (void)mb.addInst(MirOpcode::Call, callOps, i32);
        }
        mb.addReturn(mb.addConst([&] {
            MirLiteralValue lit;
            lit.value = std::int64_t{0};
            lit.core  = TypeKind::I32;
            return lit;
        }(), i32));
    }
    g.mir = std::move(mb).finish();
    return g;
}

// The scc id for the function with symbol `sym`.
std::uint32_t sccOf(Graph const& g,
                    std::unordered_map<std::uint32_t, std::uint32_t> const& sccs,
                    std::uint32_t sym) {
    auto const fit = g.funcVBySymbol.find(sym);
    EXPECT_NE(fit, g.funcVBySymbol.end()) << "symbol " << sym << " must map to a func";
    auto const sit = sccs.find(fit->second);
    EXPECT_NE(sit, sccs.end()) << "func for symbol " << sym << " must have an scc";
    return sit->second;
}

} // namespace

// ── (a) 3-cycle A→B→C→A: all three share ONE scc id ────────────────────
TEST(CallGraphScc, ThreeCycleSharesOneScc) {
    TypeInterner interner{CompilationUnitId{1}};
    Graph g = buildGraph(interner, {
        {1, {2}},  // A → B
        {2, {3}},  // B → C
        {3, {1}},  // C → A
    });
    auto const sccs = computeCallGraphSccs(g.mir);

    std::uint32_t const a = sccOf(g, sccs, 1);
    std::uint32_t const b = sccOf(g, sccs, 2);
    std::uint32_t const c = sccOf(g, sccs, 3);
    EXPECT_EQ(a, b) << "A and B are in the same 3-cycle SCC";
    EXPECT_EQ(b, c) << "B and C are in the same 3-cycle SCC";
}

// ── (b) acyclic chain A→B→C: three DISTINCT scc ids ────────────────────
TEST(CallGraphScc, AcyclicChainHasDistinctSccs) {
    TypeInterner interner{CompilationUnitId{1}};
    Graph g = buildGraph(interner, {
        {1, {2}},  // A → B
        {2, {3}},  // B → C
        {3, {}},   // C → (leaf)
    });
    auto const sccs = computeCallGraphSccs(g.mir);

    std::uint32_t const a = sccOf(g, sccs, 1);
    std::uint32_t const b = sccOf(g, sccs, 2);
    std::uint32_t const c = sccOf(g, sccs, 3);
    EXPECT_NE(a, b) << "an acyclic edge A→B must NOT collapse to one SCC";
    EXPECT_NE(b, c) << "an acyclic edge B→C must NOT collapse to one SCC";
    EXPECT_NE(a, c) << "the chain endpoints A and C are distinct SCCs";
}

// ── (c) self-edge A→A: A's own scc (a singleton, treated as cyclic) ────
// The inliner's `funcToScc.at(caller.v) == funcToScc.at(callee.v)` test
// makes a self-call refuse because caller==callee → the SAME scc id (this
// singleton). We pin both that A maps to an scc AND that a NON-self
// sibling B (no edges) gets a DIFFERENT scc — so the self-edge does not
// accidentally merge unrelated functions.
TEST(CallGraphScc, SelfEdgeIsOwnScc) {
    TypeInterner interner{CompilationUnitId{1}};
    Graph g = buildGraph(interner, {
        {1, {1}},  // A → A (self-recursion)
        {2, {}},   // B (unrelated leaf)
    });
    auto const sccs = computeCallGraphSccs(g.mir);

    std::uint32_t const a = sccOf(g, sccs, 1);
    std::uint32_t const b = sccOf(g, sccs, 2);
    // A's self-call: caller==callee → same scc id (the equality the
    // inliner relies on to refuse self-recursion).
    EXPECT_EQ(a, a);
    EXPECT_NE(a, b) << "a self-edge on A must not merge the unrelated B";
}

// ── (d) mix: a cycle {A,B} plus an acyclic tail C, D ───────────────────
// A↔B is a 2-cycle (A→B, B→A); A also calls C (acyclic); C calls D
// (acyclic). So {A,B} share one scc; C and D are each their own; and
// none of C/D shares the {A,B} scc.
TEST(CallGraphScc, MixedCycleAndTail) {
    TypeInterner interner{CompilationUnitId{1}};
    Graph g = buildGraph(interner, {
        {1, {2, 3}},  // A → B, A → C
        {2, {1}},     // B → A  (closes the A↔B cycle)
        {3, {4}},     // C → D  (acyclic tail)
        {4, {}},      // D (leaf)
    });
    auto const sccs = computeCallGraphSccs(g.mir);

    std::uint32_t const a = sccOf(g, sccs, 1);
    std::uint32_t const b = sccOf(g, sccs, 2);
    std::uint32_t const c = sccOf(g, sccs, 3);
    std::uint32_t const d = sccOf(g, sccs, 4);

    EXPECT_EQ(a, b) << "A and B form a 2-cycle → one SCC";
    EXPECT_NE(a, c) << "C is acyclic → not in the A↔B SCC";
    EXPECT_NE(a, d) << "D is acyclic → not in the A↔B SCC";
    EXPECT_NE(c, d) << "C→D is an acyclic edge → distinct SCCs";
}
