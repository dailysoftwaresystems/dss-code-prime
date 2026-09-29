// D-MIR-STRUCTCF-DERIVATION-REACHES-PAST-THE-FUNCTION — EVERY consumer of the
// natural-loop forest sees only the function it asks about, and each one is
// pinned HERE, in one file, against one fixture.
//
// ★★★ WHAT CHANGED. A function's natural loops are the loops whose back-edge
// SOURCE is one of its own blocks; `mirBackEdgeCandidates` (mir/mir_dom.hpp)
// states that rule, and `mirNaturalLoops` sweeps exactly the set it builds.
// Until 2026-09-18 that set also carried every SELF-LOOPING block of the whole
// MODULE: `mirDominatesBlock(s, s)` answers `Dominates` before consulting the
// tree, so each foreign self-loop became a one-block pseudo-loop in EVERY
// function's forest — F functions × S self-loops of work per consumer call,
// quadratic in module size. ✔MEASURED on the sqlite amalgamation, release: 98.8%
// of every LICM forest was foreign on the full 9.57 MB, and scoping the rule
// retired 11.0% fewer instructions with no artifact byte changed.
//
// ★★★ "THE VERIFIER AND EVERY PRODUCER MOVE TOGETHER" — PROVEN, NOT ASSUMED.
// The forest has exactly four consumers, and each reaches it through the ONE
// builder:
//   * the marker APPLIER `rederiveStructCfMarkers(Mir&)` → `deriveStructCfMarkers`
//     → `deriveInto` → `mirBackEdgeCandidates`. Every producer re-stamps through
//     this one function: `lowerToMir` (HIR→MIR lowering), `runSimplifyCfg`,
//     `runInlining`, `mergeCuMirs` (the cross-CU merge), `runPruneUnreachableBlocks`,
//     `optimize` (the release posture's single post-pipeline re-stamp), and the
//     merge-side synthesizers `realizeEntryShape`, `synthesizeSehFunclets`,
//     `synthesizeStdioShim`, `synthesizeThreadsShim`;
//   * the VERIFIER `MirVerifier::checkDomination` → the same
//     `deriveStructCfMarkers` (scratch overload) → `deriveInto`;
//   * LICM `LicmPolicy::analyze` → `mirBackEdgeCandidates`;
//   * the thin-LTO module SUMMARY `computeLoopDepths` → `mirBackEdgeCandidates`.
// Each arm below runs ONE consumer over the same module and COUNTS the back-edge
// sources its sweeps visited (`mirNaturalLoopSourcesSweptTake`): exactly the
// module's function blocks, once each. A consumer that lets another function's
// blocks into a function's sweep — through the shared builder or around it —
// reds its own arm, and only its own.
//
// ★★ WHY IT COUNTS AND DOES NOT TIME: a stopwatch assertion is sized on the
// machine that wrote it; the count is deterministic, identical in Debug and
// Release, and independent of load.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_dom.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_verifier.hpp"
#include "mir/summary/mir_summary.hpp"
#include "opt/passes/licm.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <string>

using namespace dss;

namespace {

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue l;
    l.value = v;
    l.core  = TypeKind::I32;
    return l;
}

// `n` functions, each `int f(bool c) { while (c) {} return 0; }` as MIR:
//   entry → spin ; spin: CondBr(c, spin, exit) ; exit: return 0
// so every function owns exactly ONE self-looping block, and the module holds n
// of them — the shape that made every function's forest carry n pseudo-loops.
Mir buildSelfLoopModule(TypeInterner& interner, std::uint32_t n) {
    TypeId const i32      = interner.primitive(TypeKind::I32);
    TypeId const boolT    = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig    = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    for (std::uint32_t k = 0; k < n; ++k) {
        mb.addFunction(fnSig, SymbolId{100u + k});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const spin  = mb.createBlock(StructCfMarker::LoopHeader);
        MirBlockId const exitB = mb.createBlock(StructCfMarker::LoopExit);
        mb.beginBlock(entry);
        MirInstId const c = mb.addArg(0, boolT);
        mb.addBr(spin);
        mb.beginBlock(spin);
        mb.addCondBr(c, spin, exitB);
        mb.beginBlock(exitB);
        mb.addReturn(mb.addConst(i32Lit(0), i32));
    }
    return std::move(mb).finish();
}

