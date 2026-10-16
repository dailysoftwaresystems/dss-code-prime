// P69 fold 3 (lane `xa`) — THE LINKAGE RECORD OF A MIR DEFINITION, AND THE ONE RULE OF WHAT A WELL-FORMED ONE IS.
// D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER (the weak kind's COMPILED origin)
// D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED (the tentative mark)
//
// A function and a global each carry ONE record (`MirLinkage`, mir/mir_node.hpp): binding, visibility, WHICH KIND of
// weak definition a weak one is, and whether a global is a TENTATIVE definition. Until this fold the last two had no
// reader below HIR: a compiled `__attribute__((weak))` reached the object writer with no kind (and was written as
// whatever the format's default spelling is), and two units each holding `int x;` were two strong definitions.
//
// WHAT IS PINNED HERE:
//   * the rule (`mirLinkageDefect`) names each defect and passes each well-formed record;
//   * the builders keep the record WHOLE and refuse an ill-formed one by name (they abort: a producer's bug);
//   * a global is a COMMON by its tentative mark READ ALONE (`Mir::globalIsCommon`), because the rule refuses the
//     mark wherever no common can be — beside a weak binding and on a thread-local global too (P69, lane `cs`);
//   * `MirVerifier` refuses an ill-formed record by name — reached through the one builder door that does not ask
//     the rule (`MirBuilder::restateLinkageUnjudged`), which exists so that this half can be shown to fire.
// The `.dssir` spelling of the record is `test_mir_text_linkage.cpp`'s.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_verifier.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace dss;

namespace {

using Kind = WeakDefinitionKind;

constexpr MirLinkage kStrong{SymbolBinding::Global, SymbolVisibility::Default};
constexpr MirLinkage kLocal{SymbolBinding::Local, SymbolVisibility::Default};
constexpr MirLinkage kOverridable{SymbolBinding::Weak, SymbolVisibility::Default, Kind::Overridable};
constexpr MirLinkage kSelectAnyHidden{SymbolBinding::Weak, SymbolVisibility::Hidden, Kind::SelectAny};
constexpr MirLinkage kWeakNoKind{SymbolBinding::Weak, SymbolVisibility::Default};
constexpr MirLinkage kKindNoWeak{SymbolBinding::Global, SymbolVisibility::Default, Kind::SelectAny};
constexpr MirLinkage kTentative{SymbolBinding::Global, SymbolVisibility::Default, Kind{}, /*tentative=*/true};
constexpr MirLinkage kTentativeLocal{SymbolBinding::Local, SymbolVisibility::Default, Kind{}, /*tentative=*/true};
constexpr MirLinkage kTentativeWeak{SymbolBinding::Weak, SymbolVisibility::Default, Kind::Overridable,
                                    /*tentative=*/true};

constexpr std::string_view kNoKind      = "it is a weak definition that states no weak-definition kind";
constexpr std::string_view kKindNotWeak = "it states a weak-definition kind beside a binding that is not weak";
constexpr std::string_view kMarkBesideWeak  = "it is a weak definition marked as a tentative definition";
constexpr std::string_view kMarkThreadLocal = "it is a thread-local global marked as a tentative definition";

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

[[nodiscard]] bool saysVerifierFailure(DiagnosticReporter const& rep, std::string_view text) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::I_VerifierFailure && d.actual.find(text) != std::string::npos) return true;
    }
    return false;
}

// One function `%5` (returning) and one global `%6`, both strong: a module the verifier accepts. `afterBuild` may
// restate either's linkage through the unjudged door before the module is finished. `storage` is the global's.
template <class Restate>
[[nodiscard]] Mir moduleOfOneFunctionAndOneGlobal(TypeInterner& in, Restate&& afterBuild,
                                                  MirThreadStorage storage = MirThreadStorage::Shared) {
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const i32    = in.primitive(TypeKind::I32);
    MirBuilder   b;
    MirFuncId const  f = b.addFunction(in.fnSig({}, voidTy, CallConv::CcSysV), SymbolId{5}, kStrong);
    MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e);
    b.addReturn();
    MirGlobalId const g = b.addGlobal(i32, SymbolId{6}, UINT32_MAX, MirFuncId{}, kStrong, /*isConst=*/false,
                                      storage);
    afterBuild(b, f, g);
    return std::move(b).finish();
}

}  // namespace

