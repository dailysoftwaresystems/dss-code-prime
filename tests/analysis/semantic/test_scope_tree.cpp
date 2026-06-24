#include "analysis/semantic/scope_tree.hpp"

#include <gtest/gtest.h>

using namespace dss;

// Slot 0 is the InvalidScope sentinel; slot 1 is the CU root scope. The
// tree must expose `root()` valid from construction.
TEST(ScopeTree, ConstructedHasRoot) {
    ScopeTree st;
    EXPECT_TRUE(st.root().valid());
    EXPECT_EQ(st.root().v, 1u);
    ASSERT_EQ(st.scopes().size(), 2u) << "slot 0 sentinel + slot 1 root";
    EXPECT_FALSE(st.scopes()[0].parent.valid());
    EXPECT_FALSE(st.scopes()[1].parent.valid());
}

// Pushing a child increments the scope-id and records the parent link
// + the anchor metadata on the new record.
TEST(ScopeTree, PushScopeRecordsParentAnchorAndTree) {
    ScopeTree st;
    const auto parent = st.root();
    const NodeId anchor{42, /*tag=*/7};
    const TreeId tree{7};
    auto const child = st.pushScope(parent, anchor, tree);
    ASSERT_TRUE(child.valid());
    EXPECT_EQ(child.v, 2u);
    EXPECT_EQ(st.scopes()[child.v].parent.v, parent.v);
    EXPECT_EQ(st.scopes()[child.v].anchor.v, anchor.v);
    EXPECT_EQ(st.scopes()[child.v].tree.v, tree.v);
    // Parent's children vector also tracks the child id.
    ASSERT_EQ(st.scopes()[parent.v].children.size(), 1u);
    EXPECT_EQ(st.scopes()[parent.v].children[0].v, child.v);
}

// Same-scope bind: first call returns InvalidSymbol (success); a second
// call with the same name returns the prior SymbolId (redecl).
TEST(ScopeTree, BindRejectsSameScopeDuplicate) {
    ScopeTree st;
    const auto root = st.root();
    EXPECT_FALSE(st.bind(root, "x", SymbolId{1}).valid());
    auto prior = st.bind(root, "x", SymbolId{2});
    ASSERT_TRUE(prior.valid());
    EXPECT_EQ(prior.v, 1u) << "the first SymbolId is reported back so the analyzer "
                              "can attach a related-location to the original decl";
}

// Parent-chain lookup walks innermost-first: a shadowing inner binding
// wins over an outer one.
TEST(ScopeTree, LookupShadows) {
    ScopeTree st;
    const auto root  = st.root();
    const auto inner = st.pushScope(root, NodeId{1, 1}, TreeId{1});

    EXPECT_FALSE(st.bind(root,  "x", SymbolId{10}).valid());
    EXPECT_FALSE(st.bind(inner, "x", SymbolId{20}).valid());

    auto fromInner = st.lookup(inner, "x");
    ASSERT_TRUE(fromInner.valid());
    EXPECT_EQ(fromInner.v, 20u) << "innermost binding wins";
    auto fromRoot  = st.lookup(root, "x");
    ASSERT_TRUE(fromRoot.valid());
    EXPECT_EQ(fromRoot.v, 10u);
}

// `bindingsOf` snapshots exactly the same-scope bindings (no parent
// walk) — the enumeration the cross-tree import-injection step uses.
TEST(ScopeTree, BindingsOfSnapshotsSameScopeOnly) {
    ScopeTree st;
    const auto root  = st.root();
    const auto inner = st.pushScope(root, NodeId{1, 1}, TreeId{1});
    EXPECT_FALSE(st.bind(root,  "x", SymbolId{10}).valid());
    EXPECT_FALSE(st.bind(inner, "y", SymbolId{20}).valid());
    auto rootBindings = st.bindingsOf(root);
    ASSERT_EQ(rootBindings.size(), 1u) << "parent walk excluded";
    EXPECT_EQ(std::get<0>(rootBindings[0]), "x");
    EXPECT_EQ(std::get<1>(rootBindings[0]), SymbolNamespace::Ordinary);
    EXPECT_EQ(std::get<2>(rootBindings[0]).v, 10u);
    auto innerBindings = st.bindingsOf(inner);
    ASSERT_EQ(innerBindings.size(), 1u);
    EXPECT_EQ(std::get<0>(innerBindings[0]), "y");
}

