// MIR-tier MANDATORY unreachable-block prune unit tests.
// Anchor: D-MIR-UNREACHABLE-PRUNE-NORMALIZE.
//
// The frontend (hir_to_mir) creates continuation blocks EAGERLY — if-join,
// loop exit/latch, &&/|| join, switch exit. When every arm seals
// (return/break) that continuation has ZERO predecessors: it is
// unreachable-from-entry, which the production MirVerifier rejects
// (I_UnreachableBlock). `runPruneUnreachableBlocks` drops every such block
// through ONE chokepoint that reuses the shared MirFunctionRebuilder.
//
// Pins (strongest provable property — exact counts + verifier-clean):
//   * Per sealing construct (if-else both-return, &&/|| sealed arm,
//     while/do-while/for returning body, switch all-return,
//     dead-code-after-return): the frontend emits >= 1 orphan block
//     (RED — MirVerifier fires I_UnreachableBlock on the UN-pruned MIR);
//     after the prune the function has NO unreachable-from-entry block,
//     MirVerifier is clean, and the EXACT surviving block count holds.
//   * PHI INTEGRITY: a reachable join whose phi had an incoming from a
//     now-pruned pred — the rebuilt phi DROPS exactly that incoming and
//     KEEPS >= 1 (pins acceptPhiIncoming) + verifier-clean.
//   * GLOBALS: a module WITH a runtime-init global AND an orphan block —
//     the prune STILL runs (does NOT carve out) and produces a
//     verifier-clean module (pins cloneGlobalsVerbatim vs the carve-out
//     trap), and the global keeps its initFunc.
//   * IDEMPOTENT: running the prune twice is a no-op on the second run.
//
// FC5 (goto/labels) LANDED 2026-06-15 — `goto`/labels now lower end-to-end. The
// goto-dead-block prune is exercised at runtime by `examples/c-subset/goto_cleanup`
// (a forward goto over a cleanup block leaves it unreachable → pruned; exit 42 with
// + without the optimizer) and `goto_infinite_escape` (the `while(1){…goto out…}`
// wrap's no-pred Unreachable is dropped by this prune). The hand-built MIR shape
// below pins the same prune + `acceptPhiIncoming` path at the unit tier:
//   `int main(){ int x=0; goto L; x=5; L: return x; }` — the dead `x=5;` block
//   branches to the reachable label `L`; the prune drops that dead pred AND
//   `acceptPhiIncoming` drops any phi-incoming it contributed to a reachable join.
//   The MirBuilder PhiIncomingFromPrunedPred pin below exercises that exact path.

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/semantic/semantic_analyzer.hpp"
#include "analysis/semantic/semantic_model.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/grammar_schema.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "hir/hir.hpp"
#include "hir/lowering/cst_to_hir.hpp"
#include "mir/lowering/hir_to_mir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_cfg.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"
#include "opt/passes/prune_unreachable.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

using namespace dss;

namespace {

// Drive: c-subset source → CompilationUnit → SemanticModel → HIR → MIR.
// Mirrors tests/mir/test_mir_lowering_c_subset.cpp's `lowerCSubset` — the
// canonical way to build AUTHENTIC eager-continuation MIR (the prune's
// real input) from C source.
struct Lowered {
    SemanticModel                   model;
    std::unique_ptr<CstToHirResult> hir;
    DiagnosticReporter              hirReporter;
    HirToMirResult                  mir;
    DiagnosticReporter              mirReporter;
};

[[nodiscard]] Lowered lowerCSubset(std::string src) {
    auto loaded = GrammarSchema::loadShipped("c-subset");
    if (!loaded) { ADD_FAILURE() << "loadShipped(c-subset) failed"; std::abort(); }
    UnitBuilder builder{*loaded};
    builder.addInMemory(std::move(src), "<mem>");
    auto cu    = std::make_shared<CompilationUnit>(std::move(builder).finish());
    auto model = analyze(cu);
    DiagnosticReporter hirReporter;
    auto hir = lowerToHir(model, hirReporter);
    DiagnosticReporter mirReporter;
    MirLoweringConfig mirCfg;
    mirCfg.globalsAllowFloat = (*loaded)->hirLowering().globalsConstEval.allowFloat;
    HirToMirResult mir = lowerToMir(hir->hir, hir->literalPool,
                                    model.lattice().interner(), mirReporter,
                                    &hir->sourceMap, mirCfg, /*ffiMap=*/nullptr,
                                    &hir->linkageMap);
    return Lowered{
        .model       = std::move(model),
        .hir         = std::move(hir),
        .hirReporter = std::move(hirReporter),
        .mir         = std::move(mir),
        .mirReporter = std::move(mirReporter),
    };
}

// Count blocks across the module that are NOT reachable-from-entry (the
// exact property the production verifier's I_UnreachableBlock rule keys
// on). Zero after the prune; >= 1 on the un-pruned eager-continuation MIR.
[[nodiscard]] std::size_t unreachableBlockCount(Mir const& mir) {
    std::size_t orphans = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        auto const rpo = mirReversePostOrder(mir, mir.funcEntry(f));
        std::unordered_set<std::uint32_t> reachable;
        for (MirBlockId const b : rpo) reachable.insert(b.v);
        std::uint32_t const n = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < n; ++bi) {
            if (!reachable.count(mir.funcBlockAt(f, bi).v)) ++orphans;
        }
    }
    return orphans;
}