// The rule, cell by cell. `mirLinkageDefect` is what the builders, the verifier and the `.dssir` reader all ask.
// Every cell here is a global WITHOUT thread storage; the two cells of the mark where no common can be — beside a
// weak binding, on a thread-local global — are `AGlobalIsACommonByItsMarkAloneAndTheMarkIsRefusedWhereNoneCanBe`'s,
// so that switching either refusal off turns THAT test red and leaves this one green.
TEST(MirLinkageRecord, TheRuleNamesEachDefectAndPassesEachWellFormedRecord) {
    using Owner = MirLinkageOwner;
    struct Cell {
        char const*      what;
        MirLinkage       linkage;
        Owner            owner;
        bool             hasInitializer;
        std::string_view defect;   // empty: well formed
    };
    MirLinkage const corrupted{SymbolBinding::Weak, SymbolVisibility::Default, static_cast<Kind>(7)};
    Cell const cells[] = {
        {"a strong function", kStrong, Owner::Function, false, ""},
        {"a local global with an initializer", kLocal, Owner::Global, true, ""},
        {"an overridable weak function", kOverridable, Owner::Function, false, ""},
        {"a select-any weak global with an initializer", kSelectAnyHidden, Owner::Global, true, ""},
        {"a tentative global", kTentative, Owner::Global, false, ""},
        {"a weak definition of no kind", kWeakNoKind, Owner::Function, false, kNoKind},
        {"a weak global of no kind", kWeakNoKind, Owner::Global, true, kNoKind},
        {"a kind beside a strong binding", kKindNoWeak, Owner::Global, true, kKindNotWeak},
        {"a kind byte that names no kind", corrupted, Owner::Function, false,
         "its weak-definition kind is not a value this build names"},
        {"a tentative FUNCTION", kTentative, Owner::Function, false,
         "it is a function marked as a tentative definition"},
        {"a tentative LOCAL global", kTentativeLocal, Owner::Global, false,
         "it is a module-private (local) global marked as a tentative definition"},
        {"a tentative global beside an initializer", kTentative, Owner::Global, true,
         "it is marked as a tentative definition and states an initializer"},
    };
    for (Cell const& c : cells) {
        SCOPED_TRACE(c.what);
        std::string_view const said =
            mirLinkageDefect(c.linkage, c.owner, c.hasInitializer, /*isThreadLocal=*/false);
        if (c.defect.empty()) {
            EXPECT_TRUE(said.empty()) << "a well-formed record was refused: " << said;
        } else {
            EXPECT_NE(said.find(c.defect), std::string_view::npos)
                << "the rule must name this defect; it said: '" << said << "'";
        }
    }
    // The kind is read only beside a weak binding, and through one accessor.
    EXPECT_EQ(kOverridable.statedWeakKind(), std::optional<Kind>{Kind::Overridable});
    EXPECT_EQ(kStrong.statedWeakKind(), std::nullopt);
}

