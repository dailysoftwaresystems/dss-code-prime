#include "core/types/grammar_schema.hpp"
#include "core/types/rule_id.hpp"
#include "core/types/source_buffer.hpp"
#include "core/types/token.hpp"
#include "core/types/tree.hpp"
#include "core/types/tree_builder.hpp"
#include "core/types/tree_cursor.hpp"
#include "core/types/tree_node.hpp"
#include "raw_tree_builder.hpp"
#include "toy_harness.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace dss;
using dss::tests::RawTreeBuilder;
using dss::tests::ToyHarness;

namespace {

constexpr std::string_view kToyConfig = R"JSON({
  "dssSchemaVersion": 1,
  "language": { "name": "CursorToy", "version": "0.1.0", "fileExtensions": [".t"] },
  "tokens": {
    " ":  [{ "kind": "Whitespace",   "flags": ["EmptySpace"] }],
    ";":  [{ "kind": "EndCommand" }],
    "=":  [{ "kind": "AssignmentOperator" }]
  },
  "keywords": [
    { "word": "var", "kind": "VarKeyword" }
  ],
  "shapes": {
    "root":       { "sequence": [{ "repeat": "statement" }] },
    "statement":  { "sequence": ["varDecl"] },
    "varDecl":    { "sequence": ["VarKeyword", "Identifier", "AssignmentOperator", "Identifier", "EndCommand"] }
  }
})JSON";

// Thin shim — delegates to the shared `ToyHarness` with this file's
// `kToyConfig`.
struct Harness : public ToyHarness {
    static Harness make(std::string sourceText) {
        Harness h;
        static_cast<ToyHarness&>(h) = ToyHarness::make(std::move(sourceText), kToyConfig, "<cursor-test>");
        return h;
    }
};

// Sample tree for "var x = y;" — 5 significant leaves + 3 whitespace
// leaves, in this exact textual order. Tests below rely on this
// specific shape; the helper below makes the dependency explicit.
Tree buildSampleTree(Harness& h) {
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    auto stmt = b.open(h.schema->rules().find("statement"));
    auto vd   = b.open(h.schema->rules().find("varDecl"));
    b.pushToken(h.tok("var", 0, CoreTokenKind::Word));
    b.pushToken(h.tok(" ",   3, CoreTokenKind::Whitespace));
    b.pushToken(h.tok("x",   4, CoreTokenKind::Word));
    b.pushToken(h.tok(" ",   5, CoreTokenKind::Whitespace));
    b.pushToken(h.tok("=",   6, CoreTokenKind::Operator));
    b.pushToken(h.tok(" ",   7, CoreTokenKind::Whitespace));
    b.pushToken(h.tok("y",   8, CoreTokenKind::Word));
    b.pushToken(h.tok(";",   9, CoreTokenKind::Operator));
    vd.close();
    stmt.close();
    root.close();
    return std::move(b).finish();
}

// Collect the text() of every visible sibling reachable by repeated
// gotoNextSibling() from the cursor's current position. Doesn't move
// the input cursor (operates on a copy).
std::vector<std::string_view> collectSiblings(TreeCursor c) {
    std::vector<std::string_view> out;
    out.push_back(c.text());
    while (c.gotoNextSibling()) out.push_back(c.text());
    return out;
}

} // namespace

// ── construction + identity ──────────────────────────────────────────────

TEST(TreeCursor, ConstructsAtRoot) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    TreeCursor c{t, t.root(), CursorMode::Cst};
    EXPECT_EQ(c.current(), t.root());
    EXPECT_EQ(c.kind(), NodeKind::Internal);
    EXPECT_EQ(c.mode(), CursorMode::Cst);
    EXPECT_EQ(&c.tree(), &t);
    EXPECT_EQ(c.depth(), 0);
}

TEST(TreeCursor, EntryPointsFromTree) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto cst = t.cursor();
    auto ast = t.astCursor();
    EXPECT_EQ(cst.mode(), CursorMode::Cst);
    EXPECT_EQ(ast.mode(), CursorMode::Ast);
    EXPECT_EQ(cst.current(), t.root());
    EXPECT_EQ(ast.current(), t.root());
}