[[nodiscard]] std::size_t totalBlockCount(Mir const& mir) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) n += mir.funcBlockCount(mir.funcAt(i));
    return n;
}

// Does a clean MirVerifier run (zero NEW Error-severity diagnostics) hold?
[[nodiscard]] bool verifierClean(Mir const& mir, TypeInterner const& interner) {
    DiagnosticReporter rep;
    MirVerifier v{mir, &interner};
    return v.verify(rep);
}

// Does the UN-pruned module trip I_UnreachableBlock specifically?
[[nodiscard]] bool verifierFiresUnreachableBlock(Mir const& mir,
                                                 TypeInterner const& interner) {
    DiagnosticReporter rep;
    MirVerifier v{mir, &interner};
    (void)v.verify(rep);
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_UnreachableBlock) return true;
    }
    return false;
}

// Shared assertion bundle for a frontend-lowered sealing construct:
//   (1) front half is clean (so the MIR is the real shape, not an error
//       recovery artifact),
//   (2) RED-on-disable: the UN-pruned MIR has >= `expectedOrphans` orphan
//       blocks AND the production verifier fires I_UnreachableBlock on it,
//   (3) after the prune: zero orphans, verifier-clean, and the surviving
//       block count is EXACTLY `before - orphansBefore` (the prune removes
//       precisely the unreachable set — a self-checking exact count that
//       doesn't hardcode a magic number), and `result.blocksPruned`
//       equals that same delta.
// `expectedOrphans` is the EXACT orphan count the shape must produce (the
// strongest provable property: if the frontend's eager-continuation shape
// ever changes, this pin moves with intent, not silently).
void expectSealedConstructPruned(std::string src, std::size_t expectedOrphans) {
    auto L = lowerCSubset(std::move(src));
    ASSERT_FALSE(L.model.hasErrors())
        << "semantic: " << (L.model.diagnostics().all().empty()
            ? "" : L.model.diagnostics().all()[0].actual);
    ASSERT_TRUE(L.hir->ok)
        << "HIR: " << (L.hirReporter.all().empty()
            ? "" : L.hirReporter.all()[0].actual);
    ASSERT_TRUE(L.mir.ok)
        << "MIR lowering: " << (L.mirReporter.all().empty()
            ? "" : L.mirReporter.all()[0].actual);

    Mir& mir = L.mir.mir;
    TypeInterner const& interner = L.model.lattice().interner();

    // (2) RED-on-disable: the frontend really does emit the orphan(s), and
    // the production verifier really does reject them. If this stops
    // holding, the prune is being tested against a shape it doesn't fix.
    std::size_t const blocksBefore  = totalBlockCount(mir);
    std::size_t const orphansBefore = unreachableBlockCount(mir);
    EXPECT_EQ(orphansBefore, expectedOrphans)
        << "the eager-continuation lowering must leave EXACTLY "
        << expectedOrphans << " orphan block(s) BEFORE the prune";
    EXPECT_TRUE(verifierFiresUnreachableBlock(mir, interner))
        << "the UN-pruned MIR must trip I_UnreachableBlock (red-on-disable)";

    // (3) Run the prune.
    DiagnosticReporter rep;
    auto const r = opt::passes::runPruneUnreachableBlocks(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(rep.errorCount(), 0u) << "prune must not emit Error diagnostics";

    EXPECT_EQ(unreachableBlockCount(mir), 0u)
        << "after the prune NO block may be unreachable-from-entry";
    EXPECT_TRUE(verifierClean(mir, interner))
        << "after the prune the MirVerifier must be clean";
    // The prune removes EXACTLY the unreachable set — derived exact count.
    EXPECT_EQ(totalBlockCount(mir), blocksBefore - orphansBefore)
        << "surviving count must equal before - orphans (prune drops only "
           "the unreachable blocks)";
    EXPECT_EQ(r.blocksPruned, orphansBefore)
        << "result.blocksPruned must equal the exact pre-vs-post delta";
}

} // namespace

