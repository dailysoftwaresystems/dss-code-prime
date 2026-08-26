// OPT11 — THE LAZY IMPORT EDGE (plan 22 §0.2, D-OPT11-LAZY-IMPORT-EDGE).
//
// `test_summary_index.cpp` pins the DECISION half — what the global pass decides
// given summaries. This file pins the TRANSFORMATION half's missing edge: a
// per-TU optimize that ASKS the index for a body the moment its own module names
// one it does not have, instead of being confined to the precomputed
// `importPlan`.
//
// ★★★ THE SUBJECT PROGRAM IS A CALL CHAIN ACROSS FOUR SEPARATE MODULES —
// `main` → `f1` → `f2` → `f3` — and it is four rather than two on purpose. The
// eager `importPlan` at `maxImportDepth = 1` offers module 0 exactly ONE body
// (`f1`); every deeper one is invisible to it. So the chain is the shape that
// tells a lazy edge from an eager plan, and a two-module fixture would be green
// for both.
//
// STRICT pins, each on a claim the row makes:
//   * LazyEdgeReachesBodiesThePrefetchPlanNeverOffers — ★ THE ROW'S CORE CLAIM,
//     and the RED-ON-DISABLE subject: bound the availability closure by
//     `maxImportDepth` (the eager behaviour) and this drops 3 → 1.
//   * PrefetchDepthChangesTheBatchCountAndNothingElse — ★★★ THE §B FORK
//     DISSOLUTION, by execution: depth 1 and depth 4 import the same bodies in
//     the same number of merges and produce a structurally identical module;
//     only the FETCH batch count moves.
//   * EveryCrossCuCallIsSplicedAfterTheLazyImport — the quality bar, end to
//     end, through the REAL optimizer and its UNCHANGED legality gate.
//   * AnImportedBodyIsDeclaredAvailableExternallyAndNotEmitted — the marking,
//     and that the existing `stripInlineDefinitions` removes it.
//   * StrippedBodiesAreNotReImported — the termination guard: `optimize()`
//     deletes the bodies it just consumed, so the module names those callees
//     again the instant the round ends.
//   * RefusesABodyThatReferencesItsOwnModulePrivateGlobal — a `static` object
//     has identity and state; a body that needs one is UNAVAILABLE.
//   * SummariesDescribeModulesRefusesAMismatchedPairing — ★★★ THE WRONG-BODY
//     GUARD. A `DefiningSite` is an ORDINAL; a summary paired with the wrong
//     module resolves every fetch to a real function with the wrong body.
//   * TheBudgetStillDecidesTheOutcome — option B of the fork remains the one
//     knob that legitimately changes what is imported.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/summary/lazy_import_optimize.hpp"
#include "mir/summary/mir_summary.hpp"
#include "mir/summary/summary_index.hpp"
#include "opt/optimizer.hpp"

#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::mirsum;

