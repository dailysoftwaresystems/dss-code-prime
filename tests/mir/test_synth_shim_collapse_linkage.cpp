// THE SYNTHESIS-ONCE RULE, pinned at the passes that mint the bodies
// (P68 round 11, D-LK-SYNTHESIZED-LIBRARY-BODY-DEFINED-STRONG-IN-EVERY-UNIT).
//
// ═══ WHY THIS FILE EXISTS ═══════════════════════════════════════════════════
//
// On pe the UCRT exports none of printf / fprintf / sprintf / snprintf / sscanf /
// vfprintf, and on pe and Mach-O the C11 <threads.h> functions come from no
// platform image either, so DSS SYNTHESIZES each body into the unit that
// references it (`synthesizeStdioShim`, `synthesizeThreadsShim`). Every separately
// compiled unit that references a family gets its OWN copy — a program CU, a DSS
// static library's member, a shipped runtime unit (built as a nested
// single-member archive: runtime/platform/src/getopt.c prints through fprintf).
// Those copies were STRONG definitions, so linking any two of them was
// K_SymbolRedefinedAcrossUnits for every body of the family — ✔MEASURED
// 2026-09-24 with main's dsscp at R11-F5: a pe program including <getopt.h> and
// <stdio.h>, and a DSS static library calling printf linked into a program
// calling printf, each refused with the same six errors; sqlite's round-close
// testfixture link failed the same way.
//
// ★ THE RULE: a synthesized library body exists ONCE per linked image, however
// many units carry it — so every body is emitted with the linkage DSS's own
// vocabulary defines for exactly that, `SymbolBinding::Weak` ("several
// translation units may define this; the linker keeps one", symbol_attrs.hpp),
// which every writer already expresses (ELF STB_WEAK, Mach-O N_WEAK_DEF, COFF
// COMDAT IMAGE_COMDAT_SELECT_ANY with associative .pdata/.xdata) and the linker's
// all-weak arm collapses. And with `SymbolVisibility::Hidden`, because the body is
// INTERNAL to its image, as the UCRT's own header inline is: ✔MEASURED before
// the change, a DSS pe DLL whose code called the family exported all six bodies
// beside its own API; hidden, a body is in no export table, is never
// preemptible, and is not a DCE root.
//
// ★ WHAT IS PINNED, per family: every function the pass ADDS is Weak + Hidden —
// each recipe's shim AND the threads family's once-adapter (`call_once` hands
// InitOnceExecuteOnce a synthesized trampoline, which is a synthesized body like
// any other) — on every vehicle the family has; and the pass leaves every
// function it did NOT add exactly as it found it (the control: a pass that
// rewrote every binding in the module would satisfy the first half).
//
// RED-ON-DISABLE (✔MEASURED through ctest): `begin`'s binding back to `Global` in
// either pass reds that family's test here. The stdio pass's also reds both
// examples (examples/c/getopt_and_stdio_in_one_program,
// examples/c/staticlib_and_program_share_synthesized_bodies) on
// K_SymbolRedefinedAcrossUnits. The threads pass's reds ONLY this file: no format
// but an exec declares that family's vehicle, so no second unit can carry a
// threads body yet and no example can collide — this test is its whole pin.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/object_format_kind.hpp"   // LibrarySynthesis, LibrarySynthVehicle
#include "core/types/strong_ids.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"        // VaListLayout
#include "core/types/type_lattice/core_type.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/merge/synth_stdio_shim.hpp"
#include "mir/merge/synth_threads_shim.hpp"
#include "mir/mir.hpp"
#include "mir/mir_opcode.hpp"
#include "mir/mir_struct_markers.hpp"
#include "mir/mir_verifier.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace dss;