// ── if-else, BOTH arms return — the reproducer shape ───────────────────
// entry: CondBr(c, then, else); then: return 1; else: return 2; join: orphan.
// Exactly ONE orphan (the eager if-join).
TEST(PruneUnreachable, IfElseBothArmsReturn) {
    expectSealedConstructPruned(
        "int main(){ int c=1; if(c){ return 1; } else { return 2; } }",
        /*expectedOrphans=*/1);
}

// ── &&  inside a both-arms-return `if` — the OUTER if's eager join
// orphans (both arms seal); the &&-diamond's own phi-join is EXERCISED
// (reachable, feeds the CondBr). Pins that a function mixing `&&` with a
// sealing `if` prunes clean (the && lowering's eager continuation
// participates without leaking an extra orphan).
TEST(PruneUnreachable, LogicalAndInSealedIf) {
    expectSealedConstructPruned(
        "int main(){ int a=1; int b=1; if(a && b){ return 1; } else { return 2; } }",
        /*expectedOrphans=*/1);
}

// ── || inside a both-arms-return `if` — mirror of the && case. ─────────
TEST(PruneUnreachable, LogicalOrInSealedIf) {
    expectSealedConstructPruned(
        "int main(){ int a=0; int b=1; if(a || b){ return 1; } else { return 2; } }",
        /*expectedOrphans=*/1);
}

// ── for with a returning body — the update/latch block orphans (the body
// returns, so the back-edge into `update` is dead). The header's false
// edge keeps `exit` reachable, which the trailing `return 0` consumes.
TEST(PruneUnreachable, ForReturningBodyOrphansUpdate) {
    expectSealedConstructPruned(
        "int main(){ int i; for(i=0;i<10;i=i+1){ return 11; } return 0; }",
        /*expectedOrphans=*/1);
}

// ── switch, ALL arms return — the switch exit orphans (cf_switch shape). ─
// `switch(x){ case 1: return 1; default: return 2; }` — every arm seals,
// so the eagerly-created exit (SwitchJoin) has no predecessor.
TEST(PruneUnreachable, SwitchAllArmsReturnOrphansExit) {
    expectSealedConstructPruned(
        "int main(){ int x=1; switch(x){ case 1: return 1; default: return 2; } }",
        /*expectedOrphans=*/1);
}

// NOTE on related shapes covered ELSEWHERE (not re-pinned in this prune file):
//   * dead-code-after-terminator (`return 42; return 7;`) is now ACCEPTED with a
//     WARNING (`H_UnreachableCode`; D-HIR-DEAD-CODE-AFTER-RETURN-REJECTED), so it
//     DOES reach this prune — the dead statement lowers into the Block-lowering
//     fresh-dead-block and is dropped. Pinned by the `dead_code_after_return`
//     corpus + the `test_hir_verifier` warning pins.
//   * a provably-infinite loop whose body returns (`while(1){return;}`,
//     `for(;;){return;}`, `do{return;}while(1)`) is wrapped as
//     `Block{ loop, Unreachable }` in HIR lowering (D-HIR-INFINITE-LOOP-NOT-
//     TERMINATING) so the function structurally terminates; pinned by the
//     `nonmain_*_inf_return` corpus. (The earlier HirBuilder double-attach on
//     such non-terminating `main`s was fixed in D-HIR-LOOP-BODY-ONLY-RETURN-
//     DOUBLE-ATTACH.) The `for`-with-condition returning-body case above still
//     exercises the update-block orphan in THIS prune file.

