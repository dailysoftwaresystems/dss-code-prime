// OPT11 — the GLOBAL PASS OVER THE SUMMARIES (plan 22 §0.2), unit-tested with
// hand-built `ModuleSummary` values.
//
// ★ Hand-built summaries, not summaries derived from hand-built MIR, and that
// is deliberate: `test_mir_summary.cpp` already pins that MIR projects onto a
// summary correctly. Pinning the index against MIR too would make every index
// test depend on the emitter and would hide an index bug behind an emitter
// bug. Here the summary IS the input, which is exactly the contract the index
// has with the world.
//
// STRICT pins, each on a whole-program question NO per-TU pass can answer:
//   * IndexResolvesStrongOverWeakAcrossModules — delegated to
//     `resolveCrossCuDefs`, so the index and `mergeCuMirs` cannot disagree.
//   * IndexUnionsTheAddressEscapeSetAcrossModules + …RefusesACalleeEscapingIn
//     AnotherModule — ★★★ THE SILENT MISCOMPILE. A TU-local escape analysis
//     would inline a callee whose address is taken in a DIFFERENT TU.
//   * IndexFindsCrossModuleRecursionSCC — mutual recursion that crosses a TU
//     boundary, which a per-TU SCC cannot see.
//   * IndexComputesWholeProgramLiveness / …ThroughGlobalInitializers /
//     …ReportsDeadSymbolsPerModule — the `scanLiveSymbols` lift, including the
//     data-relocation fixpoint.
//   * IndexPlansCrossModuleImports / …RespectsTheInlineThreshold /
//     …SkipsSameModuleCallees / …FollowsTransitiveEdgesToTheDepthBound — the
//     prefetch plan and its bounds.
//   * IndexTier2KeyChangesWhenACalleeChanges — ★★★ THE OTHER SILENT
//     MISCOMPILE: keying the post-import object on the TU alone leaves a stale
//     inlined copy of an edited callee in the caller's cached object.
//   * IndexIsDeterministic — the same input twice yields the same plan.
//   * IndexRefusesMixedTargets — the self-identity check.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "mir/summary/summary_index.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace dss;
using namespace dss::mirsum;

namespace {

// The index reports conflicts as DATA (`conflictingNames`) AND on the
// diagnostic channel, and a pin that watched only the data could not tell a
// reported conflict from a silent one — so every test here checks both.
using Collector = DiagnosticReporter;

[[nodiscard]] std::size_t countCodeIn(DiagnosticReporter const& r,
                                      DiagnosticCode c) {
    std::size_t n = 0;
    for (ParseDiagnostic const& d : r.all()) {
        if (d.code == c) ++n;
    }
    return n;
}

// A minimal INLINABLE function row: small, strong, returns, no shape refusal.
SummaryFunction fn(std::string name, std::uint32_t symbol,
                   std::uint32_t instCount = 3,
                   SymbolBinding binding = SymbolBinding::Global) {
    SummaryFunction f;
    f.name       = std::move(name);
    f.symbol     = symbol;
    f.binding    = binding;
    f.visibility = SymbolVisibility::Default;
    f.instCount  = instCount;
    f.blockCount = 1;
    f.hasReturn  = true;
    return f;
}

void calls(SummaryFunction& f, std::string callee) {
    SummaryCallSite c;
    c.calleeName = std::move(callee);
    c.direct     = true;
    f.calls.push_back(std::move(c));
    f.symbolRefs.push_back(f.calls.back().calleeName);
    std::sort(f.symbolRefs.begin(), f.symbolRefs.end());
    f.symbolRefs.erase(std::unique(f.symbolRefs.begin(), f.symbolRefs.end()),
                       f.symbolRefs.end());
}

ModuleSummary mod(std::string digest, std::vector<SummaryFunction> fns) {
    ModuleSummary s;
    s.moduleDigest   = std::move(digest);
    s.targetIdentity = "x86_64:elf64";
    s.functions      = std::move(fns);
    return s;
}

[[nodiscard]] bool planHas(SummaryIndex const& ix, std::uint32_t importer,
                           std::string const& callee) {
    for (ImportDecision const& d : ix.importPlan) {
        if (d.importerModule == importer && d.calleeName == callee) return true;
    }
    return false;
}

SummaryIndexPolicy defaultPolicy() {
    // Straight from `release.pipeline.json`: `inlineThreshold: 50` and the
    // Inlining fixpoint's own `{"max": 4}`. NOTHING here is a new constant.
    SummaryIndexPolicy p;
    p.inlineThreshold = 50;
    p.maxImportDepth  = 4;
    return p;
}

} // namespace

