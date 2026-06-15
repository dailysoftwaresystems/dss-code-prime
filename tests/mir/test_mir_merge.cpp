// Cross-module MIR merge (Cycle 25, Stage B) — `mergeCuMirs` unit tests, the
// MERGE CORE tested IN ISOLATION at the MIR tier with HAND-BUILT inputs (no real
// SemanticModel). The merge folds N per-CU `Mir` modules into ONE whole-program
// module: it reinterns every cross-CU TypeId into one host lattice, mints a
// unified SymbolId space, resolves cross-CU references (weak-vs-strong) so a
// cross-CU call becomes a DIRECT intra-module call, and drops shadowed-weak
// losers. Stage C (separate) wires it into the driver.
//
// STRICT pins (each a POSITIVE symbol-identity / structural assertion, several
// RED-on-swap if a remap targets the wrong symbol):
//   * MergeRewiresCrossCuCallToDirect — main's extern call to `f` rewires to the
//     MERGED f's symbol (== f's funcSymbol; RED if it kept the extern's id) +
//     the extern is STRIPPED + verifier-clean.
//   * MergeClonesMultiBlockCallee — a multi-block callee's CFG is cloned 1:1
//     (block count + CondBr/Return shape preserved) + verifies.
//   * MergeRederivesStaleInputMarkers — an input block carrying a STALE
//     StructCfMarker (dormant ExitBlock) is corrected to the DERIVED marker by
//     the merge's post-clone rederiveStructCfMarkers call (RED-on-disable: the
//     merge's internal stored==derived verifier fires I_StructCfMismatch).
//   * MergeReinternsTypesIntoHost — a CU1-built pointer type is HOST-stamped in
//     the merged module + structurally a pointer.
//   * MergeDropsShadowedWeak — CU0 weak f shadowed by CU1 strong f: only the
//     STRONG body survives (returns 42, not 7) + weak's name maps to the strong
//     merged symbol + verifier-clean.
//   * MergeRemapsGlobalInitFunc — a global's initFunc MirFuncId is remapped into
//     the merged func space (points at the merged init function, not a stale id).
//   * MergeReportsTwoStrongConflict — two strong `f` defs emit exactly one
//     K_SymbolRedefinedAcrossUnits.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "mir/merge/mir_merge.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_verifier.hpp"

#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

using namespace dss;

namespace {

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// A symbol→name lambda over a small fixed map (the hand-built stand-in for the
// SemanticModel name lookup the driver supplies).
std::function<std::string(SymbolId)>
namerOf(std::unordered_map<std::uint32_t, std::string> table) {
    return [table = std::move(table)](SymbolId s) -> std::string {
        auto const it = table.find(s.v);
        return it == table.end() ? std::string{} : it->second;
    };
}

// Find the merged function whose declared name == `name`; aborts the test if
// absent (the caller asserts presence first).
[[nodiscard]] std::optional<MirFuncId>
findFuncByName(Mir const& mir,
               std::unordered_map<std::uint32_t, std::string> const& names,
               std::string const& name) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        auto const it = names.find(mir.funcSymbol(f).v);
        if (it != names.end() && it->second == name) return f;
    }
    return std::nullopt;
}

// The first Call instruction in a function (the cross-CU call under test).
[[nodiscard]] std::optional<MirInstId>
firstCall(Mir const& mir, MirFuncId f) {
    std::uint32_t const nb = mir.funcBlockCount(f);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const b = mir.funcBlockAt(f, bi);
        std::uint32_t const ni = mir.blockInstCount(b);
        for (std::uint32_t ii = 0; ii < ni; ++ii) {
            MirInstId const id = mir.blockInstAt(b, ii);
            if (mir.instOpcode(id) == MirOpcode::Call) return id;
        }
    }
    return std::nullopt;
}

std::size_t countOp(Mir const& mir, MirOpcode want) {
    std::size_t n = 0;
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
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

} // namespace

