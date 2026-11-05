// P69 fold 3 (lane `xa`) — TWO UNITS' TENTATIVE DEFINITIONS OF ONE NAME ARE ONE OBJECT, IN THE MIR MERGE TOO.
// D-C-TENTATIVE-DEFINITIONS-OF-ONE-NAME-IN-TWO-UNITS-REFUSED
//
// `int v;` at file scope with no initializer is a TENTATIVE definition (C 6.9.2p2), and every reference toolchain
// that makes it a common folds two units' into one object, as large as the largest and aligned as the widest. The
// cross-unit MIR merge — the default whole-program build — knew nothing of it: a tentative definition was a strong
// one, and two units each holding `int v;` were refused as a redefinition (✔MEASURED 2026-10-12 on DSS before the
// fold, pe64 and ELF, where gcc 13.3, clang 18.1 and MinGW gcc 13.2 link the pair).
//
// The merge now hands the election the tentative definitions AS COMMONS, with the size of each — the layout
// engine's (`computeLayout`), asked in the definition's OWN unit under the layout rule that unit was analyzed
// under (`MergeCuInput::aggregateLayout`) — and raises the survivor to the widest alignment any of them asked for.
//   * Sizes are asked ONLY of a name two or more units define tentatively; a lone tentative definition wins or
//     loses by its class and is sized by nobody here.
//   * Two carriers whose units were analyzed under DIFFERENT layout rules have sizes that cannot be compared, and
//     the name is refused. ⓘ UNREACHABLE FROM ONE `compileOneTarget` BY CONSTRUCTION: every unit of one build is
//     analyzed under the one target's rule, and the driver hands each unit's own. It is pinned here on hand-built
//     inputs because it is the merge's own precondition, checked where it is relied on.
//   * A unit handed no rule at all is refused the same way, by name: no size is guessed.
// A module-private definition is no part of any of this: `static int v;` in two units is two objects.

#include "core/types/aggregate_layout.hpp"
#include "core/types/diagnostic_reporter.hpp"
#include "core/types/parse_diagnostic.hpp"
#include "core/types/symbol_attrs.hpp"
#include "core/types/target_schema.hpp"
#include "core/types/type_lattice/type_interner.hpp"
#include "core/types/type_lattice/type_lattice.hpp"
#include "core/types/type_lattice/type_layout.hpp"
#include "link/object_format_schema.hpp"
#include "mir/merge/mir_merge.hpp"
#include "mir/mir.hpp"
#include "mir/mir_node.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace dss;