// ── CST mode sees every node, in order ───────────────────────────────────

TEST(TreeCursor, CstWalksEveryLeafIncludingWhitespace) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto c = t.cursor();
    ASSERT_TRUE(c.gotoFirstChild());     // statement
    ASSERT_TRUE(c.gotoFirstChild());     // varDecl
    ASSERT_EQ(c.depth(), 2);

    ASSERT_TRUE(c.gotoFirstChild());     // first child of varDecl
    EXPECT_EQ(c.text(), "var");
    const std::vector<std::string_view> expected{
        "var", " ", "x", " ", "=", " ", "y", ";"
    };
    EXPECT_EQ(collectSiblings(c), expected);
}

// ── AST mode skips EmptySpace, in order ─────────────────────────────────

TEST(TreeCursor, AstSkipsEmptySpaceChildren) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto c = t.astCursor();
    ASSERT_TRUE(c.gotoFirstChild());     // statement
    ASSERT_TRUE(c.gotoFirstChild());     // varDecl

    ASSERT_TRUE(c.gotoFirstChild());
    EXPECT_EQ(c.text(), "var");
    const std::vector<std::string_view> expected{"var", "x", "=", "y", ";"};
    EXPECT_EQ(collectSiblings(c), expected);
}

TEST(TreeCursor, AstGotoFirstChildSkipsLeadingEmptySpace) {
    auto h = Harness::make(" var x = y;");   // leading space
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    auto stmt = b.open(h.schema->rules().find("statement"));
    auto vd   = b.open(h.schema->rules().find("varDecl"));
    b.pushToken(h.tok(" ",   0, CoreTokenKind::Whitespace));
    b.pushToken(h.tok("var", 1, CoreTokenKind::Word));
    b.pushToken(h.tok(" ",   4, CoreTokenKind::Whitespace));
    b.pushToken(h.tok("x",   5, CoreTokenKind::Word));
    b.pushToken(h.tok("=",   7, CoreTokenKind::Operator));
    b.pushToken(h.tok("y",   8, CoreTokenKind::Word));
    b.pushToken(h.tok(";",   9, CoreTokenKind::Operator));
    vd.close();
    stmt.close();
    root.close();
    Tree t = std::move(b).finish();

    auto cst = t.cursor();
    ASSERT_TRUE(cst.gotoFirstChild());      // statement
    ASSERT_TRUE(cst.gotoFirstChild());      // varDecl
    ASSERT_TRUE(cst.gotoFirstChild());      // " "
    EXPECT_EQ(cst.text(), " ");

    auto ast = t.astCursor();
    ASSERT_TRUE(ast.gotoFirstChild());      // statement
    ASSERT_TRUE(ast.gotoFirstChild());      // varDecl
    ASSERT_TRUE(ast.gotoFirstChild());      // "var" — the leading space is skipped
    EXPECT_EQ(ast.text(), "var");
}

// ── AST mode does NOT skip Missing/Synthetic — the load-bearing acceptance bullet.

TEST(TreeCursor, AstPreservesMissingAndSynthetic) {
    RawTreeBuilder rb{"x"};
    const auto rootRule = rb.internRule("root");

    rb.addNode(NodeKind::Internal, rootRule, SourceSpan::of(0, 1),
               NodeFlags::None, /*parent=*/ InvalidNode,
               /*children=*/ { NodeId{2}, NodeId{3}, NodeId{4} });

    rb.addNode(NodeKind::Error, InvalidRule, SourceSpan::empty(0),
               NodeFlags::Missing | NodeFlags::Synthetic | NodeFlags::HasError,
               /*parent=*/ NodeId{1});

    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::Synthetic, /*parent=*/ NodeId{1});

    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::EmptySpace, /*parent=*/ NodeId{1});

    Tree t = std::move(rb).finish(/*root=*/ NodeId{1});

    auto ast = t.astCursor();
    ASSERT_TRUE(ast.gotoFirstChild());
    // First visible child must be the Missing node — AST mode skips ONLY
    // EmptySpace; Missing and Synthetic are visible.
    EXPECT_EQ(ast.current(), NodeId{2});
    EXPECT_TRUE(has(ast.flags(), NodeFlags::Missing));
    ASSERT_TRUE(ast.gotoNextSibling());
    EXPECT_EQ(ast.current(), NodeId{3});
    EXPECT_TRUE(has(ast.flags(), NodeFlags::Synthetic));
    // The EmptySpace leaf is invisible — no more visible siblings.
    EXPECT_FALSE(ast.gotoNextSibling());
}