namespace {

// One hand-built TU. `lattice` is heap-held because `LazyImportCu` points at it
// and a vector reallocation must not move it out from under the driver.
struct TestCu {
    std::unique_ptr<TypeLattice> lattice;
    Mir                          mir;
    std::vector<std::string>     names;      // SymbolId.v → declared name
    std::vector<ExternImport>    externs;
};

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// `int <self>(void) { return <callee>(); }`, or `{ return <konst>; }` at the end
// of the chain. Symbol 100 is the function; symbol 10 is the extern callee.
TestCu makeLink(std::uint32_t cuIdx, std::string self, std::string callee,
                std::int64_t konst) {
    TestCu cu;
    cu.lattice = std::make_unique<TypeLattice>(CompilationUnitId{cuIdx + 1}, "c");
    TypeId const i32 = cu.lattice->interner().primitive(TypeKind::I32);
    TypeId const sig = cu.lattice->interner().fnSig({}, i32, CallConv::CcSysV);

    MirBuilder mb;
    mb.addFunction(sig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    if (callee.empty()) {
        mb.addReturn(mb.addConst(i32Lit(konst), i32));
    } else {
        MirInstId const addr = mb.addGlobalAddr(SymbolId{10}, sig);
        MirInstId const ops[] = {addr};
        mb.addReturn(mb.addInst(MirOpcode::Call, ops, i32));
    }
    cu.mir = std::move(mb).finish();

    cu.names.assign(101, std::string{});
    cu.names[100] = std::move(self);
    if (!callee.empty()) {
        cu.names[10] = callee;
        cu.externs.push_back(ExternImport{SymbolId{10}, callee, ""});
    }
    return cu;
}

// main → f1 → f2 → f3 → 7, one module each.
std::vector<TestCu> makeChain() {
    std::vector<TestCu> cus;
    cus.push_back(makeLink(0, "main", "f1", 0));
    cus.push_back(makeLink(1, "f1", "f2", 0));
    cus.push_back(makeLink(2, "f2", "f3", 0));
    cus.push_back(makeLink(3, "f3", "", 7));
    return cus;
}

std::vector<ModuleSummary> summarize(std::vector<TestCu> const& cus) {
    std::vector<ModuleSummary> out;
    for (std::uint32_t i = 0; i < cus.size(); ++i) {
        SummaryCuInput in;
        in.mir    = &cus[i].mir;
        in.nameOf = [&names = cus[i].names](SymbolId s) {
            return s.v < names.size() ? names[s.v] : std::string{};
        };
        in.externImports  = cus[i].externs;
        in.moduleDigest   = std::format("digest-{}", i);
        in.targetIdentity = "x86_64:elf64-x86_64-linux-exec";
        out.push_back(buildModuleSummary(in));
    }
    return out;
}

std::vector<LazyImportCu> viewsOf(std::vector<TestCu> const& cus) {
    std::vector<LazyImportCu> out;
    for (TestCu const& cu : cus) {
        out.push_back(LazyImportCu{&cu.mir, &cu.lattice->interner(), &cu.names,
                                   cu.externs});
    }
    return out;
}

// A NUMBERING-FREE rendering of a module: every function by NAME, every
// instruction by opcode, every symbol reference by the NAME it resolves to.
//
// ★ NOT `emitMir`, and the difference is the whole point. `.dssir` prints symbol
// HANDLES, and a merge renumbers, so two runs that produced the same PROGRAM by
// different import routes would differ in the text for a reason that is not
// quality. This renders what the program IS.
std::string structuralDump(Mir const& mir,
                           std::vector<std::string> const& names) {
    auto nameOf = [&](SymbolId s) {
        return s.v < names.size() && !names[s.v].empty()
                   ? names[s.v] : std::format("<anon:{}>", s.v);
    };
    std::vector<std::string> funcs;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::string text = std::format("fn {} bind={} blocks={}\n",
                                       nameOf(mir.funcSymbol(f)),
                                       static_cast<int>(mir.funcBinding(f)),
                                       mir.funcBlockCount(f));
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            text += std::format("  block {}\n", bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirInstId const inst = mir.blockInstAt(b, ii);
                MirOpcode const op = mir.instOpcode(inst);
                text += std::format("    {}", static_cast<int>(op));
                if (op == MirOpcode::GlobalAddr)
                    text += " sym=" + nameOf(mir.globalAddrSymbol(inst));
                if (op == MirOpcode::Const) {
                    auto const& lit =
                        mir.literalValue(mir.constLiteralIndex(inst));
                    if (auto const* iv = std::get_if<std::int64_t>(&lit.value))
                        text += std::format(" lit={}", *iv);
                }
                text += "\n";
            }
        }
        funcs.push_back(std::move(text));
    }
    std::sort(funcs.begin(), funcs.end());
    std::string all;
    for (std::string const& f : funcs) all += f;
    return all;
}

