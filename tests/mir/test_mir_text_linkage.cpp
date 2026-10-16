// P69 fold 3 (lane `xa`) — `.dssir` VERSION 6: A WEAK DEFINITION STATES ITS KIND, AND A TENTATIVE ONE SAYS SO.
// D-LK-WEAK-EXTERNAL-BODY-OUTRANKED-A-SELECT-ANY-DEFINITION-BY-LINK-ORDER
// D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED
//
// The linkage record of a MIR definition (`MirLinkage`) has two facts version 5 could not spell: the kind of a weak
// definition, written as the argument of the binding word — `weak(overridable)`, `weak(select-any)` — and a
// global's tentative mark, the word `tentative`. The reader judges the record it read by the rule the builders and
// the verifier ask (`mirLinkageDefect`) and refuses, by name and without building anything, each spelling pinned
// below. A module written by the emitter reads back to the SAME records.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_text.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

using Kind = WeakDefinitionKind;

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + d.actual;
    return s.empty() ? std::string{"\n  (no diagnostics)"} : s;
}

[[nodiscard]] bool says(DiagnosticReporter const& rep, std::string_view text) {
    for (auto const& d : rep.all()) {
        if (d.actual.find(text) != std::string::npos) return true;
    }
    return false;
}

// A module of ONE global `%10`, its attribute list `attrs` (with its leading space) and its initializer `init`.
[[nodiscard]] std::string globalDoc(std::string_view attrs, std::string_view init = "zero") {
    return std::string{"dssir 6\nmodule {\n  global %10 : i32"} + std::string{attrs} + " = " + std::string{init}
           + "\n}\n";
}

struct Refusal {
    char const*      what;
    std::string      text;
    std::string_view mustSay;
};

void expectRefused(Refusal const& r) {
    SCOPED_TRACE(r.what);
    DiagnosticReporter rep;
    auto const         res = parseMir(r.text, CompilationUnitId{1}, rep);
    ASSERT_NE(res, nullptr);
    EXPECT_FALSE(res->ok) << "this text must not be read:\n" << r.text;
    EXPECT_TRUE(says(rep, r.mustSay)) << "the refusal must say '" << r.mustSay << "'; it said:" << diagnosticsOf(rep);
}

void expectRead(char const* what, std::string const& text, MirLinkage want) {
    SCOPED_TRACE(what);
    DiagnosticReporter rep;
    auto const         res = parseMir(text, CompilationUnitId{1}, rep);
    ASSERT_NE(res, nullptr);
    ASSERT_TRUE(res->ok) << "CONTROL: this text is well formed" << diagnosticsOf(rep) << "\n" << text;
    ASSERT_EQ(res->mir.moduleGlobalCount(), 1u);
    EXPECT_EQ(res->mir.globalLinkage(res->mir.globalAt(0)), want);
}

// A module with every shape of the record: two weak functions of the two kinds, a weak global, a tentative global
// and a weak select-any one with a visibility. (A weak TENTATIVE global is no shape: the rule refuses the mark beside
// a weak binding — `ATentativeMarkIsRefusedWhereNoTentativeDefinitionCanBe`.)
struct Shapes {
    Mir                      mir;
    std::vector<std::string> names;
};
[[nodiscard]] Shapes moduleOfEveryShape(TypeInterner& in) {
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const i32    = in.primitive(TypeKind::I32);
    TypeId const sig    = in.fnSig({}, voidTy, CallConv::CcSysV);
    MirBuilder   b;
    auto const addFn = [&](std::uint32_t sym, MirLinkage linkage) {
        (void)b.addFunction(sig, SymbolId{sym}, linkage);
        MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
        b.beginBlock(e);
        b.addReturn();
    };
    addFn(1, MirLinkage{SymbolBinding::Weak, SymbolVisibility::Default, Kind::SelectAny});
    addFn(2, MirLinkage{SymbolBinding::Weak, SymbolVisibility::Hidden, Kind::Overridable});
    addFn(3, MirLinkage{SymbolBinding::Global, SymbolVisibility::Default});
    auto const addGlobal = [&](std::uint32_t sym, MirLinkage linkage) {
        (void)b.addGlobal(i32, SymbolId{sym}, UINT32_MAX, MirFuncId{}, linkage, /*isConst=*/false,
                          MirThreadStorage::Shared);
    };
    addGlobal(4, MirLinkage{SymbolBinding::Weak, SymbolVisibility::Default, Kind::Overridable});
    addGlobal(5, MirLinkage{SymbolBinding::Global, SymbolVisibility::Default, Kind{}, /*tentative=*/true});
    addGlobal(6, MirLinkage{SymbolBinding::Weak, SymbolVisibility::Protected, Kind::SelectAny});
    addGlobal(7, MirLinkage{SymbolBinding::Local, SymbolVisibility::Default});
    return Shapes{std::move(b).finish(), {"", "fa", "fb", "fc", "ga", "gb", "gc", "gd"}};
}

}  // namespace

