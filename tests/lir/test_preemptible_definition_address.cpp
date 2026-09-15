// ★★ [[D-MIR-DYLIB-SELF-CALL-BYPASSES-WEAK-COALESCING]] — the ADDRESS-LOWERING
// half, pinned in both directions. The CALL half is
// `test_preemptible_definition_routing`; this is its twin, and the two subjects
// are genuinely different rather than one rule applied twice.
//
// THE DEFECT, stated once. An artifact whose LOADER may hand the process a
// different image's body for a name it also defines must not materialize its own
// body's address for that name: if it does, `&w` evaluated inside the library and
// `&w` evaluated in the executable are two different pointers for one identifier,
// and C 6.2.2p2 gives one identifier one function/object across the whole
// program. A callback table, a registration, a `==` between function pointers —
// anything that crosses the image boundary — then answers wrong, silently.
//
// ★ IT CANNOT RIDE THE CALL FIX, AND THAT IS THE DESIGN CONSTRAINT. Under
// `direct-plt` the routed reference's VA is the PLT STUB: the right answer for a
// call and the wrong answer for an address. What an address needs is the CONTENT
// of the loader-filled slot, so the reference carries a SECOND symbol naming that
// slot (`ExternImport::addressSlotSymbol`) and the lowering emits lea-of-slot +
// deref. Under `indirect-slot` the reference's own VA already IS the slot and no
// second symbol is minted — the arm below that asserts this is what keeps the
// answer keyed on DECLARED vocabulary rather than on a format identity.
//
// THE REFERENCE READING behind the declared sets (✔MEASURED 2026-09-05, gcc
// 13.3.0 and clang 18.1.3, one object each, `-O1`):
//   .so   : weak fn → GOT load (`R_X86_64_GLOB_DAT`)   strong global fn → GOT load
//           weak DATA → GOT load                       strong global DATA → GOT load
//           `static` fn/data → bare `lea`              hidden fn/data → bare `lea`
//   exec  : ALL EIGHT a bare `lea`, `-pie` and `-no-pie` alike
// The `static`/hidden siblings sit in the SAME object as the routed ones, which is
// the control that makes "gcc loads a GOT slot" mean preemptibility rather than
// "this target routes every address through the GOT".
//
// ★ AND THE DATA HALF IS A MEASUREMENT, NOT A GENERALIZATION. The call half's
// channel was documented FUNCTIONS ONLY on the reasoning that a data object's
// address is "a separate reference kind with its own declared binding". The probe
// above refutes that: both references treat `&exported_data` in a `.so` exactly as
// they treat `&exported_function`, for the same reason and with the same
// relocation.

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

// EIGHT subjects in ONE module — the four binding/visibility flavours, once as a
// FUNCTION and once as a DATA global. Only (binding, visibility) and the kind
// differ; everything else is held constant so a difference in the emitted
// address has exactly one cause.
constexpr std::uint32_t kWeakFn   = 100u;   // Weak   / Default
constexpr std::uint32_t kGlobalFn = 101u;   // Global / Default
constexpr std::uint32_t kLocalFn  = 102u;   // Local  / Default  (`static`)
constexpr std::uint32_t kHiddenFn = 103u;   // Global / Hidden
constexpr std::uint32_t kWeakDt   = 110u;
constexpr std::uint32_t kGlobalDt = 111u;
constexpr std::uint32_t kLocalDt  = 112u;
constexpr std::uint32_t kHiddenDt = 113u;
constexpr std::uint32_t kSinkSym  = 120u;   // takes one address, so no lea folds
constexpr std::uint32_t kCallerSym = 121u;
// ★★ THE NAMELESS DEFINITION — `Global`/`Default` like the two routed data
// subjects above, and DELIBERATELY ABSENT from `definedNames()`. This is the
// EXACT shape `hir_to_mir.cpp` mints for a string-literal pool object and for a
// promoted float constant: the declared preemptible binding pair, on a SymbolId
// seeded past the whole semantic id space so no on-binary name exists for it.
constexpr std::uint32_t kAnonDt   = 130u;

