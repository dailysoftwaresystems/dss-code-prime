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
#include "core/types/target_schema.hpp"        // ProcessArgs / ArgsMechanism (c111)
#include "mir/merge/mir_merge.hpp"
#include "mir/merge/synth_pe_startup.hpp"       // synthesizePeStartup (c111)
#include "mir/merge/synth_seh_funclets.hpp"     // synthesizeSehFunclets (c116)
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"            // rederiveStructCfMarkers (c116b test)
#include "mir/mir_verifier.hpp"

#include "diagnostic_count.hpp"

#include <gtest/gtest.h>

#include <array>
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

// Find the function whose declared SymbolId == `sym` (c111 synth-entry resolution).
[[nodiscard]] std::optional<MirFuncId>
findFuncBySymbol(Mir const& mir, SymbolId sym) {
    std::size_t const nf = mir.moduleFuncCount();
    for (std::uint32_t i = 0; i < nf; ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == sym.v) return f;
    }
    return std::nullopt;
}

// c111: the Alloca count + the callee SymbolIds of every Call in `fn` (each Call's
// operand[0] is its callee GlobalAddr). Lets a pin assert the synth function's BODY
// actually fetches args + forwards to the entry — not merely that the extern row was
// added (a body that registered the import but built a wrong/empty body would still
// verify + still carry the extern; this walks the instructions to catch that).
struct SynthBodyShape {
    std::vector<std::uint32_t> callTargets;   // callee symbol .v, per Call
    std::size_t                allocaCount = 0;
    [[nodiscard]] bool calls(std::uint32_t symV) const {
        for (auto v : callTargets) if (v == symV) return true;
        return false;
    }
};
[[nodiscard]] SynthBodyShape scanBody(Mir const& mir, MirFuncId fn) {
    SynthBodyShape s;
    std::uint32_t const nb = mir.funcBlockCount(fn);
    for (std::uint32_t bi = 0; bi < nb; ++bi) {
        MirBlockId const blk = mir.funcBlockAt(fn, bi);
        std::uint32_t const ni = mir.blockInstCount(blk);
        for (std::uint32_t ii = 0; ii < ni; ++ii) {
            MirInstId const id = mir.blockInstAt(blk, ii);
            MirOpcode const op = mir.instOpcode(id);
            if (op == MirOpcode::Alloca) ++s.allocaCount;
            if (op == MirOpcode::Call) {
                MirInstId const callee = mir.instOperands(id)[0];
                if (mir.instOpcode(callee) == MirOpcode::GlobalAddr) {
                    s.callTargets.push_back(mir.globalAddrSymbol(callee).v);
                }
            }
        }
    }
    return s;
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

// c111 (D-RUNTIME-PE-MAIN-ARGS) helpers. A one-function Mir whose entry has the
// given signature, bound to SymbolId{100}, body `return 0;` (the synth reads only
// the signature, then appends — the body is irrelevant to arg-fetch synthesis).
Mir buildEntryOnly(TypeInterner& in, TypeId sig) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder mb;
    mb.addFunction(sig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

// The Windows CRT out-parameter mechanism, wired with the real msvcrt export names.
ProcessArgs crtOutParamPa() {
    ProcessArgs pa;
    pa.mechanism       = ArgsMechanism::CrtOutParam;
    pa.crtWideArgvFn   = "__wgetmainargs";
    pa.crtNarrowArgvFn = "__getmainargs";
    pa.crtLibraryPath  = "msvcrt.dll";
    return pa;
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
                           SymbolVisibility::Default, /*isConst=*/false,
                           MirThreadStorage::Shared);
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

// F5 (D-CSUBSET-SYMBOL-ADDRESS-GLOBAL): the SymbolId EMBEDDED in a
// `MirSymbolAddrValue` global initializer must be remapped into the merged id
// space (mir_merge.cpp `remapLiteralSymbols`). A symbol-address global defined in
// a NON-FIRST CU has its target RENUMBERED by the merge; a verbatim literal copy
// would carry the STALE CU-local id, so the assembler's abs64 reloc would target
// the wrong (or undefined) symbol → linker `K_SymbolUndefined` / silent wrong VA
// in any multi-`.c` build. This is invisible to the single-CU `decl_string_global`
// corpus. RED-ON-DISABLE: drop the `remapLiteralSymbols(lit, plan, ci)` call →
// `p`'s init keeps CU1's local target id (300), != the merged target id.
TEST(MirMerge, MergeRemapsSymbolAddressGlobalTarget) {
    // CU0: just `main` — occupies the first merged ids so CU1 is renumbered.
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32_0 = in0.primitive(TypeKind::I32);
    TypeId const sig0  = in0.fnSig({}, i32_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{1});  // main
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(0), i32_0));
        mir0 = std::move(mb).finish();
    }

    // CU1: `int target = 42;` + `int *p = &target;` (a symbol-address global whose
    // init literal embeds target's CU1-local SymbolId 300).
    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i32_1 = in1.primitive(TypeKind::I32);
    TypeId const p32_1 = in1.pointer(i32_1);
    Mir mir1;
    {
        MirBuilder mb;
        (void)mb.addGlobal(i32_1, SymbolId{300}, mb.literalPoolAdd(i32Lit(42)),
                           MirFuncId{}, SymbolBinding::Global,
                           SymbolVisibility::Default, /*isConst=*/false,
                           MirThreadStorage::Shared);
        MirLiteralValue saLit;
        saLit.value = MirSymbolAddrValue{/*symbol=*/300u, /*addend=*/0};
        saLit.core  = TypeKind::Ptr;
        (void)mb.addGlobal(p32_1, SymbolId{400}, mb.literalPoolAdd(saLit),
                           MirFuncId{}, SymbolBinding::Global,
                           SymbolVisibility::Default, /*isConst=*/false,
                           MirThreadStorage::Shared);
        mir1 = std::move(mb).finish();
    }

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{1, "main"}}), {}},
        MergeCuInput{&mir1, &in1, namerOf({{300, "target"}, {400, "p"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    auto globalSymNamed = [&](std::string_view want) -> std::optional<std::uint32_t> {
        for (std::uint32_t i = 0; i < mm.moduleGlobalCount(); ++i) {
            MirGlobalId const g = mm.globalAt(i);
            auto const it = merged->symbolNames.find(mm.globalSymbol(g).v);
            if (it != merged->symbolNames.end() && it->second == want)
                return mm.globalSymbol(g).v;
        }
        return std::nullopt;
    };
    auto const mergedTarget = globalSymNamed("target");
    ASSERT_TRUE(mergedTarget.has_value());
    // Sanity: the merge actually RENUMBERED target away from CU1's local 300, so
    // the equality assertion below genuinely discriminates remapped vs stale.
    ASSERT_NE(*mergedTarget, 300u)
        << "merge must renumber CU1's target; else the pin can't discriminate";

    // `p`'s init literal must be a MirSymbolAddrValue whose `.symbol` is the MERGED
    // target id — NOT CU1's local 300.
    std::optional<std::uint32_t> pInitSym;
    for (std::uint32_t i = 0; i < mm.moduleGlobalCount(); ++i) {
        MirGlobalId const g = mm.globalAt(i);
        auto const it = merged->symbolNames.find(mm.globalSymbol(g).v);
        if (it == merged->symbolNames.end() || it->second != "p") continue;
        std::uint32_t const initIdx = mm.globalInitLiteralIndex(g);
        ASSERT_NE(initIdx, UINT32_MAX);
        auto const* sa = std::get_if<MirSymbolAddrValue>(&mm.literalValue(initIdx).value);
        ASSERT_NE(sa, nullptr) << "p's init must stay a MirSymbolAddrValue";
        pInitSym = sa->symbol;
    }
    ASSERT_TRUE(pInitSym.has_value());
    EXPECT_EQ(*pInitSym, *mergedTarget)
        << "p's symbol-address init must point at the MERGED target id, not the "
           "stale CU-local id (remapLiteralSymbols).";

    MirVerifier verifier{mm, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep));
}

// const-ness preservation across the cross-CU merge global-clone site
// (D-LK4-DATA-PRODUCER-MUTABLE-GLOBAL). `mergeCuMirs` rebuilds every CU's globals
// into the merged module (mir_merge.cpp:625); it MUST carry `MirGlobal.isConst`,
// or a const global silently degrades to a writable `.data` section after a
// cross-CU link (loss of read-only-memory protection). Order-independent counts
// keep this robust to any merge reordering. RED-ON-DISABLE: drop the
// `m.globalIsConst(g)` argument at mir_merge.cpp:625 → both globals come back
// mutable and the `constCount == 1` expectation fails.
TEST(MirMerge, MergePreservesGlobalConstness) {
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32 = in0.primitive(TypeKind::I32);
    Mir mir0;
    {
        MirBuilder mb;
        // gc (sym 300) CONST init 5 → .rodata; gm (sym 301) MUTABLE init 7 → .data
        (void)mb.addGlobal(i32, SymbolId{300}, mb.literalPoolAdd(i32Lit(5)),
                           MirFuncId{}, SymbolBinding::Global,
                           SymbolVisibility::Default, /*isConst=*/true,
                           MirThreadStorage::Shared);
        (void)mb.addGlobal(i32, SymbolId{301}, mb.literalPoolAdd(i32Lit(7)),
                           MirFuncId{}, SymbolBinding::Global,
                           SymbolVisibility::Default, /*isConst=*/false,
                           MirThreadStorage::Shared);
        mir0 = std::move(mb).finish();
    }
    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{300, "gc"}, {301, "gm"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};
    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{55}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    ASSERT_EQ(mm.moduleGlobalCount(), 2u);
    int constCount = 0, mutCount = 0;
    for (std::uint32_t i = 0; i < mm.moduleGlobalCount(); ++i) {
        if (mm.globalIsConst(mm.globalAt(i))) ++constCount; else ++mutCount;
    }
    EXPECT_EQ(constCount, 1)
        << "the CONST global must survive cross-CU merge as const (else it lands "
           "in a writable .data section)";
    EXPECT_EQ(mutCount, 1) << "the mutable global must survive merge as mutable";
}

// TLS C1 (D-CSUBSET-THREAD-LOCAL, ★CRIT-3): thread-storage preservation
// across the cross-CU merge global-clone site — the FIRST of the audit's
// flag-drop clone sites. `MirGlobal.isThreadLocal` drives the emitted data
// section (`.tdata`/`.tbss` vs `.data`); a merge that drops it silently
// demotes a per-thread object to PROCESS-SHARED in every N>1 build (every
// thread reads/writes one copy — the exact miscompile the non-defaulted
// addGlobal parameter exists to prevent). Exact per-global assertions.
// RED-ON-DISABLE: drop the `m.globalIsThreadLocal(g)` argument at the
// mir_merge.cpp addGlobal (pass MirThreadStorage::Shared) → the TLS
// global comes back
// process-shared and the EXPECT_TRUE fails.
TEST(MirMerge, MergePreservesGlobalThreadLocal) {
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32 = in0.primitive(TypeKind::I32);
    Mir mir0;
    {
        MirBuilder mb;
        // gt (sym 300) THREAD-LOCAL init 5; gp (sym 301) plain init 7.
        (void)mb.addGlobal(i32, SymbolId{300}, mb.literalPoolAdd(i32Lit(5)),
                           MirFuncId{}, SymbolBinding::Global,
                           SymbolVisibility::Default, /*isConst=*/false,
                           MirThreadStorage::PerThread);
        (void)mb.addGlobal(i32, SymbolId{301}, mb.literalPoolAdd(i32Lit(7)),
                           MirFuncId{}, SymbolBinding::Global,
                           SymbolVisibility::Default, /*isConst=*/false,
                           MirThreadStorage::Shared);
        mir0 = std::move(mb).finish();
    }
    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{300, "gt"}, {301, "gp"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};
    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{55}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    ASSERT_EQ(mm.moduleGlobalCount(), 2u);
    int tlsCount = 0, plainCount = 0;
    for (std::uint32_t i = 0; i < mm.moduleGlobalCount(); ++i) {
        if (mm.globalIsThreadLocal(mm.globalAt(i))) ++tlsCount; else ++plainCount;
    }
    EXPECT_EQ(tlsCount, 1)
        << "the THREAD-LOCAL global must survive the cross-CU merge "
           "thread-local (else it silently becomes process-shared)";
    EXPECT_EQ(plainCount, 1)
        << "the plain global must survive the merge process-shared";
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

// ── c111 (D-RUNTIME-PE-MAIN-ARGS): synthesizePeStartup structural pins ─────────
// The Windows CRT out-parameter args mechanism synthesizes a pre-main init that
// fetches argc/argv via an msvcrt export and forwards them to the user entry,
// RETARGETING the program entry to the synth fn. These pins assert that shape
// HOST-INDEPENDENTLY — they run on EVERY leg, unlike the Windows-only runtime
// witness in examples/c-subset/main_argc_argv (whose pe64 arm this cycle turns on):
//   * NarrowMain — a main(int,char**) entry appends a synth fn (entry retargeted),
//     adds the NARROW __getmainargs FUNCTION import, and the module verifies;
//   * WideWmain — a wmain(int,wchar_t**) entry (argv element = pe wide-char u16)
//     binds the WIDE __wgetmainargs export instead — arm chosen by the argv ELEMENT
//     width, never a format flag (RED-on-swap if narrow/wide invert);
//   * VoidMain — a main(void) entry needs no arg setup → NO synth;
//   * NonCrtMechanism — a non-CrtOutParam (ELF stack-vector) mechanism → NO synth.

TEST(SynthPeStartup, NarrowMainAppendsGetmainargsAndRetargets) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const charPP = in.pointer(in.pointer(in.primitive(TypeKind::Char)));
    TypeId const sig    = in.fnSig(std::array<TypeId, 2>{i32, charPP}, i32, CallConv::CcMS64);
    Mir mir = buildEntryOnly(in, sig);

    std::optional<SymbolId>   entry = SymbolId{100};
    std::vector<ExternImport> ext;
    DiagnosticReporter        rep;
    ASSERT_TRUE(synthesizePeStartup(mir, in, entry, ext, crtOutParamPa(), rep));
    EXPECT_EQ(rep.errorCount(), 0u);

    // The synth init was appended alongside the original main.
    EXPECT_EQ(mir.moduleFuncCount(), 2u) << "the pre-main init must be appended";
    // The program entry is retargeted AWAY from main(100) to the synth fn.
    ASSERT_TRUE(entry.has_value());
    EXPECT_NE(entry->v, 100u) << "the entry must be retargeted to the synth init";
    // Exactly the NARROW msvcrt arg-fetch export was added, as a FUNCTION import.
    ASSERT_EQ(ext.size(), 1u);
    EXPECT_EQ(ext[0].mangledName, "__getmainargs");
    EXPECT_EQ(ext[0].libraryPath, "msvcrt.dll");
    EXPECT_FALSE(ext[0].isData) << "the CRT arg-fetch is a function, not data";
    // The retargeted entry names a REAL defined function whose BODY fetches args and
    // forwards to the original entry — not merely an extern row + an empty shell.
    auto const synthFn = findFuncBySymbol(mir, *entry);
    ASSERT_TRUE(synthFn.has_value())
        << "the new entry symbol must resolve to the appended synth function";
    auto const body = scanBody(mir, *synthFn);
    EXPECT_EQ(body.allocaCount, 4u)
        << "synth locals: argc + argv + env + startupinfo";
    EXPECT_TRUE(body.calls(ext[0].symbol.v))
        << "the synth body must CALL the CRT arg-fetch export it registered";
    EXPECT_TRUE(body.calls(100u))
        << "the synth body must forward to the ORIGINAL user entry (symbol 100)";
    // The rebuilt module is well-formed.
    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the synthesized module must verify";
}

TEST(SynthPeStartup, WideWmainPicksWgetmainargs) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32     = in.primitive(TypeKind::I32);
    TypeId const wcharPP = in.pointer(in.pointer(in.primitive(TypeKind::U16)));
    TypeId const sig     = in.fnSig(std::array<TypeId, 2>{i32, wcharPP}, i32, CallConv::CcMS64);
    Mir mir = buildEntryOnly(in, sig);

    std::optional<SymbolId>   entry = SymbolId{100};
    std::vector<ExternImport> ext;
    DiagnosticReporter        rep;
    ASSERT_TRUE(synthesizePeStartup(mir, in, entry, ext, crtOutParamPa(), rep));
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(ext.size(), 1u);
    EXPECT_EQ(ext[0].mangledName, "__wgetmainargs")
        << "a wchar_t** argv entry must bind the WIDE arg-fetch export (not narrow)";
    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the synthesized module must verify";
}

TEST(SynthPeStartup, VoidMainNeedsNoSynth) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const sig = in.fnSig({}, i32, CallConv::CcMS64);
    Mir mir = buildEntryOnly(in, sig);

    std::optional<SymbolId>   entry = SymbolId{100};
    std::vector<ExternImport> ext;
    DiagnosticReporter        rep;
    ASSERT_TRUE(synthesizePeStartup(mir, in, entry, ext, crtOutParamPa(), rep));
    EXPECT_EQ(mir.moduleFuncCount(), 1u) << "main(void) has no argc/argv to fetch";
    EXPECT_TRUE(ext.empty())             << "no CRT import when there is no setup";
    ASSERT_TRUE(entry.has_value());
    EXPECT_EQ(entry->v, 100u)            << "the entry is left unchanged";
}