// The module's function blocks, and its self-looping blocks — both re-derived
// HERE from the Mir, never from the helpers under test.
std::uint64_t ownBlocksOf(Mir const& mir) {
    std::uint64_t n = 0;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        n += mir.funcBlockCount(mir.funcAt(i));
    }
    return n;
}

std::uint64_t selfLoopingBlocksOf(Mir const& mir) {
    std::uint64_t n = 0;
    for (std::uint32_t i = 1; i < mir.blockCount(); ++i) {
        MirBlockId const b{i, mir.id().v};
        for (MirBlockId const s : mir.blockSuccessors(b)) {
            if (s.valid() && s.v == i) { ++n; break; }
        }
    }
    return n;
}

enum class Consumer { Applier, Verifier, Licm, Summary };

char const* nameOf(Consumer c) {
    switch (c) {
        case Consumer::Applier:  return "marker-applier";
        case Consumer::Verifier: return "verifier";
        case Consumer::Licm:     return "licm";
        case Consumer::Summary:  return "module-summary";
    }
    return "?";
}

struct Sweep {
    std::uint64_t swept      = 0;   // back-edge sources the consumer's sweeps visited
    std::uint64_t ownBlocks  = 0;   // Σ funcBlockCount — the linear answer
    std::uint64_t selfLoops  = 0;   // the fixture's premise
    bool          consumerOk = false;
};

// Run ONE consumer over a fresh `n`-function module and read the counter.
Sweep sweepOf(Consumer c, std::uint32_t n) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildSelfLoopModule(interner, n);
    Sweep s;
    s.ownBlocks = ownBlocksOf(mir);
    s.selfLoops = selfLoopingBlocksOf(mir);
    if (c == Consumer::Verifier) rederiveStructCfMarkers(mir);   // canonical stamps first
    (void)mirNaturalLoopSourcesSweptTake();                       // zero THIS thread's counter
    switch (c) {
        case Consumer::Applier:
            rederiveStructCfMarkers(mir);
            s.consumerOk = true;
            break;
        case Consumer::Verifier: {
            DiagnosticReporter rep;
            MirVerifier const v{mir, &interner};
            s.consumerOk = v.verify(rep);
            break;
        }
        case Consumer::Licm: {
            DiagnosticReporter rep;
            s.consumerOk = opt::passes::runLicm(mir, interner, rep).ok;
            break;
        }
        case Consumer::Summary: {
            mirsum::SummaryCuInput cu;
            cu.mir    = &mir;
            cu.nameOf = [](SymbolId sym) { return "f" + std::to_string(sym.v); };
            cu.moduleDigest   = "digest";
            cu.targetIdentity = "x86_64:elf64";
            mirsum::ModuleSummary const sum = mirsum::buildModuleSummary(cu);
            s.consumerOk = sum.functions.size() == n;
            break;
        }
    }
    s.swept = mirNaturalLoopSourcesSweptTake();
    std::cout << "[loop-reach] consumer=" << nameOf(c) << " functions=" << n
              << " ownBlocks=" << s.ownBlocks << " moduleSelfLoops=" << s.selfLoops
              << " sourcesSwept=" << s.swept << "\n";
    return s;
}

constexpr std::uint32_t kFunctions = 64;

void expectOwnBlocksOnly(Consumer c) {
    for (std::uint32_t const n : {kFunctions, 2 * kFunctions}) {
        Sweep const s = sweepOf(c, n);
        ASSERT_TRUE(s.consumerOk) << nameOf(c) << " did not run cleanly";
        ASSERT_EQ(s.selfLoops, n)
            << "premise: every function owns one self-looping block, so a "
               "sweep that reaches across functions is observable";
        EXPECT_EQ(s.swept, s.ownBlocks)
            << nameOf(c) << " (n=" << n << ") swept " << s.swept
            << " back-edge sources for a module of " << s.ownBlocks
            << " function blocks — every excess source is a block of ANOTHER "
               "function inside a function's forest, and it grows as "
               "functions × self-loops";
    }
}

} // namespace