// What the caller's address-take of each subject ended up naming, in the order
// the subjects are listed above. 0 = no `lea` was found for that subject.
struct Addressed {
    MirToLirResult              result;
    std::vector<std::uint32_t>  leaSymbols;   // in source order
    // Whether each `lea` is immediately followed by a LOAD of its own vreg —
    // i.e. whether the address came from a SLOT's content rather than the
    // symbol itself. Same order.
    std::vector<bool>           derefed;
};

[[nodiscard]] std::vector<DefinedSymbolName> definedNames() {
    return {DefinedSymbolName{SymbolId{kWeakFn},    "wk"},
            DefinedSymbolName{SymbolId{kGlobalFn},  "st"},
            DefinedSymbolName{SymbolId{kLocalFn},   "hid"},
            DefinedSymbolName{SymbolId{kHiddenFn},  "hv"},
            DefinedSymbolName{SymbolId{kWeakDt},    "wd"},
            DefinedSymbolName{SymbolId{kGlobalDt},  "sd"},
            DefinedSymbolName{SymbolId{kLocalDt},   "hidd"},
            DefinedSymbolName{SymbolId{kHiddenDt},  "hvd"},
            DefinedSymbolName{SymbolId{kSinkSym},   "sink"},
            DefinedSymbolName{SymbolId{kCallerSym}, "from_lib"}};
}

// The eight subjects, in the order every assertion below reads them.
[[nodiscard]] std::array<std::uint32_t, 8> subjects() {
    return {kWeakFn, kGlobalFn, kLocalFn, kHiddenFn,
            kWeakDt, kGlobalDt, kLocalDt, kHiddenDt};
}

