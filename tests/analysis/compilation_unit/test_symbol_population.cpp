// CU3 end-to-end test for the minimal declaration-symbol populate walk. It
// drives `populateDeclarationSymbols` over a multi-file toy CU and pins that
// every declaration name node — and only those — gets a distinct CU-scoped
// SymbolId, exercising `UnitAttribute<SymbolId>` across trees. (The walk binds
// both functionDecl and varDecl names; the toy schema only has varDecl, so the
// fixtures here exercise the varDecl path.)

#include "analysis/compilation_unit/compilation_unit.hpp"
#include "analysis/compilation_unit/symbol_population.hpp"
#include "analysis/compilation_unit/unit_attribute.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_views.hpp"

#include "analysis/compilation_unit/toy_cu_fixture.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace {

using namespace dss;
using dss::cu_test::loadToySchema;
using dss::cu_test::makeToyUnit;

} // namespace

TEST(SymbolPopulation, BindsEveryDeclNameAcrossFiles) {
    // 3 var decls total: a, b in file 0; c in file 1. The RHS identifiers
    // (x, y, z) are uses, not declarations — they must NOT be bound.
    auto cu = makeToyUnit({"var a = x; var b = y;", "var c = z;"});

    auto symbols = populateDeclarationSymbols(cu);
    EXPECT_EQ(symbols.unit(), cu.id());
    EXPECT_EQ(symbols.size(), 3u);

    std::unordered_map<std::uint32_t, Tree const*> byTag;
    for (Tree const& tree : cu.trees()) byTag.emplace(tree.id().v, &tree);

    std::multiset<std::string> names;
    std::set<std::uint32_t>     symbolValues;
    symbols.forEach([&](TreeId tree, NodeId node, SymbolId sym) {
        Tree const* owner = byTag.at(tree.v);
        // Every bound node is the Identifier name of a declaration.
        ASSERT_TRUE(IdentifierView::from(*owner, node).has_value());
        names.insert(std::string{owner->text(node)});
        symbolValues.insert(sym.v);
    });

    std::multiset<std::string> const expectedNames{"a", "b", "c"};
    EXPECT_EQ(names, expectedNames);
    // Distinct, dense, CU-scoped from 1 (0 is InvalidSymbol). Assignment order
    // (which name gets 1 vs 2 vs 3) is intentionally unspecified, so this pins
    // the set of values, not a name→id mapping.
    std::set<std::uint32_t> const expectedSymbols{1u, 2u, 3u};
    EXPECT_EQ(symbolValues, expectedSymbols);
}

TEST(SymbolPopulation, UsesAndNonDeclNodesAreUnbound) {
    auto cu = makeToyUnit({"var a = x;"});
    auto symbols = populateDeclarationSymbols(cu);
    ASSERT_EQ(symbols.size(), 1u);

    Tree const& tree = cu.trees()[0];
    // The root is not a declaration name node, so it carries no symbol.
    EXPECT_FALSE(symbols.has(tree.root()));
    EXPECT_EQ(symbols.tryGet(tree.root()), nullptr);
}

TEST(SymbolPopulation, EmptyUnitProducesNoSymbols) {
    UnitBuilder builder{loadToySchema()};
    auto cu = std::move(builder).finish();   // zero trees — valid (CU1)
    auto symbols = populateDeclarationSymbols(cu);
    EXPECT_EQ(symbols.unit(), cu.id());
    EXPECT_TRUE(symbols.empty());
}