namespace {

constexpr std::uint32_t kMainSym = 100;

MirLiteralValue i32Lit(std::int64_t v) {
    MirLiteralValue lit;
    lit.value = v;
    lit.core  = TypeKind::I32;
    return lit;
}

// One callee a scaffold `main` references: its symbol and its C signature.
struct Callee {
    std::uint32_t        sym;
    std::vector<TypeId>  params;
    TypeId               ret;
    bool                 variadic = false;
};

// `main` calls each callee once through a GlobalAddr to a NOT-yet-defined symbol — the
// shape the CST->HIR seam leaves behind for a `synthesize`-tagged descriptor row.
Mir buildCaller(TypeInterner& in, CallConv cc, std::vector<Callee> const& callees) {
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder mb;
    mb.addFunction(in.fnSig({}, i32, cc), SymbolId{kMainSym});
    MirBlockId const e = mb.createBlock(StructCfMarker::EntryBlock);
    mb.beginBlock(e);
    for (Callee const& c : callees) {
        TypeId const sig = in.fnSig(c.params, c.ret, cc, c.variadic);
        std::vector<MirInstId> ops{mb.addGlobalAddr(SymbolId{c.sym}, in.pointer(sig))};
        for (TypeId const p : c.params) {
            if (p == in.primitive(TypeKind::I32)) {
                ops.push_back(mb.addConst(i32Lit(0), p));
            } else if (p == in.primitive(TypeKind::U64)) {
                MirLiteralValue lit;
                lit.value = 16;
                lit.core  = TypeKind::U64;
                ops.push_back(mb.addConst(lit, p));
            } else {
                ops.push_back(mb.addInst(MirOpcode::Alloca, {}, p, 64));
            }
        }
        mb.addInst(MirOpcode::Call, ops, c.ret);
    }
    mb.addReturn(mb.addConst(i32Lit(0), i32));
    return std::move(mb).finish();
}

// Every function the module holds, by symbol value.
std::unordered_set<std::uint32_t> funcSymbols(Mir const& mir) {
    std::unordered_set<std::uint32_t> out;
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i)
        out.insert(mir.funcSymbol(mir.funcAt(i)).v);
    return out;
}

// The linkage every synthesized body must carry — reported per function, so a failure
// names the body and what it got.
testing::AssertionResult isCollapsibleAndInternal(Mir const& mir, std::uint32_t symV) {
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v != symV) continue;
        if (mir.funcBinding(f) != SymbolBinding::Weak)
            return testing::AssertionFailure()
                   << "synthesized body {" << symV << "} has binding '"
                   << symbolBindingName(mir.funcBinding(f))
                   << "', want 'weak' — a strong body in every unit that references the "
                      "family is K_SymbolRedefinedAcrossUnits the moment two such units "
                      "link (a program + a static library, a program + a runtime unit)";
        if (mir.funcVisibility(f) != SymbolVisibility::Hidden)
            return testing::AssertionFailure()
                   << "synthesized body {" << symV << "} has visibility '"
                   << symbolVisibilityName(mir.funcVisibility(f))
                   << "', want 'hidden' — a default-visibility body is exported from a "
                      "shared library as if it were part of its API";
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "no function defines symbol {" << symV << "}";
}

// The UCRT cores `synthesizeStdioShim` forwards to — ordinary descriptor imports.
std::vector<ExternImport> stdioCoreImports() {
    auto make = [](std::uint32_t sym, char const* name) {
        ExternImport e;
        e.symbol      = SymbolId{sym};
        e.mangledName = name;
        e.libraryPath = "ucrtbase.dll";
        e.isData      = false;
        return e;
    };
    return {make(20, "__stdio_common_vsprintf"), make(21, "__stdio_common_vfprintf"),
            make(22, "__acrt_iob_func"), make(23, "__stdio_common_vsscanf")};
}

LibrarySynthesis win32Vehicle() {
    return LibrarySynthesis{LibrarySynthVehicle::Win32, RuntimeLibraryRole::SystemPrimitives,
                            "kernel32.dll"};
}
LibrarySynthesis pthreadVehicle() {
    return LibrarySynthesis{LibrarySynthVehicle::Pthread, RuntimeLibraryRole::CLibrary,
                            "/usr/lib/libSystem.B.dylib"};
}

}  // namespace