[[nodiscard]] Addressed
lowerFixture(TargetSchema const& sch, DiagnosticReporter& rep,
             std::vector<SymbolBinding> preemptible,
             ExternCallDispatch dispatch = ExternCallDispatch::DirectPlt,
             // Repeat the weak function's address-take, to pin that ONE slot is
             // minted per definition however many sites reach it.
             bool twoWeakAddressSites = false,
             // Append a NINTH subject (index 8): a `Global`/`Default` data
             // definition with NO on-binary name — the compiler-synthesized
             // literal-pool shape. Off by default so every arm above keeps its
             // eight-subject reading unchanged.
             bool anonymousPoolGlobal = false) {
    TypeInterner interner{CompilationUnitId{1}};
    auto const i32  = interner.primitive(TypeKind::I32);
    auto const ptrT = interner.pointer(interner.primitive(TypeKind::Void));
    auto const sig  = interner.fnSig(std::span<TypeId const>{}, i32,
                                     CallConv::CcSysV);
    std::array<TypeId, 1> sinkArgs{ptrT};
    auto const sinkSig = interner.fnSig(std::span<TypeId const>{sinkArgs}, i32,
                                        CallConv::CcSysV);

    MirBuilder mb;
    auto addLeaf = [&](std::uint32_t sym, SymbolBinding b, SymbolVisibility v,
                       TypeId s) {
        mb.addFunction(s, SymbolId{sym}, b, v);
        auto const bb = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        MirLiteralValue lv;
        lv.value = std::int64_t{0};
        lv.core  = TypeKind::I32;
        mb.addReturn(mb.addConst(lv, i32));
    };
    addLeaf(kWeakFn,   SymbolBinding::Weak,   SymbolVisibility::Default, sig);
    addLeaf(kGlobalFn, SymbolBinding::Global, SymbolVisibility::Default, sig);
    addLeaf(kLocalFn,  SymbolBinding::Local,  SymbolVisibility::Default, sig);
    addLeaf(kHiddenFn, SymbolBinding::Global, SymbolVisibility::Hidden,  sig);
    // The sink: an ordinary module-private callee whose one PARAMETER holds each
    // address. A GlobalAddr at operand ≥ 1 is a call ARGUMENT, never the callee,
    // so no address-take here folds into a direct branch — which is exactly the
    // shape a `&f` value use has in C.
    addLeaf(kSinkSym,  SymbolBinding::Local,  SymbolVisibility::Default, sinkSig);

    auto addData = [&](std::uint32_t sym, SymbolBinding b, SymbolVisibility v) {
        mb.addGlobal(i32, SymbolId{sym}, /*initLiteralIndex=*/0u,
                     MirFuncId{}, b, v, /*isConst=*/false,
                     MirThreadStorage::Shared);
    };
    addData(kWeakDt,   SymbolBinding::Weak,   SymbolVisibility::Default);
    addData(kGlobalDt, SymbolBinding::Global, SymbolVisibility::Default);
    addData(kLocalDt,  SymbolBinding::Local,  SymbolVisibility::Default);
    addData(kHiddenDt, SymbolBinding::Global, SymbolVisibility::Hidden);
    if (anonymousPoolGlobal) {
        addData(kAnonDt, SymbolBinding::Global, SymbolVisibility::Default);
    }

    mb.addFunction(sig, SymbolId{kCallerSym}, SymbolBinding::Global,
                   SymbolVisibility::Default);
    {
        auto const bb = mb.createBlock(StructCfMarker::EntryBlock);
        mb.beginBlock(bb);
        auto addressOf = [&](std::uint32_t sym) {
            MirInstId const callee = mb.addGlobalAddr(SymbolId{kSinkSym}, ptrT);
            MirInstId const addr   = mb.addGlobalAddr(SymbolId{sym}, ptrT);
            std::array<MirInstId, 2> ops{callee, addr};
            return mb.addInst(MirOpcode::Call, ops, i32);
        };
        MirInstId acc = addressOf(kWeakFn);
        if (twoWeakAddressSites) {
            MirInstId const again = addressOf(kWeakFn);
            std::array<MirInstId, 2> a{acc, again};
            acc = mb.addInst(MirOpcode::Add, a, i32);
        }
        for (std::size_t i = 1; i < subjects().size(); ++i) {
            MirInstId const r = addressOf(subjects()[i]);
            std::array<MirInstId, 2> a{acc, r};
            acc = mb.addInst(MirOpcode::Add, a, i32);
        }
        if (anonymousPoolGlobal) {
            MirInstId const r = addressOf(kAnonDt);
            std::array<MirInstId, 2> a{acc, r};
            acc = mb.addInst(MirOpcode::Add, a, i32);
        }
        mb.addReturn(acc);
    }
    Mir m = std::move(mb).finish();

    Addressed out{
        lowerToLir(m, sch, interner, rep, /*externImports=*/{},
                   dispatch,
                   /*dataImportBinding=*/std::nullopt,
                   /*tlsAccess=*/std::nullopt,
                   /*sehScopes=*/{},
                   /*wideFloatSoftcallLibrary=*/std::nullopt,
                   /*externAddrBinding=*/std::nullopt,
                   /*charIsUnsigned=*/std::nullopt,
                   /*atomicsRuntime=*/std::nullopt,
                   /*indirectSlotBindings=*/{},
                   std::move(preemptible), definedNames()),
        {}, {}};

    // Read back every `lea` in the caller, IN SOURCE ORDER, and whether the very
    // next instruction LOADS the vreg it defined. The caller is the 6th function
    // (four leaves + the sink precede it).
    Lir const& lir = out.result.lir;
    if (lir.moduleFuncCount() < 6) return out;
    LirFuncId const caller = lir.funcAt(5);
    LirBlockId const entry = lir.funcEntry(caller);
    auto const leaOp = sch.opcodeByMnemonic("lea");
    if (!leaOp.has_value()) return out;
    std::uint32_t const n = lir.blockInstCount(entry);
    for (std::uint32_t i = 0; i < n; ++i) {
        LirInstId const id = lir.blockInstAt(entry, i);
        if (lir.instOpcode(id) != *leaOp) continue;
        std::uint32_t sym = 0;
        for (auto const& op : lir.instOperands(id)) {
            if (op.kind == LirOperandKind::SymbolRef) { sym = op.symbolV; break; }
        }
        if (sym == 0) continue;
        out.leaSymbols.push_back(sym);
        // The deref, if any, is the instruction immediately after: it reads the
        // lea's destination vreg as a memory BASE.
        bool deref = false;
        if (i + 1 < n) {
            LirInstId const nxt = lir.blockInstAt(entry, i + 1);
            auto const ops = lir.instOperands(nxt);
            bool sawBase = false;
            for (auto const& op : ops) {
                if (op.kind == LirOperandKind::MemBase) sawBase = true;
            }
            if (sawBase && !ops.empty()
                && ops[0].kind == LirOperandKind::Reg
                && ops[0].reg.id == lir.instResult(id).id
                && ops[0].reg.isPhysical == lir.instResult(id).isPhysical) {
                deref = true;
            }
        }
        out.derefed.push_back(deref);
    }
    return out;
}

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
// (`["global","weak"]`) all FOUR default-visibility subjects — weak and strong
// global, function and data alike — stop materializing the local body's address
// and start reading the loader-filled slot.
// RED-ON-DISABLE: delete the `symbolIsPreemptibleDefinition` arm in
// `mir_to_lir.cpp`'s `lowerGlobalAddr` → each of the four leas falls back to
// naming its own definition and the four EXPECT_NEs fail by name.
TEST(PreemptibleDefinitionAddress,
     ADefaultVisibilityDefinitionInTheDeclaredSetIsReadFromItsLoaderSlot) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(
        **target, rep, {SymbolBinding::Global, SymbolBinding::Weak});
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(out.leaSymbols.size(), 8u)
        << "one address materialization per subject, in source order";

    auto const subj = subjects();
    for (std::size_t i : {0u, 1u, 4u, 5u}) {   // weak fn, strong fn, weak dt, strong dt
        EXPECT_NE(out.leaSymbols[i], subj[i])
            << "subject #" << i << " is externally visible in a declared "
               "binding, so its address must come from the loader's slot and "
               "not from the local definition — gcc 13.3.0 and clang 18.1.3 "
               "both emit a GOT load for exactly this source";
        EXPECT_TRUE(out.derefed[i])
            << "subject #" << i << " named a slot but never dereferenced it — "
               "a pointer to the slot is not the object's address";
    }
    auto const rows = preemptionRows(out.result);
    ASSERT_EQ(rows.size(), 4u)
        << "one loader-resolved reference per preemptible definition reached";
    for (auto const* e : rows) {
        EXPECT_TRUE(e->libraryPath.empty())
            << "the name is resolved from the loader's GLOBAL scope — the "
               "winner may be ANY image in the process, including this one";
        EXPECT_TRUE(e->addressSlotSymbol.valid())
            << "'" << e->mangledName << "': under `direct-plt` the reference's "
               "own VA is a PLT stub, so the ADDRESS needs the second symbol";
        EXPECT_NE(e->addressSlotSymbol, e->symbol);
    }
}