TEST(SummaryIndex, IndexResolvesStrongOverWeakAcrossModules) {
    // Module 0 defines `f` WEAK; module 1 defines it STRONG. The linker's
    // policy says the strong definition wins, and the index must agree with
    // `mergeCuMirs` because both delegate to `resolveCrossCuDefs`.
    ModuleSummary m0 = mod("d0", {fn("f", 50, 3, SymbolBinding::Weak)});
    ModuleSummary m1 = mod("d1", {fn("f", 51, 3, SymbolBinding::Global)});
    std::vector<ModuleSummary> const mods{m0, m1};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());

    auto const site = ix->definitionOf("f");
    ASSERT_TRUE(site.has_value());
    EXPECT_EQ(site->moduleIndex, 1u) << "the STRONG definition must win";
    EXPECT_TRUE(site->isFunction);
    EXPECT_TRUE(ix->conflictingNames.empty());
}

TEST(SummaryIndex, IndexReportsTwoStrongConflict) {
    ModuleSummary m0 = mod("d0", {fn("f", 50)});
    ModuleSummary m1 = mod("d1", {fn("f", 51)});
    std::vector<ModuleSummary> const mods{m0, m1};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    // Reported, not silently resolved — and the build still proceeds with the
    // resolver's winner, which is what `mergeCuMirs` does for the same reason.
    ASSERT_TRUE(ix.has_value());
    EXPECT_EQ(ix->conflictingNames, (std::vector<std::string>{"f"}));
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(countCodeIn(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits), 1u);
}

TEST(SummaryIndex, IndexUnionsTheAddressEscapeSetAcrossModules) {
    ModuleSummary m0 = mod("d0", {fn("f", 50)});
    ModuleSummary m1 = mod("d1", {fn("g", 51)});
    m1.escapedSymbolNames = {"f"};   // module 1 takes the address of module 0's f
    std::vector<ModuleSummary> const mods{m0, m1};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    EXPECT_EQ(ix->escapedSymbols.count("f"), 1u);
    EXPECT_EQ(ix->escapedSymbols.count("g"), 0u);
}

TEST(SummaryIndex, IndexRefusesACalleeEscapingInAnotherModule) {
    // ★★★ THE SILENT MISCOMPILE THIS WHOLE FIELD EXISTS FOR.
    //
    // Module 0 CALLS `f`. Module 2 takes `f`'s ADDRESS. `inlineLegalityGate`
    // rule 4 refuses to inline a callee whose address escapes anywhere in the
    // module — and today "the module" is the whole merged program, so this
    // refusal happens. A per-TU escape analysis would see module 0 in
    // isolation, find no escape, inline `f`, and allow its out-of-line body to
    // be dropped while module 2's indirect call still reaches for it.
    //
    // RED-ON-DISABLE: delete `m2.escapedSymbolNames` and the candidate is
    // admitted, which is the pre-fix (wrong) behaviour.
    SummaryFunction caller = fn("main", 100);
    calls(caller, "f");
    ModuleSummary m0 = mod("d0", {caller});
    ModuleSummary m1 = mod("d1", {fn("f", 50)});
    ModuleSummary m2 = mod("d2", {fn("takesAddr", 60)});
    m2.escapedSymbolNames = {"f"};
    std::vector<ModuleSummary> const mods{m0, m1, m2};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    ASSERT_EQ(ix->escapedSymbols.count("f"), 1u);
    EXPECT_FALSE(planHas(*ix, 0, "f"))
        << "a callee whose address escapes in ANOTHER module must not be "
           "offered for import";

    // The complement, so the pin cannot pass by refusing everything: with the
    // escape removed the very same callee IS offered.
    ModuleSummary m2clean = mod("d2", {fn("takesAddr", 60)});
    std::vector<ModuleSummary> const clean{m0, m1, m2clean};
    Collector rep2;
    auto const ix2 = buildSummaryIndex(clean, defaultPolicy(), rep2);
    ASSERT_TRUE(ix2.has_value());
    EXPECT_TRUE(planHas(*ix2, 0, "f"));
}