std::size_t countOpcode(Mir const& mir, MirOpcode want) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t fi = 0; fi < nf; ++fi) {
        MirFuncId const f = mir.funcAt(fi);
        std::uint32_t const nb = mir.funcBlockCount(f);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(f, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                if (mir.instOpcode(mir.blockInstAt(b, ii)) == want) ++n;
            }
        }
    }
    return n;
}

SummaryIndexPolicy policyAt(std::uint32_t depth, std::uint32_t budget = 0) {
    SummaryIndexPolicy p;
    p.inlineThreshold             = 50;
    p.maxImportDepth              = depth;
    p.perModuleImportInstBudget   = budget;
    return p;
}

// The optimizer that does NOTHING — for the pins about import MECHANICS, where
// running real passes would only make a failure harder to read.
LazyOptimizeFn noOptimize() {
    return [](Mir&, TypeInterner const&, std::span<ExternImport const>) {
        return true;
    };
}

// The REAL optimizer, running the UNCHANGED `Inlining` pass and its UNCHANGED
// legality gate on the post-import module. That is the invariant this arc rests
// on, so the end-to-end pin uses the real thing rather than a model of it.
LazyOptimizeFn realInliner(TargetSchema const& target,
                           DiagnosticReporter& reporter) {
    return [&target, &reporter](Mir& mir, TypeInterner const& interner,
                                std::span<ExternImport const> externs) {
        opt::OptPipeline pipeline = opt::OptPipeline::flat(
            "lazy-import-test", {opt::PassId::Inlining, opt::PassId::Dce}, 4);
        pipeline.inlineThreshold           = 50;
        pipeline.inlineCallerGrowthPercent = 100;
        return opt::optimize(mir, target, interner, pipeline, reporter, externs)
            .ok;
    };
}

struct ImportRun {
    LazyImportOutcome        outcome;
    Mir                      mir;
    std::vector<std::string> names;
};

// Drive one importer end to end. `mir` starts as a COPY of module 0 — the driver
// replaces it in place, and every test wants module 0 left intact for the next
// arm.
ImportRun runImporter(std::vector<TestCu>& cus, SummaryIndexPolicy const& policy,
                LazyOptimizeFn const& optimizeOne, DiagnosticReporter& rep,
                SummaryIndex const& index,
                std::vector<ModuleSummary> const& summaries,
                std::vector<LazyImportCu> const& views) {
    ImportRun r;
    r.outcome = lazyImportOptimize(0, views, summaries, index, policy, "c",
                                   optimizeOne, rep);
    r.mir = r.outcome.mir.has_value() ? std::move(*r.outcome.mir)
                                      : Mir{};
    r.names = cus[0].names;
    if (r.outcome.importedBodies > 0) {
        std::uint32_t maxV = 0;
        for (auto const& [v, n] : r.outcome.symbolNames) maxV = std::max(maxV, v);
        r.names.assign(static_cast<std::size_t>(maxV) + 1, std::string{});
        for (auto const& [v, n] : r.outcome.symbolNames) r.names[v] = n;
    }
    return r;
}

} // namespace