// DIRECTION 2 — the OVER-routing veto, and the arm that keeps the set DECLARED
// rather than hard-coded. Under the Mach-O dylib declaration (`["weak"]`) the
// strong global function and the strong global datum keep their bare `lea`:
// dyld's two-level namespace binds a strong dylib definition locally. A change
// that folded ELF's answer and Mach-O's into one rule passes DIRECTION 1 and
// fails here.
TEST(PreemptibleDefinitionAddress,
     ADefinitionOutsideTheDeclaredSetKeepsItsDirectAddress) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(**target, rep, {SymbolBinding::Weak});
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    ASSERT_EQ(out.leaSymbols.size(), 8u);

    auto const subj = subjects();
    EXPECT_NE(out.leaSymbols[0], subj[0]) << "`weak` IS in this declaration";
    EXPECT_NE(out.leaSymbols[4], subj[4]) << "`weak` data likewise";
    EXPECT_EQ(out.leaSymbols[1], subj[1])
        << "`global` is NOT in this declaration — the address must stay the "
           "definition's own";
    EXPECT_EQ(out.leaSymbols[5], subj[5]);
    EXPECT_EQ(preemptionRows(out.result).size(), 2u);
}

// DIRECTION 3 — VISIBILITY is a precondition the format cannot override, checked
// against a declaration listing BOTH bindings. A `static` and a hidden-visibility
// definition are in no image's dynamic export set, so no loader can see them and
// none can replace them — under ANY format. Both references agree in the very
// same object, which is the control that gives DIRECTION 1 its meaning.
TEST(PreemptibleDefinitionAddress,
     AnUnexportedDefinitionIsNeverRoutedHoweverWideTheDeclaration) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(
        **target, rep, {SymbolBinding::Global, SymbolBinding::Weak});
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    ASSERT_EQ(out.leaSymbols.size(), 8u);

    auto const subj = subjects();
    for (std::size_t i : {2u, 3u, 6u, 7u}) {   // static fn/dt, hidden fn/dt
        EXPECT_EQ(out.leaSymbols[i], subj[i])
            << "subject #" << i << " is not in any dynamic export set, so "
               "routing its address would be an indirection through a slot "
               "nothing can ever rebind";
        EXPECT_FALSE(out.derefed[i]);
    }
}