// ── sibling navigation, both directions ─────────────────────────────────

TEST(TreeCursor, GotoLastChildAndPrevSiblingMirrorForward) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto c = t.cursor();
    ASSERT_TRUE(c.gotoFirstChild());          // statement
    ASSERT_TRUE(c.gotoFirstChild());          // varDecl
    ASSERT_TRUE(c.gotoLastChild());           // ";"
    EXPECT_EQ(c.text(), ";");

    int steps = 0;
    while (c.gotoPrevSibling()) ++steps;
    EXPECT_EQ(steps, 7);     // 8 children → 7 backward steps to first
    EXPECT_EQ(c.text(), "var");
}

TEST(TreeCursor, GotoSiblingFailsAtBoundary) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto first = t.cursor();
    ASSERT_TRUE(first.gotoFirstChild());
    ASSERT_TRUE(first.gotoFirstChild());
    ASSERT_TRUE(first.gotoFirstChild());      // "var" — no left neighbor
    EXPECT_FALSE(first.gotoPrevSibling());
    EXPECT_EQ(first.text(), "var");           // unchanged on failed move

    auto last = t.cursor();
    ASSERT_TRUE(last.gotoFirstChild());
    ASSERT_TRUE(last.gotoFirstChild());
    ASSERT_TRUE(last.gotoLastChild());        // ";" — no right neighbor
    EXPECT_FALSE(last.gotoNextSibling());
    EXPECT_EQ(last.text(), ";");
}

TEST(TreeCursor, AstGotoLastChildSkipsTrailingEmptySpace) {
    // Tree with EmptySpace as the LAST child — gotoLastChild in AST must
    // walk backward past it to the previous visible sibling.
    auto h = Harness::make("var x = y; ");   // trailing space
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    auto stmt = b.open(h.schema->rules().find("statement"));
    auto vd   = b.open(h.schema->rules().find("varDecl"));
    b.pushToken(h.tok("var", 0, CoreTokenKind::Word));
    b.pushToken(h.tok("x",   4, CoreTokenKind::Word));
    b.pushToken(h.tok("=",   6, CoreTokenKind::Operator));
    b.pushToken(h.tok("y",   8, CoreTokenKind::Word));
    b.pushToken(h.tok(";",   9, CoreTokenKind::Operator));
    b.pushToken(h.tok(" ",  10, CoreTokenKind::Whitespace));   // trailing space
    vd.close();
    stmt.close();
    root.close();
    Tree t = std::move(b).finish();

    auto cst = t.cursor();
    ASSERT_TRUE(cst.gotoFirstChild());
    ASSERT_TRUE(cst.gotoFirstChild());
    ASSERT_TRUE(cst.gotoLastChild());
    EXPECT_EQ(cst.text(), " ");    // CST sees the trailing whitespace

    auto ast = t.astCursor();
    ASSERT_TRUE(ast.gotoFirstChild());
    ASSERT_TRUE(ast.gotoFirstChild());
    ASSERT_TRUE(ast.gotoLastChild());
    EXPECT_EQ(ast.text(), ";");    // AST skips back past the whitespace
}

TEST(TreeCursor, GotoPrevSiblingFromFirstVisibleAstChildFails) {
    auto h = Harness::make(" var x = y;");   // leading whitespace
    TreeBuilder b{h.src, h.schema};
    auto root = b.open(h.schema->rules().find("root"));
    auto stmt = b.open(h.schema->rules().find("statement"));
    auto vd   = b.open(h.schema->rules().find("varDecl"));
    b.pushToken(h.tok(" ",   0, CoreTokenKind::Whitespace));
    b.pushToken(h.tok("var", 1, CoreTokenKind::Word));
    b.pushToken(h.tok("x",   5, CoreTokenKind::Word));
    b.pushToken(h.tok("=",   7, CoreTokenKind::Operator));
    b.pushToken(h.tok("y",   8, CoreTokenKind::Word));
    b.pushToken(h.tok(";",   9, CoreTokenKind::Operator));
    vd.close();
    stmt.close();
    root.close();
    Tree t = std::move(b).finish();

    auto ast = t.astCursor();
    ASSERT_TRUE(ast.gotoFirstChild());
    ASSERT_TRUE(ast.gotoFirstChild());
    ASSERT_TRUE(ast.gotoFirstChild());    // "var" — first visible child
    EXPECT_FALSE(ast.gotoPrevSibling());  // the leading " " is not visible → no prev
    EXPECT_EQ(ast.text(), "var");
}