// ── PHI INTEGRITY (pins acceptPhiIncoming) ─────────────────────────────
// A reachable join `J` whose phi has TWO incomings: one from a reachable
// pred (P_live) and one from a DEAD pred (P_dead, unreachable-from-entry).
// After the prune, P_dead is gone, so the rebuilt phi must DROP exactly
// that incoming and KEEP the live one (>= 1) — and the result must be
// verifier-clean. Built directly via MirBuilder so the dead-pred edge is
// explicit (this is the exact shape a future goto's dead block branching
// to a reachable label produces).
//
//   entry: Br(J)                       (reachable)
//   dead:  Br(J)                        (NO pred → unreachable-from-entry)
//   J:     phi[ (c1 @ entry), (c2 @ dead) ]; return phi
//
// RED-on-disable: WITHOUT acceptPhiIncoming dropping the dead incoming,
// phase-3 would either keep a phi-incoming whose pred block doesn't exist
// in the rebuilt module (→ rebuilder "pred not in blockMap" abort) or the
// verifier's I_PhiPredNotInCfg / I_UnreachableBlock would fire.
TEST(PruneUnreachable, PhiIncomingFromPrunedPredIsDropped) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const fnSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(fnSig, SymbolId{100});
    MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const dead  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const join  = mb.createBlock(StructCfMarker::Linear);

    mb.beginBlock(entry);
    MirLiteralValue c1; c1.value = std::int64_t{1}; c1.core = TypeKind::I32;
    MirInstId const v1 = mb.addConst(c1, i32);
    mb.addBr(join);

    mb.beginBlock(dead);
    MirLiteralValue c2; c2.value = std::int64_t{2}; c2.core = TypeKind::I32;
    MirInstId const v2 = mb.addConst(c2, i32);
    mb.addBr(join);

    mb.beginBlock(join);
    std::array<MirPhiIncoming, 2> inc{
        MirPhiIncoming{v1, entry},
        MirPhiIncoming{v2, dead},
    };
    MirInstId const phi = mb.addPhi(i32, inc);
    mb.addReturn(phi);
    Mir mir = std::move(mb).finish();

    // Pre-prune: `dead` is unreachable-from-entry; the verifier rejects it.
    EXPECT_EQ(unreachableBlockCount(mir), 1u);
    EXPECT_TRUE(verifierFiresUnreachableBlock(mir, interner));

    DiagnosticReporter rep;
    auto const r = opt::passes::runPruneUnreachableBlocks(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(rep.errorCount(), 0u);

    // Post-prune: `dead` is gone (2 blocks: entry + join), zero orphans,
    // verifier-clean.
    EXPECT_EQ(totalBlockCount(mir), 2u);
    EXPECT_EQ(unreachableBlockCount(mir), 0u);
    EXPECT_TRUE(verifierClean(mir, interner));

    // The surviving join's phi kept EXACTLY ONE incoming (the live one).
    MirFuncId const fn = mir.funcAt(0);
    bool foundPhi = false;
    std::uint32_t const nb = mir.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        std::uint32_t const ni = mir.blockInstCount(b);
        for (std::uint32_t ii = 0; ii < ni; ++ii) {
            MirInstId const id = mir.blockInstAt(b, ii);
            if (mir.instOpcode(id) != MirOpcode::Phi) continue;
            foundPhi = true;
            EXPECT_EQ(mir.phiIncomings(id).size(), 1u)
                << "the phi must keep exactly the one live incoming after "
                   "the dead pred was pruned (pins acceptPhiIncoming)";
        }
    }
    EXPECT_TRUE(foundPhi) << "the join's phi must survive the prune";
}