TEST(SummaryIndex, IndexFindsCrossModuleRecursionSCC) {
    // `f` (module 0) calls `g` (module 1) calls `f`. A per-TU SCC sees two
    // acyclic graphs; inlining inside this cycle would unroll an unbounded
    // recursion at inline time. `inlineLegalityGate` rule 3 refuses it, and it
    // can only do so if the index hands it a WHOLE-PROGRAM SCC.
    SummaryFunction f = fn("f", 50);
    calls(f, "g");
    SummaryFunction g = fn("g", 51);
    calls(g, "f");
    SummaryFunction h = fn("h", 52);   // outside the cycle — the control
    std::vector<ModuleSummary> const mods{mod("d0", {f}), mod("d1", {g, h})};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    ASSERT_EQ(ix->sccOf.count("f"), 1u);
    ASSERT_EQ(ix->sccOf.count("g"), 1u);
    ASSERT_EQ(ix->sccOf.count("h"), 1u);
    EXPECT_EQ(ix->sccOf.at("f"), ix->sccOf.at("g"))
        << "f and g are mutually recursive ACROSS a module boundary";
    EXPECT_NE(ix->sccOf.at("h"), ix->sccOf.at("f"))
        << "h is not in the cycle — an SCC that swallowed it would refuse "
           "every inline in the program";
}

TEST(SummaryIndex, IndexComputesWholeProgramLiveness) {
    // `main` is an externally-visible root and calls `used`. `unused` is a
    // hidden, uncalled definition — dead for the whole program, and NO per-TU
    // pass can prove that, because module 1 cannot know nobody calls it.
    SummaryFunction main = fn("main", 100);
    calls(main, "used");
    SummaryFunction used = fn("used", 50);
    SummaryFunction unused = fn("unused", 51);
    unused.visibility = SymbolVisibility::Hidden;
    unused.binding    = SymbolBinding::Local;
    std::vector<ModuleSummary> const mods{mod("d0", {main}),
                                          mod("d1", {used, unused})};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    EXPECT_EQ(ix->liveSymbols.count("main"), 1u);
    EXPECT_EQ(ix->liveSymbols.count("used"), 1u);
    EXPECT_EQ(ix->liveSymbols.count("unused"), 0u);
    EXPECT_EQ(ix->deadSymbolsPerModule.at(1),
              (std::vector<std::string>{"unused"}));
    EXPECT_TRUE(ix->deadSymbolsPerModule.at(0).empty());
}

TEST(SummaryIndex, IndexPropagatesLivenessThroughGlobalInitializers) {
    // ★ `scanLiveSymbols` PHASE 3. `handler` is called by nobody; it is
    // reachable ONLY as a symbol-address target inside the live global
    // `table`'s initializer — a function-pointer table. Without the initializer
    // edge the index would call it dead, the per-TU DCE would delete it, and
    // the linker would then fail on the dangling data relocation.
    SummaryFunction main = fn("main", 100);
    SummaryFunction handler = fn("handler", 50);
    handler.binding = SymbolBinding::Local;   // not a root on its own

    SummaryGlobal table;
    table.name           = "table";
    table.symbol         = 200;
    table.binding        = SymbolBinding::Global;
    table.visibility     = SymbolVisibility::Default;
    table.isConst        = true;
    table.initSymbolRefs = {"handler"};

    ModuleSummary m1 = mod("d1", {handler});
    m1.globals.push_back(table);
    std::vector<ModuleSummary> const mods{mod("d0", {main}), m1};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    EXPECT_EQ(ix->liveSymbols.count("handler"), 1u)
        << "a function reachable only through a data relocation is live";
    EXPECT_TRUE(ix->deadSymbolsPerModule.at(1).empty());
}