TEST(TreeCursor, GotoLastChildOnLeafFails) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto c = t.cursor();
    ASSERT_TRUE(c.gotoFirstChild());
    ASSERT_TRUE(c.gotoFirstChild());
    ASSERT_TRUE(c.gotoFirstChild());      // "var" — a Token leaf
    EXPECT_FALSE(c.gotoLastChild());
    EXPECT_EQ(c.text(), "var");
}

// ── parent navigation ───────────────────────────────────────────────────

TEST(TreeCursor, GotoParentReturnsToImmediateParent) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto c = t.cursor();
    ASSERT_TRUE(c.gotoFirstChild());           // statement
    ASSERT_TRUE(c.gotoFirstChild());           // varDecl
    ASSERT_TRUE(c.gotoFirstChild());           // "var"
    EXPECT_EQ(c.depth(), 3);

    ASSERT_TRUE(c.gotoParent());               // varDecl
    EXPECT_EQ(c.depth(), 2);
    ASSERT_TRUE(c.gotoParent());               // statement
    EXPECT_EQ(c.depth(), 1);
    ASSERT_TRUE(c.gotoParent());               // root
    EXPECT_EQ(c.depth(), 0);
    EXPECT_FALSE(c.gotoParent());              // at root
}

TEST(TreeCursor, AstGotoParentSkipsEmptySpaceAncestor) {
    // root → wrapper (EmptySpace Internal) → leaf
    // From leaf, AST gotoParent should jump past wrapper to root.
    RawTreeBuilder rb{"x"};
    const auto rootRule    = rb.internRule("root");
    const auto wrapperRule = rb.internRule("wrapper");

    rb.addNode(NodeKind::Internal, rootRule, SourceSpan::of(0, 1),
               NodeFlags::None, /*parent=*/ InvalidNode,
               /*children=*/ { NodeId{2} });
    rb.addNode(NodeKind::Internal, wrapperRule, SourceSpan::of(0, 1),
               NodeFlags::EmptySpace, /*parent=*/ NodeId{1},
               /*children=*/ { NodeId{3} });
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::None, /*parent=*/ NodeId{2});

    Tree t = std::move(rb).finish(/*root=*/ NodeId{1});

    // From root in AST, gotoFirstChild does not auto-descend: the only
    // immediate child is the EmptySpace wrapper, which is invisible at
    // the sibling level. No visible child → return false.
    auto ast = t.astCursor();
    EXPECT_FALSE(ast.gotoFirstChild());

    // Position the cursor directly on the leaf and walk up. AST mode
    // skips the wrapper and lands on root.
    TreeCursor leafCursor{t, NodeId{3}, CursorMode::Ast};
    ASSERT_TRUE(leafCursor.gotoParent());
    EXPECT_EQ(leafCursor.current(), NodeId{1});
}

// ── bookmark / restore ─────────────────────────────────────────────────

TEST(TreeCursor, BookmarkRoundTrip) {
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto c = t.cursor();
    ASSERT_TRUE(c.gotoFirstChild());
    ASSERT_TRUE(c.gotoFirstChild());
    ASSERT_TRUE(c.gotoFirstChild());           // "var"
    auto saved = c.mark();
    EXPECT_TRUE(saved.valid());
    EXPECT_EQ(saved.id(), c.current());
    EXPECT_EQ(saved.tree(), &t);
    EXPECT_EQ(saved.treeId(), t.id());

    ASSERT_TRUE(c.gotoNextSibling());
    EXPECT_NE(c.current(), saved.id());

    c.restore(saved);
    EXPECT_EQ(c.current(), saved.id());
    EXPECT_EQ(c.text(), "var");
}