// ── A cross-CU call rewires to a DIRECT intra-module call ──────────
// CU0: int main() { return f(); } where `f` is an EXTERN (the import row carries
// mangledName "f"). CU1: int f() { return 7; } (single block). After merge: BOTH
// main + f are present; main's Call's GlobalAddr operand resolves to the MERGED
// f's symbol (the POSITIVE shape — RED-on-swap if the merge keeps the extern's
// own fresh id instead of collapsing to the winner); the extern is STRIPPED.
TEST(MirMerge, MergeRewiresCrossCuCallToDirect) {
    // ── CU0: main calls extern f. ──
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32_0  = in0.primitive(TypeKind::I32);
    TypeId const sig0   = in0.fnSig({}, i32_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{100});  // main
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const fAddr = mb.addGlobalAddr(SymbolId{10}, sig0);  // extern f
        MirInstId const callOps[] = {fAddr};
        MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32_0);
        mb.addReturn(call);
        mir0 = std::move(mb).finish();
    }
    std::vector<ExternImport> ext0 = {ExternImport{SymbolId{10}, "f", "libc.so"}};

    // ── CU1: int f() { return 7; } ──
    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i32_1 = in1.primitive(TypeKind::I32);
    TypeId const sig1  = in1.fnSig({}, i32_1, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        mb.addFunction(sig1, SymbolId{50});  // f
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(7), i32_1));
        mir1 = std::move(mb).finish();
    }

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}, {10, "f"}}), ext0},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    Mir const& mm = merged->mir;
    EXPECT_EQ(mm.moduleFuncCount(), 2u) << "both main and f survive";

    auto const mainF = findFuncByName(mm, merged->symbolNames, "main");
    auto const fF    = findFuncByName(mm, merged->symbolNames, "f");
    ASSERT_TRUE(mainF.has_value());
    ASSERT_TRUE(fF.has_value());

    // POSITIVE symbol-identity: main's Call's callee GlobalAddr now names the
    // MERGED f's symbol — a DIRECT call. RED-on-swap: a wrong remap (keeping the
    // extern's fresh id) makes this symbol != f's funcSymbol.
    auto const callId = firstCall(mm, *mainF);
    ASSERT_TRUE(callId.has_value());
    MirInstId const callee = mm.instOperands(*callId)[0];
    ASSERT_EQ(mm.instOpcode(callee), MirOpcode::GlobalAddr);
    EXPECT_EQ(mm.globalAddrSymbol(callee).v, mm.funcSymbol(*fF).v)
        << "the cross-CU call must resolve to the MERGED f's symbol (direct)";

    // The cross-CU-resolved extern is STRIPPED (its call was rewired to direct).
    EXPECT_TRUE(merged->externImports.empty())
        << "the resolved extern f must not survive as an FFI import";

    // userEntrySymbol == main's merged symbol.
    ASSERT_TRUE(merged->userEntrySymbol.has_value());
    EXPECT_EQ(merged->userEntrySymbol->v, mm.funcSymbol(*mainF).v);

    MirVerifier verifier{mm, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "merged module must verify";
}

