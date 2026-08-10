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
#include "mir/merge/synth_stdio_shim.hpp"       // synthesizeStdioShim (D-FFI-PE-CRT-UCRT-MIGRATION P3)
#include "mir/merge/synth_threads_shim.hpp"      // synthesizeThreadsShim (FC17.9a)
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
#include <string_view>
#include <unordered_map>
#include <utility>
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

// ── D-LINK-EXTERN-IMPORT-REFERENCE-GATE (e): the merge OR-combines the eager bit
// when collapsing duplicate imports of one name. Two CUs both import "puts": one
// EAGER (a `#include`d shipped-descriptor import), one NON-EAGER (a hand-written
// `extern int puts()`). Neither defines puts, so it stays a real FFI import and
// the merge collapses the two rows to ONE. The surviving row MUST be EAGER (the
// eager law wins on collapse) so the linker's reference gate keeps it even when
// unreferenced. ORDER-INDEPENDENT: whichever CU lands first, the eager bit is
// ORed in. RED-ON-DISABLE: replace the OR-combine with a plain first-wins skip →
// the swapped-order arm (non-eager CU first) yields a NON-EAGER surviving row and
// goes red.
TEST(MirMerge, MergeOrCombinesEagerImportBitAcrossCollapse) {
    auto buildCallsPuts = [](SymbolId fnSym, SymbolId extSym,
                             TypeInterner& in) -> Mir {
        TypeId const i32 = in.primitive(TypeKind::I32);
        TypeId const sig = in.fnSig({}, i32, CallConv::CcSysV);
        MirBuilder mb;
        mb.addFunction(sig, fnSym);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const pAddr = mb.addGlobalAddr(extSym, sig);   // extern puts
        MirInstId const callOps[] = {pAddr};
        MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
        mb.addReturn(call);
        return std::move(mb).finish();
    };
    // Merge two CUs importing "puts" — one eager, one not — and return the
    // surviving row's eager bit. `eagerInCu0` selects which CU carries the eager
    // marker, so running both ways proves the OR-combine is order-independent.
    auto survivingPutsEager = [&](bool eagerInCu0) -> bool {
        TypeInterner in0{CompilationUnitId{1}};
        Mir mir0 = buildCallsPuts(SymbolId{100}, SymbolId{10}, in0);
        ExternImport e0{SymbolId{10}, "puts", "libc.so"};
        e0.isEagerImport = eagerInCu0;
        std::vector<ExternImport> ext0 = {e0};

        TypeInterner in1{CompilationUnitId{2}};
        Mir mir1 = buildCallsPuts(SymbolId{50}, SymbolId{20}, in1);
        ExternImport e1{SymbolId{20}, "puts", "libc.so"};
        e1.isEagerImport = !eagerInCu0;
        std::vector<ExternImport> ext1 = {e1};

        std::vector<MergeCuInput> cus = {
            MergeCuInput{&mir0, &in0, namerOf({{100, "main"}, {10, "puts"}}), ext0},
            MergeCuInput{&mir1, &in1, namerOf({{50, "helper"}, {20, "puts"}}), ext1},
        };
        std::vector<std::string> const entries = {"main"};
        DiagnosticReporter rep;
        auto merged =
            mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
        EXPECT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();
        if (!merged.has_value()) return false;
        std::size_t putsRows = 0;
        bool eager = false;
        for (ExternImport const& e : merged->externImports) {
            if (e.mangledName == "puts") { ++putsRows; eager = e.isEagerImport; }
        }
        EXPECT_EQ(putsRows, 1u) << "same-named imports must collapse to ONE row";
        return eager;
    };
    EXPECT_TRUE(survivingPutsEager(/*eagerInCu0=*/true))
        << "eager CU0 + non-eager CU1 → surviving row EAGER";
    EXPECT_TRUE(survivingPutsEager(/*eagerInCu0=*/false))
        << "non-eager CU0 + eager CU1 → surviving row EAGER (ORDER-INDEPENDENT; "
           "RED if the merge uses plain first-wins instead of the OR-combine)";
}

// ── D-LK11-EXTERN-IMPORT-DEDUP at the MIR tier ────────────────────
//
// The MIR merge is the LIVE fold route — `--compile a.c b.c` and every
// `--project` build (both sqlite legs) go through `mergeCuMirs`; the
// assembled-tier `mergeModules` fold is reached only via `--resolve-library`.
// Until this cycle the MIR tier was the WEAKER of the two: it keyed the FFI
// collapse on `mangledName` ALONE (so it folded across `libraryPath` and across
// `version`) and was first-wins on `isData` / `isThreadLocal` / `dataSizeBytes` /
// `dataAlignBytes`. The pins below cover both halves.
namespace {

// One CU: `int <fn>() { return <ext>(); }`, the extern reached via GlobalAddr.
Mir buildCallsExtern(SymbolId fnSym, SymbolId extSym, TypeInterner& in) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const sig = in.fnSig({}, i32, CallConv::CcSysV);
    MirBuilder mb;
    mb.addFunction(sig, fnSym);
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const addr      = mb.addGlobalAddr(extSym, sig);
    MirInstId const callOps[] = {addr};
    MirInstId const call      = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    return std::move(mb).finish();
}

[[nodiscard]] ExternImport libImport(std::string name, std::string lib,
                                     std::string version = {}) {
    ExternImport e;
    e.mangledName = std::move(name);
    e.libraryPath = std::move(lib);
    e.version     = std::move(version);
    return e;
}

// Merge two one-call CUs — CU0's function `main` importing `a`, CU1's `helper`
// importing `b`. NEITHER CU defines the imported name, so both rows survive
// cross-CU resolution and reach the merge's import fold. Each row's SymbolId is
// stamped here (10 / 20 — different per-CU ids for what may be one on-binary
// name, exactly as two independent CUs would assign).
[[nodiscard]] std::optional<MergedMirModule>
mergeTwoImportCus(ExternImport a, ExternImport b, DiagnosticReporter& rep) {
    a.symbol = SymbolId{10};
    b.symbol = SymbolId{20};

    TypeInterner in0{CompilationUnitId{1}};
    Mir mir0 = buildCallsExtern(SymbolId{100}, SymbolId{10}, in0);
    std::vector<ExternImport> const ext0 = {a};

    TypeInterner in1{CompilationUnitId{2}};
    Mir mir1 = buildCallsExtern(SymbolId{50}, SymbolId{20}, in1);
    std::vector<ExternImport> const ext1 = {b};

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}, {10, a.mangledName}}), ext0},
        MergeCuInput{&mir1, &in1, namerOf({{50, "helper"}, {20, b.mangledName}}), ext1},
    };
    std::vector<std::string> const entries = {"main"};
    // Every piece the merge reads is alive for the whole call; the returned
    // module owns its own `Mir` + host lattice, so the locals may die here.
    return mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
}

// The merged symbol that `fnName`'s first Call actually binds to — the thing a
// wrong fold silently changes.
[[nodiscard]] std::optional<std::uint32_t>
calleeSymbolOf(Mir const& mm,
               std::unordered_map<std::uint32_t, std::string> const& names,
               std::string const& fnName) {
    auto const f = findFuncByName(mm, names, fnName);
    if (!f.has_value()) return std::nullopt;
    auto const c = firstCall(mm, *f);
    if (!c.has_value()) return std::nullopt;
    MirInstId const callee = mm.instOperands(*c)[0];
    if (mm.instOpcode(callee) != MirOpcode::GlobalAddr) return std::nullopt;
    return mm.globalAddrSymbol(callee).v;
}

[[nodiscard]] bool diagContains(DiagnosticReporter const& rep, DiagnosticCode code,
                                std::string_view needle) {
    for (auto const& d : rep.all()) {
        if (d.code == code && d.actual.find(needle) != std::string::npos) return true;
    }
    return false;
}

// Every reported diagnostic's prose — the streamed context when a match misses
// (an empty `<<` message on a text assertion tells you nothing).
[[nodiscard]] std::string allDiagText(DiagnosticReporter const& rep) {
    std::string out;
    for (auto const& d : rep.all()) { out += '['; out += d.actual; out += ']'; }
    return out;
}

} // namespace