TEST(SynthPeStartup, NonCrtMechanismIsANoOp) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const charPP = in.pointer(in.pointer(in.primitive(TypeKind::Char)));
    TypeId const sig    = in.fnSig(std::array<TypeId, 2>{i32, charPP}, i32, CallConv::CcMS64);
    Mir mir = buildEntryOnly(in, sig);

    std::optional<SymbolId>   entry = SymbolId{100};
    std::vector<ExternImport> ext;
    ProcessArgs               pa;
    pa.mechanism = ArgsMechanism::StackVector;  // the ELF route — NOT the pe CRT one
    DiagnosticReporter        rep;
    ASSERT_TRUE(synthesizePeStartup(mir, in, entry, ext, pa, rep));
    EXPECT_EQ(mir.moduleFuncCount(), 1u) << "a non-CRT mechanism synthesizes nothing";
    EXPECT_TRUE(ext.empty());
    EXPECT_EQ(entry->v, 100u);
}

// ── c116 (D-WIN64-SEH-FUNCLETS): synthesizeSehFunclets structural pins ─────────
// The SEH funclet-synthesis pass EXTRACTS each `__try`'s filter into a synthesized
// ms_x64 funclet, reduces the parent's filter block to a `[Const; SehFilterReturn]`
// stub, and records the scope range. These pins assert that shape HOST-
// INDEPENDENTLY (every leg), complementing the Windows-only AV→42 runtime witness
// (examples/c-subset/seh_catch_av):
//   * ExtractsFilterFuncletAndStubsParent — a single-`__try` parent gains ONE
//     appended funclet fn, the __C_specific_handler personality import, one scope
//     record; the funclet READS arg0 + RETURNS; the parent keeps NO SehException*
//     op (they moved to the funclet) but KEEPS the SehTryBegin/End markers + the
//     SehFilterReturn stub (the H2 fiction edge); the rebuilt module verifies.
//   * NoSehIsANoOp — a module with no `__try` is untouched (no funclet, no import).

