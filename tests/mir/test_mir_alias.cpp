// P69 (lane `cs`) — A SECOND NAME OF A DEFINITION, AS THE MIR MODULE CARRIES IT (`MirAlias`).
//
// The compiled `alias("target")` is a fact of the MODULE, keyed by two symbols: `alias` is a name the
// module defines as the very definition `target` is. What is pinned here:
//   * the builder keeps every alias whole, refuses by name what one statement alone shows ill formed
//     (an invalid id, a name that is its own target or is stated twice, a tentative mark, a linkage
//     record the one rule refuses), and a rebuild carries the aliases (`carryModuleFactsOf`);
//   * the verifier refuses an alias whose target the module does not define, and one whose name the
//     module defines otherwise;
//   * the `.dssir` text spells an alias (`alias %A [<linkage words>] = %T`), reads back the same
//     record, and refuses each ill-formed spelling by name without building anything;
//   * the one list of the ids a module names (`forEachSymbolIdNamed`) names both of an alias's ids.

#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"
#include "mir/mir_symbols_named.hpp"
#include "mir/mir_text.hpp"
#include "mir/mir_verifier.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

using Kind = WeakDefinitionKind;

constexpr MirLinkage kStrong{SymbolBinding::Global, SymbolVisibility::Default};
constexpr MirLinkage kWeakHidden{SymbolBinding::Weak, SymbolVisibility::Hidden, Kind::Overridable};
constexpr MirLinkage kLocal{SymbolBinding::Local, SymbolVisibility::Default};

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

// A function `%1` and a global `%2`, then `afterDefinitions(builder)`.
template <class Then>
[[nodiscard]] Mir moduleWith(TypeInterner& in, Then&& afterDefinitions) {
    TypeId const voidTy = in.primitive(TypeKind::Void);
    TypeId const i32    = in.primitive(TypeKind::I32);
    MirBuilder   b;
    (void)b.addFunction(in.fnSig({}, voidTy, CallConv::CcSysV), SymbolId{1}, kStrong);
    MirBlockId const e = b.createBlock(StructCfMarker::EntryBlock);
    b.beginBlock(e);
    b.addReturn();
    (void)b.addGlobal(i32, SymbolId{2}, UINT32_MAX, MirFuncId{}, kStrong, /*isConst=*/false,
                      MirThreadStorage::Shared);
    afterDefinitions(b);
    b.stateSelfContainedSymbolIds();
    return std::move(b).finish();
}

[[nodiscard]] Mir moduleOfTwoAliases(TypeInterner& in) {
    return moduleWith(in, [](MirBuilder& b) {
        b.addAlias(SymbolId{3}, SymbolId{1}, kStrong);
        b.addAlias(SymbolId{4}, SymbolId{2}, kWeakHidden);
    });
}

// The `.dssir` text of a module of ONE function `%1` and ONE global `%2` (as the writer spells it), with
// `aliases` (each a line) added before the module's closing brace.
[[nodiscard]] std::string textWith(std::string_view aliases) {
    TypeInterner                   in{CompilationUnitId{1}};
    Mir const                      m = moduleWith(in, [](MirBuilder&) {});
    std::vector<std::string> const names{"", "g", "v"};
    MirTextContext                 ctx{&in, &names};
    DiagnosticReporter             rep;
    std::string                    text  = emitMir(m, ctx, rep);
    std::size_t const              close = text.rfind("}\n");
    text.insert(close, aliases);
    return text;
}

void addAliasWith(SymbolId alias, SymbolId target, MirLinkage linkage) {
    MirBuilder b;
    b.addAlias(alias, target, linkage);
}
void addAliasTwice() {
    MirBuilder b;
    b.addAlias(SymbolId{3}, SymbolId{1}, kStrong);
    b.addAlias(SymbolId{3}, SymbolId{2}, kStrong);
}

}  // namespace

