#include "core/types/decl_prefix_strip.hpp"

#include "core/types/rule_id.hpp"
#include "core/types/semantic_config.hpp"
#include "core/types/source_span.hpp"
#include "core/types/strong_ids.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_node.hpp"

#include "raw_tree_builder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

// D-DECL-PREFIX-STRIP-SHARED-HELPER closing test: pins the SHARED
// declaration-specifier-prefix strip helpers (`declRoleChildren` /
// `descendVisibleDecl` / `specifierPrefixChild`,
// core/types/decl_prefix_strip.hpp) directly against a SYNTHETIC tree —
// no schema, no language; rule names are non-shipped, so this is the
// engine-generic contract every consumer (semantic analyzer,
// cst_const_eval, CST→HIR lowering) now shares. The four strip cases:
//   (a) prefix declared + first visible child IS that rule  → stripped
//   (b) no prefix declared                                  → untouched
//   (c) prefix declared but first child is a DIFFERENT rule → untouched
//   (d) prefix declared but first child is a Token          → untouched
// Plus the descend contract: FIRST step prefix-stripped, LATER steps raw.

namespace dss {
namespace {

using dss::tests::RawTreeBuilder;

// Shared synthetic shape (`ws` is EmptySpace, excluded from "visible"):
//
//   decl#1 ─ ws#2    (EmptySpace Token)
//          ─ first#3 (the would-be prefix slot; Internal or Token per case)
//          ─ type#4  (Token)
//          ─ name#5  (Token)
//          ─ tail#6  (Internal, tailRule)
//                └ inner#7 (Internal, prefixRule)  ← same rule as a prefix!
//                └ leaf#8  (Token)
//
// `tail`'s leading `inner` child deliberately reuses the prefix RULE so the
// descend test can prove later path steps are RAW (never re-stripped).
// NodeId equality compares `.v` only (arenaTag is provenance metadata), so
// untagged literals compare equal to the builder-returned ids.
struct SynthTree {
    RuleId declRule;
    RuleId prefixRule;   // interned "specs"
    RuleId otherRule;    // interned "notSpecs"
    RuleId tailRule;
    Tree   tree;

    NodeId decl{1};
    NodeId ws{2};
    NodeId first{3};
    NodeId type{4};
    NodeId name{5};
    NodeId tail{6};
    NodeId inner{7};
    NodeId leaf{8};
};

// `firstIsInternal` ⇒ node #3 is Internal of (`firstAsPrefix` ? "specs" :
// "notSpecs"); otherwise it is a plain Token (case d).
[[nodiscard]] SynthTree makeTree(bool firstIsInternal, bool firstAsPrefix) {
    RawTreeBuilder rb{"synthetic", "<decl-prefix-strip>"};
    RuleId const declRule   = rb.internRule("sdecl");
    RuleId const prefixRule = rb.internRule("specs");
    RuleId const otherRule  = rb.internRule("notSpecs");
    RuleId const tailRule   = rb.internRule("tail");
    // decl#1
    rb.addNode(NodeKind::Internal, declRule, SourceSpan::of(0, 9),
               NodeFlags::None, /*parent=*/InvalidNode,
               /*children=*/{NodeId{2}, NodeId{3}, NodeId{4}, NodeId{5},
                             NodeId{6}});
    // ws#2 — EmptySpace token: must be invisible to the helpers.
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::EmptySpace, NodeId{1}, {}, SchemaTokenId{1});
    // first#3 — the would-be prefix slot.
    if (firstIsInternal) {
        rb.addNode(NodeKind::Internal, firstAsPrefix ? prefixRule : otherRule,
                   SourceSpan::of(1, 2), NodeFlags::None, NodeId{1});
    } else {
        rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(1, 2),
                   NodeFlags::None, NodeId{1}, {}, SchemaTokenId{2});
    }
    // type#4 / name#5
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(2, 3),
               NodeFlags::None, NodeId{1}, {}, SchemaTokenId{3});
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(3, 4),
               NodeFlags::None, NodeId{1}, {}, SchemaTokenId{4});
    // tail#6 with a LEADING prefix-rule child (#7) + a token (#8).
    rb.addNode(NodeKind::Internal, tailRule, SourceSpan::of(4, 9),
               NodeFlags::None, NodeId{1}, {NodeId{7}, NodeId{8}});
    rb.addNode(NodeKind::Internal, prefixRule, SourceSpan::of(4, 6),
               NodeFlags::None, NodeId{6});
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(6, 9),
               NodeFlags::None, NodeId{6}, {}, SchemaTokenId{5});
    return SynthTree{declRule, prefixRule, otherRule, tailRule,
                     std::move(rb).finish(NodeId{1})};
}

[[nodiscard]] DeclarationRule declWithPrefix(RuleId rule,
                                             std::optional<RuleId> prefix) {
    DeclarationRule d{};
    d.rule = rule;
    d.specifierPrefixRule = prefix;
    return d;
}

// (a) prefix declared AND first visible child is an Internal node of that
// rule → the leading child is dropped; positional indices shift past it.
// The EmptySpace token before the prefix must NOT defeat the strip.
TEST(DeclPrefixStrip, PrefixPresentIsStripped) {
    auto const s    = makeTree(/*firstIsInternal=*/true, /*firstAsPrefix=*/true);
    auto const decl = declWithPrefix(s.declRule, s.prefixRule);

    auto const kids = declRoleChildren(s.tree, s.decl, decl);
    ASSERT_EQ(kids.size(), 3u) << "prefix stripped: type, name, tail remain";
    EXPECT_EQ(kids[0], s.type);
    EXPECT_EQ(kids[1], s.name);
    EXPECT_EQ(kids[2], s.tail);

    // The stripped prefix stays reachable through the accessor.
    EXPECT_EQ(specifierPrefixChild(s.tree, s.decl, decl), s.first);
}