// ── GLOBALS pin (pins cloneGlobalsVerbatim vs the carve-out trap) ──────
// A module WITH a runtime-init global (initFunc.valid()) AND a function
// carrying an orphan block. The shared `cloneGlobalsOrCarveOut` would
// CARVE OUT (skip the whole rebuild) on the runtime-init global, leaving
// the orphan in place. The prune MUST instead run `cloneGlobalsVerbatim`:
// it processes the module, drops the orphan, keeps the global's initFunc
// VERBATIM, and produces a verifier-clean module.
TEST(PruneUnreachable, RuntimeInitGlobalStillPrunedNotCarvedOut) {
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32   = interner.primitive(TypeKind::I32);
    TypeId const voidT = interner.primitive(TypeKind::Void);
    TypeId const initSig = interner.fnSig({}, voidT, CallConv::CcSysV);
    TypeId const mainSig = interner.fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    // Runtime-init function for the global.
    MirFuncId const initFn = mb.addFunction(initSig, SymbolId{200});
    MirBlockId const initEntry = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(initEntry);
    mb.addReturn();

    // A second function with an ORPHAN block (entry returns; a created
    // tail block has no predecessor — the dead-code-after-return shape).
    mb.addFunction(mainSig, SymbolId{201});
    MirBlockId const mEntry = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const mDead  = mb.createBlock(StructCfMarker::Linear);
    mb.beginBlock(mEntry);
    MirLiteralValue c1; c1.value = std::int64_t{1}; c1.core = TypeKind::I32;
    mb.addReturn(mb.addConst(c1, i32));
    mb.beginBlock(mDead);   // orphan (no pred)
    MirLiteralValue c2; c2.value = std::int64_t{2}; c2.core = TypeKind::I32;
    mb.addReturn(mb.addConst(c2, i32));

    // The runtime-init global, referencing initFn.
    MirGlobalId const g = mb.addGlobal(i32, SymbolId{202},
                                       /*initLiteralIndex=*/UINT32_MAX, initFn);
    Mir mir = std::move(mb).finish();
    (void)g;

    EXPECT_EQ(unreachableBlockCount(mir), 1u)
        << "the orphan tail block must be present BEFORE the prune";
    EXPECT_TRUE(verifierFiresUnreachableBlock(mir, interner));
    ASSERT_EQ(mir.moduleGlobalCount(), 1u);

    DiagnosticReporter rep;
    auto const r = opt::passes::runPruneUnreachableBlocks(mir, interner, rep);
    ASSERT_TRUE(r.ok);
    // CRITICAL: NO X_OptPassSkipped — the prune must NOT carve out.
    for (auto const& d : rep.all()) {
        EXPECT_NE(d.code, DiagnosticCode::X_OptPassSkipped)
            << "the mandatory prune must NEVER carve out on a runtime-init "
               "global (it uses cloneGlobalsVerbatim, not the carve-out helper)";
    }
    EXPECT_EQ(rep.errorCount(), 0u);

    // Orphan dropped, verifier-clean.
    EXPECT_EQ(unreachableBlockCount(mir), 0u);
    EXPECT_TRUE(verifierClean(mir, interner));

    // The global SURVIVED with its initFunc kept VERBATIM (and still valid,
    // because func-ids are stable across the prune).
    ASSERT_EQ(mir.moduleGlobalCount(), 1u);
    MirGlobalId const g2 = mir.globalAt(0);
    EXPECT_EQ(mir.globalSymbol(g2).v, 202u);
    EXPECT_TRUE(mir.globalInitFunc(g2).valid())
        << "the runtime-init global must keep a VALID initFunc after the "
           "verbatim clone (cloneGlobalsVerbatim passes it through unremapped)";
    EXPECT_EQ(mir.moduleFuncCount(), 2u)
        << "the prune drops BLOCKS only — both functions must survive";
}

// ── IDEMPOTENT — a second prune over an already-pruned module is a no-op. ─
TEST(PruneUnreachable, IdempotentSecondRunIsNoOp) {
    auto L = lowerCSubset(
        "int main(){ int c=1; if(c){ return 1; } else { return 2; } }");
    ASSERT_TRUE(L.mir.ok);
    Mir& mir = L.mir.mir;
    TypeInterner const& interner = L.model.lattice().interner();

    DiagnosticReporter rep1;
    auto const r1 = opt::passes::runPruneUnreachableBlocks(mir, interner, rep1);
    ASSERT_TRUE(r1.ok);
    std::size_t const afterFirst = totalBlockCount(mir);
    EXPECT_EQ(unreachableBlockCount(mir), 0u);

    DiagnosticReporter rep2;
    auto const r2 = opt::passes::runPruneUnreachableBlocks(mir, interner, rep2);
    ASSERT_TRUE(r2.ok);
    EXPECT_EQ(totalBlockCount(mir), afterFirst)
        << "a second prune over an already-pruned module changes nothing";
    EXPECT_EQ(r2.blocksPruned, 0u);
    EXPECT_TRUE(verifierClean(mir, interner));
}