TEST(SummaryIndex, IndexPlansCrossModuleImports) {
    SummaryFunction main = fn("main", 100);
    calls(main, "f");
    std::vector<ModuleSummary> const mods{mod("d0", {main}),
                                          mod("d1", {fn("f", 50, /*insts=*/3)})};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    ASSERT_EQ(ix->importPlan.size(), 1u);
    EXPECT_EQ(ix->importPlan[0].importerModule, 0u);
    EXPECT_EQ(ix->importPlan[0].definingModule, 1u);
    EXPECT_EQ(ix->importPlan[0].calleeName, "f");
    EXPECT_EQ(ix->importPlan[0].calleeInstCount, 3u);
    EXPECT_EQ(ix->importPlan[0].depth, 1u);
}

TEST(SummaryIndex, IndexRespectsTheInlineThreshold) {
    // The SAME `>` comparison `inlineLegalityGate` rule 6 applies: a callee of
    // EXACTLY the threshold still inlines, one instruction over is refused.
    // Pinning the boundary and not just the middle is the point — an off-by-one
    // here silently changes what the whole program inlines.
    SummaryIndexPolicy p = defaultPolicy();
    p.inlineThreshold    = 10;

    SummaryFunction main = fn("main", 100);
    calls(main, "atThreshold");
    calls(main, "overThreshold");
    std::vector<ModuleSummary> const mods{
        mod("d0", {main}),
        mod("d1", {fn("atThreshold", 50, 10), fn("overThreshold", 51, 11)})};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, p, rep);
    ASSERT_TRUE(ix.has_value());
    EXPECT_TRUE(planHas(*ix, 0, "atThreshold"));
    EXPECT_FALSE(planHas(*ix, 0, "overThreshold"));

    // `always_inline` waives the PROFITABILITY veto and nothing else — exactly
    // what the gate does with it.
    SummaryFunction big = fn("overThreshold", 51, 11);
    big.alwaysInline = true;
    std::vector<ModuleSummary> const waived{
        mod("d0", {main}), mod("d1", {fn("atThreshold", 50, 10), big})};
    Collector rep2;
    auto const ix2 = buildSummaryIndex(waived, p, rep2);
    ASSERT_TRUE(ix2.has_value());
    EXPECT_TRUE(planHas(*ix2, 0, "overThreshold"));
}

TEST(SummaryIndex, IndexRefusesWeakAndDirectiveRefusals) {
    SummaryFunction main = fn("main", 100);
    calls(main, "weakF");
    calls(main, "noInlineF");
    calls(main, "noOptF");
    calls(main, "noReturnF");
    calls(main, "sehF");

    SummaryFunction noInlineF = fn("noInlineF", 51);
    noInlineF.noInline = true;
    SummaryFunction noOptF = fn("noOptF", 52);
    noOptF.noOptimize = true;
    SummaryFunction noReturnF = fn("noReturnF", 53);
    noReturnF.hasReturn = false;
    SummaryFunction sehF = fn("sehF", 54);
    sehF.hasSeh = true;

    std::vector<ModuleSummary> const mods{
        mod("d0", {main}),
        mod("d1", {fn("weakF", 50, 3, SymbolBinding::Weak), noInlineF, noOptF,
                   noReturnF, sehF})};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    // Every one of these is a refusal `inlineLegalityGate` makes today, so the
    // index must not waste an import offering it. None is a QUALITY loss: the
    // gate would decline each after the body arrived.
    EXPECT_FALSE(planHas(*ix, 0, "weakF"));
    EXPECT_FALSE(planHas(*ix, 0, "noInlineF"));
    EXPECT_FALSE(planHas(*ix, 0, "noOptF"));
    EXPECT_FALSE(planHas(*ix, 0, "noReturnF"));
    EXPECT_FALSE(planHas(*ix, 0, "sehF"));
    EXPECT_TRUE(ix->importPlan.empty());
}

TEST(SummaryIndex, IndexSkipsSameModuleCallees) {
    // A callee in the importer's OWN module needs no import — the in-module
    // inliner already reaches it, and planning one would crowd out a real
    // import under a budget.
    SummaryFunction main = fn("main", 100);
    calls(main, "local");
    std::vector<ModuleSummary> const mods{mod("d0", {main, fn("local", 50)})};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    EXPECT_TRUE(ix->importPlan.empty());
}