TEST(MirAlias, TheBuilderKeepsEveryAliasWholeAndARebuildCarriesThem) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const    m = moduleOfTwoAliases(in);
    ASSERT_EQ(m.moduleAliasCount(), 2u);
    EXPECT_EQ(m.aliases()[0], (MirAlias{SymbolId{3}, SymbolId{1}, kStrong}));
    EXPECT_EQ(m.aliases()[1], (MirAlias{SymbolId{4}, SymbolId{2}, kWeakHidden}));
    EXPECT_GE(m.symbolIdEnd(), 5u) << "an alias's name is a symbol the module defines: the id space is past it";

    // A rebuild: the facts of the module carried by the one list of them.
    MirBuilder rebuilt;
    rebuilt.carryModuleFactsOf(m);
    Mir const r = std::move(rebuilt).finish();
    ASSERT_EQ(r.moduleAliasCount(), 2u) << "a rebuild that drops an alias drops a definition";
    EXPECT_EQ(r.aliases()[1], m.aliases()[1]);

    // A move leaves the source defining no second name.
    Mir moved = moduleOfTwoAliases(in);
    Mir to    = std::move(moved);
    EXPECT_EQ(to.moduleAliasCount(), 2u);
    EXPECT_EQ(moved.moduleAliasCount(), 0u);  // NOLINT(bugprone-use-after-move): the emptied state is the pin
}

TEST(MirAliasDeathTest, TheBuilderRefusesAnIllFormedAliasByName) {
    EXPECT_DEATH(addAliasWith(SymbolId{}, SymbolId{1}, kStrong), "addAlias: .*both named symbols");
    EXPECT_DEATH(addAliasWith(SymbolId{3}, SymbolId{}, kStrong), "addAlias: .*both named symbols");
    EXPECT_DEATH(addAliasWith(SymbolId{3}, SymbolId{3}, kStrong), "addAlias: .*an alias of itself");
    EXPECT_DEATH(addAliasTwice(), "addAlias: .*already stated as an alias");
    EXPECT_DEATH(addAliasWith(SymbolId{3}, SymbolId{1},
                              MirLinkage{SymbolBinding::Global, SymbolVisibility::Default, Kind{}, true}),
                 "addAlias: .*tentative definition");
    EXPECT_DEATH(addAliasWith(SymbolId{3}, SymbolId{1}, MirLinkage{SymbolBinding::Weak, SymbolVisibility::Default}),
                 "addAlias: .*states no weak-definition kind");
    // CONTROL: each well-formed one builds.
    addAliasWith(SymbolId{3}, SymbolId{1}, kStrong);
    addAliasWith(SymbolId{3}, SymbolId{1}, kWeakHidden);
    addAliasWith(SymbolId{3}, SymbolId{1}, kLocal);
}

TEST(MirAlias, TheVerifierRefusesAnAliasOfNoDefinitionAndANameDefinedTwice) {
    struct Case {
        char const* what;
        SymbolId    alias;
        SymbolId    target;
        char const* mustSay;
    };
    for (Case const c : {
             Case{"a target the module does not define", SymbolId{3}, SymbolId{9}, "its target is no function or global"},
             // The name defined by something else is the one-definition rule's, said for
             // every kind of definition (MirVerifier::checkSymbolIdsDefinedOnce).
             Case{"a name the module defines as a function", SymbolId{1}, SymbolId{2},
                  "symbol %1 is defined twice in this module, as its function and as its alias"},
             Case{"a name the module defines as a global", SymbolId{2}, SymbolId{1},
                  "symbol %2 is defined twice in this module, as its global and as its alias"},
         }) {
        SCOPED_TRACE(c.what);
        TypeInterner in{CompilationUnitId{1}};
        Mir const    m = moduleWith(in, [&](MirBuilder& b) { b.addAlias(c.alias, c.target, kStrong); });
        DiagnosticReporter rep;
        MirVerifier        v{m, &in};
        EXPECT_FALSE(v.verify(rep));
        EXPECT_TRUE(says(rep, c.mustSay)) << diagnosticsOf(rep);
    }
    // CONTROL: two well-formed aliases verify.
    TypeInterner       in{CompilationUnitId{1}};
    Mir const          m = moduleOfTwoAliases(in);
    DiagnosticReporter rep;
    MirVerifier        v{m, &in};
    EXPECT_TRUE(v.verify(rep)) << diagnosticsOf(rep);
}