// THE CLAIM. Every record a builder can make is written and read back as itself.
TEST(MirTextLinkage, AWeakDefinitionsKindAndATentativeMarkSurviveTheRoundTrip) {
    TypeInterner in{CompilationUnitId{1}};
    Shapes const shapes = moduleOfEveryShape(in);
    DiagnosticReporter emitRep;
    MirTextContext     ctx{&in, &shapes.names};
    std::string const  text = emitMir(shapes.mir, ctx, emitRep);
    EXPECT_NE(text.find("dssir 6"), std::string::npos) << text;
    EXPECT_NE(text.find("weak(select-any)"), std::string::npos) << "the kind is the binding word's argument\n" << text;
    EXPECT_NE(text.find("weak(overridable)"), std::string::npos) << text;
    EXPECT_NE(text.find("tentative"), std::string::npos) << "a tentative global says so\n" << text;

    DiagnosticReporter rep;
    auto const         back = parseMir(text, CompilationUnitId{1}, rep);
    ASSERT_NE(back, nullptr);
    ASSERT_TRUE(back->ok) << "the emitter's own text was not read back" << diagnosticsOf(rep) << "\n" << text;
    Mir const& p = back->mir;
    ASSERT_EQ(p.moduleFuncCount(), shapes.mir.moduleFuncCount());
    ASSERT_EQ(p.moduleGlobalCount(), shapes.mir.moduleGlobalCount());
    for (std::uint32_t i = 0; i < p.moduleFuncCount(); ++i) {
        MirFuncId const was = shapes.mir.funcAt(i);
        MirFuncId const now = p.funcAt(i);
        ASSERT_EQ(p.funcSymbol(now), shapes.mir.funcSymbol(was));
        EXPECT_EQ(p.funcLinkage(now), shapes.mir.funcLinkage(was))
            << "function %" << p.funcSymbol(now).v << ": the linkage record did not survive\n" << text;
    }
    for (std::uint32_t i = 0; i < p.moduleGlobalCount(); ++i) {
        MirGlobalId const was = shapes.mir.globalAt(i);
        MirGlobalId const now = p.globalAt(i);
        ASSERT_EQ(p.globalSymbol(now), shapes.mir.globalSymbol(was));
        EXPECT_EQ(p.globalLinkage(now), shapes.mir.globalLinkage(was))
            << "global %" << p.globalSymbol(now).v << ": the linkage record did not survive\n" << text;
        EXPECT_EQ(p.globalIsCommon(now), shapes.mir.globalIsCommon(was));
    }
}

// The well-formed spellings, hand-written — the control of every refusal below.
TEST(MirTextLinkage, TheWellFormedSpellingsAreRead) {
    expectRead("an overridable weak global", globalDoc(" [weak(overridable)]"),
               MirLinkage{SymbolBinding::Weak, SymbolVisibility::Default, Kind::Overridable});
    expectRead("a select-any weak global, spaces inside the argument", globalDoc(" [weak( select-any ), hidden]"),
               MirLinkage{SymbolBinding::Weak, SymbolVisibility::Hidden, Kind::SelectAny});
    expectRead("a tentative global", globalDoc(" [tentative]"),
               MirLinkage{SymbolBinding::Global, SymbolVisibility::Default, Kind{}, /*tentative=*/true});
    expectRead("a thread-local global with no mark", globalDoc(" [threadlocal]"), MirLinkage{});
    expectRead("no list at all", globalDoc(""), MirLinkage{});
}

TEST(MirTextLinkage, ABareWeakIsRefusedByName) {
    // Version 5's spelling of a weak definition. A weak definition of no stated kind is no module a builder makes.
    expectRefused({"a global", globalDoc(" [weak]"), "it is a weak definition that states no weak-definition kind"});
    expectRefused({"the same list names the global", globalDoc(" [weak]"),
                   "the attribute list of global %10 states a linkage that is not well formed"});
    expectRefused({"beside a visibility", globalDoc(" [weak, hidden]"), "states no weak-definition kind"});
}