// ── CONTROL: the fold key is not the bare NAME — `libraryPath` is in it ──
// `foo` from `a.dll` and `foo` from `b.dll` are DIFFERENT dynamic symbols
// (libraryPath is the field the walkers group DT_NEEDED /
// IMAGE_IMPORT_DESCRIPTOR / LC_LOAD_DYLIB by), so they must NOT collapse.
// RED-ON-DISABLE: restore the `ffiCanonicalForName.find(name)` mangledName-only
// key and both CUs get ONE merged id → ONE surviving row (a.dll's) → the b.dll
// row VANISHES and CU1's call silently binds into a.dll. The row-count assertion
// and the per-CU callee assertions each catch that independently.
TEST(MirMerge, ExternImportsSameNameDifferentLibraryDoNotFold) {
    DiagnosticReporter rep;
    auto merged = mergeTwoImportCus(libImport("foo", "a.dll"),
                                    libImport("foo", "b.dll"), rep);
    ASSERT_TRUE(merged.has_value()) << allDiagText(rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "two libraries owning one name is not a CONFLICT, it is two imports: "
        << allDiagText(rep);

    std::unordered_map<std::string, std::uint32_t> symByLib;
    for (ExternImport const& e : merged->externImports) {
        EXPECT_EQ(e.mangledName, "foo");
        symByLib.emplace(e.libraryPath, e.symbol.v);
    }
    ASSERT_EQ(merged->externImports.size(), 2u)
        << "two DIFFERENT libraries owning one name are two distinct imports "
           "(pre-fix: 1 row — b.dll's import was silently dropped)";
    ASSERT_TRUE(symByLib.count("a.dll") == 1 && symByLib.count("b.dll") == 1)
        << "each owning library must keep its own import row";
    EXPECT_NE(symByLib["a.dll"], symByLib["b.dll"])
        << "two dynamic symbols must hold two merged ids";

    // ★ THE MISCOMPILE ITSELF: each CU's call site must bind to ITS OWN
    // library's row. Pre-fix both resolved to the single a.dll id.
    auto const mainCallee =
        calleeSymbolOf(merged->mir, merged->symbolNames, "main");
    auto const helperCallee =
        calleeSymbolOf(merged->mir, merged->symbolNames, "helper");
    ASSERT_TRUE(mainCallee.has_value() && helperCallee.has_value());
    EXPECT_EQ(*mainCallee, symByLib["a.dll"])
        << "CU0 imported foo from a.dll — its call must bind THERE";
    EXPECT_EQ(*helperCallee, symByLib["b.dll"])
        << "CU1 imported foo from b.dll — folding across libraryPath binds this "
           "call into the WRONG DLL";
    EXPECT_NE(*mainCallee, *helperCallee);

    MirVerifier verifier{merged->mir, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "merged module must verify";
}

// ── CONTROL: nor is `version` foldable ────────────────────────────
// `puts@GLIBC_2.2.5` and `puts@GLIBC_2.17` are genuinely different dynamic
// symbols (c156 D-LK-ELF-SYMBOL-VERSIONING — the misbind that bound `realpath`
// to the NULL-buffer-rejecting compat form). Folding them at the MIR tier
// reintroduces exactly the fault c156 fixed at the writer.
// RED-ON-DISABLE: same as above — the mangledName-only key yields ONE row and
// CU1's call binds to the OTHER version's slot.
TEST(MirMerge, ExternImportsSameNameDifferentVersionDoNotFold) {
    DiagnosticReporter rep;
    auto merged = mergeTwoImportCus(libImport("puts", "libc.so.6", "GLIBC_2.2.5"),
                                    libImport("puts", "libc.so.6", "GLIBC_2.17"), rep);
    ASSERT_TRUE(merged.has_value()) << allDiagText(rep);
    EXPECT_EQ(rep.errorCount(), 0u) << allDiagText(rep);

    std::unordered_map<std::string, std::uint32_t> symByVersion;
    for (ExternImport const& e : merged->externImports) {
        EXPECT_EQ(e.mangledName, "puts");
        EXPECT_EQ(e.libraryPath, "libc.so.6");
        symByVersion.emplace(e.version, e.symbol.v);
    }
    ASSERT_EQ(merged->externImports.size(), 2u)
        << "two symbol VERSIONS of one name are two dynamic symbols (pre-fix: 1)";
    ASSERT_TRUE(symByVersion.count("GLIBC_2.2.5") == 1 &&
                symByVersion.count("GLIBC_2.17") == 1);
    EXPECT_NE(symByVersion["GLIBC_2.2.5"], symByVersion["GLIBC_2.17"])
        << "two dynamic symbols must hold two merged ids";

    auto const mainCallee =
        calleeSymbolOf(merged->mir, merged->symbolNames, "main");
    auto const helperCallee =
        calleeSymbolOf(merged->mir, merged->symbolNames, "helper");
    ASSERT_TRUE(mainCallee.has_value() && helperCallee.has_value());
    EXPECT_EQ(*mainCallee, symByVersion["GLIBC_2.2.5"]);
    EXPECT_EQ(*helperCallee, symByVersion["GLIBC_2.17"])
        << "folding across `version` binds this call to the wrong glibc compat "
           "form — the c156 misbind, reintroduced one tier up";
    EXPECT_NE(*mainCallee, *helperCallee);

    MirVerifier verifier{merged->mir, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "merged module must verify";
}

// ── THE KEY'S INJECTIVITY, WITH THE COLLISION THE PROSE ONLY DESCRIBED ──
//
// `mir_merge.cpp::ffiImportKey` (and `linker.cpp`'s `dedupKey`, byte-identical)
// LENGTH-PREFIXES every field precisely because a `mangledName` and a
// `libraryPath` are ARBITRARY BYTES out of a descriptor, so any separator-joined
// encoding is non-injective. Both sites argue that at length in prose — and until
// TF-C119 the argument was pinned by NOTHING: MEASURED, every fixture in tests/
// used `foo` / `puts` / `a.dll` / `b.dll` / `GLIBC_2.x`, not one of which contains
// a `:` or a `|`, so replacing BOTH key builders with the naive
// `mangledName + ":" + libraryPath + ":" + version` left every one of them green.
// A comment that no test can falsify is a comment, not an invariant.
//
// This is the crafted pre-image pair the prose implies. Under the naive encoding
// BOTH rows key to the identical string `a:b:c:` and fold into ONE import; under
// the length-prefixed key they are `3:a:b|1:c|0:` and `1:a|3:b:c|0:` and stay two.
// The failure the fold would cause is the WORST shape in this file — not two
// unrelated rows merely losing a row, but CU1's call site silently rebound to a
// DIFFERENT dynamic symbol in a DIFFERENT library, with no diagnostic anywhere.
//
// ★ THE NAMES ARE NOT DECORATIVE, so do not "tidy" them: a `:` in a mangledName is
// unusual but perfectly legal (a descriptor ships whatever bytes the platform's
// export table holds), and the whole point of the pin is that the key must not
// CARE. The names differ between the two rows here — unlike every sibling test
// above, which varies library or version alone — because that is exactly what a
// collision means: two DIFFERENT triples reaching one key.
// RED-ON-DISABLE: swap `ffiImportKey`'s body for the separator-joined form and
// this reds on the row count (1 vs 2) and on `helper`'s callee.
TEST(MirMerge, LengthPrefixedKeyKeepsAColludingNameLibraryPairApart) {
    DiagnosticReporter rep;
    // "a:b" from "c"  vs  "a" from "b:c" — one naive key, two real imports.
    auto merged = mergeTwoImportCus(libImport("a:b", "c"),
                                    libImport("a", "b:c"), rep);
    ASSERT_TRUE(merged.has_value()) << allDiagText(rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "two unrelated imports are not a CONFLICT: " << allDiagText(rep);

    ASSERT_EQ(merged->externImports.size(), 2u)
        << "★ a separator-joined key maps BOTH triples to `a:b:c:` and folds two "
           "UNRELATED dynamic symbols into one import row — the exact "
           "non-injectivity the length prefix exists to prevent";

    std::unordered_map<std::string, std::uint32_t> symByName;
    for (ExternImport const& e : merged->externImports)
        symByName.emplace(e.mangledName + "@" + e.libraryPath, e.symbol.v);
    ASSERT_TRUE(symByName.count("a:b@c") == 1 && symByName.count("a@b:c") == 1)
        << "both triples must survive VERBATIM — neither name nor library may be "
           "re-spelled by the keying";
    EXPECT_NE(symByName["a:b@c"], symByName["a@b:c"])
        << "two dynamic symbols must hold two merged ids";

    // The miscompile itself: each CU binds to ITS OWN import.
    auto const mainCallee =
        calleeSymbolOf(merged->mir, merged->symbolNames, "main");
    auto const helperCallee =
        calleeSymbolOf(merged->mir, merged->symbolNames, "helper");
    ASSERT_TRUE(mainCallee.has_value() && helperCallee.has_value());
    EXPECT_EQ(*mainCallee, symByName["a:b@c"]);
    EXPECT_EQ(*helperCallee, symByName["a@b:c"])
        << "under a colliding key CU1's call binds into library \"c\" and calls "
           "the symbol \"a:b\" — a different function in a different image";
    EXPECT_NE(*mainCallee, *helperCallee);

    MirVerifier verifier{merged->mir, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "merged module must verify";
}

// ── THE pe CRT DIVERGENCE THIS KEY MAKES VISIBLE — PINNED AS-IS, NOT AS-WISHED ──
//
// extern_import.hpp's `isEagerImport` contract says an eager `#include`d symbol
// and a hand-written non-eager `extern` of the same name fold to one EAGER row.
// That is true only where BOTH PRODUCERS SPELL THE SAME LIBRARY, and on pe they
// do not — MEASURED, from the two config values themselves:
//   * src/dss-config/shippedLibs/stdio.json:5  `library.pe`            = ucrtbase.dll
//   * src/dss-config/sources/c-subset.lang.json:1531
//                                         `externLibraryByFormat.pe`   = msvcrt.dll
// (elf and macho each spell ONE library across both, so the contract holds there
// unchanged — which is why this test is pe-shaped and pe-named.)
//
// So on pe, CU A `#include <stdio.h>` + CU B `extern int puts(const char*);`
// produce TWO rows, TWO IMAGE_IMPORT_DESCRIPTORs, and CU B's call bound into
// msvcrt's copy of `puts` — the split CRT the UCRT migration retired. NOT
// reachable same-TU: the semantic tier suppresses the shipped row when the user
// declares the name (semantic_analyzer.cpp:13324,13354), so one TU yields one row.
//
// ★ WHAT THIS TEST IS FOR, stated plainly so nobody "fixes" it by editing the
// expectation. It does NOT bless the divergence and it does NOT claim the wider
// key caused it: both config values predate this fold, and a name-only key did not
// RECONCILE them — it HID them, folding across libraryPath, which is the very
// misbind D-LK11-EXTERN-IMPORT-DEDUP exists to prevent. Reconciling the two pe
// defaults is D-FFI-PE-CRT-UCRT-MIGRATION's job. Until then this pins the ACTUAL
// behaviour so that changing either config value MOVES A TEST instead of silently
// changing what a pe binary imports. When the reconciliation lands, this test's
// expectation becomes 1 row and its eagerness becomes OR-combined true — and the
// person doing it will be told so by this failure.
TEST(MirMerge, PeUcrtbaseAndMsvcrtRowsOfOneNameStayTwoImports) {
    ExternImport shipped = libImport("puts", "ucrtbase.dll");  // <stdio.h>
    shipped.isEagerImport = true;                              // descriptor ⇒ eager
    ExternImport handDeclared = libImport("puts", "msvcrt.dll");  // source `extern`
    handDeclared.isEagerImport = false;

    DiagnosticReporter rep;
    auto merged = mergeTwoImportCus(shipped, handDeclared, rep);
    ASSERT_TRUE(merged.has_value()) << allDiagText(rep);
    EXPECT_EQ(rep.errorCount(), 0u)
        << "two libraries owning one name is not a conflict — it is two imports: "
        << allDiagText(rep);

    ASSERT_EQ(merged->externImports.size(), 2u)
        << "★ pe's shipped-descriptor library (ucrtbase.dll) and its source-extern "
           "default (msvcrt.dll) DIFFER, so these are two dynamic symbols and the "
           "eagerness OR-combine never runs. If this now reads 1, the two config "
           "values were reconciled — update extern_import.hpp's pe paragraph and "
           "this test together";

    std::unordered_map<std::string, ExternImport const*> byLib;
    for (ExternImport const& e : merged->externImports) {
        EXPECT_EQ(e.mangledName, "puts");
        byLib.emplace(e.libraryPath, &e);
    }
    ASSERT_TRUE(byLib.count("ucrtbase.dll") == 1 && byLib.count("msvcrt.dll") == 1)
        << "both CRT images must keep their own import row";
    EXPECT_TRUE(byLib["ucrtbase.dll"]->isEagerImport)
        << "the descriptor row stays EAGER (D-FFI-DESCRIPTOR-EAGER-IMPORT)";
    EXPECT_FALSE(byLib["msvcrt.dll"]->isEagerImport)
        << "★ and the hand-declared row stays NON-eager: it did NOT fold, so it "
           "never received the descriptor row's eagerness. This is the observable "
           "half of the divergence — the linker's reference gate treats the two "
           "rows by different rules";
    EXPECT_NE(byLib["ucrtbase.dll"]->symbol.v, byLib["msvcrt.dll"]->symbol.v);

    // …and CU B's call really does bind the msvcrt row. That is the miscompile-
    // shaped consequence, and it is what makes this a behaviour pin rather than a
    // row count.
    auto const mainCallee =
        calleeSymbolOf(merged->mir, merged->symbolNames, "main");
    auto const helperCallee =
        calleeSymbolOf(merged->mir, merged->symbolNames, "helper");
    ASSERT_TRUE(mainCallee.has_value() && helperCallee.has_value());
    EXPECT_EQ(*mainCallee, byLib["ucrtbase.dll"]->symbol.v);
    EXPECT_EQ(*helperCallee, byLib["msvcrt.dll"]->symbol.v)
        << "the hand-declaring CU calls MSVCRT's puts — one program, two CRTs";

    MirVerifier verifier{merged->mir, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "merged module must verify";
}

// ── A cross-CU disagreement about ONE dynamic symbol FAILS LOUD ───
// Same (mangledName, libraryPath, version) ⇒ the rows DO fold — and then the
// payload must be FOLDED, never picked. `isData` selects the BINDING MODEL (the
// ELF copy-relocation data slot vs the function-import path) and `isThreadLocal`
// the (unimplemented) initial-exec TLS model, so silently keeping either row
// binds the loser CU's references through the WRONG model — the
// D-LK-EXTERN-DATA-IMPORT silent-miscompile shape. Two DIFFERING non-zero
// `dataSizeBytes`/`dataAlignBytes` size ONE copy-relocation `.bss` slot two ways.
//
// The code is K_ExternImportAttributeConflict (0x801B), NOT
// K_SymbolRedefinedAcrossUnits: nobody DEFINES this symbol — two `extern`
// DECLARATIONS of it contradict each other, and the remediation differs.
//
// RED-ON-DISABLE: the pre-fix merge was first-wins on all four fields and
// reported NOTHING, so every `countCode(...) == 1u` here reads 0.
TEST(MirMerge, ConflictingExternImportAttributesFailLoudAtTheMirTier) {
    {   // isData: CU0 imports a function, CU1 the same name as a data object.
        SCOPED_TRACE("isData");
        ExternImport fnRow   = libImport("shared", "libc.so.6");
        ExternImport dataRow = libImport("shared", "libc.so.6");
        dataRow.isData         = true;
        dataRow.dataSizeBytes  = 8;
        dataRow.dataAlignBytes = 8;
        DiagnosticReporter rep;
        auto merged = mergeTwoImportCus(fnRow, dataRow, rep);
        ASSERT_TRUE(merged.has_value());
        EXPECT_EQ(test_support::countCode(
                      rep, DiagnosticCode::K_ExternImportAttributeConflict), 1u)
            << allDiagText(rep);
        EXPECT_TRUE(diagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                 "conflicting `isData` (data object vs function "
                                 "import) across compilation units (false vs true)"))
            << "the diagnostic must name the FIELD and BOTH values; got: "
            << allDiagText(rep);
        EXPECT_TRUE(diagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                 "D-LK11-EXTERN-IMPORT-DEDUP"))
            << allDiagText(rep);
        EXPECT_TRUE(diagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                 "(library \"libc.so.6\")"))
            << "and identify WHICH import: " << allDiagText(rep);
    }
    {   // isThreadLocal: one CU calls the same data object thread-local.
        SCOPED_TRACE("isThreadLocal");
        ExternImport plain = libImport("tlsvar", "libc.so.6");
        plain.isData         = true;
        plain.dataSizeBytes  = 4;
        plain.dataAlignBytes = 4;
        ExternImport tls = plain;
        tls.isThreadLocal = true;
        DiagnosticReporter rep;
        auto merged = mergeTwoImportCus(plain, tls, rep);
        ASSERT_TRUE(merged.has_value());
        EXPECT_EQ(test_support::countCode(
                      rep, DiagnosticCode::K_ExternImportAttributeConflict), 1u)
            << allDiagText(rep);
        EXPECT_TRUE(diagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                 "conflicting `isThreadLocal` (thread storage "
                                 "duration) across compilation units (false vs true)"))
            << allDiagText(rep);
    }
    {   // Two DIFFERING non-zero sizes for ONE copy-relocation slot.
        SCOPED_TRACE("dataSizeBytes");
        ExternImport small = libImport("gvar", "libc.so.6");
        small.isData         = true;
        small.dataSizeBytes  = 4;
        small.dataAlignBytes = 4;
        ExternImport big = small;
        big.dataSizeBytes = 8;
        DiagnosticReporter rep;
        auto merged = mergeTwoImportCus(small, big, rep);
        ASSERT_TRUE(merged.has_value());
        EXPECT_EQ(test_support::countCode(
                      rep, DiagnosticCode::K_ExternImportAttributeConflict), 1u)
            << allDiagText(rep);
        EXPECT_TRUE(diagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                 "conflicting `dataSizeBytes` (copy-relocation slot "
                                 "size) across compilation units (4 vs 8)"))
            << allDiagText(rep);
    }
    {   // ...and for its ALIGNMENT (a distinct field, distinct prose).
        SCOPED_TRACE("dataAlignBytes");
        ExternImport a = libImport("gvar3", "libc.so.6");
        a.isData         = true;
        a.dataSizeBytes  = 8;
        a.dataAlignBytes = 8;
        ExternImport b = a;
        b.dataAlignBytes = 16;
        DiagnosticReporter rep;
        auto merged = mergeTwoImportCus(a, b, rep);
        ASSERT_TRUE(merged.has_value());
        EXPECT_EQ(test_support::countCode(
                      rep, DiagnosticCode::K_ExternImportAttributeConflict), 1u)
            << allDiagText(rep);
        EXPECT_TRUE(diagContains(rep, DiagnosticCode::K_ExternImportAttributeConflict,
                                 "conflicting `dataAlignBytes` (copy-relocation slot "
                                 "alignment) across compilation units (8 vs 16)"))
            << allDiagText(rep);
    }
}

// ── CONTROL + fold: a ZERO size/align is an INCOMPLETE TYPE, not a conflict ──
// `extern const char v[];` in one TU beside a sized declaration in another is
// legal C (extern_import.hpp:76-81 — both fields stay 0 for an incomplete type),
// so the merge takes the NON-ZERO shape and reports nothing. Both orders are
// checked: the INCOMPLETE-FIRST order is the discriminating one — the pre-fix
// first-wins merge kept CU0's 0/0 and shipped a copy-relocation `.bss` slot of
// size ZERO, which the walker then rejects (or, worse, sizes wrong).
TEST(MirMerge, IncompleteExternDataTypeFoldsToTheSizedShapeNotAConflict) {
    ExternImport sized = libImport("gvar2", "libc.so.6");
    sized.isData         = true;
    sized.dataSizeBytes  = 8;
    sized.dataAlignBytes = 8;
    ExternImport incomplete = libImport("gvar2", "libc.so.6");
    incomplete.isData = true;   // 0 / 0 — size unknown in THIS translation unit

    auto const foldedShape = [](ExternImport const& first, ExternImport const& second,
                                DiagnosticReporter& rep)
        -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
        auto merged = mergeTwoImportCus(first, second, rep);
        if (!merged.has_value()) return std::nullopt;
        EXPECT_EQ(merged->externImports.size(), 1u)
            << "one (name, library, version) is ONE import row";
        if (merged->externImports.size() != 1) return std::nullopt;
        ExternImport const& row = merged->externImports.front();
        EXPECT_TRUE(row.isData) << "the data-object model must survive the fold";
        return std::pair{row.dataSizeBytes, row.dataAlignBytes};
    };

    {   // ★ THE DISCRIMINATING ORDER: incomplete first.
        SCOPED_TRACE("incomplete first");
        DiagnosticReporter rep;
        auto const shape = foldedShape(incomplete, sized, rep);
        ASSERT_TRUE(shape.has_value());
        EXPECT_EQ(test_support::countCode(
                      rep, DiagnosticCode::K_ExternImportAttributeConflict), 0u)
            << "a zero size is an incomplete type, not a disagreement: "
            << allDiagText(rep);
        EXPECT_EQ(shape->first, 8u)
            << "the COMPLETE type sizes the copy-relocation slot (pre-fix "
               "first-wins kept CU0's 0 and shipped an unsized slot)";
        EXPECT_EQ(shape->second, 8u);
    }
    {   // The other order agrees (the fold is order-independent).
        SCOPED_TRACE("sized first");
        DiagnosticReporter rep;
        auto const shape = foldedShape(sized, incomplete, rep);
        ASSERT_TRUE(shape.has_value());
        EXPECT_EQ(test_support::countCode(
                      rep, DiagnosticCode::K_ExternImportAttributeConflict), 0u)
            << allDiagText(rep);
        EXPECT_EQ(shape->first, 8u);
        EXPECT_EQ(shape->second, 8u);
    }
}