TEST(SummaryIndex, IndexFollowsTransitiveEdgesToTheDepthBound) {
    // main(m0) → a(m1) → b(m2) → c(m3) → d(m4). Depth 4 reaches a, b, c, d.
    // ⓘ The bound is not "4 levels of arbitrary functions" — every edge must
    // first pass `isInlineCandidate`, so it is "4 levels of functions ALREADY
    // SMALL ENOUGH TO INLINE", which is what keeps the frontier from
    // degenerating into the whole program.
    SummaryFunction main = fn("main", 100); calls(main, "a");
    SummaryFunction a = fn("a", 50);        calls(a, "b");
    SummaryFunction b = fn("b", 51);        calls(b, "c");
    SummaryFunction c = fn("c", 52);        calls(c, "d");
    SummaryFunction d = fn("d", 53);        calls(d, "e");
    SummaryFunction e = fn("e", 54);
    std::vector<ModuleSummary> const mods{
        mod("d0", {main}), mod("d1", {a}), mod("d2", {b}), mod("d3", {c}),
        mod("d4", {d}), mod("d5", {e})};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    ASSERT_TRUE(ix.has_value());
    for (char const* n : {"a", "b", "c", "d"}) {
        EXPECT_TRUE(planHas(*ix, 0, n)) << "depth-4 prefetch should reach " << n;
    }
    EXPECT_FALSE(planHas(*ix, 0, "e")) << "one level past the bound";

    // Depth 1 is the shallow control, so the pin cannot pass by ignoring the
    // bound entirely.
    SummaryIndexPolicy shallow = defaultPolicy();
    shallow.maxImportDepth = 1;
    Collector rep2;
    auto const ix2 = buildSummaryIndex(mods, shallow, rep2);
    ASSERT_TRUE(ix2.has_value());
    EXPECT_TRUE(planHas(*ix2, 0, "a"));
    EXPECT_FALSE(planHas(*ix2, 0, "b"));
}

TEST(SummaryIndex, IndexBudgetBoundsPrefetchedInstructions) {
    // The budget is the knob that turns this into LLVM ThinLTO's budget-based
    // import policy without a second implementation. 0 (the default) is no
    // ceiling; a nonzero value stops charging when the next callee would not
    // fit.
    SummaryFunction main = fn("main", 100);
    calls(main, "big1");
    calls(main, "big2");
    std::vector<ModuleSummary> const mods{
        mod("d0", {main}),
        mod("d1", {fn("big1", 50, 30), fn("big2", 51, 30)})};

    SummaryIndexPolicy p = defaultPolicy();
    p.perModuleImportInstBudget = 40;   // fits one 30-inst callee, not two
    Collector rep;
    auto const ix = buildSummaryIndex(mods, p, rep);
    ASSERT_TRUE(ix.has_value());
    EXPECT_EQ(ix->importPlan.size(), 1u);

    p.perModuleImportInstBudget = 0;    // no ceiling
    Collector rep2;
    auto const ix2 = buildSummaryIndex(mods, p, rep2);
    ASSERT_TRUE(ix2.has_value());
    EXPECT_EQ(ix2->importPlan.size(), 2u);
}