// ── ★ THE ROW'S CORE CLAIM ─────────────────────────────────────────────────
// At `maxImportDepth = 1` the precomputed prefetch plan offers module 0 exactly
// ONE body. The lazy edge pages in all THREE, because it keeps asking the index
// as the module keeps naming callees it does not have.
//
// RED-ON-DISABLE: bound `takeClosure`'s loop by `levels` (make the closure eager
// again) and `importedBodies` falls to 1 while `importPlan` stays at 1 — the two
// numbers converge, which is exactly what "the edge is not lazy" looks like.
TEST(LazyImportEdge, LazyEdgeReachesBodiesThePrefetchPlanNeverOffers) {
    std::vector<TestCu> cus = makeChain();
    auto const summaries = summarize(cus);
    auto const views     = viewsOf(cus);
    DiagnosticReporter rep;
    auto const policy = policyAt(1);
    auto index = buildSummaryIndex(summaries, policy, rep);
    ASSERT_TRUE(index.has_value());
    ASSERT_TRUE(summariesDescribeModules(views, summaries, rep));

    std::size_t eagerForModule0 = 0;
    for (ImportDecision const& d : index->importPlan)
        if (d.importerModule == 0) ++eagerForModule0;
    EXPECT_EQ(eagerForModule0, 1u)
        << "the eager plan at depth 1 can only see the first call-graph level";

    ImportRun const r = runImporter(cus, policy, noOptimize(), rep, *index, summaries,
                              views);
    ASSERT_TRUE(r.outcome.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(r.outcome.importedBodies, 3u)
        << "the lazy edge pages in f1, f2 AND f3 — the two deeper ones are "
           "bodies the depth-1 prefetch plan never offered";
    EXPECT_EQ(rep.errorCount(), 0u);
}

// ── ★★★ THE §B FORK, DISSOLVED BY EXECUTION ────────────────────────────────
// Depth is a PREFETCH knob: it changes how many fetch batches the closure costs
// and NOTHING else. Same bodies, same number of clone calls, structurally
// identical module.
TEST(LazyImportEdge, PrefetchDepthChangesTheBatchCountAndNothingElse) {
    std::string dumpAtDepth1;
    std::string dumpAtDepth4;
    LazyImportOutcome o1;
    LazyImportOutcome o4;

    for (std::uint32_t depth : {1u, 4u}) {
        std::vector<TestCu> cus = makeChain();
        auto const summaries = summarize(cus);
        auto const views     = viewsOf(cus);
        DiagnosticReporter rep;
        auto const policy = policyAt(depth);
        auto index = buildSummaryIndex(summaries, policy, rep);
        ASSERT_TRUE(index.has_value());
        ImportRun r = runImporter(cus, policy, noOptimize(), rep, *index, summaries,
                            views);
        ASSERT_TRUE(r.outcome.ok);
        EXPECT_EQ(rep.errorCount(), 0u);
        if (depth == 1) {
            o1 = std::move(r.outcome);
            dumpAtDepth1 = structuralDump(r.mir, r.names);
        } else {
            o4 = std::move(r.outcome);
            dumpAtDepth4 = structuralDump(r.mir, r.names);
        }
    }

    // QUALITY does not move.
    EXPECT_EQ(o1.importedBodies, o4.importedBodies);
    EXPECT_EQ(o1.importedBodies, 3u);
    EXPECT_EQ(o1.importMerges, o4.importMerges);
    EXPECT_EQ(o1.importMerges, 1u)
        << "one clone per round whatever the depth — otherwise the merge's "
           "symbol renumbering becomes a function of a latency knob";
    EXPECT_EQ(dumpAtDepth1, dumpAtDepth4)
        << "the same program, whatever the prefetch depth";

    // LATENCY does. A three-level chain fetched one level at a time is three
    // batches; four levels at a time is one.
    EXPECT_EQ(o1.importBatches, 3u);
    EXPECT_EQ(o4.importBatches, 1u);
    EXPECT_NE(o1.importBatches, o4.importBatches)
        << "if depth stopped changing the batch count it would have stopped "
           "being honoured at all, and the equality above would be vacuous";
}

// ── the quality bar, end to end, through the UNCHANGED gate ────────────────
TEST(LazyImportEdge, EveryCrossCuCallIsSplicedAfterTheLazyImport) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());
    TargetSchema const& target = **targetR;

    std::vector<TestCu> cus = makeChain();
    auto const summaries = summarize(cus);
    auto const views     = viewsOf(cus);
    DiagnosticReporter rep;
    auto const policy = policyAt(1);
    auto index = buildSummaryIndex(summaries, policy, rep);
    ASSERT_TRUE(index.has_value());

    EXPECT_EQ(countOpcode(cus[0].mir, MirOpcode::Call), 1u)
        << "before: main makes exactly one cross-CU call";

    ImportRun const r = runImporter(cus, policy, realInliner(target, rep), rep, *index,
                              summaries, views);
    ASSERT_TRUE(r.outcome.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    // ★ ALL THREE cross-CU calls are gone. The gate that accepted them is the
    // SAME `inlineLegalityGate` running on the post-import module — this file
    // changed what was AVAILABLE, never what was LEGAL.
    EXPECT_EQ(countOpcode(r.mir, MirOpcode::Call), 0u)
        << "main → f1 → f2 → f3 collapsed entirely";
}