// ── plan 00 §0.3: a diagnostic slot is claimed in the header AND the RENDERER ──
// TF-C118's `D-DIAG-OPT-FAMILY-NIBBLE-CLAIMED-IN-HEADER-BUT-NOT-IN-RENDERER`: the
// X_* family had a header slot and no renderer arm, so every optimizer diagnostic
// printed under the PARSER's letter for months. `K_ExternImportAttributeConflict`
// rides the pre-existing 0x8000 → 'K' arm rather than claiming a new nibble, so
// this pin is what proves that arm actually covers the new code (rather than
// assuming it) — in BOTH renderings the user can see: the positioned
// `error[K001B]:` band letter and the buffer-less `error[<name>]` spelling.
TEST(MirMerge, ExternImportAttributeConflictRendersInTheLinkerBand) {
    EXPECT_EQ(diagnosticCodePrefix(DiagnosticCode::K_ExternImportAttributeConflict),
              "K001B")
        << "the 0x8000 nibble must render as the linker band with the nibble "
           "stripped — a missing arm would print it as the PARSER's 'P801B'";
    EXPECT_EQ(diagnosticCodeName(DiagnosticCode::K_ExternImportAttributeConflict),
              "K_ExternImportAttributeConflict")
        << "the buffer-less rendering names the code; an unmirrored enumerator "
           "prints \"Unknown\"";
    // ...and it is a DISTINCT code from the definition-tier one it replaced at
    // the conflict arms (an extern-import ATTRIBUTE clash is not a redefinition).
    EXPECT_NE(static_cast<int>(DiagnosticCode::K_ExternImportAttributeConflict),
              static_cast<int>(DiagnosticCode::K_SymbolRedefinedAcrossUnits));
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

// D-LINK-LOCAL-FN-ADDR-STATIC-DATA-VA0: a file-LOCAL (`static`, internal-linkage /
// Local binding) global or function must NEVER be identified with a same-named
// EXTERNALLY-VISIBLE (Global) symbol in ANOTHER CU — C 6.2.2p3 internal linkage
// makes them DISTINCT entities. The canonical sqlite defect: os_unix.c's
// `static struct unix_syscall aSyscall[]` (24-byte rows → the static `posixOpen`)
// shares the NAME `aSyscall` with test_syscall.c's NON-static `aSyscall[]` (32-byte
// rows → `ts_open`). The merge (a) FOLDED the Local onto the Global winner's id in
// `assignSymbol` AND (b) DROPPED the Local as a "shadowed loser" in
// `isShadowedLoser`, so os_unix's own code read test_syscall's table → a call
// through a NULL fn-ptr at the first FILE-DB open (invisible until BOTH TUs link;
// the 2-TU `sqlite3.c + main` build lacks the extern `aSyscall`, hence "works at 2,
// crashes at 88"). This models it at the merge tier: CU0 has a GLOBAL `T` → &fa;
// CU1 has a same-named STATIC (Local) `T` → &fb. Post-merge BOTH `T`s must survive
// as DISTINCT globals, each keeping its OWN symbol-address init target.
// RED-ON-DISABLE: drop the `!isLocalDef` guard in `assignSymbol` OR the `isLocal`
// guard in `isShadowedLoser` → CU1's Local `T` folds onto / is shadowed by CU0's
// `T`, so only ONE `T` survives (the `size()==2` pin fails) and it points at fa.
TEST(MirMerge, MergeKeepsLocalStaticGlobalDistinctFromSameNamedExtern) {
    // CU0 — the test_syscall analogue: private `int fa(void){return 1;}` and a
    // NON-static (Global, externally visible) `int(*T)(void) = &fa;`.
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32_0 = in0.primitive(TypeKind::I32);
    TypeId const sig0  = in0.fnSig({}, i32_0, CallConv::CcSysV);
    TypeId const pfn0  = in0.pointer(sig0);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(sig0, SymbolId{10}, SymbolBinding::Local);  // fa (private)
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(1), i32_0));
        MirLiteralValue saA;
        saA.value = MirSymbolAddrValue{/*symbol=*/10u, /*addend=*/0};   // &fa
        saA.core  = TypeKind::Ptr;
        (void)mb.addGlobal(pfn0, SymbolId{20}, mb.literalPoolAdd(saA),
                           MirFuncId{}, SymbolBinding::Global,   // EXTERNAL "T"
                           SymbolVisibility::Default, /*isConst=*/false,
                           MirThreadStorage::Shared);
        mir0 = std::move(mb).finish();
    }
    // CU1 — the os_unix analogue: private `int fb(void){return 2;}` and a STATIC
    // (Local) `int(*T)(void) = &fb;` with the SAME name `T` as CU0's extern.
    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i32_1 = in1.primitive(TypeKind::I32);
    TypeId const sig1  = in1.fnSig({}, i32_1, CallConv::CcSysV);
    TypeId const pfn1  = in1.pointer(sig1);
    Mir mir1;
    {
        MirBuilder mb;
        mb.addFunction(sig1, SymbolId{60}, SymbolBinding::Local);  // fb (private)
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(2), i32_1));
        MirLiteralValue saB;
        saB.value = MirSymbolAddrValue{/*symbol=*/60u, /*addend=*/0};   // &fb
        saB.core  = TypeKind::Ptr;
        (void)mb.addGlobal(pfn1, SymbolId{70}, mb.literalPoolAdd(saB),
                           MirFuncId{}, SymbolBinding::Local,   // STATIC "T" (same name)
                           SymbolVisibility::Default, /*isConst=*/false,
                           MirThreadStorage::Shared);
        mir1 = std::move(mb).finish();
    }

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{10, "fa"}, {20, "T"}}), {}},
        MergeCuInput{&mir1, &in1, namerOf({{60, "fb"}, {70, "T"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();
    Mir const& mm = merged->mir;

    // Both private functions survive with DISTINCT merged ids (a Local func/global
    // is module-private and is never folded by name).
    auto const fa = findFuncByName(mm, merged->symbolNames, "fa");
    auto const fb = findFuncByName(mm, merged->symbolNames, "fb");
    ASSERT_TRUE(fa.has_value());
    ASSERT_TRUE(fb.has_value());
    std::uint32_t const faSym = mm.funcSymbol(*fa).v;
    std::uint32_t const fbSym = mm.funcSymbol(*fb).v;
    EXPECT_NE(faSym, fbSym);

    // Collect EVERY merged global named "T" together with its symbol-address init
    // target.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tGlobals;  // (Tsym, initTarget)
    for (std::uint32_t i = 0; i < mm.moduleGlobalCount(); ++i) {
        MirGlobalId const g = mm.globalAt(i);
        std::uint32_t const gsym = mm.globalSymbol(g).v;
        auto const it = merged->symbolNames.find(gsym);
        if (it == merged->symbolNames.end() || it->second != "T") continue;
        std::uint32_t const initIdx = mm.globalInitLiteralIndex(g);
        ASSERT_NE(initIdx, UINT32_MAX);
        auto const* sa =
            std::get_if<MirSymbolAddrValue>(&mm.literalValue(initIdx).value);
        ASSERT_NE(sa, nullptr) << "each T's init must stay a MirSymbolAddrValue";
        tGlobals.emplace_back(gsym, sa->symbol);
    }

    // THE PIN: the file-local `static T` and the extern `T` remain TWO distinct
    // globals with distinct merged ids — the Local was NOT folded onto / shadowed
    // by the Global.
    ASSERT_EQ(tGlobals.size(), 2u)
        << "the file-local `static T` and the extern `T` must remain TWO distinct "
           "globals; folding the Local onto the same-named Global is the miscompile.";
    EXPECT_NE(tGlobals[0].first, tGlobals[1].first);

    // Each `T` keeps its OWN init target: one points at CU0's fa, the OTHER at CU1's
    // fb (never both folded to fa).
    bool const pointsFa =
        tGlobals[0].second == faSym || tGlobals[1].second == faSym;
    bool const pointsFb =
        tGlobals[0].second == fbSym || tGlobals[1].second == fbSym;
    EXPECT_TRUE(pointsFa) << "one T must still point at CU0's fa";
    EXPECT_TRUE(pointsFb)
        << "the OTHER T must still point at CU1's fb (not folded to CU0's fa) — the "
           "os_unix-reads-test_syscall's-table miscompile.";

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
// ★★ TF-C85: the CROSS-CU merge is the THIRD `MirFunc` copy hop (the other two
// are `mir_rebuild_helper` and the inliner's own rebuild). The merged module is
// what the optimizer runs on for an N>1 build, so a `noOptimize` flag dropped
// here lets a function the source put inside a `#pragma optimize("", off)`
// region be optimized after link — invisibly, because the single-CU path stays
// correct and only the multi-CU one changes.
//
// RED-ON-DISABLE: drop the `src_.funcNoOptimize(f)` argument at mir_merge.cpp's
// addFunction (let it default to false) and the first EXPECT_TRUE fails. The
// un-flagged sibling keeps the test from being satisfiable by hardcoding true,
// and the cleared inline flags pin the adjacent-bool swap.
TEST(MirMerge, MergePreservesFunctionNoOptimize) {
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32   = in0.primitive(TypeKind::I32);
    TypeId const fnSig = in0.fnSig({}, i32, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        // sym 400: noOptimize ONLY — the two inline flags deliberately clear, so
        // a transposed argument at the copy site fails here.
        mb.addFunction(fnSig, SymbolId{400}, SymbolBinding::Global,
                       SymbolVisibility::Default, /*noInline=*/false,
                       /*alwaysInline=*/false, /*noOptimize=*/true);
        MirBlockId const b0 = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(b0);
        mb.addReturn(mb.addConst(i32Lit(7), i32));
        // sym 401: plain.
        mb.addFunction(fnSig, SymbolId{401});
        MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(b1);
        mb.addReturn(mb.addConst(i32Lit(8), i32));
        mir0 = std::move(mb).finish();
    }
    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{400, "shielded"}, {401, "plain"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};
    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{55}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    ASSERT_EQ(mm.moduleFuncCount(), 2u);
    int flagged = 0, plain = 0;
    for (std::uint32_t i = 0; i < mm.moduleFuncCount(); ++i) {
        MirFuncId const f = mm.funcAt(i);
        if (mm.funcNoOptimize(f)) ++flagged; else ++plain;
        EXPECT_FALSE(mm.funcNoInline(f))
            << "neither source function is noinline — an adjacent-bool swap at "
               "the merge copy site would land the flag here";
        EXPECT_FALSE(mm.funcAlwaysInline(f));
    }
    EXPECT_EQ(flagged, 1)
        << "the no-optimize function must survive the cross-CU merge still "
           "excluded, or an N>1 build silently optimizes it after link";
    EXPECT_EQ(plain, 1) << "and the plain function must not acquire the flag";
}

// ★★ TF-C92 (D-CSUBSET-NO-SANITIZE-THREAD): the SAME cross-CU copy hop for the
// thread-sanitizer exclusion. The merged module is the artifact an N>1 build's
// `.dssir` describes, so a flag dropped here means a per-function fact recorded in
// CU A silently disappears at the link boundary — and because NO pass reads this
// flag (MEASURED: `grep -rni sanitiz src/` is empty), nothing else in the suite
// would notice. Single-CU builds stay correct, which is exactly what makes the
// multi-CU loss invisible without this pin.
//
// RED-ON-DISABLE: drop the `src_.funcNoSanitizeThread(f)` argument at mir_merge.cpp's
// addFunction (let the 8th parameter default to false) and the `flagged == 1`
// expectation fails. The un-flagged sibling keeps the test from being satisfiable by
// hardcoding true; `noOptimize` is asserted CLEAR on both records because it is the
// IMMEDIATELY PRECEDING argument, so a one-position shift fails here too.
TEST(MirMerge, MergePreservesFunctionNoSanitizeThread) {
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32   = in0.primitive(TypeKind::I32);
    TypeId const fnSig = in0.fnSig({}, i32, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        // sym 500: noSanitizeThread ONLY — all three sibling flags deliberately
        // clear, so a transposed/shifted argument at the copy site fails here.
        mb.addFunction(fnSig, SymbolId{500}, SymbolBinding::Global,
                       SymbolVisibility::Default, /*noInline=*/false,
                       /*alwaysInline=*/false, /*noOptimize=*/false,
                       /*noSanitizeThread=*/true);
        MirBlockId const b0 = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(b0);
        mb.addReturn(mb.addConst(i32Lit(9), i32));
        // sym 501: plain.
        mb.addFunction(fnSig, SymbolId{501});
        MirBlockId const b1 = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(b1);
        mb.addReturn(mb.addConst(i32Lit(10), i32));
        mir0 = std::move(mb).finish();
    }
    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{500, "racy_ok"}, {501, "plain2"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};
    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{56}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    ASSERT_EQ(mm.moduleFuncCount(), 2u);
    int flagged = 0, plain = 0;
    for (std::uint32_t i = 0; i < mm.moduleFuncCount(); ++i) {
        MirFuncId const f = mm.funcAt(i);
        if (mm.funcNoSanitizeThread(f)) ++flagged; else ++plain;
        EXPECT_FALSE(mm.funcNoOptimize(f))
            << "neither source function is nooptimize — a one-position argument "
               "shift at the merge copy site would land the flag here";
        EXPECT_FALSE(mm.funcNoInline(f));
        EXPECT_FALSE(mm.funcAlwaysInline(f));
    }
    EXPECT_EQ(flagged, 1)
        << "the thread-sanitizer exclusion must survive the cross-CU merge, or a "
           "fact recorded in one CU vanishes at link with no other symptom";
    EXPECT_EQ(plain, 1) << "and the plain function must not acquire the flag";
}

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

// ── FC17.9(a) (D-CSUBSET-C11-THREADS-HEADER): synthesizeThreadsShim ──────────────
// A caller references mtx_lock (pre-minted SymbolId{10}, seeded into functionSymbols by
// the CST→HIR seam so the reference lowered to a GlobalAddr against a NOT-yet-defined
// callee). The shim pass must (M4-a) turn that symbol into a DEFINED function, and
// (M4-c) import EnterCriticalSection from kernel32 WITHOUT importing mtx_lock itself (the
// eager-import law — kernel32 exports no mtx_lock). RED-on-disable: drop the seam and the
// def never lands / the import re-appears.
TEST(SynthThreadsShim, SynthesizesDefinitionAndHelperImportNotTheShimName) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const pV  = in.pointer(in.primitive(TypeKind::Void));
    TypeId const mainSig = in.fnSig({}, i32, CallConv::CcMS64);
    std::array<TypeId, 1> const lockParams{pV};
    TypeId const lockSig = in.fnSig(lockParams, i32, CallConv::CcSysV);  // the descriptor sig

    MirBuilder mb;
    mb.addFunction(mainSig, SymbolId{100});   // main
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const slot     = mb.addInst(MirOpcode::Alloca, {}, pV, 40);   // a mtx_t
    MirInstId const lockAddr = mb.addGlobalAddr(SymbolId{10}, in.pointer(lockSig)); // ref mtx_lock
    MirInstId const callOps[] = {lockAddr, slot};
    MirInstId const call = mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(call);
    Mir mir = std::move(mb).finish();

    std::unordered_map<std::uint32_t, std::string> recipes{{10u, "mtx_lock"}};
    std::vector<ExternImport> externs;   // a threads.h-only TU imports no cond-var/CS yet
    DiagnosticReporter rep;
    LibrarySynthesis const win32{LibrarySynthVehicle::Win32, "kernel32.dll"};
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, win32, CSymbolDecorationScheme::None,
                                      externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    // M4(a): SymbolId{10} (mtx_lock) is now a DEFINED module function.
    bool foundLockDef = false;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i)
        if (mir.funcSymbol(mir.funcAt(i)).v == 10u) foundLockDef = true;
    EXPECT_TRUE(foundLockDef) << "mtx_lock must be a synthesized definition (M4-a)";

    // M4(c): the shim NAME is never a kernel32 import; the helper it calls IS.
    bool importedLock = false, importedEnter = false;
    for (auto const& imp : externs) {
        if (imp.mangledName == "mtx_lock") importedLock = true;
        if (imp.mangledName == "EnterCriticalSection") {
            importedEnter = true;
            EXPECT_EQ(imp.libraryPath, "kernel32.dll");
            EXPECT_FALSE(imp.isData);
        }
    }
    EXPECT_FALSE(importedLock) << "mtx_lock must NOT be a kernel32 import (eager-import law, M4-c)";
    EXPECT_TRUE(importedEnter) << "the synthesized mtx_lock body must import EnterCriticalSection";

    // The synthesized module (the CcSysV user call + the CcMS64 shim definition +
    // GlobalAddr to a fresh kernel32 helper) must survive MirVerifier — the mixed-CallConv
    // call/def is verified per-instruction (the verifier tolerates the not-cross-checked CC).
    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the shim-synthesized module must verify";
}

// An EMPTY recipe map (every elf/macho + non-threads TU) is a clean no-op: the module is
// unchanged and no import is planted. Locks the pass to a pure data gate (never a format
// check).
TEST(SynthThreadsShim, EmptyRecipeMapIsNoOp) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder mb;
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();

    std::unordered_map<std::uint32_t, std::string> recipes;   // empty
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    // nullopt vehicle: the empty-map gate MUST short-circuit BEFORE the vehicle check, so
    // an empty map is a clean no-op even with no declared vehicle (elf's steady state).
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, std::nullopt, CSymbolDecorationScheme::None,
                                      externs, rep));
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_EQ(mir.moduleFuncCount(), 1u) << "no shim appended for an empty map";
    EXPECT_TRUE(externs.empty()) << "no import planted for an empty map";
}

// ── FC17.9(a) Cycle 2 (D-CSUBSET-C11-THREADS-TRAMPOLINES) ────────────────────────
// thrd_create is DIRECT-PASS: it hands the caller's start routine STRAIGHT to
// CreateThread — NO malloc closure, NO __dss_thrd_tramp. RED-on-disable: a regression
// to a closure/trampoline would (a) add a 3rd synthesized function and (b) make the
// CreateThread lpStartAddress a GlobalAddr(tramp) instead of the func Arg.
TEST(SynthThreadsShim, ThrdCreateDirectPassesStartRoutineNoTrampoline) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const pV  = in.pointer(in.primitive(TypeKind::Void));
    TypeId const mainSig = in.fnSig({}, i32, CallConv::CcMS64);
    std::array<TypeId, 3> const cp{pV, pV, pV};
    TypeId const createSig = in.fnSig(cp, i32, CallConv::CcMS64);   // (thr, func, arg)->int

    MirBuilder mb;
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pV, 8);
    MirInstId const ga   = mb.addGlobalAddr(SymbolId{10}, in.pointer(createSig));
    MirInstId const co[] = {ga, slot, slot, slot};   // a referenced-only shim call
    mb.addInst(MirOpcode::Call, co, i32);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();

    std::unordered_map<std::uint32_t, std::string> recipes{{10u, "thrd_create"}};
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes,
                                      LibrarySynthesis{LibrarySynthVehicle::Win32, "kernel32.dll"},
                                      CSymbolDecorationScheme::None, externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    // DIRECT-PASS adds ONLY thrd_create (main + thrd_create) — no trampoline function.
    EXPECT_EQ(mir.moduleFuncCount(), 2u)
        << "thrd_create is DIRECT-PASS — no __dss_thrd_tramp closure function";

    // CreateThread imported; malloc NOT (a closure would need it).
    std::optional<std::uint32_t> createSym;
    bool importedMalloc = false;
    for (auto const& imp : externs) {
        if (imp.mangledName == "CreateThread") {
            createSym = imp.symbol.v;
            EXPECT_EQ(imp.libraryPath, "kernel32.dll");
        }
        if (imp.mangledName == "malloc") importedMalloc = true;
    }
    ASSERT_TRUE(createSym.has_value()) << "thrd_create's body must import CreateThread";
    EXPECT_FALSE(importedMalloc) << "DIRECT-PASS thrd_create allocates no closure — no malloc";

    // The func Arg (argIndex 1) + arg Arg (argIndex 2) must be the CreateThread call's
    // lpStartAddress + lpParameter operands — at their EXACT positions (a func/arg swap
    // is red here, not only via the runtime example).
    MirFuncId createFn{};
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i)
        if (mir.funcSymbol(mir.funcAt(i)).v == 10u) createFn = mir.funcAt(i);
    ASSERT_TRUE(createFn.valid());
    MirInstId funcArg{}, argArg{}, createCall{};
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(createFn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(createFn, bi);
        for (std::uint32_t j = 0; j < mir.blockInstCount(b); ++j) {
            MirInstId const id = mir.blockInstAt(b, j);
            if (mir.instOpcode(id) == MirOpcode::Arg) {
                if (mir.argIndex(id) == 1u) funcArg = id;   // thrd_start_t (start routine)
                if (mir.argIndex(id) == 2u) argArg  = id;   // void* (thread param)
            }
            if (mir.instOpcode(id) == MirOpcode::Call) {
                auto ops = mir.instOperands(id);
                if (!ops.empty() && mir.instOpcode(ops[0]) == MirOpcode::GlobalAddr
                    && mir.globalAddrSymbol(ops[0]).v == *createSym)
                    createCall = id;
            }
        }
    }
    ASSERT_TRUE(funcArg.valid());
    ASSERT_TRUE(argArg.valid());
    ASSERT_TRUE(createCall.valid());
    // CreateThread(lpThreadAttributes, dwStackSize, lpStartAddress, lpParameter,
    // dwCreationFlags, lpThreadId) — the Call operands are [callee, a..f], so
    // lpStartAddress is operand[3] and lpParameter operand[4]. The start routine (Arg 1)
    // MUST land at [3] and the thread param (Arg 2) at [4] — a func/arg SWAP fails HERE.
    auto createOps = mir.instOperands(createCall);
    ASSERT_EQ(createOps.size(), 7u) << "CreateThread takes 6 args (callee + 6 operands)";
    EXPECT_EQ(createOps[3].v, funcArg.v)
        << "the start routine (Arg 1) is CreateThread's lpStartAddress (operand 3), DIRECT-passed";
    EXPECT_EQ(createOps[4].v, argArg.v)
        << "the thread param (Arg 2) is CreateThread's lpParameter (operand 4)";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the thrd_create-synthesized module must verify";
}