// What a builder is handed is what the module's ONE accessor returns: no field of the record is dropped or crossed.
TEST(MirLinkageRecord, TheBuildersKeepTheRecordWhole) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const sig    = in.fnSig({}, voidTy, CallConv::CcSysV);
    MirBuilder   b;
    MirFuncId    fns[2];
    MirLinkage const fnLinkage[2] = {kSelectAnyHidden, kOverridable};
    for (int i = 0; i < 2; ++i) {
        fns[i] = b.addFunction(sig, SymbolId{static_cast<std::uint32_t>(10 + i)}, fnLinkage[i]);
        MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(e);
        b.addReturn();
    }
    MirLinkage const globalLinkage[3] = {kOverridable, kTentative, kSelectAnyHidden};
    MirGlobalId      globals[3];
    for (int i = 0; i < 3; ++i) {
        globals[i] = b.addGlobal(i32, SymbolId{static_cast<std::uint32_t>(20 + i)}, UINT32_MAX, MirFuncId{},
                                 globalLinkage[i], /*isConst=*/false, MirThreadStorage::Shared);
    }
    Mir const m = std::move(b).finish();
    for (int i = 0; i < 2; ++i) {
        EXPECT_EQ(m.funcLinkage(fns[i]), fnLinkage[i]) << "function " << i << ": the record came back changed";
        EXPECT_EQ(m.funcBinding(fns[i]), fnLinkage[i].binding);
        EXPECT_EQ(m.funcVisibility(fns[i]), fnLinkage[i].visibility);
    }
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(m.globalLinkage(globals[i]), globalLinkage[i]) << "global " << i << ": the record came back changed";
    }
    EXPECT_NE(m.funcLinkage(fns[0]).statedWeakKind(), m.funcLinkage(fns[1]).statedWeakKind())
        << "the two kinds of weak definition must stay two";
}

// A COMMON is a global that carries the tentative mark — the mark READ ALONE, asked by the merge and by the
// pipeline. The mark means "this definition may be written as a common", the tier above clears it on every definition
// that is its unit's outright, and the rule refuses it wherever no common can be: a weak tentative definition is a
// WEAK one (its kind decides; no reference writes a weak common), and a thread-local one is a definition (✔MEASURED
// 2026-10-12: no reference writes a thread-local tentative definition as a common, and two units holding one are
// refused by every linker measured). So the two are refused by name here, and the globals that ARE them — built as
// the front end builds them, without the mark — are no common.
TEST(MirLinkageRecord, AGlobalIsACommonByItsMarkAloneAndTheMarkIsRefusedWhereNoneCanBe) {
    using Owner = MirLinkageOwner;
    EXPECT_NE(mirLinkageDefect(kTentativeWeak, Owner::Global, false, /*isThreadLocal=*/false).find(kMarkBesideWeak),
              std::string_view::npos)
        << "a weak tentative definition is a WEAK definition: its kind decides";
    EXPECT_NE(mirLinkageDefect(kTentative, Owner::Global, false, /*isThreadLocal=*/true).find(kMarkThreadLocal),
              std::string_view::npos)
        << "a thread-local tentative definition is a definition";
    // CONTROL: the same facts, each without the mark, and the mark on a strong shared global, are well formed.
    EXPECT_TRUE(mirLinkageDefect(kOverridable, Owner::Global, false, /*isThreadLocal=*/false).empty());
    EXPECT_TRUE(mirLinkageDefect(kStrong, Owner::Global, false, /*isThreadLocal=*/true).empty());
    EXPECT_TRUE(mirLinkageDefect(kTentative, Owner::Global, false, /*isThreadLocal=*/false).empty());

    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder   b;
    auto const add = [&](std::uint32_t sym, MirLinkage linkage, MirThreadStorage storage) {
        return b.addGlobal(i32, SymbolId{sym}, UINT32_MAX, MirFuncId{}, linkage, /*isConst=*/false, storage);
    };
    MirGlobalId const tentative     = add(1, kTentative, MirThreadStorage::Shared);
    MirGlobalId const threadLocal   = add(2, kStrong, MirThreadStorage::PerThread);
    MirGlobalId const weak          = add(3, kOverridable, MirThreadStorage::Shared);
    MirGlobalId const zeroNotStated = add(4, kStrong, MirThreadStorage::Shared);
    Mir const m = std::move(b).finish();
    EXPECT_TRUE(m.globalIsCommon(tentative)) << "`int x;` — the link folds it with the other units'";
    EXPECT_FALSE(m.globalIsCommon(threadLocal)) << "`_Thread_local int t;` — built without the mark";
    EXPECT_FALSE(m.globalIsCommon(weak)) << "`__attribute__((weak)) int t;` — built without the mark";
    EXPECT_FALSE(m.globalIsCommon(zeroNotStated)) << "CONTROL: a zero-filled global nobody marked tentative";
    EXPECT_TRUE(mirLinkageIsCommon(kTentative));
    EXPECT_FALSE(mirLinkageIsCommon(kStrong));
}