// (b) no prefix declared → untouched, even though the first visible child
// HAPPENS to be an Internal node (of any rule). The accessor is invalid.
TEST(DeclPrefixStrip, NoPrefixDeclaredIsUntouched) {
    auto const s    = makeTree(/*firstIsInternal=*/true, /*firstAsPrefix=*/true);
    auto const decl = declWithPrefix(s.declRule, std::nullopt);

    auto const kids = declRoleChildren(s.tree, s.decl, decl);
    ASSERT_EQ(kids.size(), 4u) << "no specifierPrefixRule ⇒ plain visible "
                                  "children (EmptySpace still excluded)";
    EXPECT_EQ(kids[0], s.first);
    EXPECT_EQ(kids[1], s.type);
    EXPECT_EQ(kids[2], s.name);
    EXPECT_EQ(kids[3], s.tail);
    EXPECT_FALSE(specifierPrefixChild(s.tree, s.decl, decl).valid());
}

// (c) prefix declared but the first visible child is an Internal node of a
// DIFFERENT rule → untouched (no strip, accessor invalid).
TEST(DeclPrefixStrip, DifferentRuleFirstChildIsUntouched) {
    auto const s    = makeTree(/*firstIsInternal=*/true, /*firstAsPrefix=*/false);
    auto const decl = declWithPrefix(s.declRule, s.prefixRule);

    auto const kids = declRoleChildren(s.tree, s.decl, decl);
    ASSERT_EQ(kids.size(), 4u);
    EXPECT_EQ(kids[0], s.first) << "a non-prefix Internal first child stays";
    EXPECT_FALSE(specifierPrefixChild(s.tree, s.decl, decl).valid());
}

// (d) prefix declared but the first visible child is a TOKEN → untouched
// (the strip requires NodeKind::Internal; a leading keyword token is content).
TEST(DeclPrefixStrip, TokenFirstChildIsUntouched) {
    auto const s    = makeTree(/*firstIsInternal=*/false, /*firstAsPrefix=*/false);
    auto const decl = declWithPrefix(s.declRule, s.prefixRule);

    auto const kids = declRoleChildren(s.tree, s.decl, decl);
    ASSERT_EQ(kids.size(), 4u);
    EXPECT_EQ(kids[0], s.first) << "a Token first child is never stripped";
    EXPECT_FALSE(specifierPrefixChild(s.tree, s.decl, decl).valid());
}

// descendVisibleDecl: the FIRST step resolves against the prefix-stripped
// role children; LATER steps use RAW visible children — `tail`'s own leading
// prefix-RULE child (#7) is NOT re-stripped, so index 0 under `tail` lands on
// it. Also pins empty-path ⇒ start, and out-of-range ⇒ InvalidNode.
TEST(DeclPrefixStrip, DescendFirstStepStrippedLaterStepsRaw) {
    auto const s    = makeTree(/*firstIsInternal=*/true, /*firstAsPrefix=*/true);
    auto const decl = declWithPrefix(s.declRule, s.prefixRule);

    // First step prefix-free: [0]=type (NOT the prefix), [2]=tail.
    std::array<std::uint32_t, 1> const p0{0u};
    EXPECT_EQ(descendVisibleDecl(s.tree, s.decl, p0, decl), s.type);

    // Two-step: [2]=tail, then RAW [0] = the nested prefix-rule node — the
    // RED lever for a future "re-strip every step" regression (re-stripping
    // would land on leaf#8 instead).
    std::array<std::uint32_t, 2> const p20{2u, 0u};
    EXPECT_EQ(descendVisibleDecl(s.tree, s.decl, p20, decl), s.inner);
    std::array<std::uint32_t, 2> const p21{2u, 1u};
    EXPECT_EQ(descendVisibleDecl(s.tree, s.decl, p21, decl), s.leaf);

    // Empty path returns the start node itself.
    EXPECT_EQ(descendVisibleDecl(s.tree, s.decl, {}, decl), s.decl);

    // Out-of-range: first step (3 role kids) and a later step both fail to
    // InvalidNode, never wrap or fall back.
    std::array<std::uint32_t, 1> const pOver{3u};
    EXPECT_FALSE(descendVisibleDecl(s.tree, s.decl, pOver, decl).valid());
    std::array<std::uint32_t, 2> const pDeepOver{2u, 2u};
    EXPECT_FALSE(descendVisibleDecl(s.tree, s.decl, pDeepOver, decl).valid());
}

// With NO prefix declared, the descend first step counts the leading
// Internal child like any other — [0] IS that child (the case-(b) view).
TEST(DeclPrefixStrip, DescendNoPrefixDeclaredUsesRawFirstStep) {
    auto const s    = makeTree(/*firstIsInternal=*/true, /*firstAsPrefix=*/true);
    auto const decl = declWithPrefix(s.declRule, std::nullopt);

    std::array<std::uint32_t, 1> const p0{0u};
    EXPECT_EQ(descendVisibleDecl(s.tree, s.decl, p0, decl), s.first);
}

} // namespace
} // namespace dss