// call_once synthesizes ONE module-scoped __dss_once_tramp, address-takes it, and the
// adapter invokes the C11 void(*)(void) INDIRECTLY. RED-on-disable: dropping the adapter
// (passing the bare fn as PINIT_ONCE_FN) removes the 3rd function + the indirect call.
TEST(SynthThreadsShim, CallOnceSynthesizesAddressTakenTrampolineWithIndirectCall) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const pV     = in.pointer(voidTy);
    TypeId const mainSig = in.fnSig({}, i32, CallConv::CcMS64);
    std::array<TypeId, 2> const cp{pV, pV};
    TypeId const onceSig = in.fnSig(cp, voidTy, CallConv::CcMS64);   // (flag, fn)->void

    MirBuilder mb;
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pV, 8);
    MirInstId const ga   = mb.addGlobalAddr(SymbolId{10}, in.pointer(onceSig));
    MirInstId const co[] = {ga, slot, slot};
    mb.addInst(MirOpcode::Call, co, InvalidType);   // call_once returns void
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();

    std::unordered_map<std::uint32_t, std::string> recipes{{10u, "call_once"}};
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes,
                                      LibrarySynthesis{LibrarySynthVehicle::Win32, "kernel32.dll"},
                                      CSymbolDecorationScheme::None, externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    // 3 functions: main + call_once + the synthesized __dss_once_tramp.
    EXPECT_EQ(mir.moduleFuncCount(), 3u)
        << "call_once synthesizes the module-scoped __dss_once_tramp adapter";

    // InitOnceExecuteOnce imported; call_once itself never imported.
    std::optional<std::uint32_t> ioeo;
    bool importedCallOnce = false;
    for (auto const& imp : externs) {
        if (imp.mangledName == "InitOnceExecuteOnce") {
            ioeo = imp.symbol.v;
            EXPECT_EQ(imp.libraryPath, "kernel32.dll");
        }
        if (imp.mangledName == "call_once") importedCallOnce = true;
    }
    EXPECT_TRUE(ioeo.has_value()) << "call_once's body imports InitOnceExecuteOnce";
    EXPECT_FALSE(importedCallOnce) << "call_once is a synthesized def, never a kernel32 import";

    // The trampoline = the 3rd function (symbol not main's 100 nor the recipe's 10),
    // minted ABOVE the recipe id.
    std::optional<std::uint32_t> trampSym;
    MirFuncId trampFn{}, onceFn{};
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        std::uint32_t const s = mir.funcSymbol(f).v;
        if (s == 10u) onceFn = f;
        else if (s != 100u) { trampSym = s; trampFn = f; }
    }
    ASSERT_TRUE(trampSym.has_value());
    ASSERT_TRUE(onceFn.valid());
    ASSERT_TRUE(trampFn.valid());
    EXPECT_GT(*trampSym, 10u) << "the trampoline symbol is minted ABOVE the recipe id";

    // call_once ADDRESS-TAKES the trampoline (GlobalAddr(trampSym) in its body).
    bool addressTaken = false;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(onceFn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(onceFn, bi);
        for (std::uint32_t j = 0; j < mir.blockInstCount(b); ++j) {
            MirInstId const id = mir.blockInstAt(b, j);
            if (mir.instOpcode(id) == MirOpcode::GlobalAddr
                && mir.globalAddrSymbol(id).v == *trampSym)
                addressTaken = true;
        }
    }
    EXPECT_TRUE(addressTaken) << "call_once passes &__dss_once_tramp to InitOnceExecuteOnce";

    // The trampoline makes an INDIRECT call: a Call whose callee (operand 0) is an Arg,
    // not a GlobalAddr to a named import.
    bool indirectCall = false;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(trampFn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(trampFn, bi);
        for (std::uint32_t j = 0; j < mir.blockInstCount(b); ++j) {
            MirInstId const id = mir.blockInstAt(b, j);
            if (mir.instOpcode(id) == MirOpcode::Call) {
                auto ops = mir.instOperands(id);
                if (!ops.empty() && mir.instOpcode(ops[0]) == MirOpcode::Arg)
                    indirectCall = true;
            }
        }
    }
    EXPECT_TRUE(indirectCall)
        << "the trampoline invokes the C11 callback INDIRECTLY through its param Arg";

    // The trampoline's terminator Returns the constant 1 (TRUE): InitOnceExecuteOnce
    // treats a FALSE (0) return as init-FAILED and would RE-RUN the init — so a `ret 0`
    // regression (breaking exactly-once) is red at the unit tier, not just the example.
    bool returnsOne = false;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(trampFn); ++bi) {
        MirInstId const term = mir.blockTerminator(mir.funcBlockAt(trampFn, bi));
        if (mir.instOpcode(term) != MirOpcode::Return) continue;
        auto ops = mir.instOperands(term);
        ASSERT_EQ(ops.size(), 1u) << "__dss_once_tramp's Return carries the BOOL value";
        ASSERT_EQ(mir.instOpcode(ops[0]), MirOpcode::Const) << "the return value is a constant";
        MirLiteralValue const& lit = mir.literalValue(mir.constLiteralIndex(ops[0]));
        if (auto const* i = std::get_if<std::int64_t>(&lit.value)) returnsOne = (*i == 1);
        else if (auto const* u = std::get_if<std::uint64_t>(&lit.value)) returnsOne = (*u == 1u);
    }
    EXPECT_TRUE(returnsOne)
        << "__dss_once_tramp must return TRUE(1) — a ret 0 makes InitOnceExecuteOnce re-run init";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the call_once + trampoline module must verify";
}

// thrd_join is the first MULTI-block recipe (WaitForSingleObject; if(res)
// GetExitCodeThread; CloseHandle). Running MirVerifier PINS the canonical StructCfMarkers
// the module-wide rederiveStructCfMarkers stamped on the entry/then/join blocks (a wrong
// marker fires I_StructCfMismatch).
TEST(SynthThreadsShim, ThrdJoinIsMultiBlockAndVerifies) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32  = in.primitive(TypeKind::I32);
    TypeId const pV   = in.pointer(in.primitive(TypeKind::Void));
    TypeId const pI32 = in.pointer(i32);
    TypeId const mainSig = in.fnSig({}, i32, CallConv::CcMS64);
    std::array<TypeId, 2> const jp{pV, pI32};
    TypeId const joinSig = in.fnSig(jp, i32, CallConv::CcMS64);   // (thrd_t, int*)->int

    MirBuilder mb;
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const slot = mb.addInst(MirOpcode::Alloca, {}, pV, 8);
    MirInstId const ga   = mb.addGlobalAddr(SymbolId{10}, in.pointer(joinSig));
    MirInstId const co[] = {ga, slot, slot};
    mb.addInst(MirOpcode::Call, co, i32);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();

    std::unordered_map<std::uint32_t, std::string> recipes{{10u, "thrd_join"}};
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes,
                                      LibrarySynthesis{LibrarySynthVehicle::Win32, "kernel32.dll"},
                                      CSymbolDecorationScheme::None, externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    MirFuncId joinFn{};
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i)
        if (mir.funcSymbol(mir.funcAt(i)).v == 10u) joinFn = mir.funcAt(i);
    ASSERT_TRUE(joinFn.valid());
    EXPECT_GT(mir.funcBlockCount(joinFn), 1u)
        << "thrd_join is MULTI-block (the res!=NULL guard is a real branch)";

    bool wfso = false, gect = false, ch = false;
    for (auto const& imp : externs) {
        if (imp.mangledName == "WaitForSingleObject") wfso = true;
        if (imp.mangledName == "GetExitCodeThread")   gect = true;
        if (imp.mangledName == "CloseHandle")         ch   = true;
    }
    EXPECT_TRUE(wfso && gect && ch)
        << "thrd_join imports WaitForSingleObject + GetExitCodeThread + CloseHandle";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep))
        << "the multi-block thrd_join module must verify (canonical markers rederived)";
}

// ── FC17.9(a): MULTI-CU threads. A shim symbol is REFERENCED-ONLY per CU (skipped from
// import at CST→HIR; defined POST-merge). The merge's step-3c must pre-register it a
// merged id, else the clone ABORTS (mergedSymbolOf) on the caller's GlobalAddr — the
// exact latent crash the audit caught. RED-on-disable: WITHOUT step-3c this test does not
// fail-soft, it std::abort()s the process (a hard crash) — the strongest red-on-disable.
// After the merge the shim lands in symbolNames as a not-yet-defined vocab symbol; the
// program.cpp reconstruction (mirrored here) synthesizes it → the merged module verifies.
TEST(MirMerge, MultiCuThreadsShimRegistersAndSynthesizes) {
    // CU0: int main() { mtx_lock(&slot); return 0; } — mtx_lock is a referenced-only shim
    // (SymbolId{10}: a GlobalAddr callee, NOT a defined func, NOT an ExternImport).
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32_0 = in0.primitive(TypeKind::I32);
    TypeId const pV0   = in0.pointer(in0.primitive(TypeKind::Void));
    TypeId const mainSig = in0.fnSig({}, i32_0, CallConv::CcMS64);
    std::array<TypeId, 1> const lockParams{pV0};
    TypeId const lockSig = in0.fnSig(lockParams, i32_0, CallConv::CcSysV);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(mainSig, SymbolId{100});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const slot     = mb.addInst(MirOpcode::Alloca, {}, pV0, 40);
        MirInstId const lockAddr = mb.addGlobalAddr(SymbolId{10}, in0.pointer(lockSig));
        MirInstId const callOps[] = {lockAddr, slot};
        mb.addInst(MirOpcode::Call, callOps, i32_0);
        mb.addReturn(mb.addConst(i32Lit(0), i32_0));
        mir0 = std::move(mb).finish();
    }
    std::unordered_map<std::uint32_t, std::string> const recipes0{{10u, "mtx_lock"}};

    // CU1: a plain helper (only to force the N>1 merge path).
    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i32_1 = in1.primitive(TypeKind::I32);
    TypeId const sig1  = in1.fnSig({}, i32_1, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        mb.addFunction(sig1, SymbolId{50});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(7), i32_1));
        mir1 = std::move(mb).finish();
    }

    MergeCuInput cu0{&mir0, &in0, namerOf({{100, "main"}, {10, "mtx_lock"}}), {}};
    cu0.synthRecipes = &recipes0;   // ← the referenced-only shim, threaded to the merge
    MergeCuInput cu1{&mir1, &in1, namerOf({{50, "helper"}}), {}};
    std::vector<MergeCuInput> cus{cu0, cu1};

    std::vector<std::string> const entries{"main"};
    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value())
        << "the merge must NOT abort on a referenced-only shim GlobalAddr (multi-CU defect)";
    EXPECT_EQ(rep.errorCount(), 0u);

    // step-3c registered the shim with a merged id + a symbolNames entry.
    std::optional<std::uint32_t> shimV;
    for (auto const& [v, name] : merged->symbolNames)
        if (name == "mtx_lock") shimV = v;
    ASSERT_TRUE(shimV.has_value())
        << "step-3c must register the referenced-only shim in symbolNames";
    // It is referenced-only in the merged module (no def, no import) — the exact state the
    // program.cpp reconstruction detects and hands to synthesizeThreadsShim.
    for (std::uint32_t i = 0; i < merged->mir.moduleFuncCount(); ++i)
        EXPECT_NE(merged->mir.funcSymbol(merged->mir.funcAt(i)).v, *shimV)
            << "the shim is not yet defined pre-synthesis";
    EXPECT_TRUE(merged->externImports.empty()) << "the shim is never an import";

    std::unordered_map<std::uint32_t, std::string> mergedRecipes{{*shimV, "mtx_lock"}};
    std::vector<ExternImport> externs = merged->externImports;
    LibrarySynthesis const win32{LibrarySynthVehicle::Win32, "kernel32.dll"};
    ASSERT_TRUE(synthesizeThreadsShim(merged->mir, merged->host.interner(),
                                      mergedRecipes, win32, CSymbolDecorationScheme::None, externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    bool defined = false;
    for (std::uint32_t i = 0; i < merged->mir.moduleFuncCount(); ++i)
        if (merged->mir.funcSymbol(merged->mir.funcAt(i)).v == *shimV) defined = true;
    EXPECT_TRUE(defined) << "the shim is synthesized as a merged-module definition";

    MirVerifier verifier{merged->mir, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "the merged + shim-synthesized module must verify";
}

// ── D-CSUBSET-C11-THREADS-MACHO: the `pthread` vehicle (Darwin libSystem) ──────────────

// The pthread vehicle mirrors the win32 test above: SymbolId{10} (mtx_lock) becomes a
// DEFINED shim over pthread_mutex_lock imported from libSystem — NEVER the shim name, and
// NEVER a kernel32 primitive. Proves the vehicle switch reads the DECLARED vehicle, not the
// format.
TEST(SynthThreadsShim, PthreadVehicleSynthesizesDefinitionAndPthreadHelperImport) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const pV  = in.pointer(in.primitive(TypeKind::Void));
    TypeId const mainSig = in.fnSig({}, i32, CallConv::CcMS64);
    std::array<TypeId, 1> const lockParams{pV};
    TypeId const lockSig = in.fnSig(lockParams, i32, CallConv::CcAAPCS64);

    MirBuilder mb;
    mb.addFunction(mainSig, SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const slot     = mb.addInst(MirOpcode::Alloca, {}, pV, 64);   // a macho mtx_t (64B)
    MirInstId const lockAddr = mb.addGlobalAddr(SymbolId{10}, in.pointer(lockSig));
    MirInstId const callOps[] = {lockAddr, slot};
    mb.addInst(MirOpcode::Call, callOps, i32);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();

    std::unordered_map<std::uint32_t, std::string> recipes{{10u, "mtx_lock"}};
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    LibrarySynthesis const pthread{LibrarySynthVehicle::Pthread, "/usr/lib/libSystem.B.dylib"};
    ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, pthread, CSymbolDecorationScheme::LeadingUnderscore,
                                      externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    bool foundLockDef = false;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i)
        if (mir.funcSymbol(mir.funcAt(i)).v == 10u) foundLockDef = true;
    EXPECT_TRUE(foundLockDef) << "mtx_lock must be a synthesized pthread-shim definition";

    // The helper import is macho-C-MANGLED (`_pthread_mutex_lock`) so dyld resolves it in
    // libSystem — the un-mangled name is what SIGABRT'd the first witness.
    bool importedLock = false, importedPthreadLock = false, importedKernel32 = false;
    for (auto const& imp : externs) {
        if (imp.mangledName == "mtx_lock" || imp.mangledName == "_mtx_lock") importedLock = true;
        if (imp.mangledName == "_pthread_mutex_lock") {
            importedPthreadLock = true;
            EXPECT_EQ(imp.libraryPath, "/usr/lib/libSystem.B.dylib");
            EXPECT_FALSE(imp.isData);
        }
        if (imp.mangledName == "EnterCriticalSection" || imp.libraryPath == "kernel32.dll")
            importedKernel32 = true;
    }
    EXPECT_FALSE(importedLock) << "mtx_lock must NOT be an import (eager-import law)";
    EXPECT_TRUE(importedPthreadLock)
        << "the pthread mtx_lock body must import the macho-mangled _pthread_mutex_lock";
    EXPECT_FALSE(importedKernel32) << "the pthread vehicle must NEVER emit a kernel32 primitive";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the pthread-shim-synthesized module must verify";
}

// RED-on-disable for the new fail-loud: a NON-empty recipe map with NO declared vehicle
// must fail loud (never silently assume win32). This is the guard that keeps the vehicle a
// config value, not a defaulted format identity.
TEST(SynthThreadsShim, MissingLibrarySynthesisWithNonEmptyRecipesFailsLoud) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder mb;
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{100});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    Mir mir = std::move(mb).finish();

    std::unordered_map<std::uint32_t, std::string> recipes{{10u, "mtx_lock"}};
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    EXPECT_FALSE(synthesizeThreadsShim(mir, in, recipes, std::nullopt, CSymbolDecorationScheme::LeadingUnderscore,
                                       externs, rep))
        << "recipes present + no vehicle MUST fail loud, never assume a primitive family";
    EXPECT_TRUE(rep.hasErrors());
}

// Per-recipe COVERAGE (multi-form contract, §A.5): every one of the 21 pthread recipes (the
// 18 non-trampoline + the 3 trampolines thrd_create/call_once/thrd_join) must (a) land as a
// definition, (b) import its SPECIFIC pthread primitive from libSystem, and (c) verify. A
// subset-only test would let a latent miss at an unexercised recipe survive.
TEST(SynthThreadsShim, PthreadAllTwentyOneRecipesEmitAndVerify) {
    struct Case { char const* recipe; char const* helper; };
    static constexpr Case kCases[] = {
        {"mtx_init", "pthread_mutex_init"},   {"mtx_lock", "pthread_mutex_lock"},
        {"mtx_unlock", "pthread_mutex_unlock"},{"mtx_trylock", "pthread_mutex_trylock"},
        {"mtx_destroy", "pthread_mutex_destroy"},
        {"cnd_init", "pthread_cond_init"},    {"cnd_signal", "pthread_cond_signal"},
        {"cnd_broadcast", "pthread_cond_broadcast"}, {"cnd_wait", "pthread_cond_wait"},
        {"cnd_destroy", "pthread_cond_destroy"},
        {"tss_create", "pthread_key_create"}, {"tss_get", "pthread_getspecific"},
        {"tss_set", "pthread_setspecific"},   {"tss_delete", "pthread_key_delete"},
        {"thrd_current", "pthread_self"},     {"thrd_yield", "sched_yield"},
        {"thrd_exit", "pthread_exit"},        {"thrd_detach", "pthread_detach"},
        {"thrd_create", "pthread_create"},    {"call_once", "pthread_once"},
        {"thrd_join", "pthread_join"},
    };
    for (auto const& c : kCases) {
        TypeInterner in{CompilationUnitId{1}};
        TypeId const i32 = in.primitive(TypeKind::I32);
        MirBuilder mb;
        mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{100});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(0), i32));
        Mir mir = std::move(mb).finish();

        std::unordered_map<std::uint32_t, std::string> recipes{{10u, c.recipe}};
        std::vector<ExternImport> externs;
        DiagnosticReporter rep;
        LibrarySynthesis const pthread{LibrarySynthVehicle::Pthread, "/usr/lib/libSystem.B.dylib"};
        ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, pthread, CSymbolDecorationScheme::LeadingUnderscore,
                                          externs, rep))
            << "recipe '" << c.recipe << "' must synthesize";
        EXPECT_FALSE(rep.hasErrors()) << c.recipe;

        bool defined = false;
        for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i)
            if (mir.funcSymbol(mir.funcAt(i)).v == 10u) defined = true;
        EXPECT_TRUE(defined) << c.recipe << " must be a synthesized definition";

        // The helper is macho-C-mangled (leading `_`), matching libSystem's export.
        std::string const expectedHelper = std::string("_") + c.helper;
        bool importedHelper = false, importedShimName = false;
        for (auto const& imp : externs) {
            if (imp.mangledName == expectedHelper) {
                importedHelper = true;
                EXPECT_EQ(imp.libraryPath, "/usr/lib/libSystem.B.dylib") << c.recipe;
            }
            if (imp.mangledName == c.recipe) importedShimName = true;
        }
        EXPECT_TRUE(importedHelper)
            << c.recipe << " must import its pthread primitive '" << expectedHelper << "'";
        EXPECT_FALSE(importedShimName) << c.recipe << " shim name must never be imported";

        MirVerifier verifier{mir, &in};
        EXPECT_TRUE(verifier.verify(rep)) << c.recipe << " synthesized module must verify";
    }
}