// ── the builders refuse, by name ─────────────────────────────────────────────────────────────────────────────────
// (Each death statement is a function call: a brace-initializer's comma trips the macro's two-argument parser.)
namespace {

void addFunctionWith(MirLinkage linkage) {
    TypeInterner in{CompilationUnitId{1}};
    MirBuilder   b;
    (void)b.addFunction(in.fnSig({}, in.primitive(TypeKind::Void), CallConv::CcSysV), SymbolId{5}, linkage);
}
void addGlobalWith(MirLinkage linkage, bool withInitializer,
                   MirThreadStorage storage = MirThreadStorage::Shared) {
    TypeInterner in{CompilationUnitId{1}};
    TypeId const i32 = in.primitive(TypeKind::I32);
    MirBuilder   b;
    MirLiteralValue one;
    one.value = std::int64_t{1};
    one.core  = TypeKind::I32;
    std::uint32_t const literal = withInitializer ? b.literalPoolAdd(one) : UINT32_MAX;
    (void)b.addGlobal(i32, SymbolId{6}, literal, MirFuncId{}, linkage, /*isConst=*/false, storage);
}
void addFunctionThroughTheBindingForm(SymbolBinding binding) {
    TypeInterner in{CompilationUnitId{1}};
    MirBuilder   b;
    (void)b.addFunction(in.fnSig({}, in.primitive(TypeKind::Void), CallConv::CcSysV), SymbolId{5}, binding,
                        SymbolVisibility::Default);
}
void addGlobalThroughTheBindingForm(SymbolBinding binding) {
    TypeInterner in{CompilationUnitId{1}};
    MirBuilder   b;
    (void)b.addGlobal(in.primitive(TypeKind::I32), SymbolId{6}, UINT32_MAX, MirFuncId{}, binding,
                      SymbolVisibility::Default, /*isConst=*/false, MirThreadStorage::Shared);
}

}  // namespace

TEST(MirLinkageRecordDeathTest, ABuilderRefusesAWeakDefinitionThatStatesNoKind) {
    EXPECT_DEATH(addFunctionWith(kWeakNoKind),
                 "addFunction: symbol id 5: it is a weak definition that states no weak-definition kind");
    EXPECT_DEATH(addGlobalWith(kWeakNoKind, true),
                 "addGlobal: symbol id 6: it is a weak definition that states no weak-definition kind");
    // CONTROL: the same two calls with the kind stated build.
    addFunctionWith(kOverridable);
    addGlobalWith(kSelectAnyHidden, true);
}

TEST(MirLinkageRecordDeathTest, ABuilderRefusesAKindBesideABindingThatIsNotWeak) {
    EXPECT_DEATH(addFunctionWith(kKindNoWeak),
                 "addFunction: symbol id 5: it states a weak-definition kind beside a binding that is not weak");
    EXPECT_DEATH(addGlobalWith(kKindNoWeak, true),
                 "addGlobal: symbol id 6: it states a weak-definition kind beside a binding that is not weak");
}

TEST(MirLinkageRecordDeathTest, TheBindingFormOfBothBuildersRefusesAWeakBinding) {
    EXPECT_DEATH(addFunctionThroughTheBindingForm(SymbolBinding::Weak),
                 "addFunction: symbol id 5: a weak binding was passed through the");
    EXPECT_DEATH(addGlobalThroughTheBindingForm(SymbolBinding::Weak),
                 "addGlobal: symbol id 6: a weak binding was passed through the");
    // CONTROL: every other binding goes through that form.
    addFunctionThroughTheBindingForm(SymbolBinding::Global);
    addGlobalThroughTheBindingForm(SymbolBinding::Local);
}