// ── the marking, and the strip that consumes it ────────────────────────────
TEST(LazyImportEdge, AnImportedBodyIsDeclaredAvailableExternallyAndNotEmitted) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());

    std::vector<TestCu> cus = makeChain();
    auto const summaries = summarize(cus);
    auto const views     = viewsOf(cus);
    DiagnosticReporter rep;
    auto const policy = policyAt(4);
    auto index = buildSummaryIndex(summaries, policy, rep);
    ASSERT_TRUE(index.has_value());

    ImportRun const r = runImporter(cus, policy, realInliner(**targetR, rep), rep,
                              *index, summaries, views);
    ASSERT_TRUE(r.outcome.ok) << "errorCount=" << rep.errorCount();

    // Every imported name carries an extern row — the C99 6.7.4p7 pairing that
    // makes it an inline definition rather than a second definition of a symbol
    // another object already defines.
    for (char const* n : {"f1", "f2", "f3"}) {
        bool found = false;
        for (ExternImport const& e : r.outcome.externImports)
            if (e.mangledName == n) { found = true; break; }
        EXPECT_TRUE(found) << "no extern row declares imported body " << n;
    }
    // …and the existing `stripInlineDefinitions` epilogue removed the bodies, so
    // this TU emits only its own function.
    EXPECT_EQ(r.mir.moduleFuncCount(), 1u)
        << "an imported body must never be EMITTED";
}

// ── termination: the strip deletes what the round just consumed ────────────
TEST(LazyImportEdge, StrippedBodiesAreNotReImported) {
    auto targetR = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(targetR.has_value());

    std::vector<TestCu> cus = makeChain();
    auto const summaries = summarize(cus);
    auto const views     = viewsOf(cus);
    DiagnosticReporter rep;
    auto const policy = policyAt(2);
    auto index = buildSummaryIndex(summaries, policy, rep);
    ASSERT_TRUE(index.has_value());

    ImportRun const r = runImporter(cus, policy, realInliner(**targetR, rep), rep,
                              *index, summaries, views);
    ASSERT_TRUE(r.outcome.ok);
    // ONE optimize, ONE clone. Without the already-taken guard the module names
    // f1/f2/f3 again the instant the strip removes them, and the import and the
    // strip chase each other to the round cap.
    EXPECT_EQ(r.outcome.optimizeRuns, 1u);
    EXPECT_EQ(r.outcome.importMerges, 1u);
    EXPECT_EQ(r.outcome.demandLeftAtBound, 0u);
}