// A hand-built SEH parent matching the c115 hir_to_mir CFG:
//   entry:    SehTryBegin(id) → [tryBB, filterBB]
//   tryBB:    <guarded body: a load>; SehTryEnd(id); Br(joinBB)
//   filterBB: code = SehExceptionCode(); SehFilterReturn(code) → handlerBB
//   handlerBB: Br(joinBB)
//   joinBB:   return 0
// `sym` is the parent's SymbolId; the guarded body is a single block (c116a).
Mir buildSehParent(TypeInterner& in, SymbolId sym) {
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const u32   = in.primitive(TypeKind::U32);
    TypeId const pI32  = in.pointer(i32);
    TypeId const sig   = in.fnSig({}, i32, CallConv::CcMS64);
    MirBuilder mb;
    mb.addFunction(sig, sym);
    MirBlockId const entry    = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tryBB    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filterBB = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handlerBB= mb.createBlock(StructCfMarker::Linear);
    MirBlockId const joinBB   = mb.createBlock(StructCfMarker::Linear);
    std::uint32_t const region = 0;

    mb.beginBlock(entry);
    mb.addSehTryBegin(tryBB, filterBB, region);

    mb.beginBlock(tryBB);
    // A guarded load off a stack slot (something that could fault at runtime).
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    (void)mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{slot}, i32);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, region);
    mb.addBr(joinBB);

    mb.beginBlock(filterBB);
    MirInstId const code = mb.addInst(MirOpcode::SehExceptionCode, {}, u32);
    // filter value = (code == 0xC0000005) as i32.
    MirLiteralValue av; av.value = std::int64_t{0xC0000005}; av.core = TypeKind::U32;
    MirInstId const avc = mb.addConst(std::move(av), u32);
    MirInstId const cmp = mb.addInst(MirOpcode::ICmpEq,
                                     std::array<MirInstId, 2>{code, avc}, i32);
    mb.addSehFilterReturn(cmp, handlerBB, region);

    mb.beginBlock(handlerBB);
    mb.addBr(joinBB);

    mb.beginBlock(joinBB);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