TEST(MirLinkageRecordDeathTest, ABuilderRefusesATentativeMarkWhereNoTentativeDefinitionCanBe) {
    EXPECT_DEATH(addFunctionWith(kTentative), "addFunction: symbol id 5: it is a function marked as a tentative");
    EXPECT_DEATH(addGlobalWith(kTentativeLocal, false),
                 "addGlobal: symbol id 6: it is a module-private \\(local\\) global marked as a tentative");
    EXPECT_DEATH(addGlobalWith(kTentative, true),
                 "addGlobal: symbol id 6: it is marked as a tentative definition and states an initializer");
    EXPECT_DEATH(addGlobalWith(kTentativeWeak, false),
                 "addGlobal: symbol id 6: it is a weak definition marked as a tentative definition");
    EXPECT_DEATH(addGlobalWith(kTentative, false, MirThreadStorage::PerThread),
                 "addGlobal: symbol id 6: it is a thread-local global marked as a tentative definition");
    // CONTROL: a tentative global with no initializer builds, and so do the weak and the thread-local one unmarked.
    addGlobalWith(kTentative, false);
    addGlobalWith(kOverridable, false);
    addGlobalWith(kStrong, false, MirThreadStorage::PerThread);
}

// ── the verifier refuses, by name ────────────────────────────────────────────────────────────────────────────────
TEST(MirLinkageRecord, TheVerifierRefusesAWeakDefinitionThatStatesNoKind) {
    for (bool const onTheFunction : {true, false}) {
        SCOPED_TRACE(onTheFunction ? "the function" : "the global");
        TypeInterner in{CompilationUnitId{1}};
        Mir const    m = moduleOfOneFunctionAndOneGlobal(in, [&](MirBuilder& b, MirFuncId f, MirGlobalId g) {
            if (onTheFunction) b.restateLinkageUnjudged(f, kWeakNoKind);
            else b.restateLinkageUnjudged(g, kWeakNoKind);
        });
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_FALSE(v.verify(rep)) << "a weak definition of no kind has no spelling in an object format";
        EXPECT_TRUE(saysVerifierFailure(rep, std::string{"the linkage of symbol %"} + (onTheFunction ? "5" : "6")
                                                 + " is not well formed: " + std::string{kNoKind}))
            << "the verifier must name the symbol and the defect" << diagnosticsOf(rep);
    }
    // CONTROL: the same module with nothing restated verifies, and so does one whose weak definitions state kinds.
    for (bool const weak : {false, true}) {
        TypeInterner in{CompilationUnitId{1}};
        Mir const    m = moduleOfOneFunctionAndOneGlobal(in, [&](MirBuilder& b, MirFuncId f, MirGlobalId g) {
            if (!weak) return;
            b.restateLinkageUnjudged(f, kOverridable);
            b.restateLinkageUnjudged(g, kSelectAnyHidden);
        });
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_TRUE(v.verify(rep)) << diagnosticsOf(rep);
    }
}

TEST(MirLinkageRecord, TheVerifierRefusesAKindBesideABindingThatIsNotWeak) {
    for (bool const onTheFunction : {true, false}) {
        SCOPED_TRACE(onTheFunction ? "the function" : "the global");
        TypeInterner in{CompilationUnitId{1}};
        Mir const    m = moduleOfOneFunctionAndOneGlobal(in, [&](MirBuilder& b, MirFuncId f, MirGlobalId g) {
            if (onTheFunction) b.restateLinkageUnjudged(f, kKindNoWeak);
            else b.restateLinkageUnjudged(g, kKindNoWeak);
        });
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_FALSE(v.verify(rep)) << "a kind is read only on a weak definition";
        EXPECT_TRUE(saysVerifierFailure(rep, std::string{"the linkage of symbol %"} + (onTheFunction ? "5" : "6")
                                                 + " is not well formed: " + std::string{kKindNotWeak}))
            << "the verifier must name the symbol and the defect" << diagnosticsOf(rep);
    }
}