TEST(MirAlias, TheTextSpellsAnAliasAndReadsBackTheSameRecord) {
    TypeInterner         in{CompilationUnitId{1}};
    Mir const            m = moduleOfTwoAliases(in);
    std::vector<std::string> const names{"", "g", "v", "f", "w"};
    MirTextContext       ctx{&in, &names};
    DiagnosticReporter   emitRep;
    std::string const    text = emitMir(m, ctx, emitRep);
    EXPECT_NE(text.find("alias %3 = %1"), std::string::npos) << text;
    EXPECT_NE(text.find("alias %4 [weak(overridable), hidden] = %2"), std::string::npos) << text;

    DiagnosticReporter rep;
    auto const         back = parseMir(text, CompilationUnitId{1}, rep);
    ASSERT_NE(back, nullptr);
    ASSERT_TRUE(back->ok) << diagnosticsOf(rep) << "\n" << text;
    ASSERT_EQ(back->mir.moduleAliasCount(), 2u);
    EXPECT_EQ(back->mir.aliases()[0], m.aliases()[0]);
    EXPECT_EQ(back->mir.aliases()[1], m.aliases()[1]);
}

TEST(MirAlias, TheTextRefusesAnIllFormedAliasByName) {
    struct Case {
        char const* what;
        char const* line;
        char const* mustSay;
    };
    for (Case const c : {
             Case{"the invalid name", "  alias %0 = %1\n", "names symbol %0 as its name"},
             Case{"the invalid target", "  alias %3 = %0\n", "names symbol %0 as its target"},
             Case{"its own target", "  alias %3 = %3\n", "names itself as its target"},
             Case{"twice", "  alias %3 = %1\n  alias %3 = %2\n", "is stated twice"},
             Case{"a tentative mark", "  alias %3 [tentative] = %1\n", "unknown alias attribute 'tentative'"},
             Case{"a weak alias of no kind", "  alias %3 [weak] = %1\n", "states no weak-definition kind"},
             Case{"a target the text never introduces", "  alias %3 = %9\n", "alias names symbol %9"},
         }) {
        SCOPED_TRACE(c.what);
        std::string const  text = textWith(c.line);
        DiagnosticReporter rep;
        auto const         res = parseMir(text, CompilationUnitId{1}, rep);
        ASSERT_NE(res, nullptr);
        EXPECT_FALSE(res->ok) << text;
        EXPECT_TRUE(says(rep, c.mustSay)) << diagnosticsOf(rep) << "\n" << text;
    }
    // CONTROL: the same text with a well-formed alias is read.
    std::string const  text = textWith("  alias %3 [local] = %1\n");
    DiagnosticReporter rep;
    auto const         res = parseMir(text, CompilationUnitId{1}, rep);
    ASSERT_NE(res, nullptr);
    EXPECT_TRUE(res->ok) << diagnosticsOf(rep) << "\n" << text;
}

TEST(MirAlias, TheOneListOfNamedIdsNamesBothIdsOfAnAlias) {
    TypeInterner in{CompilationUnitId{1}};
    Mir const    m = moduleOfTwoAliases(in);
    std::vector<std::pair<std::uint32_t, MirSymbolMention>> seen;
    forEachSymbolIdNamed(m, [&](SymbolId s, MirSymbolMention mention, MirInstId) {
        if (mention == MirSymbolMention::Alias || mention == MirSymbolMention::AliasTarget)
            seen.emplace_back(s.v, mention);
    });
    std::vector<std::pair<std::uint32_t, MirSymbolMention>> const want{
        {3, MirSymbolMention::Alias}, {1, MirSymbolMention::AliasTarget},
        {4, MirSymbolMention::Alias}, {2, MirSymbolMention::AliasTarget}};
    EXPECT_EQ(seen, want) << "the one list names an alias's name and then its target";
    EXPECT_TRUE(mirSymbolMentionDefines(MirSymbolMention::Alias)) << "an alias's name is defined by the module";
    EXPECT_FALSE(mirSymbolMentionDefines(MirSymbolMention::AliasTarget));
}