// ── THE STDIO FAMILY: all six recipes, the Win64 variadic model ──────────────
TEST(SynthShimCollapseLinkage, EveryStdioShimIsWeakAndHidden) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    TypeId const u64 = in.primitive(TypeKind::U64);
    TypeId const pCh = in.pointer(in.primitive(TypeKind::Char));
    std::vector<Callee> const callees{
        {10, {pCh, pCh}, i32, true},        // sprintf(buf, fmt, ...)
        {11, {pCh}, i32, true},             // printf(fmt, ...)
        {12, {pCh, pCh}, i32, true},        // fprintf(stream, fmt, ...)
        {13, {pCh, pCh, pCh}, i32, false},  // vfprintf(stream, fmt, ap)
        {14, {pCh, pCh}, i32, true},        // sscanf(str, fmt, ...)
        {15, {pCh, u64, pCh}, i32, true},   // snprintf(buf, n, fmt, ...)
    };
    Mir mir = buildCaller(in, CallConv::CcMS64, callees);
    std::unordered_map<std::uint32_t, std::string> const recipes{
        {10, "sprintf"}, {11, "printf"},  {12, "fprintf"},
        {13, "vfprintf"}, {14, "sscanf"}, {15, "snprintf"}};
    VaListLayout win64;
    win64.strategy                 = VaListStrategy::HomogeneousPointer;
    win64.namedArgSlotBytes        = 8;
    win64.variadicUsesOverflowBase = false;
    DiagnosticReporter rep;
    ASSERT_TRUE(synthesizeStdioShim(mir, in, recipes, win64, stdioCoreImports(), rep));
    ASSERT_FALSE(rep.hasErrors());

    for (auto const& [symV, recipe] : recipes)
        EXPECT_TRUE(isCollapsibleAndInternal(mir, symV)) << "the '" << recipe << "' shim";

    // The control: the caller the pass did not synthesize keeps the linkage it was built
    // with (MirBuilder's default for a plain definition: global, default visibility).
    for (std::uint32_t i = 0; i < mir.moduleFuncCount(); ++i) {
        MirFuncId const f = mir.funcAt(i);
        if (mir.funcSymbol(f).v != kMainSym) continue;
        EXPECT_EQ(mir.funcBinding(f), SymbolBinding::Global);
        EXPECT_EQ(mir.funcVisibility(f), SymbolVisibility::Default);
    }
    MirVerifier verifier{mir, &in};
    EXPECT_TRUE(verifier.verify(rep));
}

// ── THE THREADS FAMILY: a shim per vehicle, and the once-adapter ─────────────
//
// `call_once` is the recipe that makes the pass mint a body NO descriptor names — the
// adapter it hands InitOnceExecuteOnce (Win32) — so the assertion is over EVERY function
// the pass added, found by difference, not only over the recipe symbols. The pass mints
// that adapter whenever call_once is present, on BOTH vehicles (on pthread nothing names
// it and DCE drops it later), and a win32 call_once without it is the pass's own internal
// error, which the ASSERT on the pass's result catches — so the difference always holds
// it here (✔MEASURED: the Global mutant reds it as the fourth body on both vehicles).
TEST(SynthShimCollapseLinkage, EveryThreadsBodyIsWeakAndHiddenOnBothVehicles) {
    struct Vehicle {
        char const*             name;
        LibrarySynthesis        synthesis;
        CallConv                cc;
        CSymbolDecorationScheme scheme;
    };
    for (Vehicle const& v : {Vehicle{"win32", win32Vehicle(), CallConv::CcMS64,
                                     CSymbolDecorationScheme::None},
                             Vehicle{"pthread", pthreadVehicle(), CallConv::CcApple,
                                     CSymbolDecorationScheme::LeadingUnderscore}}) {
        SCOPED_TRACE(v.name);
        TypeInterner in{CompilationUnitId{1}};
        TypeId const i32 = in.primitive(TypeKind::I32);
        TypeId const pV  = in.pointer(in.primitive(TypeKind::Void));
        TypeId const vd  = in.primitive(TypeKind::Void);
        std::vector<Callee> const callees{
            {10, {pV, i32}, i32},   // mtx_init(mtx_t*, int)
            {11, {pV}, i32},        // mtx_lock(mtx_t*)
            {12, {pV, pV}, vd},     // call_once(once_flag*, void(*)(void))
        };
        Mir mir = buildCaller(in, v.cc, callees);
        std::unordered_set<std::uint32_t> const before = funcSymbols(mir);
        std::unordered_map<std::uint32_t, std::string> const recipes{
            {10, "mtx_init"}, {11, "mtx_lock"}, {12, "call_once"}};
        std::vector<ExternImport> externs;
        DiagnosticReporter rep;
        ASSERT_TRUE(synthesizeThreadsShim(mir, in, recipes, v.synthesis, v.scheme, externs, rep));
        ASSERT_FALSE(rep.hasErrors());

        std::size_t added = 0;
        for (std::uint32_t const symV : funcSymbols(mir)) {
            if (before.count(symV) != 0) continue;
            ++added;
            EXPECT_TRUE(isCollapsibleAndInternal(mir, symV));
        }
        EXPECT_GE(added, recipes.size())
            << "every recipe must have produced a body — fewer means the probe above "
               "checked less than it claims";
        for (auto const& [symV, recipe] : recipes)
            EXPECT_EQ(before.count(symV), 0u) << recipe << " was defined before the pass ran";
    }
}
