// ★★ [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]] — the CALL-LOWERING
// half, pinned in both directions.
//
// THE DEFECT, stated once. An artifact whose LOADER may hand the process a
// different image's body for a name it also defines must not BRANCH to its own
// body for that name: if it does, that image answers one identifier differently
// from every other image in the process, silently. ✔MEASURED on both rails
// before the fix — a DSS-built ELF `.so` returned its OWN bodies (rc 4) where
// the gcc-built `.so` from identical source returned the executable's (rc 6);
// a DSS-built darwin dylib returned rc 1 where ld64's returned rc 2 (cycle P61,
// Apple Silicon, same shape).
//
// WHAT THE ENGINE MUST DECIDE, and why every arm below exists. The routing is
// (visibility × binding × the format's declaration), and getting any one of the
// three wrong is a defect in its own direction:
//   * route too little → the divergence above survives;
//   * route too much   → every ordinary call in the artifact pays a load
//                        through memory for nothing, and a `static` helper the
//                        loader cannot even see becomes uninlinable.
// So the four cells are asserted as a MATRIX over one module, not as four
// separate happy paths.
//
// THE REFERENCE READING behind the declared sets (✔MEASURED 2026-09-05, gcc
// 13.3.0 and clang 18.1.3, one object each):
//   .so   : weak callee → `call <wk@plt>`   strong global callee → `call <st@plt>`
//           `static` callee → DIRECT        hidden-visibility callee → DIRECT
//   exec  : weak AND strong self-calls DIRECT, `-pie` and `-no-pie` alike
// and, one ecosystem over, dyld's two-level namespace coalesces only WEAK
// definitions — a STRICT SUBSET. That subset relationship is the whole reason
// the set is DECLARED per format instead of hard-coded, and
// `ACalleeOutsideTheDeclaredSetKeepsItsDirectBranch` below is the arm that
// would go red if someone folded the two answers into one.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/extern_import.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "lir/lir.hpp"
#include "lir/lir_node.hpp"
#include "lir/lowering/mir_to_lir.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_opcode.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

using namespace dss;

