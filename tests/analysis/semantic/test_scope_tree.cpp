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
    EXPECT_EQ(rootBindings[0].first, "x");
    EXPECT_EQ(rootBindings[0].second.v, 10u);
    auto innerBindings = st.bindingsOf(inner);
    ASSERT_EQ(innerBindings.size(), 1u);
    EXPECT_EQ(innerBindings[0].first, "y");
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