TEST(SynthSehFunclets, ExtractsFilterFuncletAndStubsParent) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSehParent(in, SymbolId{100});

    std::vector<ExternImport> ext;
    std::vector<MirSehScope>  scopes;
    DiagnosticReporter        rep;
    ASSERT_TRUE(synthesizeSehFunclets(mir, in, ext, scopes, rep));
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    EXPECT_EQ(rep.errorCount(), 0u);

    // One funclet was appended alongside the parent.
    EXPECT_EQ(mir.moduleFuncCount(), 2u) << "the filter funclet must be appended";
    // Exactly the __C_specific_handler personality import was added (SEH-gated).
    ASSERT_EQ(ext.size(), 1u);
    EXPECT_EQ(ext[0].mangledName, "__C_specific_handler");
    EXPECT_EQ(ext[0].libraryPath, "msvcrt.dll");
    EXPECT_FALSE(ext[0].isData);
    // One scope record, naming the funclet + the personality.
    ASSERT_EQ(scopes.size(), 1u);
    EXPECT_EQ(scopes[0].parentFuncSymbol.v, 100u);
    EXPECT_EQ(scopes[0].personalitySymbol.v, ext[0].symbol.v);

    // The funclet: resolve it by its recorded symbol; it reads arg0 (the exception
    // pointers) and returns — NO SehException* op survives (they became a load).
    auto const funclet = findFuncBySymbol(mir, scopes[0].filterFuncletSymbol);
    ASSERT_TRUE(funclet.has_value());
    bool funcletHasArg = false, funcletReturns = false, funcletSeh = false;
    {
        std::uint32_t const nb = mir.funcBlockCount(*funclet);
        for (std::uint32_t bi = 0; bi < nb; ++bi) {
            MirBlockId const b = mir.funcBlockAt(*funclet, bi);
            std::uint32_t const ni = mir.blockInstCount(b);
            for (std::uint32_t ii = 0; ii < ni; ++ii) {
                MirOpcode const op = mir.instOpcode(mir.blockInstAt(b, ii));
                if (op == MirOpcode::Arg) funcletHasArg = true;
                if (op == MirOpcode::Return) funcletReturns = true;
                if (op == MirOpcode::SehExceptionCode
                    || op == MirOpcode::SehExceptionInfo) funcletSeh = true;
            }
        }
    }
    EXPECT_TRUE(funcletHasArg)  << "the funclet reads arg0 (EXCEPTION_POINTERS*)";
    EXPECT_TRUE(funcletReturns) << "the funclet returns the filter value";
    EXPECT_FALSE(funcletSeh)    << "SehException* was rewritten into a funclet load";

    // The PARENT keeps the region markers + the SehFilterReturn stub (the H2
    // fiction edge) but carries NO SehException* op (they moved to the funclet).
    EXPECT_EQ(countOp(mir, MirOpcode::SehTryBegin), 1u);
    EXPECT_EQ(countOp(mir, MirOpcode::SehTryEnd), 1u);
    EXPECT_EQ(countOp(mir, MirOpcode::SehFilterReturn), 1u);
    EXPECT_EQ(countOp(mir, MirOpcode::SehExceptionCode), 0u)
        << "the parent's filter read moved into the funclet (stub has none)";

    // The rebuilt module is well-formed.
    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the SEH-lowered module must verify";
}