namespace {

// The four callee flavours, in ONE module, each with a body and a call site in
// one caller. Only the callee's (binding, visibility) differs — everything else
// is held constant so a difference in the emitted call can have exactly one
// cause.
constexpr std::uint32_t kWeakSym   = 100u;   // Weak   / Default
constexpr std::uint32_t kGlobalSym = 101u;   // Global / Default
constexpr std::uint32_t kLocalSym  = 102u;   // Local  / Default  (`static`)
constexpr std::uint32_t kHiddenSym = 103u;   // Global / Hidden
constexpr std::uint32_t kCallerSym = 104u;

struct Routed {
    MirToLirResult result;
    // The SymbolId the caller's call to each callee ended up naming. The
    // routing rewrites exactly this operand and nothing else, so it is the
    // whole observable.
    std::uint32_t weakTarget   = 0;
    std::uint32_t globalTarget = 0;
    std::uint32_t localTarget  = 0;
    std::uint32_t hiddenTarget = 0;
};

[[nodiscard]] std::vector<DefinedSymbolName> definedNames() {
    return {DefinedSymbolName{SymbolId{kWeakSym},   "wk"},
            DefinedSymbolName{SymbolId{kGlobalSym}, "st"},
            DefinedSymbolName{SymbolId{kLocalSym},  "hid"},
            DefinedSymbolName{SymbolId{kHiddenSym}, "hv"},
            DefinedSymbolName{SymbolId{kCallerSym}, "from_lib"}};
}

[[nodiscard]] Routed
lowerFixture(TargetSchema const& sch, DiagnosticReporter& rep,
             std::vector<SymbolBinding> preemptible,
             // Repeat the weak callee's call site, to pin that ONE reference is
             // minted per definition however many sites reach it.
             bool twoWeakCallSites = false) {
    TypeInterner interner{CompilationUnitId{1}};
    auto const i32  = interner.primitive(TypeKind::I32);
    auto const ptrT = interner.pointer(interner.primitive(TypeKind::Void));
    auto const sig  = interner.fnSig(std::span<TypeId const>{}, i32,
                                     CallConv::CcSysV);

    MirBuilder mb;
    auto addLeaf = [&](std::uint32_t sym, SymbolBinding b, SymbolVisibility v) {
        mb.addFunction(sig, SymbolId{sym}, b, v);
        auto const bb = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        MirLiteralValue lv;
        lv.value = std::int64_t{0};
        lv.core  = TypeKind::I32;
        mb.addReturn(mb.addConst(lv, i32));
    };
    addLeaf(kWeakSym,   SymbolBinding::Weak,   SymbolVisibility::Default);
    addLeaf(kGlobalSym, SymbolBinding::Global, SymbolVisibility::Default);
    addLeaf(kLocalSym,  SymbolBinding::Local,  SymbolVisibility::Default);
    addLeaf(kHiddenSym, SymbolBinding::Global, SymbolVisibility::Hidden);

    mb.addFunction(sig, SymbolId{kCallerSym}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    {
        auto const bb = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        auto callTo = [&](std::uint32_t sym) {
            MirInstId const addr = mb.addGlobalAddr(SymbolId{sym}, ptrT);
            std::array<MirInstId, 1> ops{addr};
            return mb.addInst(MirOpcode::Call, ops, i32);
        };
        MirInstId acc = callTo(kWeakSym);
        if (twoWeakCallSites) {
            MirInstId const again = callTo(kWeakSym);
            std::array<MirInstId, 2> a{acc, again};
            acc = mb.addInst(MirOpcode::Add, a, i32);
        }
        for (std::uint32_t const s : {kGlobalSym, kLocalSym, kHiddenSym}) {
            MirInstId const r = callTo(s);
            std::array<MirInstId, 2> a{acc, r};
            acc = mb.addInst(MirOpcode::Add, a, i32);
        }
        mb.addReturn(acc);
    }
    Mir m = std::move(mb).finish();

    Routed out{
        lowerToLir(m, sch, interner, rep, /*externImports=*/{},
                   ExternCallDispatch::DirectPlt,
                   /*dataImportBinding=*/std::nullopt,
                   /*tlsAccess=*/std::nullopt,
                   /*sehScopes=*/{},
                   /*wideFloatSoftcallLibrary=*/std::nullopt,
                   /*externAddrBinding=*/std::nullopt,
                   /*charIsUnsigned=*/std::nullopt,
                   /*atomicsRuntime=*/std::nullopt,
                   /*indirectSlotBindings=*/{},
                   std::move(preemptible), definedNames()),
        0u, 0u, 0u, 0u};

    // Read back the SymbolRef each call names, IN SOURCE ORDER. The caller is
    // the 5th function; its calls appear in the order they were built.
    Lir const& lir = out.result.lir;
    if (lir.moduleFuncCount() < 5) return out;
    LirFuncId const caller = lir.funcAt(4);
    LirBlockId const entry = lir.funcEntry(caller);
    auto const callOp = sch.opcodeByMnemonic("call");
    std::vector<std::uint32_t> targets;
    for (std::uint32_t i = 0; i < lir.blockInstCount(entry); ++i) {
        LirInstId const id = lir.blockInstAt(entry, i);
        if (!callOp.has_value() || lir.instOpcode(id) != *callOp) continue;
        for (auto const& op : lir.instOperands(id)) {
            if (op.kind == LirOperandKind::SymbolRef) {
                targets.push_back(op.symbolV);
                break;
            }
        }
    }
    std::size_t const n = targets.size();
    // Source order: weak [, weak again], global, local, hidden.
    if (n >= 4) {
        out.weakTarget   = targets[0];
        out.globalTarget = targets[n - 3];
        out.localTarget  = targets[n - 2];
        out.hiddenTarget = targets[n - 1];
    }
    return out;
}

// How many loader-resolved references this lowering minted for a preemptible
// definition, and — the half that matters — what they SAY. A row that carried
// the right name with `isPreemptionReference` unset would be folded straight
// back onto the definition by the linker's cross-CU resolver, restoring the
// defect one tier below where it was fixed.
[[nodiscard]] std::vector<ExternImport const*>
preemptionRows(MirToLirResult const& r) {
    std::vector<ExternImport const*> out;
    for (auto const& e : r.externImports) {
        if (e.isPreemptionReference) out.push_back(&e);
    }
    return out;
}

} // namespace

// DIRECTION 1 — the defect itself. Under the ELF `.so` declaration
// (`["global", "weak"]`) BOTH default-visibility callees stop being branched to
// and start being asked of the loader.
// RED-ON-DISABLE: delete the `definitionIsPreemptible` rewrite in
// `mir_to_lir.cpp`'s `lowerCall` → both targets fall back to the definitions'
// own SymbolIds and the two EXPECT_NEs below fail by name.
TEST(PreemptibleDefinitionRouting,
     ADefaultVisibilityCalleeInTheDeclaredSetIsRoutedThroughTheLoader) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(
        **target, rep,
        {SymbolBinding::Global, SymbolBinding::Weak});
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    EXPECT_NE(out.weakTarget, kWeakSym)
        << "a WEAK definition may be coalesced away at load, so a call to it "
           "from inside the artifact must name the loader-resolved reference, "
           "not the local body";
    EXPECT_NE(out.globalTarget, kGlobalSym)
        << "on ELF a STRONG global definition in a shared object is "
           "interposable too — gcc 13.3.0 and clang 18.1.3 both emit "
           "`call <st@plt>` for exactly this source, and binding it locally is "
           "the same divergence one binding over";