// CONTROL — what the markers SAY is unchanged by the narrowing: every function's
// own blocks derive EntryBlock / LoopHeader / LoopExit whether or not other
// functions' self-loops are in its sweep (foreign pseudo-loops only ever claimed
// FOREIGN slots). Green under the reach mutant, which is what makes the arms
// below evidence about the REACH and its cost specifically.
TEST(NaturalLoopReach, OwnSlotMarkersAreTheSameControl) {
    TypeInterner interner{CompilationUnitId{1}};
    Mir mir = buildSelfLoopModule(interner, kFunctions);
    rederiveStructCfMarkers(mir);
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        ASSERT_EQ(mir.funcBlockCount(f), 3u);
        EXPECT_EQ(mir.blockMarker(mir.funcBlockAt(f, 0)), StructCfMarker::EntryBlock);
        EXPECT_EQ(mir.blockMarker(mir.funcBlockAt(f, 1)), StructCfMarker::LoopHeader);
        EXPECT_EQ(mir.blockMarker(mir.funcBlockAt(f, 2)), StructCfMarker::LoopExit);
    }
    DiagnosticReporter rep;
    EXPECT_TRUE((MirVerifier{mir, &interner}.verify(rep)))
        << "the verifier re-derives through the same rule the applier stamped with";
}

TEST(NaturalLoopReach, MarkerApplierSweepsOnlyEachFunctionsOwnBlocks) {
    expectOwnBlocksOnly(Consumer::Applier);
}

TEST(NaturalLoopReach, VerifierSweepsOnlyEachFunctionsOwnBlocks) {
    expectOwnBlocksOnly(Consumer::Verifier);
}

TEST(NaturalLoopReach, LicmSweepsOnlyEachFunctionsOwnBlocks) {
    expectOwnBlocksOnly(Consumer::Licm);
}

TEST(NaturalLoopReach, ModuleSummarySweepsOnlyEachFunctionsOwnBlocks) {
    expectOwnBlocksOnly(Consumer::Summary);
}

// The one OBSERVABLE the reach produced besides cost: LICM analyzed every
// FOREIGN self-loop as a loop of the function it was working on, so a
// self-loop with no unique preheader drew one `X_OptPassSkipped` Info PER
// FUNCTION IN THE MODULE — each about a loop that function does not own
// (✔MEASURED on the corpus: `computed_goto_inline_host`, release, all three
// targets — the only diagnostic text the narrowing changed). Here the owner has
// an UNREACHABLE self-looping block (no predecessor but itself, hence no
// preheader) and `kOthers` functions have no loop at all; the Info must come
// exactly once, from the owner. `dedupWindow = 0` so no report is collapsed —
// the default window would hide identical repeats and make this arm vacuous.
TEST(NaturalLoopReach, LicmReportsASelfLoopOnlyFromTheFunctionThatOwnsIt) {
    constexpr std::uint32_t kOthers = 5;
    TypeInterner interner{CompilationUnitId{1}};
    TypeId const i32      = interner.primitive(TypeKind::I32);
    TypeId const boolT    = interner.primitive(TypeKind::Bool);
    TypeId const params[] = {boolT};
    TypeId const fnSig    = interner.fnSig(params, i32, CallConv::CcSysV);
    MirBuilder mb;
    MirBlockId gSelf{};
    for (std::uint32_t k = 0; k <= kOthers; ++k) {
        mb.addFunction(fnSig, SymbolId{100u + k});
        MirBlockId const entry = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(entry);
        MirInstId const c = mb.addArg(0, boolT);
        mb.addReturn(mb.addConst(i32Lit(0), i32));
        if (k != 2) continue;   // the owner sits in the MIDDLE of the module
        gSelf = mb.createBlock();
        MirBlockId const tail = mb.createBlock();
        mb.beginBlock(gSelf); mb.addCondBr(c, gSelf, tail);   // unreachable self-loop
        mb.beginBlock(tail);  mb.addReturn(mb.addConst(i32Lit(1), i32));
    }
    Mir mir = std::move(mb).finish();
    ASSERT_TRUE(gSelf.valid());

    DiagnosticReporter::Config cfg;
    cfg.dedupWindow = 0;
    cfg.maxPerCode  = 1000;
    DiagnosticReporter rep{cfg};
    ASSERT_TRUE(opt::passes::runLicm(mir, interner, rep).ok);
    std::string const needle = "header v=" + std::to_string(gSelf.v) + " ";
    std::size_t aboutTheSelfLoop = 0;
    for (ParseDiagnostic const& d : rep.all()) {
        if (d.code == DiagnosticCode::X_OptPassSkipped
            && d.actual.find(needle) != std::string::npos) {
            ++aboutTheSelfLoop;
        }
    }
    EXPECT_EQ(aboutTheSelfLoop, 1u)
        << "the self-loop's Info must come from its OWN function only; "
        << aboutTheSelfLoop << " reports means " << aboutTheSelfLoop - 1
        << " other function(s) analyzed a loop they do not own";
}