// The merge-path reconstruction (program.cpp) is vehicle-agnostic: the SAME referenced-only
// shim, handed the pthread vehicle, synthesizes over libSystem. Mirrors the win32 multi-CU
// test with the pthread vehicle (proves the vehicle threads through the merge seam too).
TEST(MirMerge, MultiCuThreadsShimSynthesizesPthreadVehicle) {
    TypeInterner in0{CompilationUnitId{1}};
    TypeId const i32_0 = in0.primitive(TypeKind::I32);
    TypeId const pV0   = in0.pointer(in0.primitive(TypeKind::Void));
    TypeId const mainSig = in0.fnSig({}, i32_0, CallConv::CcMS64);
    std::array<TypeId, 1> const lockParams{pV0};
    TypeId const lockSig = in0.fnSig(lockParams, i32_0, CallConv::CcAAPCS64);
    Mir mir0;
    {
        MirBuilder mb;
        mb.addFunction(mainSig, SymbolId{100});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirInstId const slot     = mb.addInst(MirOpcode::Alloca, {}, pV0, 64);
        MirInstId const lockAddr = mb.addGlobalAddr(SymbolId{10}, in0.pointer(lockSig));
        MirInstId const callOps[] = {lockAddr, slot};
        mb.addInst(MirOpcode::Call, callOps, i32_0);
        mb.addReturn(mb.addConst(i32Lit(0), i32_0));
        mir0 = std::move(mb).finish();
    }
    std::unordered_map<std::uint32_t, std::string> const recipes0{{10u, "mtx_lock"}};

    TypeInterner in1{CompilationUnitId{2}};
    TypeId const i32_1 = in1.primitive(TypeKind::I32);
    TypeId const sig1  = in1.fnSig({}, i32_1, CallConv::CcSysV);
    Mir mir1;
    {
        MirBuilder mb;
        mb.addFunction(sig1, SymbolId{50});
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        mb.addReturn(mb.addConst(i32Lit(7), i32_1));
        mir1 = std::move(mb).finish();
    }

    MergeCuInput cu0{&mir0, &in0, namerOf({{100, "main"}, {10, "mtx_lock"}}), {}};
    cu0.synthRecipes = &recipes0;
    MergeCuInput cu1{&mir1, &in1, namerOf({{50, "helper"}}), {}};
    std::vector<MergeCuInput> cus{cu0, cu1};

    std::vector<std::string> const entries{"main"};
    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, entries, rep);
    ASSERT_TRUE(merged.has_value());
    EXPECT_EQ(rep.errorCount(), 0u);

    std::optional<std::uint32_t> shimV;
    for (auto const& [v, name] : merged->symbolNames)
        if (name == "mtx_lock") shimV = v;
    ASSERT_TRUE(shimV.has_value());

    std::unordered_map<std::uint32_t, std::string> mergedRecipes{{*shimV, "mtx_lock"}};
    std::vector<ExternImport> externs = merged->externImports;
    LibrarySynthesis const pthread{LibrarySynthVehicle::Pthread, "/usr/lib/libSystem.B.dylib"};
    ASSERT_TRUE(synthesizeThreadsShim(merged->mir, merged->host.interner(),
                                      mergedRecipes, pthread, CSymbolDecorationScheme::LeadingUnderscore,
                                      externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    bool importedPthreadLock = false;
    for (auto const& imp : externs)
        if (imp.mangledName == "_pthread_mutex_lock") importedPthreadLock = true;
    EXPECT_TRUE(importedPthreadLock)
        << "the merged pthread shim must import the macho-mangled _pthread_mutex_lock";

    MirVerifier verifier{merged->mir, &merged->host.interner()};
    EXPECT_TRUE(verifier.verify(rep)) << "the merged + pthread-shim module must verify";
}

// ── D-LANG-TYPE-IDENTITY-VOCABULARY: vocabulary identity survives the merge ──
//
// A whole-program / static-link merge folds N per-CU interners into ONE host
// lattice. If the re-intern walker rebuilt primitives from the TypeKind alone it
// would DROP the vocabulary tag, silently re-collapsing `long` onto `int` (LLP64)
// and `long` onto `long long` (LP64) at exactly the boundary the front-end split
// them at — a cross-CU type-identity miscompile invisible to any single-CU test.
//
// CU0's `f` returns `long *`; CU1's `g` returns `long long *`. Under LP64 both
// pointees are I64, so the two return types are distinguishable ONLY by the
// vocabulary tag. RED-ON-DISABLE: revert type_reintern's primitive arm to
// `dst.primitive(kind)` and the two host pointer types become ONE.
TEST(MirMerge, MergePreservesVocabularyIdentityAcrossCus) {
    auto buildPtrReturner = [](TypeInterner& in, char const* vocab,
                               SymbolId sym, Mir& out) -> TypeId {
        TypeId const elem = in.primitive(TypeKind::I64, vocab);
        TypeId const ptr  = in.pointer(elem);
        TypeId const sig  = in.fnSig({}, ptr, CallConv::CcSysV);
        MirBuilder mb;
        mb.addFunction(sig, sym);
        MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(e);
        MirLiteralValue nullp;
        nullp.value = std::uint64_t{0};
        nullp.core  = TypeKind::Ptr;
        mb.addReturn(mb.addConst(nullp, ptr));
        out = std::move(mb).finish();
        return ptr;
    };

    TypeInterner in0{CompilationUnitId{1}};
    TypeInterner in1{CompilationUnitId{2}};
    Mir mir0;
    Mir mir1;
    TypeId const cu0Ptr = buildPtrReturner(in0, "long",      SymbolId{100}, mir0);
    TypeId const cu1Ptr = buildPtrReturner(in1, "long long", SymbolId{50},  mir1);
    // Fixture precondition: within their own CUs the two are already distinct.
    ASSERT_EQ(in0.kind(in0.operands(cu0Ptr)[0]), TypeKind::I64);
    ASSERT_EQ(in1.kind(in1.operands(cu1Ptr)[0]), TypeKind::I64);

    std::vector<MergeCuInput> cus = {
        MergeCuInput{&mir0, &in0, namerOf({{100, "main"}}), {}},
        MergeCuInput{&mir1, &in1, namerOf({{50, "g"}}), {}},
    };
    std::vector<std::string> const entries = {"main"};

    DiagnosticReporter rep;
    auto merged = mergeCuMirs(cus, TypeLattice{CompilationUnitId{88}}, entries, rep);
    ASSERT_TRUE(merged.has_value()) << "errorCount=" << rep.errorCount();

    Mir const& mm = merged->mir;
    auto const& hi = merged->host.interner();
    auto const fMain = findFuncByName(mm, merged->symbolNames, "main");
    auto const fG    = findFuncByName(mm, merged->symbolNames, "g");
    ASSERT_TRUE(fMain.has_value());
    ASSERT_TRUE(fG.has_value());

    auto returnedPointee = [&](MirFuncId fn) {
        MirInstId const c = mm.blockInstAt(mm.funcEntry(fn), 0);
        EXPECT_EQ(mm.instOpcode(c), MirOpcode::Const);
        TypeId const ty = mm.instType(c);
        EXPECT_EQ(ty.arenaTag, 88u) << "must be HOST-interned";
        EXPECT_EQ(hi.kind(ty), TypeKind::Ptr);
        return hi.operands(ty)[0];
    };
    TypeId const p0 = returnedPointee(*fMain);
    TypeId const p1 = returnedPointee(*fG);

    EXPECT_EQ(hi.kind(p0), TypeKind::I64);
    EXPECT_EQ(hi.kind(p1), TypeKind::I64) << "representation is identical";
    EXPECT_EQ(hi.name(p0), "long");
    EXPECT_EQ(hi.name(p1), "long long");
    EXPECT_NE(p0.v, p1.v)
        << "two vocabulary entries sharing a representation must stay TWO types "
           "in the merged host lattice — a collapse here is a cross-CU identity "
           "miscompile no single-CU test can see";

    MirVerifier verifier{mm, &hi};
    EXPECT_TRUE(verifier.verify(rep)) << "merged module must verify";
}

// ── D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3): synthesizeStdioShim ───────────────────
//
// The <stdio.h> printf-family sibling of the SynthThreadsShim suite above. The modern
// UCRT (`ucrtbase.dll`) exports NO concrete `sprintf` — only the common core
// `__stdio_common_vsprintf` — so the pe `sprintf` row in stdio.json carries
// `synthesize: "sprintf"` and this pass supplies the body. NOTHING about that surface was
// unit-covered when it landed; these tests are that cover.
//
// Two properties carry the weight, and BOTH fail SILENTLY rather than loudly if they
// regress (wrong text at runtime, never a compile or link error):
//   * the body must route through the module's ALREADY-IMPORTED UCRT core and mint no
//     import of its own (the "the helpers are ordinary descriptor imports" contract — the
//     eager-import law is what proves the core really exists as an export, so a
//     self-minted import would bypass that proof);
//   * the body must carry the `VaHomeArgAreaAddr` leaf, which is simultaneously (a) the
//     Win64 `va_list` value `&home[namedArgCount]`, (b) lir_callconv's prologue-spill
//     signal, and (c) the ONLY thing making the inliner refuse to splice the shim into a
//     caller (src/opt/passes/inlining.cpp). Under the MULTI-CU driver the shim is
//     synthesized PRE-optimize, so the release pipeline's Inlining pass really is offered
//     this body — see examples/c-subset/shipped_sprintf_ucrt_crosscu for the end-to-end
//     runtime witness of that seam.

namespace {

// The shim's caller-side scaffold: one `main` that references `sprintf` (pre-minted
// SymbolId{10}, seeded into functionSymbols by the CST→HIR seam so the reference lowered
// to a GlobalAddr against a NOT-yet-defined callee) and returns.
Mir buildSprintfCaller(TypeInterner& in) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const pCh = in.pointer(in.primitive(TypeKind::Char));
    std::array<TypeId, 2> const sp{pCh, pCh};
    TypeId const sprintfSig = in.fnSig(sp, i32, CallConv::CcMS64, /*isVariadic=*/true);

    MirBuilder mb;
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{100});   // main
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const buf = mb.addInst(MirOpcode::Alloca, {}, pCh, 64);
    MirInstId const ga  = mb.addGlobalAddr(SymbolId{10}, in.pointer(sprintfSig));
    MirInstId const co[] = {ga, buf, buf};
    mb.addInst(MirOpcode::Call, co, i32);
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

// The UCRT core as an ORDINARY descriptor import — exactly what stdio.json's pe
// `__stdio_common_vsprintf` row produces, bound to ucrtbase.dll.
std::vector<ExternImport> ucrtCoreImports() {
    ExternImport core;
    core.symbol      = SymbolId{20};
    core.mangledName = "__stdio_common_vsprintf";
    core.libraryPath = "ucrtbase.dll";
    core.isData      = false;
    return {core};
}

// Locate a definition by its SymbolId value.
std::optional<MirFuncId> findFuncBySymbol(Mir const& mir, std::uint32_t symV) {
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v == symV) return f;
    }
    return std::nullopt;
}

// D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) — WHY THESE TESTS PASS A WHOLE `VaListLayout`
// AND NOT THE `VaListStrategy` THEY USED TO. The strategy ALONE does not determine the
// va leaf: `HomogeneousPointer` forks on `variadicUsesOverflowBase` into the Win64 home
// base (`VaHomeArgAreaAddr`) and the Apple-arm64 overflow base (`VaOverflowArgAreaAddr`)
// — see src/mir/merge/synth_stdio_shim.hpp. So the layout is spelled out FIELD BY FIELD
// here, and `variadicUsesOverflowBase = false` is written explicitly rather than left to
// the struct's default: it is the field that makes `VaHomeArgAreaAddr` the RIGHT leaf
// below instead of an accident. `namedArgSlotBytes` is filled so the layout is a
// plausible whole, not a one-field stub. The overflow-base twin arm is pinned by its own
// dedicated suite (tests/mir/test_synth_stdio_shim_valist.cpp).
VaListLayout vaListLayoutOf(VaListStrategy strategy) {
    VaListLayout l;
    l.strategy                 = strategy;
    l.namedArgSlotBytes        = 8;
    l.variadicUsesOverflowBase = false;
    return l;
}

} // namespace

// The HAPPY PATH, asserted STRUCTURALLY rather than "it returned true": the referenced
// `sprintf` becomes a DEFINED variadic function whose single block carries the
// `VaHomeArgAreaAddr` leaf at the right named-arg count and calls the module's existing
// UCRT core — while the import list is left EXACTLY as it was found.
//
// RED-ON-DISABLE (each assertion independently): drop the `ap` operand from the ops array
// and the leaf assertion reds; point `coreAddr` at a freshly minted symbol and the
// "callee is SymbolId{20}" assertion reds; have the pass push its own ExternImport and
// the "imports unchanged" assertion reds.
TEST(SynthStdioShim, SprintfSynthesizesVariadicBodyOverImportedUcrtCore) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSprintfCaller(in);

    std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "sprintf"}};
    std::vector<ExternImport> externs = ucrtCoreImports();
    std::vector<ExternImport> const before = externs;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes,
                                    vaListLayoutOf(VaListStrategy::HomogeneousPointer),
                                    externs, rep));
    EXPECT_FALSE(rep.hasErrors());

    // (a) SymbolId{10} (sprintf) is now a DEFINED module function, and main survived.
    auto const shim = findFuncBySymbol(mir, 10u);
    ASSERT_TRUE(shim.has_value()) << "sprintf must be a synthesized definition";
    EXPECT_TRUE(findFuncBySymbol(mir, 100u).has_value()) << "main must be cloned verbatim";
    EXPECT_EQ(mir.moduleFuncCount(), 2u) << "exactly one shim appended (main + sprintf)";

    // (b) The shim's OWN signature is VARIADIC with the 2 FIXED params (buf, fmt) —
    // `...` is a marker, so a non-variadic sig here would mean the Win64 prologue never
    // spills the home area at all and `ap` would point at uninitialized stack.
    TypeId const shimSig = mir.funcSignature(*shim);
    EXPECT_TRUE(in.fnIsVariadic(shimSig)) << "the sprintf shim must itself be variadic";
    EXPECT_EQ(in.fnParams(shimSig).size(), 2u) << "sprintf's FIXED arity is (buf, fmt)";

    // (c) The body: exactly one VaHomeArgAreaAddr, payload == the named-arg slot count
    // (2), and a Call whose CALLEE operand is a GlobalAddr to the imported core.
    ASSERT_EQ(mir.funcBlockCount(*shim), 1u) << "every printf-family recipe is single-block";
    MirBlockId const b = mir.funcBlockAt(*shim, 0);
    std::uint32_t vaLeaves = 0;
    std::uint32_t calls    = 0;
    std::optional<std::uint32_t> calleeSym;
    for (std::uint32_t i = 0; i < mir.blockInstCount(b); ++i) {
        MirInstId const id = mir.blockInstAt(b, i);
        if (mir.instOpcode(id) == MirOpcode::VaHomeArgAreaAddr) {
            ++vaLeaves;
            EXPECT_EQ(mir.instPayload(id), 2u)
                << "the va leaf is &home[namedArgCount]; sprintf's named count is 2";
        }
        if (mir.instOpcode(id) == MirOpcode::Call) {
            ++calls;
            auto const ops = mir.instOperands(id);
            ASSERT_FALSE(ops.empty());
            if (mir.instOpcode(ops[0]) == MirOpcode::GlobalAddr)
                calleeSym = mir.globalAddrSymbol(ops[0]).v;
            // callee + (opts, buf, count, fmt, locale, ap) == 7 operands.
            EXPECT_EQ(ops.size(), 7u) << "__stdio_common_vsprintf takes 6 arguments";
        }
    }
    EXPECT_EQ(vaLeaves, 1u)
        << "exactly one VaHomeArgAreaAddr: the va_list value, the prologue-spill signal, "
           "AND the inliner's refusal trigger (src/opt/passes/inlining.cpp)";
    EXPECT_EQ(calls, 1u) << "the shim forwards through exactly one core call";
    ASSERT_TRUE(calleeSym.has_value()) << "the shim must call through a GlobalAddr";
    EXPECT_EQ(*calleeSym, 20u)
        << "the shim must call the module's ALREADY-IMPORTED __stdio_common_vsprintf";

    // (d) The pass mints NO import — the cores are ordinary descriptor imports, which is
    // what makes the eager-import law their existence proof. `sprintf` itself must never
    // be imported (ucrtbase exports no such symbol; importing it would break every
    // binary's LOAD).
    ASSERT_EQ(externs.size(), before.size()) << "synthesizeStdioShim must mint no import";
    for (std::size_t i = 0; i < externs.size(); ++i)
        EXPECT_EQ(externs[i].mangledName, before[i].mangledName);
    for (auto const& e : externs)
        EXPECT_NE(e.mangledName, "sprintf") << "sprintf must NEVER be an import";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep)) << "the stdio-shim-synthesized module must verify";
}