TEST(SynthSehFunclets, NoSehIsANoOp) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const sig = in.fnSig({}, i32, CallConv::CcMS64);
    Mir mir = buildEntryOnly(in, sig);

    std::vector<ExternImport> ext;
    std::vector<MirSehScope>  scopes;
    DiagnosticReporter        rep;
    ASSERT_TRUE(synthesizeSehFunclets(mir, in, ext, scopes, rep));
    EXPECT_EQ(mir.moduleFuncCount(), 1u) << "no __try → no funclet appended";
    EXPECT_TRUE(ext.empty())             << "no __try → no personality import";
    EXPECT_TRUE(scopes.empty())          << "no __try → no scope records";
}

// c116b (D-WIN64-SEH-FUNCLETS): a MULTI-BLOCK guarded body. The try body is a small
// diamond (entry → {then, else} → merge; merge holds SehTryEnd) so the region spans
// FOUR blocks. The pass must (a) accept it (not fail loud) and (b) lay the body out
// contiguously with `endBlock` = the body's LAST laid-out block (the merge block, one
// of the region's blocks — never the join/handler). A hand-built parent whose join is
// deliberately created BEFORE some body blocks would, without the relayout, leave the
// scope range non-contiguous; this pin asserts the region-contiguity invariant.
Mir buildSehParentMultiBlockBody(TypeInterner& in, SymbolId sym) {
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const u32   = in.primitive(TypeKind::U32);
    TypeId const pI32  = in.pointer(i32);
    TypeId const sig   = in.fnSig({}, i32, CallConv::CcMS64);
    MirBuilder mb;
    mb.addFunction(sig, sym);
    MirBlockId const entry    = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tryBB    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const thenBB   = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const elseBB   = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const mergeBB  = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filterBB = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handlerBB= mb.createBlock(StructCfMarker::Linear);
    MirBlockId const joinBB   = mb.createBlock(StructCfMarker::Linear);
    std::uint32_t const region = 0;

    mb.beginBlock(entry);
    mb.addSehTryBegin(tryBB, filterBB, region);

    TypeId const boolTy = in.primitive(TypeKind::Bool);
    mb.beginBlock(tryBB);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    MirInstId const v    = mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{slot}, i32);
    MirInstId const zero = mb.addConst(i32Lit(0), i32);
    MirInstId const cnd  = mb.addInst(MirOpcode::ICmpNe,
                                      std::array<MirInstId, 2>{v, zero}, boolTy);
    mb.addCondBr(cnd, thenBB, elseBB);

    mb.beginBlock(thenBB);
    mb.addBr(mergeBB);
    mb.beginBlock(elseBB);
    mb.addBr(mergeBB);

    mb.beginBlock(mergeBB);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, region);
    mb.addBr(joinBB);

    mb.beginBlock(filterBB);
    MirInstId const code = mb.addInst(MirOpcode::SehExceptionCode, {}, u32);
    MirLiteralValue av; av.value = std::int64_t{0xC0000005}; av.core = TypeKind::U32;
    MirInstId const avc = mb.addConst(std::move(av), u32);
    MirInstId const cmp = mb.addInst(MirOpcode::ICmpEq,
                                     std::array<MirInstId, 2>{code, avc}, i32);
    mb.addSehFilterReturn(cmp, handlerBB, region);

    mb.beginBlock(handlerBB);
    mb.addBr(joinBB);
    mb.beginBlock(joinBB);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