// ── availability refusal: a body that needs the SOURCE TU's `static` ───────
TEST(LazyImportEdge, RefusesABodyThatReferencesItsOwnModulePrivateGlobal) {
    std::vector<TestCu> cus;
    cus.push_back(makeLink(0, "main", "f1", 0));

    // CU1: `static int s; int f1(void) { return s; }` — `s` is Local, so it has
    // no cross-TU name, and duplicating it into the importer would give the
    // program two objects where the source declared one.
    TestCu cu1;
    cu1.lattice = std::make_unique<TypeLattice>(CompilationUnitId{2}, "c");
    TypeId const i32 = cu1.lattice->interner().primitive(TypeKind::I32);
    TypeId const sig = cu1.lattice->interner().fnSig({}, i32, CallConv::CcSysV);
    TypeId const ptr = cu1.lattice->interner().pointer(i32);
    {
        MirBuilder mb;
        mb.addGlobal(i32, SymbolId{60}, UINT32_MAX, MirFuncId{},
                     SymbolBinding::Local, SymbolVisibility::Default,
                     /*isConst=*/false, MirThreadStorage::Shared, 0);
        mb.addFunction(sig, SymbolId{100});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const addr = mb.addGlobalAddr(SymbolId{60}, ptr);
        MirInstId const ops[] = {addr};
        mb.addReturn(mb.addInst(MirOpcode::Load, ops, i32));
        cu1.mir = std::move(mb).finish();
    }
    cu1.names.assign(101, std::string{});
    cu1.names[100] = "f1";
    // ⚠ NAMED but LOCAL. The name exists in the TU's own symbol table; what
    // makes it unavailable is that no cross-CU winner claims it, which is the
    // rule `isImportable` applies.
    cu1.names[60]  = "s";
    cus.push_back(std::move(cu1));

    auto const summaries = summarize(cus);
    auto const views     = viewsOf(cus);
    DiagnosticReporter rep;
    auto const policy = policyAt(4);
    auto index = buildSummaryIndex(summaries, policy, rep);
    ASSERT_TRUE(index.has_value());
    ASSERT_TRUE(summariesDescribeModules(views, summaries, rep));

    EXPECT_FALSE(isImportable(0, "f1", views, summaries, *index, policy))
        << "a body that reads its own TU's `static` object cannot be moved";

    ImportRun const r = runImporter(cus, policy, noOptimize(), rep, *index, summaries,
                              views);
    ASSERT_TRUE(r.outcome.ok);
    EXPECT_EQ(r.outcome.importedBodies, 0u);
    EXPECT_EQ(rep.errorCount(), 0u) << "a refusal is not an error";
}

// ── ★★★ THE WRONG-BODY GUARD ───────────────────────────────────────────────
TEST(LazyImportEdge, SummariesDescribeModulesRefusesAMismatchedPairing) {
    std::vector<TestCu> cus = makeChain();
    auto summaries = summarize(cus);
    auto const views = viewsOf(cus);
    DiagnosticReporter clean;
    EXPECT_TRUE(summariesDescribeModules(views, summaries, clean));
    EXPECT_EQ(clean.errorCount(), 0u);

    // Pair module 1 with module 2's summary. Both are structurally identical
    // one-function modules, so nothing about the SHAPE gives it away — only the
    // NAME does, which is exactly why the check is on content and not on counts.
    std::swap(summaries[1], summaries[2]);
    DiagnosticReporter rep;
    EXPECT_FALSE(summariesDescribeModules(views, summaries, rep))
        << "a summary paired with the wrong module makes every ordinal fetch "
           "return the WRONG BODY";
    EXPECT_GT(rep.errorCount(), 0u) << "and it must say so, loudly";
}

// ── option B: the budget is the knob that DOES change the outcome ──────────
TEST(LazyImportEdge, TheBudgetStillDecidesTheOutcome) {
    std::vector<TestCu> cus = makeChain();
    auto const summaries = summarize(cus);
    auto const views     = viewsOf(cus);
    DiagnosticReporter rep;
    // Each link is 3 MIR instructions; a budget of 4 admits exactly one body.
    auto const policy = policyAt(4, /*budget=*/4);
    auto index = buildSummaryIndex(summaries, policy, rep);
    ASSERT_TRUE(index.has_value());

    ImportRun const r = runImporter(cus, policy, noOptimize(), rep, *index, summaries,
                              views);
    ASSERT_TRUE(r.outcome.ok);
    EXPECT_EQ(r.outcome.importedBodies, 1u)
        << "the ThinLTO-style instruction budget is a REAL bound, unlike depth";
}