// ── Two CUs importing the SAME real-FFI symbol dedup to ONE import ──
// CU0: int main()   { return puts(); }   (puts is an EXTERN, mangledName "puts")
// CU1: int helper() { return puts(); }   (the SAME extern "puts", no definition)
// Neither CU DEFINES puts, so it stays a real FFI import. The merge must collapse
// the two same-named extern rows to ONE canonical merged symbol: the merged
// module carries exactly ONE "puts" ExternImport (one IAT slot) AND both calls'
// GlobalAddr operands resolve to that SAME merged symbol. RED before the fix: two
// distinct merged ids → two "puts" import rows + the two calls disagree.
TEST(MirMerge, MergeDedupsSameNamedFfiImports) {
    auto buildCallsPuts = [](CompilationUnitId cu, SymbolId fnSym,
                             SymbolId extSym, TypeInterner& in) -> Mir {
        (void)cu;
        TypeId const i32 = in.primitive(TypeKind::I32);
        TypeId const sig = in.fnSig({}, i32, CallConv::CcSysV);
        MirBuilder mb;
        mb.addFunction(sig, fnSym);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const pAddr = mb.addGlobalAddr(extSym, sig);  // extern puts
        MirInstId const callOps[] = {pAddr};
        MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
        mb.addReturn(call);
        return std::move(mb).finish();
    };

    // CU0: main (sym 100) calls extern puts (sym 10).
    TypeInterner in0{CompilationUnitId{1}};
    Mir mir0 = buildCallsPuts(CompilationUnitId{1}, SymbolId{100}, SymbolId{10}, in0);
    std::vector<ExternImport> ext0 = {ExternImport{SymbolId{10}, "puts", "libc.so"}};

    // CU1: helper (sym 50) calls extern puts (sym 20 — a DIFFERENT local id for the
    // same on-binary name, exactly as two independent CUs would assign).
    TypeInterner in1{CompilationUnitId{2}};
    Mir mir1 = buildCallsPuts(CompilationUnitId{2}, SymbolId{50}, SymbolId{20}, in1);
    std::vector<ExternImport> ext1 = {ExternImport{SymbolId{20}, "puts", "libc.so"}};

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}, {10, "puts"}}), ext0},
        MergeCuInput{&mir1, &in1, namerOf({{50, "helper"}, {20, "puts"}}), ext1},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    Mir const& mm = merged->mir;
    EXPECT_EQ(mm.moduleFuncCount(), 2u) << "both main and helper survive";

    // Exactly ONE surviving "puts" import row (one IAT slot).
    std::size_t putsRows = 0;
    SymbolId putsImportSym{};
    for (ExternImport const& e : merged->externImports) {
        if (e.mangledName == "puts") { ++putsRows; putsImportSym = e.symbol; }
    }
    EXPECT_EQ(putsRows, 1u) << "same-named FFI imports must collapse to ONE row";

    // Both CUs' calls resolve to the SAME merged symbol == the surviving import's.
    auto const mainF   = findFuncByName(mm, merged->symbolNames, "main");
    auto const helperF = findFuncByName(mm, merged->symbolNames, "helper");
    ASSERT_TRUE(mainF.has_value());
    ASSERT_TRUE(helperF.has_value());
    auto const mainCall   = firstCall(mm, *mainF);
    auto const helperCall = firstCall(mm, *helperF);
    ASSERT_TRUE(mainCall.has_value());
    ASSERT_TRUE(helperCall.has_value());
    MirInstId const mainCallee   = mm.instOperands(*mainCall)[0];
    MirInstId const helperCallee = mm.instOperands(*helperCall)[0];
    ASSERT_EQ(mm.instOpcode(mainCallee), MirOpcode::GlobalAddr);
    ASSERT_EQ(mm.instOpcode(helperCallee), MirOpcode::GlobalAddr);
    SymbolId const mainSym   = mm.globalAddrSymbol(mainCallee);
    SymbolId const helperSym = mm.globalAddrSymbol(helperCallee);
    EXPECT_EQ(mainSym.v, helperSym.v)
        << "both CUs' puts calls must resolve to ONE merged symbol";
    EXPECT_EQ(mainSym.v, putsImportSym.v)
        << "the shared call symbol must be the surviving import's symbol";

    MirVerifier verifier{mm, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "merged module must verify";
}