TEST(SynthSehFunclets, MultiBlockGuardedBodyIsContiguousAndBounded) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSehParentMultiBlockBody(in, SymbolId{100});

    std::vector<ExternImport> ext;
    std::vector<MirSehScope>  scopes;
    DiagnosticReporter        rep;
    ASSERT_TRUE(synthesizeSehFunclets(mir, in, ext, scopes, rep));
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(scopes.size(), 1u);

    // The rebuilt parent must lay the guarded body's blocks (tryBB entry, then, else,
    // merge) CONTIGUOUSLY, with `beginBlock` first and `endBlock` the last of the
    // run. Resolve the parent, then confirm: (a) the block index of `beginBlock`
    // through `endBlock` is a contiguous span, and (b) every block in that span is a
    // region block (NOT the handler/join). We identify region blocks structurally:
    // the guarded body's blocks are exactly those reachable from beginBlock without
    // passing the SehTryEnd block's successors or the handler.
    auto const parent = findFuncBySymbol(mir, SymbolId{100});
    ASSERT_TRUE(parent.has_value());
    std::uint32_t const nb = mir.funcBlockCount(*parent);
    // Position (layout index) of begin + end in the rebuilt block list.
    std::optional<std::uint32_t> beginPos, endPos;
    for (std::uint32_t i = 0; i < nb; ++i) {
        MirBlockId const b = mir.funcBlockAt(*parent, i);
        if (b.v == scopes[0].beginBlock.v) beginPos = i;
        if (b.v == scopes[0].endBlock.v)   endPos = i;
    }
    ASSERT_TRUE(beginPos.has_value());
    ASSERT_TRUE(endPos.has_value());
    EXPECT_LE(*beginPos, *endPos) << "the guarded body's begin must precede its end";
    // The [begin,end] layout span must be 4 blocks (tryBB, then, else, merge) — the
    // full region, contiguous. (Any interleaved non-region block would make the span
    // wider than the region, breaking the scope-table [Begin,End) correctness.)
    EXPECT_EQ(*endPos - *beginPos + 1u, 4u)
        << "the multi-block guarded body must be laid out CONTIGUOUSLY (4 blocks) so "
           "the scope-table [Begin,End) covers exactly the region";

    // The endBlock must be the block that holds SehTryEnd (the body's fall-through
    // exit), NOT the join/handler.
    bool endHoldsTryEnd = false;
    {
        MirBlockId const eb = scopes[0].endBlock;
        std::uint32_t const ni = mir.blockInstCount(eb);
        for (std::uint32_t i = 0; i < ni; ++i)
            if (mir.instOpcode(mir.blockInstAt(eb, i)) == MirOpcode::SehTryEnd)
                endHoldsTryEnd = true;
    }
    EXPECT_TRUE(endHoldsTryEnd)
        << "endBlock must be the guarded body's SehTryEnd (fall-through exit) block";

    // The hand-built diamond stamps every block Linear; in the real pipeline the
    // mandatory prune's rederiveStructCfMarkers has already canonicalized the markers
    // (if/merge) before synthesizeSehFunclets runs. Mirror that here so the verifier's
    // stored-vs-derived marker check reflects a real post-prune module.
    rederiveStructCfMarkers(mir);
    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the multi-block SEH-lowered module must verify";
}