// DIRECTION 4 — the BACK-COMPAT arm, and the named CONTROL every red-on-disable
// run on this subject reports beside its reds. Every format that declares
// nothing — every relocatable object, every static library, every main-executable
// flavour — must lower BYTE-IDENTICALLY to the pre-change engine. Asserted by
// EXACT positive SymbolIds through the same read path the reds use, so a mutant
// that breaks the READER cannot leave this arm green.
TEST(PreemptibleDefinitionAddress, NoDeclarationLeavesEveryAddressDirect) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(**target, rep, /*preemptible=*/{});
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u);
    ASSERT_EQ(out.leaSymbols.size(), 8u);

    auto const subj = subjects();
    for (std::size_t i = 0; i < subj.size(); ++i) {
        EXPECT_EQ(out.leaSymbols[i], subj[i]) << "subject #" << i;
        EXPECT_FALSE(out.derefed[i]) << "subject #" << i;
    }
    EXPECT_TRUE(preemptionRows(out.result).empty())
        << "an undeclaring format must mint nothing at all";
}

// DIRECTION 5 — ONE slot per DEFINITION, not per address site. The loader fills
// one slot for one name; two would be two answers to reconcile, which is the
// shape of the defect one tier down.
TEST(PreemptibleDefinitionAddress,
     OneSlotIsMintedPerDefinitionHoweverManyAddressSites) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(**target, rep, {SymbolBinding::Weak},
                                  ExternCallDispatch::DirectPlt,
                                  /*twoWeakAddressSites=*/true);
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    ASSERT_EQ(out.leaSymbols.size(), 9u) << "the weak function is addressed twice";
    auto const rows = preemptionRows(out.result);
    ASSERT_EQ(rows.size(), 2u) << "the weak function and the weak datum";
    EXPECT_EQ(out.leaSymbols[0], out.leaSymbols[1])
        << "two address sites for one definition share one slot symbol";
}

// DIRECTION 6 — the answer is keyed on DECLARED VOCABULARY, and this is the arm
// that proves it is not keyed on a format identity. Under an `indirect-slot`
// dispatch the reference's own VA already IS the slot the loader fills — that is
// what makes `call_indirect_via_extern` the correct call shape there — so the
// ADDRESS reads that same symbol and NO second symbol is minted. A change that
// always minted one would pass every arm above and fail here, having bought a
// slot pointing at a slot.
TEST(PreemptibleDefinitionAddress,
     AnIndirectSlotDispatchNeedsNoSecondSymbolBecauseTheReferenceIsTheSlot) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(**target, rep, {SymbolBinding::Weak},
                                  ExternCallDispatch::IndirectSlot);
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    ASSERT_EQ(out.leaSymbols.size(), 8u);

    auto const subj = subjects();
    EXPECT_NE(out.leaSymbols[0], subj[0]) << "still routed";
    EXPECT_TRUE(out.derefed[0]) << "still a slot read";
    auto const rows = preemptionRows(out.result);
    ASSERT_EQ(rows.size(), 2u);
    bool sawWeakFn = false;
    for (auto const* e : rows) {
        EXPECT_FALSE(e->addressSlotSymbol.valid())
            << "'" << e->mangledName << "': under `indirect-slot` the "
               "reference's VA IS the slot, so a second symbol would name a "
               "slot holding a slot address";
        if (e->mangledName == "wk") {
            sawWeakFn = true;
            EXPECT_EQ(out.leaSymbols[0], e->symbol.v)
                << "the address reads the reference itself";
        }
    }
    EXPECT_TRUE(sawWeakFn);
}