TEST(SummaryIndex, IndexTier2KeyChangesWhenACalleeChanges) {
    // ★★★ THE SILENT MISCOMPILE. Keying the POST-IMPORT object on the importing
    // TU alone means editing a callee leaves a STALE INLINED COPY of its old
    // body inside the caller's cached object. Module 0 is byte-identical in
    // both runs — only module 1's digest moves — and module 0's Tier-2 key MUST
    // still move with it.
    SummaryFunction main = fn("main", 100);
    calls(main, "f");
    std::vector<ModuleSummary> const before{mod("d0", {main}),
                                            mod("d1", {fn("f", 50)})};
    std::vector<ModuleSummary> const after{mod("d0", {main}),
                                           mod("d1-EDITED", {fn("f", 50)})};

    Collector r1, r2;
    auto const a = buildSummaryIndex(before, defaultPolicy(), r1);
    auto const b = buildSummaryIndex(after, defaultPolicy(), r2);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    EXPECT_EQ(a->tier2KeyInputs[0].ownDigest, b->tier2KeyInputs[0].ownDigest)
        << "the IMPORTER itself did not change";
    EXPECT_NE(a->tier2KeyInputs[0].importedFrom,
              b->tier2KeyInputs[0].importedFrom)
        << "editing the CALLEE must invalidate the CALLER's post-import object";
    ASSERT_EQ(a->tier2KeyInputs[0].importedFrom.size(), 1u);
    EXPECT_EQ(a->tier2KeyInputs[0].importedFrom[0].first, "f");
    EXPECT_EQ(a->tier2KeyInputs[0].importedFrom[0].second, "d1");
    EXPECT_EQ(b->tier2KeyInputs[0].importedFrom[0].second, "d1-EDITED");

    // The POLICY is in the key too — a different `inlineThreshold` inlines
    // different things, so a cached object from another policy is not reusable.
    SummaryIndexPolicy p = defaultPolicy();
    p.inlineThreshold = 7;
    Collector r3;
    auto const c = buildSummaryIndex(before, p, r3);
    ASSERT_TRUE(c.has_value());
    EXPECT_NE(a->tier2KeyInputs[0].policyIdentity,
              c->tier2KeyInputs[0].policyIdentity);
}

TEST(SummaryIndex, IndexIsDeterministic) {
    // The determinism half of the arc's bar, at the decision layer: the plan is
    // a stable VALUE, not an artefact of hash iteration or of the order a
    // worklist happened to pop.
    SummaryFunction main = fn("main", 100);
    calls(main, "a");
    calls(main, "b");
    calls(main, "c");
    SummaryFunction a = fn("a", 50); calls(a, "d");
    std::vector<ModuleSummary> const mods{
        mod("d0", {main}),
        mod("d1", {a, fn("b", 51), fn("c", 52), fn("d", 53)})};

    Collector r1, r2;
    auto const x = buildSummaryIndex(mods, defaultPolicy(), r1);
    auto const y = buildSummaryIndex(mods, defaultPolicy(), r2);
    ASSERT_TRUE(x.has_value());
    ASSERT_TRUE(y.has_value());
    ASSERT_EQ(x->importPlan.size(), y->importPlan.size());
    for (std::size_t i = 0; i < x->importPlan.size(); ++i) {
        EXPECT_EQ(x->importPlan[i].calleeName, y->importPlan[i].calleeName);
        EXPECT_EQ(x->importPlan[i].importerModule,
                  y->importPlan[i].importerModule);
        EXPECT_EQ(x->importPlan[i].depth, y->importPlan[i].depth);
    }
    // Sorted by (importerModule, calleeName), so the plan is order-independent
    // of how it was discovered.
    EXPECT_TRUE(std::is_sorted(
        x->importPlan.begin(), x->importPlan.end(),
        [](ImportDecision const& l, ImportDecision const& r) {
            if (l.importerModule != r.importerModule)
                return l.importerModule < r.importerModule;
            return l.calleeName < r.calleeName;
        }));
}

TEST(SummaryIndex, IndexRefusesMixedTargets) {
    // The self-identity fields exist so a summary can never be mixed across
    // machines. The bodies behind these two summaries were compiled for
    // different targets; importing one into the other is a miscompile, so this
    // is a structural REFUSAL, not a warning.
    ModuleSummary m0 = mod("d0", {fn("main", 100)});
    ModuleSummary m1 = mod("d1", {fn("f", 50)});
    m1.targetIdentity = "arm64:macho64";
    std::vector<ModuleSummary> const mods{m0, m1};

    Collector rep;
    auto const ix = buildSummaryIndex(mods, defaultPolicy(), rep);
    EXPECT_FALSE(ix.has_value());
    EXPECT_EQ(rep.errorCount(), 1u);
    EXPECT_EQ(countCodeIn(rep, DiagnosticCode::K_CrossCuMergeUnsupported), 1u);
}