// c116b H1 (D-WIN64-SEH-FUNCLETS): a filter that READS A PARENT LOCAL. The parent has
// a local alloca `slot`; the filter compares `Load [slot]` against a constant. The
// funclet-extraction must recover the parent local via a `RecoverParentFrameSlot` op
// (off the establisher arg) — the parent alloca is NOT re-created in the funclet.
Mir buildSehParentFilterReadsLocal(TypeInterner& in, SymbolId sym) {
    TypeId const i32   = in.primitive(TypeKind::I32);
    TypeId const u32   = in.primitive(TypeKind::U32);
    TypeId const pI32  = in.pointer(i32);
    TypeId const sig   = in.fnSig({}, i32, CallConv::CcMS64);
    MirBuilder mb;
    mb.addFunction(sig, sym);
    MirBlockId const entry    = mb.createBlock(StructCfMarker::EntryBlock);
    MirBlockId const tryBB    = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const filterBB = mb.createBlock(StructCfMarker::Linear);
    MirBlockId const handlerBB= mb.createBlock(StructCfMarker::Linear);
    MirBlockId const joinBB   = mb.createBlock(StructCfMarker::Linear);
    std::uint32_t const region = 0;

    // The parent local `marker` — an alloca in the ENTRY block (the c69 convention).
    mb.beginBlock(entry);
    MirInstId const marker = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    mb.addSehTryBegin(tryBB, filterBB, region);

    mb.beginBlock(tryBB);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pI32, 4);
    (void)mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{slot}, i32);
    mb.addInst(MirOpcode::SehTryEnd, {}, InvalidType, region);
    mb.addBr(joinBB);

    // filter: (SehExceptionCode()==0xC0000005) & (Load[marker]==42)  — reads a PARENT
    // local. Bitwise & keeps it single-block.
    mb.beginBlock(filterBB);
    MirInstId const code = mb.addInst(MirOpcode::SehExceptionCode, {}, u32);
    MirLiteralValue av; av.value = std::int64_t{0xC0000005}; av.core = TypeKind::U32;
    MirInstId const avc = mb.addConst(std::move(av), u32);
    MirInstId const c1  = mb.addInst(MirOpcode::ICmpEq,
                                     std::array<MirInstId, 2>{code, avc}, i32);
    MirInstId const mv  = mb.addInst(MirOpcode::Load, std::array<MirInstId, 1>{marker}, i32);
    MirInstId const k42 = mb.addConst(i32Lit(42), i32);
    MirInstId const c2  = mb.addInst(MirOpcode::ICmpEq,
                                     std::array<MirInstId, 2>{mv, k42}, i32);
    MirInstId const both= mb.addInst(MirOpcode::And,
                                     std::array<MirInstId, 2>{c1, c2}, i32);
    mb.addSehFilterReturn(both, handlerBB, region);

    mb.beginBlock(handlerBB);
    mb.addBr(joinBB);
    mb.beginBlock(joinBB);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