// ── A cross-CU call into a MULTI-BLOCK callee: clone + rewire ──────
// CU0: int main() { return f(); } where `f` is an EXTERN (import row "f").
// CU1's f is a diamond: entry CondBr → then(return 7) / else(return 9). This pins
// BOTH halves of the "cross-CU call into a multi-block callee" form:
//   (a) f's 3-block CFG is cloned 1:1 (block count + CondBr + both Returns) +
//       verifies, AND
//   (b) main's cross-CU Call REWIRES — its GlobalAddr operand resolves to the
//       MERGED multi-block f's funcSymbol (RED-on-swap if it kept the extern's
//       fresh id) + the extern is STRIPPED.
TEST(MirMerge, MergeClonesMultiBlockCallee) {
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32_0 = in0.primitive(TypeKind::I32);
    TypeId const sig0  = in0.fnSig({}, i32_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{100});  // main
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const fAddr = mb.addGlobalAddr(SymbolId{10}, sig0);
        MirInstId const callOps[] = {fAddr};
        MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32_0);
        mb.addReturn(call);
        mir0 = std::move(mb).finish();
    }
    std::vector<ExternImport> ext0 = {ExternImport{SymbolId{10}, "f", "libc.so"}};

    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i32_1  = in1.primitive(TypeKind::I32);
    TypeId const boolT1 = in1.primitive(TypeKind::Bool);
    TypeId const sig1   = in1.fnSig({}, i32_1, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        mb.addFunction(sig1, SymbolId{50});  // f
        // Derivation-consistent stamps (both arms return → IfThen/IfElse
        // around the virtual exit; ExitBlock is a dormant marker the
        // merged module's equality verifier would correct anyway —
        // mergeCuMirs re-derives post-clone).
        MirBlockId const e    = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const then = mb.createBlock(StructCfMarker::IfThen);
        MirBlockId const els  = mb.createBlock(StructCfMarker::IfElse);
        mb.beginBlock(e);
        MirLiteralValue tru; tru.value = std::int64_t{1}; tru.core = TypeKind::Bool;
        MirInstId const cond = mb.addConst(tru, boolT1);
        mb.addCondBr(cond, then, els);
        mb.beginBlock(then);
        mb.addReturn(mb.addConst(i32Lit(7), i32_1));
        mb.beginBlock(els);
        mb.addReturn(mb.addConst(i32Lit(9), i32_1));
        mir1 = std::move(mb).finish();
    }

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}, {10, "f"}}), ext0},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    auto const fF = findFuncByName(mm, merged->symbolNames, "f");
    ASSERT_TRUE(fF.has_value());
    EXPECT_EQ(mm.funcBlockCount(*fF), 3u) << "the diamond's 3 blocks cloned 1:1";
    // Entry's terminator is a CondBr; the two arms each Return.
    MirBlockId const entry = mm.funcEntry(*fF);
    EXPECT_EQ(mm.instOpcode(mm.blockTerminator(entry)), MirOpcode::CondBr);
    EXPECT_EQ(countOp(mm, MirOpcode::CondBr), 1u);
    // f's two Returns are present (main's own Return is the third in the module).
    EXPECT_EQ(countOp(mm, MirOpcode::Return), 3u);

    // (b) main's cross-CU call REWIRED into the MULTI-block f. Its callee
    // GlobalAddr now names the merged multi-block f's symbol — a DIRECT call.
    // RED-on-swap: keeping the extern's fresh id makes this != f's funcSymbol.
    auto const mainF = findFuncByName(mm, merged->symbolNames, "main");
    ASSERT_TRUE(mainF.has_value());
    auto const callId = firstCall(mm, *mainF);
    ASSERT_TRUE(callId.has_value());
    MirInstId const callee = mm.instOperands(*callId)[0];
    ASSERT_EQ(mm.instOpcode(callee), MirOpcode::GlobalAddr);
    EXPECT_EQ(mm.globalAddrSymbol(callee).v, mm.funcSymbol(*fF).v)
        << "the cross-CU call must resolve to the MERGED multi-block f (direct)";
    // The resolved extern is STRIPPED (its call became a direct intra-module call).
    EXPECT_TRUE(merged->externImports.empty())
        << "the resolved extern f must not survive as an FFI import";

    MirVerifier verifier{mm, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "merged multi-block module must verify";
}