// An EMPTY recipe map is a clean no-op — and it must short-circuit BEFORE the va-strategy
// check, so it stays clean even with NO strategy resolved (every elf/macho build and
// every pe TU that includes no printf family). Locks the pass to a pure DATA gate, never
// a format check. RED-ON-DISABLE: move the `recipeBySymbol.empty()` early return below
// the strategy gate and this reds.
TEST(SynthStdioShim, EmptyRecipeMapIsNoOpEvenWithNoVaStrategy) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSprintfCaller(in);
    std::size_t const before = mir.moduleFuncCount();

    std::unordered_map<std::uint32_t, std::string> const recipes;   // empty
    std::vector<ExternImport> externs;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, std::nullopt, externs, rep));
    EXPECT_FALSE(rep.hasErrors());
    EXPECT_EQ(mir.moduleFuncCount(), before) << "no shim appended for an empty map";
    EXPECT_TRUE(externs.empty()) << "no import planted for an empty map";
}

// FAIL-LOUD (1/3): a recipe id with NO switch arm. The family split routes every Stdio id
// here, so an id this pass cannot build MUST be a reported error AND a `false` return —
// never a silently missing definition (which surfaces, if at all, as an undefined symbol
// far downstream, and on pe as a 0xC0000139 at LOAD).
TEST(SynthStdioShim, UnknownRecipeIdFailsLoud) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSprintfCaller(in);

    std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "no_such_recipe"}};
    std::vector<ExternImport> externs = ucrtCoreImports();
    DiagnosticReporter rep;
    EXPECT_FALSE(synthesizeStdioShim(mir, in, recipes,
                                     vaListLayoutOf(VaListStrategy::HomogeneousPointer),
                                     externs, rep))
        << "a recipe with no synth arm MUST fail loud, never silently skip the definition";
    EXPECT_TRUE(rep.hasErrors())
        << "the refusal must carry a real diagnostic, not a bare false";
}

// FAIL-LOUD (2/3): the UCRT core is NOT among the module's imports — i.e. stdio.json
// declared a `synthesize` row without the `__stdio_common_v*` row it needs. Unchecked,
// this is exactly the drift that yields a shim calling nothing.
TEST(SynthStdioShim, MissingUcrtCoreImportFailsLoud) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSprintfCaller(in);

    std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "sprintf"}};
    std::vector<ExternImport> externs;   // NO __stdio_common_vsprintf
    DiagnosticReporter rep;
    EXPECT_FALSE(synthesizeStdioShim(mir, in, recipes,
                                     vaListLayoutOf(VaListStrategy::HomogeneousPointer),
                                     externs, rep))
        << "an unimported UCRT core MUST fail loud (descriptor/pass drift)";
    EXPECT_TRUE(rep.hasErrors()) << "the refusal must carry a real diagnostic";
}

// FAIL-LOUD (3/3): NO va-list model resolved. `CuMirModule::vaListLayout` (D-FFI-PE-CRT-
// UCRT-MIGRATION Phase 3 widened it from `optional<VaListStrategy>` to
// `optional<VaListLayout>`) is `std::optional` precisely so UNRESOLVED is distinguishable
// from resolved — and widening it did NOT weaken that, because a default-constructed
// `VaListLayout` is a REAL one: its `strategy` defaults to SysVRegisterSave, so a
// non-optional field would READ as "the target declared SysV" when the truth is "nobody
// ever asked the target". With a real stdio recipe in hand that ambiguity must be an
// ERROR: the alternative is forwarding a va_list under a guessed ABI, which miscompiles
// silently.
TEST(SynthStdioShim, NulloptVaListStrategyWithRecipesFailsLoud) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSprintfCaller(in);

    std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "sprintf"}};
    std::vector<ExternImport> externs = ucrtCoreImports();
    DiagnosticReporter rep;
    EXPECT_FALSE(synthesizeStdioShim(mir, in, recipes, std::nullopt, externs, rep))
        << "recipes present + NO resolved va_list strategy MUST fail loud, never default";
    EXPECT_TRUE(rep.hasErrors()) << "the refusal must carry a real diagnostic";
}

// FAIL-LOUD (bonus): a RESOLVED but unimplemented strategy. Only the HomogeneousPointer
// arm is built (both of its bases — see `vaListLayoutOf` above); no elf/macho descriptor
// declares a stdio synthesize recipe, so the SysVRegisterSave and Aapcs64DualCursor arms
// would be speculative builds. Refusing is right; silently emitting the pointer-shaped
// forward under a register-save-area target would be a wrong-ABI miscompile. Distinct from
// the nullopt case above: this one is "the target ANSWERED, with a model we don't
// implement".
TEST(SynthStdioShim, UnimplementedVaListStrategyFailsLoud) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildSprintfCaller(in);

    std::unordered_map<std::uint32_t, std::string> const recipes{{10u, "sprintf"}};
    std::vector<ExternImport> externs = ucrtCoreImports();
    DiagnosticReporter rep;
    EXPECT_FALSE(synthesizeStdioShim(mir, in, recipes,
                                     vaListLayoutOf(VaListStrategy::SysVRegisterSave),
                                     externs, rep))
        << "an unimplemented va_list model MUST fail loud, never forward under a wrong ABI";
    EXPECT_TRUE(rep.hasErrors()) << "the refusal must carry a real diagnostic";
}

// ═══════════════════════════════════════════════════════════════════════════════════
// D-FFI-PE-CRT-UCRT-MIGRATION (Phase 3) — THE STDIO SHIM OPERAND CONTRACT (SIX ARMS)
// ═══════════════════════════════════════════════════════════════════════════════════
//
// WHY THIS SECTION EXISTS, stated as the defect it closes rather than as "more
// coverage". Everything above pins the shim's SHAPE by reading three things about the
// synthesized call — `ops[0]` (the callee), `ops.size()`, and `ops.back()`. Every
// operand BETWEEN those was unasserted, and three of the five recipes shipped at the
// time (`fprintf`, `vfprintf`, `sscanf`) had no unit at all. TF-C119 added a SIXTH
// recipe, `snprintf`; its SINGLE-ARM operand pins live beside its va-leaf and
// multi-block story in tests/mir/test_synth_stdio_shim_valist.cpp (ARM 6/6), and the
// all-six together test at the end of this section covers it here.
//
// ★ THAT GAP WAS MEASURED, NOT SUPPOSED (TF-C112). Each mutation below was applied to
// src/mir/merge/synth_stdio_shim.cpp one at a time and BOTH mir stdio binaries re-run:
//   * TRANSPOSING `buf` and `fmt` in the sprintf arm — a wrong-argument miscompile that
//     formats the destination buffer and writes the result over the format string —
//     passed EVERY assertion in both files, AND the MirVerifier. Both operands are
//     `char*`, so no type check at any tier can see it; only the POSITION can.
//   * `kIobStdout = 1 -> 2`, i.e. `printf` silently writing to STDERR, likewise passed
//     everything: the sole test holding that operand discarded it with `has_value()`.
// So the rule below is: EVERY operand of EVERY arm is pinned BY POSITION, and where an
// operand is a COMPUTED value (`printf`'s stream, every variadic arm's `ap`) it is
// pinned as THAT instruction's result rather than as "an instruction of that kind
// exists somewhere in the body" — an aggregate a sibling code path can satisfy on its
// own is not a guard.
//
// The `_Options` / `_BufferCount` / iob-index values are re-stated here from the UCRT
// contract (corecrt_stdio_config.h, and stdio.json's own `$comment`) rather than read
// back out of the pass: the pass's constants are file-local `constexpr`s in an
// anonymous namespace, and importing them would make any future change to them
// self-approving.
//
// ★ RED-ON-DISABLE, DEMONSTRATED BY BREAKING EACH GUARDED THING. Every mutation below
// was applied to src/mir/merge/synth_stdio_shim.cpp ONE AT A TIME, both mir stdio
// binaries re-run, and the source then restored (verified byte-identical afterwards by
// `git hash-object`). Each reds the named assertion and nothing else in this file:
//   sprintf `buf`/`fmt` transposed      -> SprintfArmPassesBufCountFmtInThatOrder
//                                          ("slot holds parameter `Arg 1`, want `Arg 0`")
//   sprintf _Options LEGACY_NULLTERM->0 -> the same test ("Const is 0, want 1")
//   sscanf _BufferCount -> 0            -> SscanfArmUsesTheScanfCoreWithZeroOptions
//                                          ("Const is 0, want -1")
//   printf _Locale nullP() -> u64c(0)   -> PrintfArmForwardsStdoutFmtAndApByPosition
//                                          ("Const core is #9, want #27")
//   kIobStdout 1 -> 2                   -> the same test ("Const is 2, want 1")
//   printf forwards NULL as _Stream     -> the same test, on the CHAIN — the accessor
//     while still calling the accessor      call is STILL emitted, so "the body calls
//                                           __acrt_iob_func" stays true; only "the
//                                           `_Stream` slot IS that call" catches it
//   fprintf `stream`/`fmt` transposed   -> FprintfArmForwardsItsStreamAndFmtByPosition
//   fprintf vaStart(2) -> vaStart(1)    -> that test + the all-six test's fprintf row
//   vfprintf `sig` -> `vsig`            -> VfprintfArmForwardsItsDeclaredApAndIsNotVariadic
//   vfprintf Arg 2 -> vaStart(2)        -> that test, on the `ap` slot AND both
//                                          zero-leaf assertions
//   sscanf re-pointed at the vsprintf   -> SscanfArmUsesTheScanfCoreWithZeroOptions,
//     core                                 which MISSES its core rather than matching
//                                          the wrong one, plus the all-six test
//   the sprintf missing-core path emits -> EveryPostCloneFailurePathLeavesTheModuleUntouched
//     the body and publishes the           (func count 2 vs 1, and the symbol DEFINED)
//     builder before returning false       while the fail-loud assertions stay green

namespace {

// Pre-minted shim symbols — the rows stdio.json tags `synthesize`. `sprintf` keeps id
// 10 (the id every test above already uses) so the two halves of this file describe
// one module rather than two conventions.
constexpr std::uint32_t kShimSprintf  = 10;
constexpr std::uint32_t kShimPrintf   = 11;
constexpr std::uint32_t kShimFprintf  = 12;
constexpr std::uint32_t kShimVfprintf = 13;
constexpr std::uint32_t kShimSscanf   = 14;
// TF-C119. The family's structural outlier: THREE named args, a non-pointer among
// them, and the only recipe whose body is more than one block.
constexpr std::uint32_t kShimSnprintf = 15;

// The UCRT cores + the stdin/stdout/stderr accessor, as ORDINARY descriptor imports.
constexpr std::uint32_t kCoreVsprintf = 20;
constexpr std::uint32_t kCoreVfprintf = 21;
constexpr std::uint32_t kCoreVsscanf  = 22;
constexpr std::uint32_t kCoreAcrtIob  = 23;

// The UCRT `_Options` bits, restated from corecrt_stdio_config.h. `sprintf` passes
// LEGACY_VSPRINTF_NULL_TERMINATION (bit 0); every other arm passes ZERO — and on the
// scanf side that is load-bearing rather than incidental, because bit 0 there is
// SECURECRT, which turns `__stdio_common_vsscanf` into `sscanf_s` and makes every `%s`
// consume an EXTRA buffer-size argument out of `ap`. Wrong bits corrupt the argument
// stream or the NUL handling; none of them diagnoses.
constexpr std::int64_t kOptNone                   = 0;
constexpr std::int64_t kOptLegacyVsprintfNullTerm = 1;
// UCRT's UNBOUNDED sentinel, `(size_t)-1`. The pass builds it as `~0ull` narrowed into
// the pool's signed arm, so the stored literal is -1.
constexpr std::int64_t kBufferCountUnbounded = -1;
// TF-C119: `snprintf`'s bit — STANDARD_SNPRINTF_BEHAVIOR (bit 1,
// corecrt_stdio_config.h:116). WITHOUT it the core is the pre-C99 `_snprintf`,
// which returns -1 on truncation where C99 requires the would-be length. It is the
// first shipped recipe to pass a nonzero bit that is NOT sprintf's legacy bit 0,
// which is precisely why a value hoisted out of the sprintf arm must red here.
constexpr std::int64_t kOptStandardSnprintfBehavior = 2;
// `__acrt_iob_func(0/1/2)` == stdin/stdout/stderr. `printf` IS `fprintf` to STDOUT, so
// this index is the difference between conforming output and output on the wrong
// stream — with nothing at any tier to notice.
constexpr std::int64_t kIobStdout = 1;

// All four helpers the six arms reach through, as ORDINARY descriptor imports (what
// stdio.json's pe rows produce). Distinct from `ucrtCoreImports()` above, which
// deliberately carries only the sprintf core.
std::vector<ExternImport> allStdioHelperImports() {
    auto make = [](std::uint32_t sym, char const* name) {
        ExternImport e;
        e.symbol      = SymbolId{sym};
        e.mangledName = name;
        e.libraryPath = "ucrtbase.dll";
        e.isData      = false;
        return e;
    };
    return {make(kCoreVsprintf, "__stdio_common_vsprintf"),
            make(kCoreVfprintf, "__stdio_common_vfprintf"),
            make(kCoreVsscanf, "__stdio_common_vsscanf"),
            make(kCoreAcrtIob, "__acrt_iob_func")};
}

// Each shim's C declaration, so the caller-side scaffold references it with its REAL
// arity and variadicity instead of a stub that only happens to verify. `vfprintf` is
// the odd one: three DECLARED parameters and NOT variadic (C 7.21.6.8).
// `sizeParamIndex` exists for `snprintf` alone: it is the only recipe with a named
// parameter that is NOT a pointer (`size_t n` at index 1). The scaffold must spell that
// honestly — the pass DEFINES the shim with a `u64` in that slot and the MirVerifier
// every test below runs compares the definition against this reference, so a `ptr` here
// would red on a SCAFFOLD defect and read as a pass defect.
constexpr std::uint32_t kNoSizeParam = 0xFFFFFFFFu;
struct ShimDecl {
    std::uint32_t symbol;
    std::uint32_t fixedArgc;
    bool          variadic;
    std::uint32_t sizeParamIndex = kNoSizeParam;
};
constexpr ShimDecl kDeclPrintf{kShimPrintf, 1, true};      // (fmt, ...)
constexpr ShimDecl kDeclFprintf{kShimFprintf, 2, true};    // (stream, fmt, ...)
constexpr ShimDecl kDeclVfprintf{kShimVfprintf, 3, false}; // (stream, fmt, ap)
constexpr ShimDecl kDeclSprintf{kShimSprintf, 2, true};    // (buf, fmt, ...)
constexpr ShimDecl kDeclSscanf{kShimSscanf, 2, true};      // (buf, fmt, ...)
constexpr ShimDecl kDeclSnprintf{kShimSnprintf, 3, true, 1};  // (buf, size_t n, fmt, ...)

// The caller-side scaffold: one `main` that references each shim through a GlobalAddr
// against a NOT-yet-defined callee — the shape the CST->HIR seam leaves behind for a
// `synthesize`-tagged descriptor row — and calls it at its declared arity.
Mir buildStdioCaller(TypeInterner& in, std::vector<ShimDecl> const& decls) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const u64 = in.primitive(TypeKind::U64);
    TypeId const pCh = in.pointer(in.primitive(TypeKind::Char));

    MirBuilder mb;
    mb.addFunction(in.fnSig({}, i32, CallConv::CcMS64), SymbolId{100});   // main
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    MirInstId const buf = mb.addInst(MirOpcode::Alloca, {}, pCh, 64);
    MirLiteralValue nLit;
    nLit.value = 16;
    nLit.core  = TypeKind::U64;
    MirInstId const nArg = mb.addConst(nLit, u64);
    for (ShimDecl const& d : decls) {
        std::vector<TypeId> params(d.fixedArgc, pCh);
        if (d.sizeParamIndex != kNoSizeParam) params[d.sizeParamIndex] = u64;
        TypeId const shimSig = in.fnSig(params, i32, CallConv::CcMS64, d.variadic);
        std::vector<MirInstId> call{mb.addGlobalAddr(SymbolId{d.symbol}, in.pointer(shimSig))};
        for (std::uint32_t i = 0; i < d.fixedArgc; ++i)
            call.push_back(i == d.sizeParamIndex ? nArg : buf);
        mb.addInst(MirOpcode::Call, call, i32);
    }
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

std::unordered_map<std::uint32_t, std::string> recipeMap(
    std::vector<std::pair<std::uint32_t, char const*>> const& rows) {
    std::unordered_map<std::uint32_t, std::string> m;
    for (auto const& [sym, name] : rows) m.emplace(sym, name);
    return m;
}

// Locate the synthesized forward BY ITS CALLEE SYMBOL. Selecting by callee (rather
// than "the first Call in the block") is load-bearing twice over: `printf`'s body holds
// TWO calls, and asking for a SPECIFIC core is what makes a mis-wired arm — `sscanf`
// routed to `__stdio_common_vsprintf`, which shares the very same six-parameter TypeId
// so the verifier stays silent — a MISS rather than a false match.
std::optional<MirInstId> coreCallOf(Mir const& mir, MirFuncId fn, std::uint32_t coreSymV) {
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < mir.blockInstCount(b); ++i) {
            MirInstId const id = mir.blockInstAt(b, i);
            if (mir.instOpcode(id) != MirOpcode::Call) continue;
            auto const ops = mir.instOperands(id);
            if (ops.empty()) continue;
            if (mir.instOpcode(ops[0]) != MirOpcode::GlobalAddr) continue;
            if (mir.globalAddrSymbol(ops[0]).v == coreSymV) return id;
        }
    }
    return std::nullopt;
}

std::uint32_t countOpcodeIn(Mir const& mir, MirFuncId fn, MirOpcode op) {
    std::uint32_t n = 0;
    for (std::uint32_t bi = 0; bi < mir.funcBlockCount(fn); ++bi) {
        MirBlockId const b = mir.funcBlockAt(fn, bi);
        for (std::uint32_t i = 0; i < mir.blockInstCount(b); ++i)
            if (mir.instOpcode(mir.blockInstAt(b, i)) == op) ++n;
    }
    return n;
}