TEST(TreeCursor, BookmarkCstAndAstAreInterchangeableOnSameTree) {
    // Bookmark doesn't capture mode — the same Bookmark can restore
    // either a CST or AST cursor of the same Tree.
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);

    auto cst = t.cursor();
    ASSERT_TRUE(cst.gotoFirstChild());
    ASSERT_TRUE(cst.gotoFirstChild());
    ASSERT_TRUE(cst.gotoFirstChild());
    auto saved = cst.mark();

    auto ast = t.astCursor();
    ast.restore(saved);
    EXPECT_EQ(ast.current(), saved.id());
    EXPECT_EQ(ast.mode(), CursorMode::Ast);
}

TEST(TreeCursorDeathTest, RestoreFromOtherTreeAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto hA = Harness::make("var x = y;");
    Tree a = buildSampleTree(hA);
    auto hB = Harness::make("var x = y;");
    Tree b = buildSampleTree(hB);

    auto cA = a.cursor();
    auto cB = b.cursor();
    auto markB = cB.mark();
    EXPECT_DEATH({ cA.restore(markB); }, "cross-tree Bookmark");
}

TEST(TreeCursorDeathTest, RestoreFromDefaultBookmarkAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    auto h = Harness::make("var x = y;");
    Tree t = buildSampleTree(h);
    auto c = t.cursor();
    TreeCursor::Bookmark empty;
    EXPECT_DEATH({ c.restore(empty); }, "invalid Bookmark");
}

// ── isAtLeaf semantics differ between CST and AST ──────────────────────

TEST(TreeCursor, IsAtLeafCstVsAst) {
    // A node whose only children are all EmptySpace is a leaf in AST
    // mode but NOT in CST mode.
    RawTreeBuilder rb{" "};
    const auto rootRule = rb.internRule("root");
    rb.addNode(NodeKind::Internal, rootRule, SourceSpan::of(0, 1),
               NodeFlags::None, /*parent=*/ InvalidNode,
               /*children=*/ { NodeId{2} });
    rb.addNode(NodeKind::Token, InvalidRule, SourceSpan::of(0, 1),
               NodeFlags::EmptySpace, /*parent=*/ NodeId{1});
    Tree t = std::move(rb).finish(/*root=*/ NodeId{1});

    auto cst = t.cursor();
    auto ast = t.astCursor();
    EXPECT_FALSE(cst.isAtLeaf());     // root has 1 child in CST
    EXPECT_TRUE(ast.isAtLeaf());      // root has 0 visible children in AST
}

// ── empty-tree behavior ──────────────────────────────────────────────────

TEST(TreeCursor, EmptyTreeMovementFailsCleanlyWithoutAbort) {
    // A Tree constructed with no nodes and an InvalidNode root — the
    // shape `TreeBuilder::finish()` produces when no `open()` ran. Cursor
    // entry points return cursors at InvalidNode; every movement returns
    // false; accessors return sentinel/empty values; depth is 0.
    detail::TreeData td;
    td.id = TreeId{99};
    // No source, no rules, no schema, no diagnostics — empty in every sense.
    Tree t{std::move(td)};

    auto cst = t.cursor();
    EXPECT_FALSE(cst.current().valid());
    EXPECT_EQ(cst.kind(), NodeKind::Internal);   // sentinel default
    EXPECT_EQ(cst.flags(), NodeFlags::None);
    EXPECT_EQ(cst.text(), std::string_view{});
    EXPECT_TRUE(cst.span().isEmpty());
    EXPECT_TRUE(cst.isAtLeaf());
    EXPECT_EQ(cst.depth(), 0);

    EXPECT_FALSE(cst.gotoFirstChild());
    EXPECT_FALSE(cst.gotoLastChild());
    EXPECT_FALSE(cst.gotoNextSibling());
    EXPECT_FALSE(cst.gotoPrevSibling());
    EXPECT_FALSE(cst.gotoParent());

    auto ast = t.astCursor();
    EXPECT_FALSE(ast.gotoFirstChild());
    EXPECT_TRUE(ast.isAtLeaf());
}