// ── Merge RE-DERIVES markers: a stale input stamp is corrected ─────
// CU1's f is two blocks: entry --Br--> tail(Return 7), with the TAIL hand-
// stamped `StructCfMarker::ExitBlock` — a DORMANT marker NO derivation rule
// ever assigns (mir_struct_markers.hpp spec: an unclaimed straight-line block
// derives `Linear`). The input stamp is STALE by construction. The merge clone
// copies markers VERBATIM (clone phase 1), so the post-clone
// `rederiveStructCfMarkers(merged)` call in mergeCuMirs is the ONLY thing
// standing between the stale stamp and the merge's internal stored==derived
// equality verifier.
//
// RED-on-disable lever: remove the `rederiveStructCfMarkers(merged)` call in
// mir_merge.cpp → the stale ExitBlock survives the clone → the merge's
// internal MirVerifier emits I_StructCfMismatch ("stored marker ExitBlock !=
// derived marker Linear") → mergeCuMirs returns nullopt → this test goes RED
// at `merged.has_value()`.
TEST(MirMerge, MergeRederivesStaleInputMarkers) {
    // CU0: int main() { return 0; }
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32_0 = in0.primitive(TypeKind::I32);
    TypeId const sig0  = in0.fnSig({}, i32_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{100});  // main
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(0), i32_0));
        mir0 = std::move(mb).finish();
    }

    // CU1: int f() — entry --Br--> tail(Return 7); the tail carries the STALE
    // ExitBlock stamp the merge's rederive must correct to Linear.
    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i32_1 = in1.primitive(TypeKind::I32);
    TypeId const sig1  = in1.fnSig({}, i32_1, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        mb.addFunction(sig1, SymbolId{50});  // f
        MirBlockId const e    = mb.createBlock(StructCfMarker::EntryBlock);
        MirBlockId const tail = mb.createBlock(StructCfMarker::ExitBlock);  // STALE
        mb.beginBlock(e);
        mb.addBr(tail);
        mb.beginBlock(tail);
        mb.addReturn(mb.addConst(i32Lit(7), i32_1));
        mir1 = std::move(mb).finish();
    }
    // Precondition pin: the INPUT really is stale (stored ExitBlock where the
    // derivation says Linear) — guards the fixture against silently becoming
    // derivation-consistent, which would re-open the no-lever gap.
    EXPECT_EQ(mir1.blockMarker(mir1.funcBlockAt(mir1.funcAt(0), 1)),
              StructCfMarker::ExitBlock)
        << "fixture precondition: the input tail must carry the stale stamp";

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}}), {}},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    // (a) the merge SUCCEEDS: its internal stored==derived verifier passes
    // BECAUSE the post-clone rederive corrected the stale stamp first.
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    Mir const& mm = merged->mir;
    auto const fF = findFuncByName(mm, merged->symbolNames, "f");
    ASSERT_TRUE(fF.has_value());
    ASSERT_EQ(mm.funcBlockCount(*fF), 2u) << "f's 2 blocks cloned 1:1";

    // (b) the merged tail carries the DERIVED marker. Clone phase 1 preserves
    // block order 1:1, so index 1 is the tail — pinned by its Return.
    MirBlockId const tail = mm.funcBlockAt(*fF, 1);
    ASSERT_EQ(mm.instOpcode(mm.blockTerminator(tail)), MirOpcode::Return);
    EXPECT_EQ(mm.blockMarker(tail), StructCfMarker::Linear)
        << "the stale ExitBlock stamp must be re-derived to Linear";
    EXPECT_EQ(mm.blockMarker(mm.funcEntry(*fF)), StructCfMarker::EntryBlock);

    // (c) the merged module verifies clean — zero I_StructCfMismatch.
    MirVerifier verifier{mm, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "merged module must verify";
    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::I_StructCfMismatch), 0u);
}