    auto const rows = preemptionRows(out.result);
    ASSERT_EQ(rows.size(), 2u)
        << "one loader-resolved reference per preemptible definition reached";
    for (auto const* e : rows) {
        EXPECT_TRUE(e->libraryPath.empty())
            << "the name must be resolved from the loader's GLOBAL scope — the "
               "winner may be ANY image in the process, including this one — so "
               "naming a library would bind it to the wrong search";
        EXPECT_FALSE(e->isData);
        EXPECT_TRUE(e->mangledName == "wk" || e->mangledName == "st")
            << "unexpected minted name '" << e->mangledName << '\'';
    }
    EXPECT_NE(out.weakTarget, out.globalTarget)
        << "two definitions must not share one reference";
}

// DIRECTION 2 — the OVER-routing veto, and the arm that keeps the set DECLARED
// rather than hard-coded. Under the Mach-O dylib declaration (`["weak"]`) the
// STRONG global callee keeps its direct branch: dyld's two-level namespace
// binds a strong dylib definition locally, so routing it would buy an
// indirection and nothing else. A change that folded ELF's answer and Mach-O's
// into one rule passes DIRECTION 1 and fails here.
TEST(PreemptibleDefinitionRouting,
     ACalleeOutsideTheDeclaredSetKeepsItsDirectBranch) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(**target, rep, {SymbolBinding::Weak});
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    EXPECT_NE(out.weakTarget, kWeakSym)
        << "`weak` IS in this declaration, so it is still routed";
    EXPECT_EQ(out.globalTarget, kGlobalSym)
        << "`global` is NOT in this declaration — the call must stay a direct "
           "branch to the local body";
    EXPECT_EQ(preemptionRows(out.result).size(), 1u);
}

// DIRECTION 3 — VISIBILITY is a precondition the format cannot override, and it
// is checked against a declaration that lists BOTH bindings. A `static` callee
// and a hidden-visibility callee are in no image's dynamic export set, so no
// loader can see them and none can replace them — under ANY format. Both
// references agree in the very same object.
TEST(PreemptibleDefinitionRouting,
     AnUnexportedCalleeIsNeverRoutedHoweverWideTheDeclaration) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(
        **target, rep, {SymbolBinding::Global, SymbolBinding::Weak});
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();

    EXPECT_EQ(out.localTarget, kLocalSym)
        << "a `static` (Local) definition is module-private: no loader can see "
           "it, so routing it would be an indirection through a slot nothing "
           "can ever rebind";
    EXPECT_EQ(out.hiddenTarget, kHiddenSym)
        << "hidden visibility is exactly the source-level opt-out from "
           "interposition — gcc and clang leave such a callee DIRECT in the "
           "same `.so` whose default-visibility callees they PLT-route";
    EXPECT_EQ(preemptionRows(out.result).size(), 2u)
        << "only the two exported definitions may mint a reference";
}

// DIRECTION 4 — the BACK-COMPAT arm, stated rather than assumed. Every format
// that declares nothing (every relocatable object, every static library, every
// main-executable flavour — the executable is always its own winner) must lower
// BYTE-IDENTICALLY to the pre-change engine. This is the direction a reader
// would not think to check, and the one every shipped artifact depends on.
TEST(PreemptibleDefinitionRouting, NoDeclarationLeavesEveryCallDirect) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(**target, rep, /*preemptible=*/{});
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);

    EXPECT_EQ(out.weakTarget,   kWeakSym);
    EXPECT_EQ(out.globalTarget, kGlobalSym);
    EXPECT_EQ(out.localTarget,  kLocalSym);
    EXPECT_EQ(out.hiddenTarget, kHiddenSym);
    EXPECT_TRUE(preemptionRows(out.result).empty())
        << "an undeclared format must mint nothing at all";
}

// DIRECTION 5 — ONE reference per DEFINITION, not per call site. The loader
// resolves a name; two references to one name would mean two slots the loader
// fills with the same answer, and — worse — two rows the import dedup would
// have to reconcile.
TEST(PreemptibleDefinitionRouting,
     OneReferenceIsMintedPerDefinitionHoweverManyCallSites) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(**target, rep, {SymbolBinding::Weak},
                                  /*twoWeakCallSites=*/true);
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    auto const rows = preemptionRows(out.result);
    ASSERT_EQ(rows.size(), 1u)
        << "two call sites to one preemptible definition share one reference";
    EXPECT_EQ(rows[0]->mangledName, "wk");
}