// C 6.2.3 tag namespace: a Tag binding and an Ordinary binding of the SAME
// name coexist in one scope without collision, and lookup honors the
// namespace. bindingsOf enumerates both. RED-ON-DISABLE: without the
// tagBindings map, the second bind returns the prior Ordinary SymbolId
// (collision) and this test's first EXPECT_FALSE fails.
TEST(ScopeTree, TagAndOrdinaryNamespacesAreIndependent) {
    ScopeTree st;
    const auto root = st.root();
    EXPECT_FALSE(st.bind(root, "Pair", SymbolId{10},
                         SymbolNamespace::Ordinary).valid());
    // Same name, Tag namespace — must NOT collide with the Ordinary binding.
    EXPECT_FALSE(st.bind(root, "Pair", SymbolId{20},
                         SymbolNamespace::Tag).valid())
        << "a tag and an ordinary symbol of the same name must not collide";
    auto ord = st.lookup(root, "Pair", SymbolNamespace::Ordinary);
    auto tag = st.lookup(root, "Pair", SymbolNamespace::Tag);
    ASSERT_TRUE(ord.valid());
    ASSERT_TRUE(tag.valid());
    EXPECT_EQ(ord.v, 10u);
    EXPECT_EQ(tag.v, 20u);
    // A same-namespace redeclaration still collides (returns the prior id).
    EXPECT_EQ(st.bind(root, "Pair", SymbolId{30}, SymbolNamespace::Tag).v, 20u);
    EXPECT_EQ(st.bindingsOf(root).size(), 2u) << "both namespaces enumerated";
}

// MF-2 cross-tree substrate: `bindingsOf` carries each binding's namespace and
// `injectBinding(..., ns)` re-injects into the SAME namespace — the exact
// primitive the cross-tree import-injection conflict scan re-keys on. A tag
// `Foo` in one scope re-injects as a tag into another scope (resolved only via
// the Tag lookup, never Ordinary), proving the namespace survives the round
// trip. RED-ON-DISABLE: drop the namespace from bindingsOf/injectBinding and a
// tag would re-inject Ordinary — the Tag lookup below would miss.
TEST(ScopeTree, BindingsOfCarriesNamespaceForCrossTreeReKey) {
    ScopeTree st;
    const auto src = st.pushScope(st.root(), NodeId{1, 1}, TreeId{1});
    const auto dst = st.pushScope(st.root(), NodeId{2, 1}, TreeId{2});
    // `Foo` exists as BOTH a tag and an ordinary symbol in the source scope.
    EXPECT_FALSE(st.bind(src, "Foo", SymbolId{11}, SymbolNamespace::Tag).valid());
    EXPECT_FALSE(
        st.bind(src, "Foo", SymbolId{22}, SymbolNamespace::Ordinary).valid());

    // Replay the cross-tree injection: copy each (name, ns, sym) into dst,
    // preserving the namespace — exactly what the analyzer's re-keyed scan does.
    for (auto const& [name, ns, sym] : st.bindingsOf(src)) {
        st.injectBinding(dst, std::string{name}, sym, ns);
    }

    // Each namespace resolved independently in the target scope.
    auto tag = st.lookup(dst, "Foo", SymbolNamespace::Tag);
    auto ord = st.lookup(dst, "Foo", SymbolNamespace::Ordinary);
    ASSERT_TRUE(tag.valid());
    ASSERT_TRUE(ord.valid());
    EXPECT_EQ(tag.v, 11u) << "the tag re-injected as a tag";
    EXPECT_EQ(ord.v, 22u) << "the ordinary symbol re-injected ordinary";
}

// `injectBinding` copies a binding into a scope with no redecl check —
// the mechanism cross-file visibility injection rides on. After
// injection a `lookup` resolves it.
TEST(ScopeTree, InjectBindingMakesNameVisible) {
    ScopeTree st;
    const auto a = st.pushScope(st.root(), NodeId{1, 1}, TreeId{1});
    const auto b = st.pushScope(st.root(), NodeId{2, 1}, TreeId{2});
    EXPECT_FALSE(st.bind(a, "shared", SymbolId{7}).valid());
    EXPECT_FALSE(st.lookup(b, "shared").valid()) << "isolated before injection";
    st.injectBinding(b, "shared", SymbolId{7});
    auto found = st.lookup(b, "shared");
    ASSERT_TRUE(found.valid());
    EXPECT_EQ(found.v, 7u);
}

// Miss returns InvalidSymbol cleanly (no abort) so callers can emit
// S_UndeclaredIdentifier.
TEST(ScopeTree, LookupMissesReturnsInvalid) {
    ScopeTree st;
    EXPECT_FALSE(st.lookup(st.root(), "nope").valid());
    EXPECT_TRUE(st.bindingsOf(st.root()).empty());
}

// release() consumes the tree into a vector of records — sized to the
// scope count.
TEST(ScopeTree, ReleaseHandsOverRecords) {
    ScopeTree st;
    st.pushScope(st.root(), NodeId{1, 1}, TreeId{1});
    auto records = std::move(st).release();
    ASSERT_EQ(records.size(), 3u) << "sentinel + root + one child";
}