// ── Types are re-interned into the host lattice ────────────────────
// CU1's f uses a pointer type built in CU1's interner. The merged f's inst types
// must be HOST-stamped (host owner arenaTag) + structurally a pointer.
TEST(MirMerge, MergeReinternsTypesIntoHost) {
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32_0 = in0.primitive(TypeKind::I32);
    TypeId const sig0  = in0.fnSig({}, i32_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{100});  // main
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(0), i32_0));
        mir0 = std::move(mb).finish();
    }

    // CU1: long* f() { return (long*)0; }  — the return type is Ptr<I64>, built
    // ENTIRELY in CU1's interner (a distinct arena from the host).
    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i64_1   = in1.primitive(TypeKind::I64);
    TypeId const ptrI64  = in1.pointer(i64_1);
    TypeId const sig1    = in1.fnSig({}, ptrI64, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        mb.addFunction(sig1, SymbolId{50});  // f
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirLiteralValue nullp; nullp.value = std::uint64_t{0}; nullp.core = TypeKind::Ptr;
        MirInstId const z = mb.addConst(nullp, ptrI64);
        mb.addReturn(z);
        mir1 = std::move(mb).finish();
    }

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}}), {}},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{77}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    auto const fF = findFuncByName(mm, merged->symbolNames, "f");
    ASSERT_TRUE(fF.has_value());

    // f's Const(null) instruction's type must be HOST-stamped (owner 77), not
    // CU1's (owner 2), and structurally Ptr<I64>.
    MirBlockId const e = mm.funcEntry(*fF);
    MirInstId const c0 = mm.blockInstAt(e, 0);
    ASSERT_EQ(mm.instOpcode(c0), MirOpcode::Const);
    TypeId const ty = mm.instType(c0);
    EXPECT_EQ(ty.arenaTag, 77u) << "the inst type must be HOST-interned (CU 77)";
    ASSERT_EQ(merged->host.interner().kind(ty), TypeKind::Ptr);
    EXPECT_EQ(merged->host.interner().kind(merged->host.interner().operands(ty)[0]),
              TypeKind::I64);

    MirVerifier verifier{mm, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── A shadowed-weak definition is DROPPED ──────────────────────────
// CU0 weak f → returns 7; CU1 strong f → returns 42. The strong shadows the
// weak: ONLY the strong f survives (merged func count == 1; its body returns 42,
// NOT 7), and the weak's name maps to the strong's merged symbol. This is the
// structural precondition for the c26 cross-CU Weak runtime pin.
TEST(MirMerge, MergeDropsShadowedWeak) {
    auto buildF = [](CompilationUnitId cu, SymbolBinding binding,
                     std::int64_t ret, TypeInterner& in) -> Mir {
        TypeId const i32  = in.primitive(TypeKind::I32);
        TypeId const sig  = in.fnSig({}, i32, CallConv::CcSysV);
        MirBuilder mb;
        mb.addFunction(sig, SymbolId{50}, binding, SymbolVisibility::Default);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(ret), i32));
        (void)cu;
        return std::move(mb).finish();
    };

    TypeInterner in0{CompilationUnitId{1}};
    Mir mir0 = buildF(CompilationUnitId{1}, SymbolBinding::Weak, 7, in0);
    TypeInterner in1{CompilationUnitId{2}};
    Mir mir1 = buildF(CompilationUnitId{2}, SymbolBinding::Global, 42, in1);

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{50, "f"}}), {}},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u) << "weak-vs-strong is NOT a conflict";

    Mir const& mm = merged->mir;
    ASSERT_EQ(mm.moduleFuncCount(), 1u) << "only the STRONG f survives";
    MirFuncId const f = mm.funcAt(0);
    EXPECT_EQ(mm.funcBinding(f), SymbolBinding::Global);
    // The surviving body returns 42 (the STRONG one), NOT 7 (the weak loser).
    MirInstId const ret = mm.blockTerminator(mm.funcEntry(f));
    ASSERT_EQ(mm.instOpcode(ret), MirOpcode::Return);
    MirInstId const retVal = mm.instOperands(ret)[0];
    ASSERT_EQ(mm.instOpcode(retVal), MirOpcode::Const);
    EXPECT_EQ(std::get<std::int64_t>(mm.literalValue(mm.constLiteralIndex(retVal)).value),
              std::int64_t{42})
        << "the STRONG body (return 42) must survive, not the weak (return 7)";

    // The weak's name "f" maps to the STRONG f's merged symbol (one canonical id).
    auto const it = merged->symbolNames.find(mm.funcSymbol(f).v);
    ASSERT_NE(it, merged->symbolNames.end());
    EXPECT_EQ(it->second, "f");

    MirVerifier verifier{mm, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── A global's initFunc is remapped into the merged func space ─────
// A single CU carries a global `g` whose initializer is a module-init function
// `__init__`. After merge the global's initFunc must point at the MERGED
// __init__ (not a stale/cross-module id). Exercises the runtime-init-globals
// form + the N==1 general path.
TEST(MirMerge, MergeRemapsGlobalInitFunc) {
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32 = in0.primitive(TypeKind::I32);
    TypeId const sig = in0.fnSig({}, i32, CallConv::CcSysV);
    Mir mir0;
    MirFuncId srcInitFunc{};
    {
        MirBuilder mb;
        // __init__ (symbol 200): the module-init function.
        srcInitFunc = mb.addFunction(sig, SymbolId{200});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(0), i32));
        // global g (symbol 300): initialized by __init__ at load.
        (void)mb.addGlobal(i32, SymbolId{300}, /*initLiteralIndex=*/UINT32_MAX,
                           srcInitFunc, SymbolBinding::Global,
                           SymbolVisibility::Default);
        mir0 = std::move(mb).finish();
    }

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{200, "__init__"}, {300, "g"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{55}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    ASSERT_EQ(mm.moduleGlobalCount(), 1u);
    MirGlobalId const g = mm.globalAt(0);
    MirFuncId const mergedInit = mm.globalInitFunc(g);
    ASSERT_TRUE(mergedInit.valid()) << "the global must keep its initFunc";

    // The remapped initFunc must be the MERGED __init__ function — same id the
    // module exposes for the function whose name is "__init__". RED-on-swap: a
    // stale (unremapped) id would not match the merged __init__ slot.
    auto const initF = findFuncByName(mm, merged->symbolNames, "__init__");
    ASSERT_TRUE(initF.has_value());
    EXPECT_EQ(mergedInit.v, initF->v)
        << "the global's initFunc must point at the MERGED init function";

    MirVerifier verifier{mm, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── Two strong definitions report a conflict ───────────────────────
// CU0 strong f + CU1 strong f → exactly one K_SymbolRedefinedAcrossUnits.
TEST(MirMerge, MergeReportsTwoStrongConflict) {
    auto buildF = [](std::int64_t ret, TypeInterner& in) -> Mir {
        TypeId const i32 = in.primitive(TypeKind::I32);
        TypeId const sig = in.fnSig({}, i32, CallConv::CcSysV);
        MirBuilder mb;
        mb.addFunction(sig, SymbolId{50}, SymbolBinding::Global,
                       SymbolVisibility::Default);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(ret), i32));
        return std::move(mb).finish();
    };

    TypeInterner in0{CompilationUnitId{1}};
    Mir mir0 = buildF(7, in0);
    TypeInterner in1{CompilationUnitId{2}};
    Mir mir1 = buildF(42, in1);

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{50, "f"}}), {}},
        MergeCuInput{&mir1, &in1, namerOf({{50, "f"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);

    EXPECT_EQ(test_support::countCode(rep, DiagnosticCode::K_SymbolRedefinedAcrossUnits),
              1u)
        << "exactly one two-strong conflict must be reported";
}