TEST(MirLinkageRecord, TheVerifierRefusesATentativeMarkOnAFunctionAndOnALocalGlobal) {
    // Beside a weak binding, and on a thread-local global: the verifier hands the rule the global's thread storage.
    {
        TypeInterner in{CompilationUnitId{1}};
        Mir const    m = moduleOfOneFunctionAndOneGlobal(
            in, [&](MirBuilder& b, MirFuncId, MirGlobalId g) { b.restateLinkageUnjudged(g, kTentativeWeak); });
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_FALSE(v.verify(rep));
        EXPECT_TRUE(saysVerifierFailure(rep, kMarkBesideWeak)) << diagnosticsOf(rep);
    }
    {
        TypeInterner in{CompilationUnitId{1}};
        Mir const    m = moduleOfOneFunctionAndOneGlobal(
            in, [&](MirBuilder& b, MirFuncId, MirGlobalId g) { b.restateLinkageUnjudged(g, kTentative); },
            MirThreadStorage::PerThread);
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_FALSE(v.verify(rep));
        EXPECT_TRUE(saysVerifierFailure(rep, kMarkThreadLocal)) << diagnosticsOf(rep);
    }
    {
        TypeInterner in{CompilationUnitId{1}};
        Mir const    m = moduleOfOneFunctionAndOneGlobal(
            in, [&](MirBuilder& b, MirFuncId f, MirGlobalId) { b.restateLinkageUnjudged(f, kTentative); });
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_FALSE(v.verify(rep));
        EXPECT_TRUE(saysVerifierFailure(rep, "it is a function marked as a tentative definition"))
            << diagnosticsOf(rep);
    }
    {
        TypeInterner in{CompilationUnitId{1}};
        Mir const    m = moduleOfOneFunctionAndOneGlobal(
            in, [&](MirBuilder& b, MirFuncId, MirGlobalId g) { b.restateLinkageUnjudged(g, kTentativeLocal); });
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_FALSE(v.verify(rep));
        EXPECT_TRUE(saysVerifierFailure(rep, "it is a module-private (local) global marked as a tentative"))
            << diagnosticsOf(rep);
    }
    // CONTROL: the tentative mark on the strong global is a module the verifier accepts.
    TypeInterner in{CompilationUnitId{1}};
    Mir const    m = moduleOfOneFunctionAndOneGlobal(
        in, [&](MirBuilder& b, MirFuncId, MirGlobalId g) { b.restateLinkageUnjudged(g, kTentative); });
    DiagnosticReporter rep;
    MirVerifier        v{m, &in};
    EXPECT_TRUE(v.verify(rep)) << diagnosticsOf(rep);
}

// The verifier's own answer to "does this global state an initializer" (`checkLinkageRecords` computes it from the
// module, where the builders and the `.dssir` reader are handed it): a tentative mark beside an initializer of
// EITHER shape — a constant literal, a runtime initializer function — is refused by name, and the same strong global
// with no initializer is not (P69 fold 3b, the review of fold 3a, F10: no verifier test reached the cell).
TEST(MirLinkageRecord, TheVerifierRefusesATentativeMarkBesideAnInitializerOfEitherShape) {
    for (bool const literal : {true, false}) {
        SCOPED_TRACE(literal ? "a constant literal" : "a runtime initializer function");
        TypeInterner       in{CompilationUnitId{1}};
        TypeId const       voidTy = in.primitive(TypeKind::Void);
        TypeId const       i32    = in.primitive(TypeKind::I32);
        MirBuilder         b;
        MirFuncId const    f = b.addFunction(in.fnSig({}, voidTy, CallConv::CcSysV), SymbolId{5}, kStrong);
        MirBlockId const   e = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(e);
        b.addReturn();
        MirLiteralValue one;
        one.value = std::int64_t{1};
        one.core  = TypeKind::I32;
        std::uint32_t const index = literal ? b.literalPoolAdd(one) : UINT32_MAX;
        MirGlobalId const   g = b.addGlobal(i32, SymbolId{6}, index, literal ? MirFuncId{} : f, kStrong,
                                            /*isConst=*/false, MirThreadStorage::Shared);
        b.restateLinkageUnjudged(g, kTentative);
        Mir const          m = std::move(b).finish();
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_FALSE(v.verify(rep)) << "a tentative definition is by its definition the one with no initializer";
        EXPECT_TRUE(saysVerifierFailure(rep, "the linkage of symbol %6 is not well formed: it is marked as a "
                                             "tentative definition and states an initializer"))
            << diagnosticsOf(rep);
    }
}