// DIRECTION 7 — AN ON-BINARY NAME IS A PRECONDITION OF PREEMPTIBILITY, and this
// is the arm that says so at the tier that answers the question.
//
// ⚠ THIS ONE PINS A SHIPPED, BLOCKING REGRESSION, not a hypothetical. A LOADER
// RESOLVES A REFERENCE BY NAME, so a definition with no on-binary spelling is in
// no image's dynamic export set and no loader can replace it — whatever its
// binding and visibility say. The engine did not ask that question, and
// `hir_to_mir.cpp` mints the string-literal POOL global and the promoted
// FLOAT-CONSTANT global with exactly the declared `Global`/`Default` pair and no
// name. ✔MEASURED 2026-09-06 with the shipped CLI, before the precondition
// landed: `const char *greet(void){ return "hello, world"; }` was a HARD REFUSAL
// (rc 1) at BOTH formats that declare a preemptible binding set
// (`elf64-x86_64-linux-dyn`, `elf64-aarch64-linux-dyn`) and rc 0 at every format
// that declares none — a `.so` containing a string literal did not compile, in a
// program every reference compiles. The same for a float constant, for
// `__func__`, and for any call passing a literal.
//
// THE THREE-SIDED ASSERTION, and why fewer sides would not have caught it:
//   * NO DIAGNOSTIC — the refusal itself is the regression;
//   * the nameless subject keeps its OWN direct address, undereferenced — it is
//     the pool object, not a slot holding a pointer to one;
//   * the four NAMED default-visibility subjects in the SAME lowering are still
//     routed, and four preemption rows still exist. Without this third side,
//     "the nameless one is direct" is equally consistent with "the declaration
//     was not read at all", which is the arm `NoDeclarationLeavesEveryAddressDirect`
//     already covers and would make this test vacuous.
//
// RED-ON-DISABLE: delete the `definedNameBySymbol_.contains(s.v)` precondition in
// `symbolIsPreemptibleDefinition` → the nameless subject is judged preemptible,
// `resolvePreemptionImport` finds no name for it and reports, and both the
// zero-error assertion and the nine-lea assertion fail by name.
TEST(PreemptibleDefinitionAddress,
     ANamelessDefinitionIsPreemptibleByNobodyAndKeepsItsDirectAddress) {
    auto target = TargetSchema::loadShipped("x86_64");
    ASSERT_TRUE(target.has_value());
    DiagnosticReporter rep;
    auto const out = lowerFixture(
        **target, rep, {SymbolBinding::Global, SymbolBinding::Weak},
        ExternCallDispatch::DirectPlt,
        /*twoWeakAddressSites=*/false, /*anonymousPoolGlobal=*/true);
    ASSERT_TRUE(out.result.ok) << "errorCount=" << rep.errorCount();
    EXPECT_EQ(rep.errorCount(), 0u)
        << "a compiler-synthesized literal-pool object carries the declared "
           "binding pair and no name; refusing it makes every shared library "
           "containing a string literal uncompilable";
    ASSERT_EQ(out.leaSymbols.size(), 9u)
        << "one address materialization per subject, the nameless one last";

    EXPECT_EQ(out.leaSymbols[8], kAnonDt)
        << "a definition no loader can name must materialize its own address";
    EXPECT_FALSE(out.derefed[8])
        << "there is no slot for a name that is in no dynamic export set, so a "
           "deref here would read the pool object's first bytes as a pointer";

    // THE NON-VACUITY CONTROL, in the SAME lowering: the declaration WAS read,
    // and the nameless subject was singled out rather than nothing being routed.
    auto const subj = subjects();
    for (std::size_t i : {0u, 1u, 4u, 5u}) {
        EXPECT_NE(out.leaSymbols[i], subj[i])
            << "named subject #" << i << " must still be routed";
        EXPECT_TRUE(out.derefed[i]) << "named subject #" << i;
    }
    EXPECT_EQ(preemptionRows(out.result).size(), 4u)
        << "the four NAMED default-visibility definitions, and only those";
}