TEST(MirTextLinkage, AKindThisBuildDoesNotNameIsRefusedWithTheAcceptedSet) {
    expectRefused({"an unknown kind", globalDoc(" [weak(strongest)]"), "unknown weak-definition kind 'strongest'"});
    expectRefused({"the accepted set is said", globalDoc(" [weak(strongest)]"), "select-any"});
    expectRefused({"an empty argument", globalDoc(" [weak()]"), "unknown weak-definition kind ''"});
}

TEST(MirTextLinkage, AnArgumentOnABindingThatIsNotWeakIsRefused) {
    expectRefused({"a kind on `global`", globalDoc(" [global(select-any)]"), "'global' takes no argument"});
    expectRefused({"a kind on `local`", globalDoc(" [local(overridable)]"), "'local' takes no argument"});
}

TEST(MirTextLinkage, ASecondBindingWordInOneListIsRefused) {
    expectRefused({"weak then local", globalDoc(" [weak(overridable), local]"), "states its binding twice"});
    expectRefused({"global then weak", globalDoc(" [global, weak(select-any)]"), "states its binding twice"});
}

// A second VISIBILITY word in one list is refused like a second binding word: the later one used to replace the
// earlier in silence (P69 fold 3b, the review of fold 3a, F9).
TEST(MirTextLinkage, ASecondVisibilityWordInOneListIsRefused) {
    expectRefused({"hidden then protected", globalDoc(" [hidden, protected]"), "states its visibility twice"});
    expectRefused({"the same word twice", globalDoc(" [weak(overridable), hidden, hidden]"),
                   "states its visibility twice"});
    expectRead("CONTROL: one visibility beside a binding", globalDoc(" [weak(overridable), protected]"),
               MirLinkage{SymbolBinding::Weak, SymbolVisibility::Protected, Kind::Overridable});
}

TEST(MirTextLinkage, AnArgumentLeftOpenIsRefused) {
    expectRefused({"the list closes inside the argument", globalDoc(" [weak(overridable]"),
                   "opens an argument that is not closed with ')' on its line"});
}

TEST(MirTextLinkage, ATentativeMarkIsRefusedWhereNoTentativeDefinitionCanBe) {
    expectRefused({"beside an initializer", globalDoc(" [tentative]", "lit int 0 : i32"),
                   "it is marked as a tentative definition and states an initializer"});
    expectRefused({"on a local global", globalDoc(" [local, tentative]"),
                   "it is a module-private (local) global marked as a tentative definition"});
    expectRefused({"beside a weak binding", globalDoc(" [weak(select-any), tentative]"),
                   "it is a weak definition marked as a tentative definition"});
    expectRefused({"on a thread-local global", globalDoc(" [threadlocal, tentative]"),
                   "it is a thread-local global marked as a tentative definition"});
    // On a FUNCTION the word is no attribute at all: the emitter's own text, with the word put in the list of the
    // one function whose list is a kind alone (`[weak(select-any)] {` — a global's list never ends in a brace, and
    // a global states that kind too, so the list is found by what follows it).
    TypeInterner in{CompilationUnitId{1}};
    Shapes const shapes = moduleOfEveryShape(in);
    DiagnosticReporter emitRep;
    MirTextContext     ctx{&in, &shapes.names};
    std::string        text = emitMir(shapes.mir, ctx, emitRep);
    std::string const  was  = "[weak(select-any)] {";
    auto const         at   = text.find(was);
    ASSERT_NE(at, std::string::npos) << text;
    ASSERT_EQ(text.find(was, at + 1), std::string::npos) << "one function's list, and no other, is rewritten\n" << text;
    text.replace(at, was.size(), "[tentative] {");
    expectRefused({"on a function", text, "unknown function attribute 'tentative'"});
    // ⓘ A flag word said twice in a GLOBAL's list (`[tentative, protected, tentative]`) is read as said once, as
    // `const` and `thread_local` are: only a second BINDING is refused, because two bindings can disagree. (Met
    // while writing this case: the first rewrite landed in a global's list and the text was read.)
}

TEST(MirTextLinkage, AVersionFiveTextIsRefusedByTheVersionCheck) {
    expectRefused({"version 5", "dssir 5\nmodule {\n}\n", "expected version 6, got '5'"});
    DiagnosticReporter rep;
    auto const         res = parseMir("dssir 6\nmodule {\n}\n", CompilationUnitId{1}, rep);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(res->ok) << "CONTROL: the empty module of the current version" << diagnosticsOf(rep);
}