namespace {

struct Loaded {
    std::shared_ptr<TargetSchema const>       target;
    std::shared_ptr<ObjectFormatSchema const> format;
};
[[nodiscard]] Loaded load() {
    Loaded l;
    auto   t = TargetSchema::loadShipped("x86_64");
    auto   f = ObjectFormatSchema::loadShipped("elf64-x86_64-linux-exec");
    EXPECT_TRUE(t.has_value() && f.has_value());
    if (t.has_value()) l.target = *t;
    if (f.has_value()) l.format = *f;
    return l;
}

[[nodiscard]] std::string diagnosticsOf(DiagnosticReporter const& rep) {
    std::string s;
    for (auto const& d : rep.all()) s += "\n  " + std::string{diagnosticCodeName(d.code)} + ": " + d.actual;
    return s;
}

[[nodiscard]] std::string nameOfSeven(SymbolId s) { return s.v == 7 ? std::string{"shared"} : std::string{}; }

// What one unit says of the name `shared` (symbol 7): a global of `elements` 32-bit integers (1: a plain `int`),
// tentative or defined with the value 5, external or module-private, at a stated alignment (0: the type's own).
struct Says {
    std::int64_t  elements  = 1;
    bool          tentative = true;
    SymbolBinding binding   = SymbolBinding::Global;
    std::uint32_t alignment = 0;
};

struct Unit {
    TypeInterner interner;
    Mir          mir;
    explicit Unit(std::uint32_t cu) : interner{CompilationUnitId{cu}} {}
};

void build(Unit& unit, Says const& says) {
    TypeId const i32 = unit.interner.primitive(TypeKind::I32);
    TypeId const ty  = says.elements == 1 ? i32 : unit.interner.array(i32, says.elements);
    MirBuilder   b;
    MirLinkage   linkage{says.binding, SymbolVisibility::Default};
    linkage.tentative     = says.tentative;
    std::uint32_t literal = UINT32_MAX;
    if (!says.tentative) {
        MirLiteralValue v;
        v.value = std::int64_t{5};
        v.core  = TypeKind::I32;
        literal = b.literalPoolAdd(v);
    }
    (void)b.addGlobal(ty, SymbolId{7}, literal, MirFuncId{}, linkage, /*isConst=*/false, mirThreadStorageOf(false),
                      says.alignment);
    unit.mir = std::move(b).finish();
}

struct Merged {
    std::optional<MergedMirModule> module;
    DiagnosticReporter             rep;
};

// Merges two units, each under the layout rule given it (nullopt: the unit is handed none).
void merge(Merged& out, Loaded const& L, Says const& first, Says const& second,
           std::optional<AggregateLayoutParams> firstRule, std::optional<AggregateLayoutParams> secondRule) {
    Unit a{1}, b{2};
    build(a, first);
    build(b, second);
    std::vector<MergeCuInput> cus(2);
    cus[0].mir = &a.mir; cus[0].interner = &a.interner; cus[0].nameOf = nameOfSeven; cus[0].aggregateLayout = firstRule;
    cus[1].mir = &b.mir; cus[1].interner = &b.interner; cus[1].nameOf = nameOfSeven; cus[1].aggregateLayout = secondRule;
    out.module = mergeCuMirs(cus, TypeLattice{CompilationUnitId{99}}, {}, *L.format, out.rep);
}

// The definitions of `shared` in a merged module.
[[nodiscard]] std::vector<MirGlobalId> definitionsOfShared(MergedMirModule const& m) {
    std::vector<MirGlobalId> found;
    for (std::uint32_t i = 0; i < m.mir.moduleGlobalCount(); ++i) {
        MirGlobalId const g  = m.mir.globalAt(i);
        auto const        it = m.symbolNames.find(m.mir.globalSymbol(g).v);
        if (it != m.symbolNames.end() && it->second == "shared") found.push_back(g);
    }
    return found;
}

[[nodiscard]] std::optional<std::uint64_t> sizeOf(Loaded const& L, MergedMirModule const& m, MirGlobalId g) {
    auto const lay = computeLayout(m.mir.globalType(g), m.host.interner(), L.target->aggregateLayout(),
                                   L.format->dataModel());
    if (!lay.has_value()) return std::nullopt;
    return static_cast<std::uint64_t>(lay->size);
}

[[nodiscard]] bool refusedSaying(DiagnosticReporter const& rep, std::string_view sentence) {
    for (auto const& d : rep.all()) {
        if (d.code == DiagnosticCode::K_CrossCuMergeUnsupported
            && d.actual.find("symbol 'shared' is defined tentatively by 2 compilation units") != std::string::npos
            && d.actual.find(sentence) != std::string::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace

TEST(MirMergeTentativeFold, TwoTentativeDefinitionsAreOneObjectTheLargerInEitherOrder) {
    auto const L = load();
    ASSERT_TRUE(L.target && L.format);
    auto const rule = L.target->aggregateLayout();
    struct Order {
        char const*  what;
        std::int64_t first, second;
    };
    for (auto const& order : {Order{"the larger definition's unit first", 10, 1},
                              Order{"the smaller definition's unit first", 1, 10},
                              Order{"two of one size", 1, 1}}) {
        SCOPED_TRACE(order.what);
        Merged m;
        merge(m, L, Says{order.first}, Says{order.second}, rule, rule);
        ASSERT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
        ASSERT_TRUE(m.module.has_value());
        auto const defs = definitionsOfShared(*m.module);
        ASSERT_EQ(defs.size(), 1u) << "two tentative definitions of one name are ONE object";
        EXPECT_TRUE(m.module->mir.globalIsCommon(defs[0])) << "and it is still a common: a later link may fold it again";
        EXPECT_EQ(m.module->mir.globalInitLiteralIndex(defs[0]), UINT32_MAX) << "a common states no initializer";
        EXPECT_EQ(sizeOf(L, *m.module, defs[0]),
                  std::optional<std::uint64_t>{4u * static_cast<std::uint64_t>(std::max(order.first, order.second))})
            << "the object is as large as the largest definition of it";
    }
}

// ✔MEASURED 2026-10-12 on the references: `_Alignas(4096) char v;` beside `char v[40];` (`-fcommon`), both link
// orders, read from the image's symbol and from the program (both units see one address, a multiple of 4096, with
// 40 bytes there):
//   ELF  gcc 13.3.0 and clang 18.1.3 objects, each under GNU ld 2.42 and ld.lld 18.1.3 — 8 of 8: one object of
//        40 bytes at a multiple of 4096.
//   PE   MinGW gcc 13.2.0 under GNU ld 2.42 — 2 of 2 (COFF states a common's alignment in the object's
//        directives, `-aligncomm:"v",12` beside `-aligncomm:"v",5`).
//   THE CONTROL, the large definition alone: its address is NOT a multiple of 4096 (ELF 4 of 4, PE 1 of 1).
//   Mach-O: not measured.
// So the one object is the largest definition's size at the widest alignment ANY definition asked for — the small
// definition's, here.
TEST(MirMergeTentativeFold, TheOneObjectTakesTheWidestAlignmentAnyDefinitionAskedFor) {
    auto const L = load();
    ASSERT_TRUE(L.target && L.format);
    auto const rule = L.target->aggregateLayout();
    for (bool const smallFirst : {true, false}) {
        SCOPED_TRACE(smallFirst ? "the small, wide-aligned definition's unit first"
                                : "the large definition's unit first");
        Says const small{1, true, SymbolBinding::Global, 64};
        Says const large{10, true, SymbolBinding::Global, 0};
        Merged     m;
        merge(m, L, smallFirst ? small : large, smallFirst ? large : small, rule, rule);
        ASSERT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
        ASSERT_TRUE(m.module.has_value());
        auto const defs = definitionsOfShared(*m.module);
        ASSERT_EQ(defs.size(), 1u);
        EXPECT_EQ(sizeOf(L, *m.module, defs[0]), std::optional<std::uint64_t>{40u});
        EXPECT_EQ(m.module->mir.globalAlignmentBytes(defs[0]), 64u)
            << "the survivor is the larger definition, and it is aligned as the widest any definition asked";
    }
    // The control: with no definition asking for more than its type's, the survivor states nothing it did not.
    Merged m;
    merge(m, L, Says{1}, Says{10}, rule, rule);
    ASSERT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
    ASSERT_TRUE(m.module.has_value());
    auto const defs = definitionsOfShared(*m.module);
    ASSERT_EQ(defs.size(), 1u);
    EXPECT_EQ(m.module->mir.globalAlignmentBytes(defs[0]), 0u) << "the type's own alignment: none is stated";
}

// ⓘ UNREACHABLE FROM ONE `compileOneTarget` BY CONSTRUCTION — one build analyzes every unit under one target's
// layout rule and the driver hands each unit its own. Hand-built inputs are the only way to reach the refusal,
// and it is the merge's own precondition: a size measured under one rule is not comparable with one measured
// under another.
TEST(MirMergeTentativeFold, TwoUnitsAnalyzedUnderDifferentLayoutRulesAreRefusedByName) {
    auto const L = load();
    ASSERT_TRUE(L.target && L.format);
    auto const rule  = L.target->aggregateLayout();
    auto       other = rule;
    other.maxAlignment = rule.maxAlignment == 1u ? 2u : 1u;   // any rule that is not the first one
    Merged m;
    merge(m, L, Says{1}, Says{10}, rule, other);
    EXPECT_TRUE(refusedSaying(m.rep, "CU #1 and CU #2 were analyzed under different layout rules"))
        << diagnosticsOf(m.rep);
    // The control: the same two units under the one rule fold.
    Merged same;
    merge(same, L, Says{1}, Says{10}, rule, rule);
    EXPECT_EQ(same.rep.errorCount(), 0u) << diagnosticsOf(same.rep);
}

TEST(MirMergeTentativeFold, AUnitHandedNoLayoutRuleIsRefusedByNameAndNoSizeIsGuessed) {
    auto const L = load();
    ASSERT_TRUE(L.target && L.format);
    auto const rule = L.target->aggregateLayout();
    struct Case {
        char const*                          what;
        std::optional<AggregateLayoutParams> first, second;
        char const*                          sentence;
    };
    for (auto const& c : {Case{"the second unit has none", rule, std::nullopt,
                               "the definition in CU #2 cannot be sized: the merge was handed no layout rule"},
                          Case{"the first unit has none", std::nullopt, rule,
                               "the definition in CU #1 cannot be sized: the merge was handed no layout rule"}}) {
        SCOPED_TRACE(c.what);
        Merged m;
        merge(m, L, Says{1}, Says{10}, c.first, c.second);
        EXPECT_TRUE(refusedSaying(m.rep, c.sentence)) << diagnosticsOf(m.rep);
    }
}

// A size is asked only where the fold needs one. A tentative definition with no sibling — alone, or beside a
// definition that is not tentative — wins or loses by its class, and a unit that was handed no layout rule is
// then no obstacle.
TEST(MirMergeTentativeFold, ALoneTentativeDefinitionIsSizedByNobody) {
    auto const L = load();
    ASSERT_TRUE(L.target && L.format);
    for (bool const tentativeFirst : {true, false}) {
        SCOPED_TRACE(tentativeFirst ? "the tentative definition's unit first" : "the strong definition's unit first");
        Says const tentative{10, true};
        Says const strong{1, false};
        Merged     m;
        merge(m, L, tentativeFirst ? tentative : strong, tentativeFirst ? strong : tentative, std::nullopt, std::nullopt);
        ASSERT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
        ASSERT_TRUE(m.module.has_value());
        auto const defs = definitionsOfShared(*m.module);
        ASSERT_EQ(defs.size(), 1u);
        EXPECT_FALSE(m.module->mir.globalIsCommon(defs[0])) << "a common yields to a strong definition of its name";
        EXPECT_NE(m.module->mir.globalInitLiteralIndex(defs[0]), UINT32_MAX) << "the strong definition's value is kept";
    }
}

// `static int shared;` in two units is two objects: a module-private definition is no part of the election, and
// the merge folds nothing by a name no linker resolves.
TEST(MirMergeTentativeFold, ModulePrivateDefinitionsOfOneNameStayTwoObjects) {
    auto const L = load();
    ASSERT_TRUE(L.target && L.format);
    auto const rule = L.target->aggregateLayout();
    // A module-private global states no tentative mark — the linkage rule refuses one, because the link folds
    // tentative definitions of an EXTERNAL name — so what a unit makes of `static int shared;` is a definition of
    // its own, as it is here (with a value, which changes nothing a merge reads).
    Says const priv{1, false, SymbolBinding::Local};
    Merged     m;
    merge(m, L, priv, priv, rule, rule);
    ASSERT_EQ(m.rep.errorCount(), 0u) << diagnosticsOf(m.rep);
    ASSERT_TRUE(m.module.has_value());
    EXPECT_EQ(m.module->mir.moduleGlobalCount(), 2u) << "two units, two objects";
    for (std::uint32_t i = 0; i < m.module->mir.moduleGlobalCount(); ++i) {
        EXPECT_FALSE(m.module->mir.globalIsCommon(m.module->mir.globalAt(i)));
    }
}