TEST(SynthSehFunclets, FilterReadingParentLocalEmitsRecoverParentFrameSlot) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSehParentFilterReadsLocal(in, SymbolId{100});

    std::vector<ExternImport> ext;
    std::vector<MirSehScope>  scopes;
    DiagnosticReporter        rep;
    ASSERT_TRUE(synthesizeSehFunclets(mir, in, ext, scopes, rep));
    for (auto const& d : rep.all()) ADD_FAILURE() << d.actual;
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(scopes.size(), 1u);

    // The funclet recovers the parent local via RecoverParentFrameSlot (off arg1, the
    // establisher). The parent alloca is NOT re-created inside the funclet.
    auto const funclet = findFuncBySymbol(mir, scopes[0].filterFuncletSymbol);
    ASSERT_TRUE(funclet.has_value());
    std::uint32_t recoverCount = 0, funcletAllocas = 0, funcletLoads = 0;
    std::uint32_t const nbf = mir.funcBlockCount(*funclet);
    for (std::uint32_t bi = 0; bi < nbf; ++bi) {
        MirBlockId const b = mir.funcBlockAt(*funclet, bi);
        std::uint32_t const ni = mir.blockInstCount(b);
        for (std::uint32_t ii = 0; ii < ni; ++ii) {
            MirOpcode const op = mir.instOpcode(mir.blockInstAt(b, ii));
            if (op == MirOpcode::RecoverParentFrameSlot) ++recoverCount;
            if (op == MirOpcode::Alloca) ++funcletAllocas;
            if (op == MirOpcode::Load)   ++funcletLoads;
        }
    }
    EXPECT_EQ(recoverCount, 1u)
        << "the filter's parent-local read must recover via RecoverParentFrameSlot";
    EXPECT_EQ(funcletAllocas, 0u)
        << "the parent alloca must NOT be re-created in the funclet (it is recovered)";
    // The funclet still LOADS: the recovered marker value + the exception-code chain
    // (SehExceptionCode → *(u32*)*(void**)arg0 = two loads). So ≥1 Load survives.
    EXPECT_GE(funcletLoads, 1u);

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the H1 SEH-lowered module must verify";
}