// ── SINGLE-SLOT IDENTITY PROBES ─────────────────────────────────────────────────────
// Each answers "does THIS operand slot hold THAT value" and reports what it found
// instead. They check the opcode FIRST and only then read the opcode-specific payload,
// because `Mir::argIndex` / `Mir::constLiteralIndex` / `Mir::instPayload` abort LOUD on
// a wrong opcode: a plain `EXPECT_EQ(opcode, …)` followed by a payload read would take
// the whole test binary down on the first mismatch instead of failing one assertion and
// letting the remaining slots report too.

testing::AssertionResult isArg(Mir const& mir, MirInstId op, std::uint32_t ordinal) {
    if (mir.instOpcode(op) != MirOpcode::Arg)
        return testing::AssertionFailure()
               << "slot holds opcode #" << static_cast<int>(mir.instOpcode(op))
               << ", not the parameter `Arg " << ordinal << "`";
    if (mir.argIndex(op) != ordinal)
        return testing::AssertionFailure()
               << "slot holds parameter `Arg " << mir.argIndex(op) << "`, want `Arg "
               << ordinal << "` — the arm forwarded the WRONG PARAMETER into this slot "
                             "(a transposition; both are pointers, so no type check "
                             "anywhere can see it)";
    if (mir.argPosition(op) != ordinal)
        return testing::AssertionFailure()
               << "`Arg " << ordinal << "` records flat call-operand position "
               << mir.argPosition(op);
    return testing::AssertionSuccess();
}

testing::AssertionResult isIntConst(Mir const& mir, MirInstId op, std::int64_t want,
                                    TypeKind wantCore) {
    if (mir.instOpcode(op) != MirOpcode::Const)
        return testing::AssertionFailure()
               << "slot holds opcode #" << static_cast<int>(mir.instOpcode(op))
               << ", not a Const (want " << want << ")";
    MirLiteralValue const& lit = mir.literalValue(mir.constLiteralIndex(op));
    auto const* got = std::get_if<std::int64_t>(&lit.value);
    if (got == nullptr)
        return testing::AssertionFailure() << "Const does not carry an integer literal";
    if (*got != want)
        return testing::AssertionFailure()
               << "Const is " << *got << ", want " << want;
    if (lit.core != wantCore)
        return testing::AssertionFailure()
               << "Const core is #" << static_cast<int>(lit.core) << ", want #"
               << static_cast<int>(wantCore)
               << " — the core is what separates a zero `_Options` MASK from a NULL "
                  "`_Locale` POINTER, which are otherwise the same literal";
    return testing::AssertionSuccess();
}

// `_Locale = NULL` (the ambient locale) — a null POINTER const, not an integer zero.
testing::AssertionResult isNullLocale(Mir const& mir, MirInstId op) {
    return isIntConst(mir, op, 0, TypeKind::Ptr);
}

// The `ap` slot must hold the va LEAF ITSELF at the recipe's own named-arg count.
testing::AssertionResult isVaLeaf(Mir const& mir, MirInstId op, MirOpcode leaf,
                                  std::uint32_t payload) {
    if (mir.instOpcode(op) != leaf)
        return testing::AssertionFailure()
               << "the `ap` slot holds opcode #" << static_cast<int>(mir.instOpcode(op))
               << ", not the va leaf #" << static_cast<int>(leaf);
    if (mir.instPayload(op) != payload)
        return testing::AssertionFailure()
               << "va leaf payload is " << mir.instPayload(op) << ", want " << payload
               << " — the leaf is &home[namedArgCount], so a wrong count silently skips "
                  "or re-reads an argument slot";
    return testing::AssertionSuccess();
}

// `printf`'s STREAM operand must BE the `__acrt_iob_func(1)` call's result — the full
// chain, not "the body calls the accessor somewhere". A shim that fetched stdout and
// then handed the core a different value would satisfy the weaker form.
testing::AssertionResult isAcrtIobCall(Mir const& mir, MirInstId op, std::int64_t index) {
    if (mir.instOpcode(op) != MirOpcode::Call)
        return testing::AssertionFailure()
               << "the `_Stream` slot holds opcode #"
               << static_cast<int>(mir.instOpcode(op)) << ", not the accessor call";
    auto const ops = mir.instOperands(op);
    if (ops.size() != 2)
        return testing::AssertionFailure()
               << "__acrt_iob_func takes exactly one argument (callee + 1 == 2 operands), "
                  "found "
               << ops.size();
    if (mir.instOpcode(ops[0]) != MirOpcode::GlobalAddr ||
        mir.globalAddrSymbol(ops[0]).v != kCoreAcrtIob)
        return testing::AssertionFailure()
               << "the `_Stream` slot's call does not go to __acrt_iob_func";
    return isIntConst(mir, ops[1], index, TypeKind::U32);
}

VaListLayout const kWin64Layout = [] {
    VaListLayout l;
    l.strategy                 = VaListStrategy::HomogeneousPointer;
    l.namedArgSlotBytes        = 8;
    l.variadicUsesOverflowBase = false;
    return l;
}();

}  // namespace

// ── ARM 1/6 · printf ────────────────────────────────────────────────────────────────
//   int printf(char const* fmt, ...)
//     -> __stdio_common_vfprintf(0, __acrt_iob_func(1), fmt, NULL, ap)
//
// The STREAM operand is the interesting one: it is the only COMPUTED argument in the
// family, and its index selects the destination file. `__acrt_iob_func(2)` is stderr —
// which compiles, links, loads, runs, and puts every `printf` on the wrong stream. It
// is pinned here as the accessor call's RESULT with the accessor's own argument read.
TEST(SynthStdioShim, PrintfArmForwardsStdoutFmtAndApByPosition) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildStdioCaller(in, {kDeclPrintf});

    std::vector<ExternImport> const externs = allStdioHelperImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipeMap({{kShimPrintf, "printf"}}),
                                    kWin64Layout, externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kShimPrintf);
    ASSERT_TRUE(shim.has_value()) << "printf must be a synthesized definition";

    TypeId const shimSig = mir.funcSignature(*shim);
    EXPECT_TRUE(in.fnIsVariadic(shimSig)) << "printf is variadic";
    EXPECT_EQ(in.fnParams(shimSig).size(), 1u) << "printf's FIXED arity is (fmt)";

    auto const call = coreCallOf(mir, *shim, kCoreVfprintf);
    ASSERT_TRUE(call.has_value())
        << "printf must forward through the STREAM core __stdio_common_vfprintf";
    auto const ops = mir.instOperands(*call);
    ASSERT_EQ(ops.size(), 6u)
        << "callee + (_Options, _Stream, _Format, _Locale, _ArgList)";

    EXPECT_TRUE(isIntConst(mir, ops[1], kOptNone, TypeKind::U64))
        << "_Options must be 0: DSS links no legacy_stdio_definitions.lib, so plain 0 "
           "IS the modern C-conforming behavior";
    EXPECT_TRUE(isAcrtIobCall(mir, ops[2], kIobStdout))
        << "_Stream must be __acrt_iob_func(1) == stdout; index 2 is stderr and nothing "
           "downstream can tell the difference";
    EXPECT_TRUE(isArg(mir, ops[3], 0)) << "_Format is printf's Arg 0";
    EXPECT_TRUE(isNullLocale(mir, ops[4]));
    EXPECT_TRUE(isVaLeaf(mir, ops[5], MirOpcode::VaHomeArgAreaAddr, 1))
        << "_ArgList is the va leaf at printf's 1 named arg";

    EXPECT_FALSE(coreCallOf(mir, *shim, kCoreVsprintf).has_value())
        << "printf must not reach the BUFFERED core";
    EXPECT_FALSE(coreCallOf(mir, *shim, kCoreVsscanf).has_value());

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── ARM 2/6 · fprintf ───────────────────────────────────────────────────────────────
//   int fprintf(FILE* stream, char const* fmt, ...)
//     -> __stdio_common_vfprintf(0, stream, fmt, NULL, ap)
//
// Structurally `printf` minus the accessor: the stream arrives as `Arg 0`. `stream` and
// `fmt` are ADJACENT operands of the same core, and transposing them is a wrong-output
// miscompile no type check can see (`FILE*` is `ptr<void>` and `fmt` is `char*` at MIR,
// but the CALL is untyped against the core's FnSig) — so both are pinned by ordinal.
TEST(SynthStdioShim, FprintfArmForwardsItsStreamAndFmtByPosition) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildStdioCaller(in, {kDeclFprintf});

    std::vector<ExternImport> const externs = allStdioHelperImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipeMap({{kShimFprintf, "fprintf"}}),
                                    kWin64Layout, externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kShimFprintf);
    ASSERT_TRUE(shim.has_value()) << "fprintf must be a synthesized definition";

    TypeId const shimSig = mir.funcSignature(*shim);
    EXPECT_TRUE(in.fnIsVariadic(shimSig)) << "fprintf is variadic";
    EXPECT_EQ(in.fnParams(shimSig).size(), 2u) << "fprintf's FIXED arity is (stream, fmt)";

    auto const call = coreCallOf(mir, *shim, kCoreVfprintf);
    ASSERT_TRUE(call.has_value());
    auto const ops = mir.instOperands(*call);
    ASSERT_EQ(ops.size(), 6u)
        << "callee + (_Options, _Stream, _Format, _Locale, _ArgList)";

    EXPECT_TRUE(isIntConst(mir, ops[1], kOptNone, TypeKind::U64));
    EXPECT_TRUE(isArg(mir, ops[2], 0)) << "_Stream is fprintf's Arg 0";
    EXPECT_TRUE(isArg(mir, ops[3], 1)) << "_Format is fprintf's Arg 1";
    EXPECT_TRUE(isNullLocale(mir, ops[4]));
    EXPECT_TRUE(isVaLeaf(mir, ops[5], MirOpcode::VaHomeArgAreaAddr, 2))
        << "_ArgList is the va leaf at fprintf's 2 named args";

    // fprintf takes its stream as an ARGUMENT — it must never fetch one.
    EXPECT_FALSE(coreCallOf(mir, *shim, kCoreAcrtIob).has_value())
        << "fprintf writes to the CALLER's stream; calling __acrt_iob_func here would "
           "pin output to a fixed file regardless of the argument";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── ARM 3/6 · vfprintf — THE ONE THAT IS NOT VARIADIC ───────────────────────────────
//   int vfprintf(FILE* stream, char const* fmt, va_list ap)
//     -> __stdio_common_vfprintf(0, stream, fmt, NULL, ap)
//
// C 7.21.6.8 gives `vfprintf` a DECLARED `va_list ap` parameter that already points at
// the CALLER's first unnamed argument, so the shim forwards `Arg 2` verbatim. Two
// things must therefore be true here and nowhere else in the family, and BOTH are
// silent if they regress:
//   * NO `Va*ArgAreaAddr` leaf. One would re-derive `ap` from THIS frame — which has no
//     varargs at all — so `ap` would point at whatever follows the shim's own named
//     args instead of at the caller's list.
//   * a NON-variadic signature. The va leaf's presence is `lir_callconv`'s
//     prologue-spill signal, and a `vsig` here would additionally declare a variadic
//     frame nothing ever fills.
TEST(SynthStdioShim, VfprintfArmForwardsItsDeclaredApAndIsNotVariadic) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildStdioCaller(in, {kDeclVfprintf});

    std::vector<ExternImport> const externs = allStdioHelperImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipeMap({{kShimVfprintf, "vfprintf"}}),
                                    kWin64Layout, externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kShimVfprintf);
    ASSERT_TRUE(shim.has_value()) << "vfprintf must be a synthesized definition";

    TypeId const shimSig = mir.funcSignature(*shim);
    EXPECT_FALSE(in.fnIsVariadic(shimSig))
        << "★ vfprintf is NOT variadic (C 7.21.6.8) — `ap` is a declared parameter. A "
           "variadic signature here hands lir_callconv a prologue-spill signal for a "
           "function that receives no varargs";
    EXPECT_EQ(in.fnParams(shimSig).size(), 3u)
        << "vfprintf's arity is (stream, fmt, ap) — all three DECLARED";

    auto const call = coreCallOf(mir, *shim, kCoreVfprintf);
    ASSERT_TRUE(call.has_value());
    auto const ops = mir.instOperands(*call);
    ASSERT_EQ(ops.size(), 6u)
        << "callee + (_Options, _Stream, _Format, _Locale, _ArgList)";

    EXPECT_TRUE(isIntConst(mir, ops[1], kOptNone, TypeKind::U64));
    EXPECT_TRUE(isArg(mir, ops[2], 0)) << "_Stream is vfprintf's Arg 0";
    EXPECT_TRUE(isArg(mir, ops[3], 1)) << "_Format is vfprintf's Arg 1";
    EXPECT_TRUE(isNullLocale(mir, ops[4]));
    EXPECT_TRUE(isArg(mir, ops[5], 2))
        << "★ _ArgList must be the DECLARED `Arg 2`, forwarded verbatim — not a leaf, "
           "and not some other parameter";

    EXPECT_EQ(countOpcodeIn(mir, *shim, MirOpcode::VaHomeArgAreaAddr), 0u)
        << "★ vfprintf must emit NO va leaf: one would re-derive `ap` from a frame with "
           "no varargs in it";
    EXPECT_EQ(countOpcodeIn(mir, *shim, MirOpcode::VaOverflowArgAreaAddr), 0u)
        << "neither leaf, on either base";
    EXPECT_FALSE(coreCallOf(mir, *shim, kCoreAcrtIob).has_value())
        << "vfprintf writes to the CALLER's stream";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── ARM 4/6 · sprintf ───────────────────────────────────────────────────────────────
//   int sprintf(char* buf, char const* fmt, ...)
//     -> __stdio_common_vsprintf(LEGACY_VSPRINTF_NULL_TERMINATION, buf, (size_t)-1,
//                                fmt, NULL, ap)
//
// ★ THE TRANSPOSITION THIS TEST WAS WRITTEN FOR. `buf` and `fmt` are BOTH `char*`, they
// sit two operands apart in the same call, and swapping them makes the shim format the
// destination buffer as a control string and write the result over the format string.
// Measured (TF-C112): that swap passed every pre-existing assertion in both stdio test
// binaries and the MirVerifier, because the only thing that distinguishes the two
// operands is their POSITION.
TEST(SynthStdioShim, SprintfArmPassesBufCountFmtInThatOrder) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildStdioCaller(in, {kDeclSprintf});

    std::vector<ExternImport> const externs = allStdioHelperImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipeMap({{kShimSprintf, "sprintf"}}),
                                    kWin64Layout, externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kShimSprintf);
    ASSERT_TRUE(shim.has_value());

    TypeId const shimSig = mir.funcSignature(*shim);
    EXPECT_TRUE(in.fnIsVariadic(shimSig));
    EXPECT_EQ(in.fnParams(shimSig).size(), 2u) << "sprintf's FIXED arity is (buf, fmt)";

    auto const call = coreCallOf(mir, *shim, kCoreVsprintf);
    ASSERT_TRUE(call.has_value())
        << "sprintf must forward through the BUFFERED core __stdio_common_vsprintf";
    auto const ops = mir.instOperands(*call);
    ASSERT_EQ(ops.size(), 7u)
        << "callee + (_Options, _Buffer, _BufferCount, _Format, _Locale, _ArgList)";

    EXPECT_TRUE(isIntConst(mir, ops[1], kOptLegacyVsprintfNullTerm, TypeKind::U64))
        << "sprintf pairs LEGACY_VSPRINTF_NULL_TERMINATION (bit 0) with the unbounded "
           "count; dropping it changes the core's NUL handling SILENTLY";
    EXPECT_TRUE(isArg(mir, ops[2], 0))
        << "★ _Buffer is sprintf's Arg 0. If this reads `Arg 1`, the arm transposed buf "
           "and fmt: the shim formats the destination as a control string and writes "
           "the result over the format string — both are char*, so nothing else sees it";
    EXPECT_TRUE(isIntConst(mir, ops[3], kBufferCountUnbounded, TypeKind::U64))
        << "_BufferCount is UCRT's (size_t)-1 UNBOUNDED sentinel — sprintf has no limit";
    EXPECT_TRUE(isArg(mir, ops[4], 1))
        << "★ _Format is sprintf's Arg 1 — the other half of the transposition pair";
    EXPECT_TRUE(isNullLocale(mir, ops[5]));
    EXPECT_TRUE(isVaLeaf(mir, ops[6], MirOpcode::VaHomeArgAreaAddr, 2));

    EXPECT_FALSE(coreCallOf(mir, *shim, kCoreVsscanf).has_value())
        << "sprintf must not reach the SCANF core — the two share one six-parameter "
           "TypeId, so a swap is invisible to the verifier";
    EXPECT_FALSE(coreCallOf(mir, *shim, kCoreAcrtIob).has_value());

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── ARM 5/6 · sscanf ────────────────────────────────────────────────────────────────
//   int sscanf(char const* buf, char const* fmt, ...)
//     -> __stdio_common_vsscanf(0, buf, (size_t)-1, fmt, NULL, ap)
//
// ★ THE MIS-WIRE THIS TEST WAS WRITTEN FOR. `__stdio_common_vsscanf` and
// `__stdio_common_vsprintf` take the SAME six parameters, so the pass gives them ONE
// shared TypeId — which means routing `sscanf` to the printf core is type-correct at
// every tier and caught by nothing. The callee symbol is therefore asserted POSITIVELY
// (the call must go to the scanf core) and the printf core is asserted ABSENT.
//
// `_Options` is the second load-bearing operand: bit 0 here is SECURECRT, which turns
// the core into `sscanf_s` — every `%s` then consumes an EXTRA buffer-size argument out
// of `ap`, silently desynchronising the whole argument stream.
TEST(SynthStdioShim, SscanfArmUsesTheScanfCoreWithZeroOptions) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildStdioCaller(in, {kDeclSscanf});

    std::vector<ExternImport> const externs = allStdioHelperImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipeMap({{kShimSscanf, "sscanf"}}),
                                    kWin64Layout, externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    auto const shim = findFuncBySymbol(mir, kShimSscanf);
    ASSERT_TRUE(shim.has_value());

    TypeId const shimSig = mir.funcSignature(*shim);
    EXPECT_TRUE(in.fnIsVariadic(shimSig));
    EXPECT_EQ(in.fnParams(shimSig).size(), 2u) << "sscanf's FIXED arity is (buf, fmt)";

    auto const call = coreCallOf(mir, *shim, kCoreVsscanf);
    ASSERT_TRUE(call.has_value())
        << "★ sscanf MUST forward through __stdio_common_vsscanf. Routing it to "
           "__stdio_common_vsprintf is type-identical (one shared TypeId) and would "
           "make the shim FORMAT into the caller's read-only source string";
    EXPECT_FALSE(coreCallOf(mir, *shim, kCoreVsprintf).has_value())
        << "★ and it must NOT reach the printf core at all";

    auto const ops = mir.instOperands(*call);
    ASSERT_EQ(ops.size(), 7u)
        << "callee + (_Options, _Buffer, _BufferCount, _Format, _Locale, _ArgList)";

    EXPECT_TRUE(isIntConst(mir, ops[1], kOptNone, TypeKind::U64))
        << "★ _Options MUST be 0. Bit 0 is SECURECRT (`sscanf_s`: every %s eats an extra "
           "buffer-size argument out of `ap`) and bit 1 is LEGACY_WIDE_SPECIFIERS — both "
           "corrupt argument consumption rather than diagnose";
    EXPECT_TRUE(isArg(mir, ops[2], 0)) << "_Buffer is sscanf's Arg 0 (the source string)";
    EXPECT_TRUE(isIntConst(mir, ops[3], kBufferCountUnbounded, TypeKind::U64))
        << "_BufferCount is the UNBOUNDED sentinel — a sscanf source is NUL-terminated";
    EXPECT_TRUE(isArg(mir, ops[4], 1)) << "_Format is sscanf's Arg 1";
    EXPECT_TRUE(isNullLocale(mir, ops[5]));
    EXPECT_TRUE(isVaLeaf(mir, ops[6], MirOpcode::VaHomeArgAreaAddr, 2))
        << "_ArgList is the va leaf at sscanf's 2 named args";

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── ALL SIX IN ONE PASS ─────────────────────────────────────────────────────────────
//
// The arms above each run alone, which cannot catch a value that is right per-recipe
// but shared across them. Synthesizing the whole family in ONE invocation is what makes
// a hoisted constant fail: a hardcoded named-arg count reds on whichever recipe does
// not have it, and a core resolved once and reused reds on whichever arm needs the
// other one. It also pins the emission ORDER contract (sorted by SymbolId — the
// determinism the pass sorts for), and that `main` survives the rebuild.
//
// ★ TF-C119 — `snprintf` MAKES THIS FIXTURE COVER SOMETHING NO SINGLE-RECIPE ONE CAN,
// and that is why it was extended rather than left at five. `snprintf` is the ONLY arm
// whose body is more than one block (the `r < 0 ? -1 : r` clamp:
// synth_stdio_shim.cpp:489-506) and therefore the only one that leaves the builder
// sitting in a NON-ENTRY block when the emission loop moves on to the next recipe. The
// pass stamps structural markers module-wide with ONE `rederiveStructCfMarkers` after
// `finish()` — so the MIXED single-/multi-block module is the shape it must survive,
// and the shape the real pe64 build produces on every `#include <stdio.h>`. Before this
// extension that mixed case was exercised by no fixture at all: the five-arm module was
// uniformly single-block, and the one multi-block module was a one-recipe fixture in
// tests/mir/test_synth_stdio_shim_valist.cpp. The marker assertions below (and the
// MirVerifier, which RECOMPUTES the derivation and compares stored == derived per
// reachable block) are what makes that coverage real rather than incidental.
TEST(SynthStdioShim, AllSixArmsSynthesizeTogetherWithPerRecipeValues) {
    TypeInterner in{CompilationUnitId{1}};
    Mir mir = buildStdioCaller(
        in, {kDeclSprintf, kDeclPrintf, kDeclFprintf, kDeclVfprintf, kDeclSscanf,
             kDeclSnprintf});

    std::vector<ExternImport> const externs = allStdioHelperImports();
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in,
                                    recipeMap({{kShimSprintf, "sprintf"},
                                               {kShimPrintf, "printf"},
                                               {kShimFprintf, "fprintf"},
                                               {kShimVfprintf, "vfprintf"},
                                               {kShimSscanf, "sscanf"},
                                               {kShimSnprintf, "snprintf"}}),
                                    kWin64Layout, externs, rep));
    ASSERT_FALSE(rep.hasErrors());

    EXPECT_EQ(mir.moduleFuncCount(), 7u) << "main + six shims";
    EXPECT_TRUE(findFuncBySymbol(mir, 100u).has_value()) << "main must survive the rebuild";

    // Emission order is SORTED BY SymbolId — the pass sorts precisely because
    // `unordered_map` iteration is not stable and a shifting function order would make
    // the binary non-reproducible. main is the clone, so the shims start at index 1.
    ASSERT_EQ(mir.moduleFuncCount(), 7u);
    std::vector<std::uint32_t> const wantOrder{100,           kShimSprintf,  kShimPrintf,
                                               kShimFprintf,  kShimVfprintf, kShimSscanf,
                                               kShimSnprintf};
    for (std::uint32_t i = 0; i < wantOrder.size(); ++i)
        EXPECT_EQ(mir.funcSymbol(mir.funcAt(i)).v, wantOrder[i])
            << "function #" << i << ": shim emission must be sorted by SymbolId so the "
                                   "output is byte-reproducible";

    // Per-recipe named-arg counts, all produced by the SAME invocation: printf has one
    // fixed parameter, snprintf has three, and the rest have two — so any hoisted
    // constant reds here. snprintf's `3` is the one a copy-paste gets wrong: it is the
    // only value in this column that no sibling shares.
    struct LeafCase { std::uint32_t shim; std::uint32_t core; std::size_t apSlot;
                      std::uint32_t named; };
    for (LeafCase const& c : std::vector<LeafCase>{
             {kShimPrintf, kCoreVfprintf, 5, 1},
             {kShimFprintf, kCoreVfprintf, 5, 2},
             {kShimSprintf, kCoreVsprintf, 6, 2},
             {kShimSscanf, kCoreVsscanf, 6, 2},
             {kShimSnprintf, kCoreVsprintf, 6, 3}}) {
        SCOPED_TRACE(testing::Message() << "shim symbol " << c.shim);
        auto const fn = findFuncBySymbol(mir, c.shim);
        ASSERT_TRUE(fn.has_value());
        auto const call = coreCallOf(mir, *fn, c.core);
        ASSERT_TRUE(call.has_value()) << "each arm must reach ITS OWN core";
        auto const ops = mir.instOperands(*call);
        ASSERT_EQ(ops.size(), c.apSlot + 1);
        EXPECT_TRUE(isVaLeaf(mir, ops[c.apSlot], MirOpcode::VaHomeArgAreaAddr, c.named));
    }

    // ★ snprintf and sprintf SHARE `__stdio_common_vsprintf`, so the two arms sit one
    // constant apart in the same call shape — pinned here in the SAME module so a value
    // hoisted out of either arm reds. These are the two operands that carry snprintf's
    // entire meaning, and both faults are silent (see the single-arm test in
    // tests/mir/test_synth_stdio_shim_valist.cpp for what each one does at runtime).
    {
        auto const sn = findFuncBySymbol(mir, kShimSnprintf);
        auto const sp = findFuncBySymbol(mir, kShimSprintf);
        ASSERT_TRUE(sn.has_value() && sp.has_value());
        auto const snCall = coreCallOf(mir, *sn, kCoreVsprintf);
        auto const spCall = coreCallOf(mir, *sp, kCoreVsprintf);
        ASSERT_TRUE(snCall.has_value() && spCall.has_value());
        auto const snOps = mir.instOperands(*snCall);
        auto const spOps = mir.instOperands(*spCall);
        ASSERT_EQ(snOps.size(), 7u);
        ASSERT_EQ(spOps.size(), 7u);
        EXPECT_TRUE(isIntConst(mir, snOps[1], kOptStandardSnprintfBehavior, TypeKind::U64))
            << "snprintf's _Options is bit 1 (STANDARD_SNPRINTF_BEHAVIOR)";
        EXPECT_TRUE(isIntConst(mir, spOps[1], kOptLegacyVsprintfNullTerm, TypeKind::U64))
            << "…and sprintf's, in the SAME module, is still bit 0 — one _Options "
               "hoisted across both arms reds on whichever it does not fit";
        EXPECT_TRUE(isArg(mir, snOps[3], 1))
            << "snprintf's _BufferCount is the caller's REAL n (`Arg 1`)";
        EXPECT_TRUE(isIntConst(mir, spOps[3], kBufferCountUnbounded, TypeKind::U64))
            << "…while sprintf's, in the same module, is the (size_t)-1 sentinel";
    }

    // …and vfprintf, in the same module, still emits no leaf and forwards its parameter.
    auto const vf = findFuncBySymbol(mir, kShimVfprintf);
    ASSERT_TRUE(vf.has_value());
    EXPECT_EQ(countOpcodeIn(mir, *vf, MirOpcode::VaHomeArgAreaAddr), 0u);
    EXPECT_FALSE(in.fnIsVariadic(mir.funcSignature(*vf)));
    auto const vfCall = coreCallOf(mir, *vf, kCoreVfprintf);
    ASSERT_TRUE(vfCall.has_value());
    ASSERT_EQ(mir.instOperands(*vfCall).size(), 6u);
    EXPECT_TRUE(isArg(mir, mir.instOperands(*vfCall)[5], 2));

    // Exactly ONE body fetches stdout, and it is printf's.
    for (std::uint32_t sym : {kShimFprintf, kShimVfprintf, kShimSprintf, kShimSscanf,
                              kShimSnprintf})
        EXPECT_FALSE(coreCallOf(mir, *findFuncBySymbol(mir, sym), kCoreAcrtIob).has_value())
            << "only printf has a fixed destination stream; symbol " << sym;
    EXPECT_TRUE(
        coreCallOf(mir, *findFuncBySymbol(mir, kShimPrintf), kCoreAcrtIob).has_value());

    // ── THE MIXED-MODULE MARKER REDERIVE ────────────────────────────────────────────
    // One `rederiveStructCfMarkers` runs over the WHOLE module after `finish()`, and
    // this module is mixed: five one-block bodies plus `main`, and one three-block body.
    // Markers are a pure function of the CFG (mir_struct_markers.hpp), so the derivation
    // must not be perturbed by a neighbouring function's shape — a rederive that leaked
    // the multi-block arm's state, or that ran per-function against the wrong block
    // range, shows up HERE and in the verifier below and nowhere else in the suite.
    for (std::uint32_t sym : {100u, kShimSprintf, kShimPrintf, kShimFprintf,
                              kShimVfprintf, kShimSscanf}) {
        SCOPED_TRACE(testing::Message() << "single-block function " << sym);
        auto const fn = findFuncBySymbol(mir, sym);
        ASSERT_TRUE(fn.has_value());
        ASSERT_EQ(mir.funcBlockCount(*fn), 1u)
            << "every arm but snprintf is a single straight-line block";
        EXPECT_EQ(mir.blockMarker(mir.funcBlockAt(*fn, 0)), StructCfMarker::EntryBlock);
    }
    {
        auto const sn = findFuncBySymbol(mir, kShimSnprintf);
        ASSERT_TRUE(sn.has_value());
        ASSERT_EQ(mir.funcBlockCount(*sn), 3u)
            << "★ snprintf keeps its `r < 0 ? -1 : r` clamp when synthesized ALONGSIDE "
               "five single-block siblings — collapsing it to a bare `return r` drops "
               "the UCRT header's normalization";
        EXPECT_EQ(mir.blockMarker(mir.funcBlockAt(*sn, 0)), StructCfMarker::EntryBlock);
        // Both arms RETURN, so the if has no real join: rule 4 marks succs[0] IfThen and
        // succs[1] IfElse and derives no IfJoin (mir_struct_markers.hpp).
        EXPECT_EQ(mir.blockMarker(mir.funcBlockAt(*sn, 1)), StructCfMarker::IfThen);
        EXPECT_EQ(mir.blockMarker(mir.funcBlockAt(*sn, 2)), StructCfMarker::IfElse);
        EXPECT_EQ(countOpcodeIn(mir, *sn, MirOpcode::CondBr), 1u);
        EXPECT_EQ(countOpcodeIn(mir, *sn, MirOpcode::ICmpSlt), 1u)
            << "the clamp's predicate is a SIGNED less-than — an unsigned compare makes "
               "every negative result look large and positive";
    }

    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── THE "NO HALF-BUILT DEFINITION" PIN, MOVED ONTO THE PATHS THAT CAN VIOLATE IT ────
//
// ★ THIS PIN WAS ON THE WRONG PATHS. Both places it existed (the nullopt-layout and
// unimplemented-strategy refusals, in test_synth_stdio_shim_valist.cpp) return BEFORE
// `MirBuilder` is even constructed, so "the module is unchanged" there is guaranteed by
// statement order and asserts nothing. The paths that CAN violate it are the ones below:
// every one of them is reached AFTER the whole module has been cloned into the builder,
// with a function possibly open.
//
// That is not a hypothetical. src/mir/merge/synth_stdio_shim.cpp's own comment at the
// `coreSym` lambda records the shape it used to have — report the failure, hand back a
// default-constructed `SymbolId`, and let the arm use it as operand 0 of the `Call`,
// which `MirBuilder::checkSameModule_` waves through because an untagged id passes. The
// invariant the pass now holds, and this test states: on ANY failure the caller's `Mir`
// is left EXACTLY as it was found — no appended definition, no partial body, and the
// shim symbol still UNDEFINED so the next stage reports an undefined symbol rather than
// consuming a wrong-ABI body.
TEST(SynthStdioShim, EveryPostCloneFailurePathLeavesTheModuleUntouched) {
    struct Case {
        char const*   what;
        std::uint32_t shim;
        char const*   recipe;
        char const*   dropHelper;   // "" == drop nothing (the unknown-recipe backstop)
        ShimDecl      decl;         // the caller-side reference, at the arm's real arity
    };
    // One case per `return false` that sits AFTER the clone loop in the pass.
    std::vector<Case> const cases{
        {"printf without the stdout accessor", kShimPrintf, "printf", "__acrt_iob_func",
         kDeclPrintf},
        {"printf without the stream core", kShimPrintf, "printf", "__stdio_common_vfprintf",
         kDeclPrintf},
        {"fprintf without the stream core", kShimFprintf, "fprintf", "__stdio_common_vfprintf",
         kDeclFprintf},
        {"vfprintf without the stream core", kShimVfprintf, "vfprintf", "__stdio_common_vfprintf",
         kDeclVfprintf},
        {"sprintf without the buffered core", kShimSprintf, "sprintf", "__stdio_common_vsprintf",
         kDeclSprintf},
        {"sscanf without the scanf core", kShimSscanf, "sscanf", "__stdio_common_vsscanf",
         kDeclSscanf},
        // ★ the MULTI-BLOCK arm: its refusal is the one with the most module state to
        // leak (three blocks and an open builder), so it earns its own row rather than
        // riding sprintf's — both refuse over the SAME missing core.
        {"snprintf without the buffered core", kShimSnprintf, "snprintf",
         "__stdio_common_vsprintf", kDeclSnprintf},
        {"an id the recipe/switch vocabularies disagree on", kShimSprintf, "no_such_recipe", "",
         kDeclSprintf},
    };

    for (Case const& c : cases) {
        SCOPED_TRACE(c.what);
        TypeInterner in{CompilationUnitId{1}};
        Mir mir = buildStdioCaller(in, {c.decl});
        std::size_t const before = mir.moduleFuncCount();

        std::vector<ExternImport> externs;
        for (auto const& e : allStdioHelperImports())
            if (e.mangledName != c.dropHelper) externs.push_back(e);
        std::size_t const wantHelpers = (*c.dropHelper == '\0') ? std::size_t{4} : std::size_t{3};
        ASSERT_EQ(externs.size(), wantHelpers)
            << "the case must actually remove the helper it names — otherwise this row "
               "silently tests the happy path";

        DiagnosticReporter rep;
        EXPECT_FALSE(synthesizeStdioShim(mir, in, recipeMap({{c.shim, c.recipe}}),
                                         kWin64Layout, externs, rep))
            << "a missing helper / unknown recipe MUST fail loud";
        EXPECT_TRUE(rep.hasErrors()) << "the refusal must carry a real diagnostic";

        // ★ The pin, on the path where it means something.
        EXPECT_EQ(mir.moduleFuncCount(), before)
            << "a failure AFTER the module was cloned into the builder must not publish "
               "the builder: no definition may be appended";
        EXPECT_FALSE(findFuncBySymbol(mir, c.shim).has_value())
            << "the shim symbol must remain UNDEFINED — a half-built body would be "
               "consumed by the next stage as though synthesis had succeeded";
        EXPECT_TRUE(findFuncBySymbol(mir, 100u).has_value())
            << "and the caller's own functions must survive untouched";

        MirVerifier verifier{mir, &in};
        DiagnosticReporter vrep;
        EXPECT_TRUE(verifier.verify(vrep))
            << "the module handed back on the failure path must still verify";
    }
}
